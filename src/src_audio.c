#include "setting.h"
#include "loop.h"
#include "server.h"
#include "state.h"
#include "sources.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <libgen.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "tunables.h"

/* Volume lives in the sound server (PulseAudio, or PipeWire's pulse layer), not the kernel.
 * This speaks just enough of the PulseAudio native protocol over its unix socket: subscribe to
 * sink and server events, and read the default sink's volume and mute state when they change.
 * The socket is one more fd in the loop, so there is nothing to wake while the level is
 * stable, and no helper process.
 *   audio.volume, audio.mute              the default output: percent, on or off
 *   audio.mic.volume, audio.mic.mute      the default input; gone while there is none
 *   audio.output                          the default output's description
 *   audio.ID                              an output or an input (a sink or a source of the server): its description, then kind=output
 *                                         or input, and role= what it is for: headphone, headset, speaker, lineout, earpiece, microphone,
 *                                         linein, radio, monitor (a screen's sound), beeper (the PC speaker) or bluetooth, from what is
 *                                         plugged in now, what the device says it is, and its name. Monitors of outputs are not here.
 *   audio.ID.volume, audio.ID.mute        its own level, percent, and on or off
 *   audio.ID.default                      on for the output and the input that are the default
 * A level that moved because the device or its port did (the level that comes with it is that device's own, which the
 * server may set in two steps) says so, `cause=device`, and is not the person changing the volume. The output
 * that moved because a 3.5 mm jack did says `cause=jack`: the jack's own event tells of it. */

#define PROTOCOL_VERSION 35
#define INVALID_INDEX 0xFFFFFFFFu
#define VOLUME_NORM 0x10000u
#define COOKIE_LEN 256
#define DESCRIPTOR_LEN 20


enum { CMD_ERROR = 0, CMD_REPLY = 2, CMD_AUTH = 8, CMD_SET_CLIENT_NAME = 9,
       CMD_GET_SINK_INFO = 21, CMD_GET_SINK_INFO_LIST = 22, CMD_GET_SOURCE_INFO = 23, CMD_GET_SOURCE_INFO_LIST = 24, CMD_SUBSCRIBE = 35,
       CMD_SUBSCRIBE_EVENT = 66, CMD_GET_CARD_INFO_LIST = 89 };
enum { MASK_SINK = 0x1, MASK_SOURCE = 0x2, MASK_SERVER = 0x80, MASK_CARD = 0x200 };
enum { FACILITY_MASK = 0xF, FACILITY_SINK = 0, FACILITY_SOURCE = 1, FACILITY_SERVER = 7,
       FACILITY_CARD = 9 };
/* A card port: whether something is plugged into it, its direction and what kind it is */
enum { AVAILABLE_NO = 1 };
enum { DIRECTION_OUTPUT = 1, DIRECTION_INPUT = 2 };
enum { PORT_LINE = 4, PORT_MIC = 5, PORT_HEADPHONES = 3, PORT_HEADSET = 6 };

typedef enum { S_DOWN, S_AUTH, S_NAME, S_SUBSCRIBE, S_READY } State;

static int sock = -1, debounce_fd = -1, retry_fd = -1, watch_fd = -1;   /* the server, the volume-drag coalescing, a short retry, and the watch for its socket */
static State state = S_DOWN;
static uint32_t next_tag;
static int retries;       /* looks at a socket that was made and is not listening yet, since the last time it was */
/* What is watched: the default output and the default input, each with the same volume and
 * mute fields in the server's reply. One request is outstanding at a time. */
typedef enum { T_SINK, T_SOURCE, T_CARDS, T_SINKS, T_SOURCES, T_COUNT } TrackId;

typedef struct {
    const char *object;    /* the alias the server resolves to the current default device */
    uint32_t command;
    int last_pct, last_muted;
    char name[128];        /* the device's name, to notice that the default changed */
    char port[128];        /* its active port: a different one brings that port's own remembered volume */
    long long settle_until_ms;   /* after a change of device or port the server may still be setting its volume */
    bool dirty;            /* an event arrived: ask once the burst settles */
    bool want;             /* a request is queued */
} Track;

static Track tracks[T_COUNT] = {
    [T_SINK] = { "@DEFAULT_SINK@", CMD_GET_SINK_INFO, -1, -1, "", "", 0, false, false },
    [T_SOURCE] = { "@DEFAULT_SOURCE@", CMD_GET_SOURCE_INFO, -1, -1, "", "", 0, false, false },
    [T_CARDS] = { NULL, CMD_GET_CARD_INFO_LIST, -1, -1, "", "", 0, false, false },
    [T_SINKS] = { NULL, CMD_GET_SINK_INFO_LIST, -1, -1, "", "", 0, false, false },
    [T_SOURCES] = { NULL, CMD_GET_SOURCE_INFO_LIST, -1, -1, "", "", 0, false, false },
};
static int pending = -1;   /* the track the outstanding request is for, or -1 */


static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static long long jack_until_ms;
void audio_jack_plugged(void) { jack_until_ms = now_ms() + JACK_MS; }

static uint8_t inbuf[DESCRIPTOR_LEN + MAX_FRAME];
static size_t inlen;

/* ---- tagstruct writing ---- */

typedef struct { uint8_t b[512]; size_t n; } Out;

static void put_u32(Out *o, uint32_t v) {
    o->b[o->n++] = 'L';
    v = htonl(v);
    memcpy(o->b + o->n, &v, 4);
    o->n += 4;
}

static void put_str(Out *o, const char *s) {
    size_t l = strlen(s) + 1;
    o->b[o->n++] = 't';
    memcpy(o->b + o->n, s, l);
    o->n += l;
}

static void put_arbitrary(Out *o, const void *d, uint32_t l) {
    uint32_t be = htonl(l);
    o->b[o->n++] = 'x';
    memcpy(o->b + o->n, &be, 4);
    memcpy(o->b + o->n + 4, d, l);
    o->n += 4 + l;
}

static bool send_packet(const Out *o) {
    uint8_t hdr[DESCRIPTOR_LEN] = {0};
    uint32_t len = htonl(o->n), channel = htonl(0xFFFFFFFFu);  /* control channel */
    memcpy(hdr, &len, 4);
    memcpy(hdr + 4, &channel, 4);
    struct iovec iov[2] = { { hdr, sizeof hdr }, { (void *)o->b, o->n } };
    return writev(sock, iov, 2) == (ssize_t)(sizeof hdr + o->n);
}

static void command(Out *o, uint32_t cmd) {
    o->n = 0;
    put_u32(o, cmd);
    put_u32(o, ++next_tag);
}

/* ---- tagstruct reading ---- */

typedef struct { const uint8_t *p, *end; bool bad; } In;

static bool need(In *in, size_t n) {
    if (in->bad || (size_t)(in->end - in->p) < n) { in->bad = true; return false; }
    return true;
}

static uint32_t get_u32(In *in) {
    if (!need(in, 5) || in->p[0] != 'L') { in->bad = true; return 0; }
    uint32_t v;
    memcpy(&v, in->p + 1, 4);
    in->p += 5;
    return ntohl(v);
}

/* A string into out ("" for a null string). */
static void get_string(In *in, char *out, size_t n) {
    out[0] = '\0';
    if (!need(in, 1)) return;
    if (*in->p == 'N') { in->p++; return; }
    if (*in->p != 't') { in->bad = true; return; }
    const uint8_t *nul = memchr(in->p + 1, 0, in->end - in->p - 1);
    if (!nul) { in->bad = true; return; }
    snprintf(out, n, "%s", (const char *)in->p + 1);
    in->p = nul + 1;
}

static void skip_sample_spec(In *in) {  /* 'a' format(1) channels(1) rate(4) */
    if (!need(in, 7) || in->p[0] != 'a') { in->bad = true; return; }
    in->p += 7;
}

static void skip_channel_map(In *in) {  /* 'm' channels(1) positions(channels) */
    if (!need(in, 2) || in->p[0] != 'm') { in->bad = true; return; }
    size_t n = in->p[1];
    if (!need(in, 2 + n)) return;
    in->p += 2 + n;
}

/* 'v' channels(1) volume(4 each); returns the loudest channel. */
static uint32_t get_cvolume(In *in) {
    if (!need(in, 2) || in->p[0] != 'v') { in->bad = true; return 0; }
    size_t n = in->p[1];
    if (!need(in, 2 + 4 * n)) return 0;
    uint32_t max = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, in->p + 2 + 4 * i, 4);
        v = ntohl(v);
        if (v > max) max = v;
    }
    in->p += 2 + 4 * n;
    return max;
}

static bool get_bool(In *in) {
    if (!need(in, 1) || (*in->p != '1' && *in->p != '0')) { in->bad = true; return false; }
    return *in->p++ == '1';
}

/* ---- connection ---- */

static void arm(int fd, long ms) {
    struct itimerspec its = { .it_value = { .tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000L } };
    timerfd_settime(fd, 0, &its, NULL);
}

static void report_ports(const PortChanges *c);   /* below, with the card readings */

/* What is true now: a level and the mute of the output or the input. `device`: it moved because the device or its port did. */
static void publish_level(TrackId t, int pct, bool muted, bool device) {
    char text[32];
    snprintf(text, sizeof text, device ? "%d\tcause=device" : "%d", pct);
    bus_set(t == T_SINK ? "audio.volume" : "audio.mic.volume", text);
    snprintf(text, sizeof text, "%s%s", muted ? "on" : "off", device ? "\tcause=device" : "");
    bus_set(t == T_SINK ? "audio.mute" : "audio.mic.mute", text);
}

/* The default output's description (what the device calls itself, so cleaned, and cut to fit); `jack`: a 3.5 mm jack moved it. */
static void publish_output(const char *description, bool jack) {
    char name[160], text[VALUE_LEN];
    snprintf(name, sizeof name, "%s", description);
    state_text_clean(name);
    if (!*name) return;
    state_text_cut(name, jack ? VALUE_LEN - 1 - strlen("\tcause=jack") : VALUE_LEN - 1);
    snprintf(text, sizeof text, jack ? "%s\tcause=jack" : "%s", name);
    bus_set("audio.output", text);
}

/* There is no such default device (or no server): what was said of it is gone. */
static void retract_level(TrackId t) {
    bus_unset(t == T_SINK ? "audio.volume" : "audio.mic.volume");
    bus_unset(t == T_SINK ? "audio.mute" : "audio.mic.mute");
    if (t == T_SINK) bus_unset("audio.output");
}

static void watch_server(void);
static void forget_devices(void);

static void disconnect(void) {
    if (sock >= 0) {
        loop_remove(sock);
        close(sock);
        sock = -1;
    }
    state = S_DOWN;
    retract_level(T_SINK);
    retract_level(T_SOURCE);
    forget_devices();
    inlen = 0;
    pending = -1;
    for (int t = 0; t < T_COUNT; t++) tracks[t].want = tracks[t].dirty = false;
    report_ports(NULL);
    watch_server();   /* it may have gone with its folder, and is waited for where it will be made again */
}

static bool server_path(struct sockaddr_un *a) {
    const char *server = getenv("PULSE_SERVER"), *dir = getenv("XDG_RUNTIME_DIR");
    a->sun_family = AF_UNIX;
    if (server && *server) {
        if (!strncmp(server, "unix:", 5)) server += 5;
        if (*server != '/') return false;  /* TCP and friends: not supported */
        return snprintf(a->sun_path, sizeof a->sun_path, "%s", server) < (int)sizeof a->sun_path;
    }
    if (!dir || !*dir) return false;
    return snprintf(a->sun_path, sizeof a->sun_path, "%s/pulse/native", dir) < (int)sizeof a->sun_path;
}

/* Sends the next queued request, if none is outstanding. */
static void request_next(void) {
    if (pending >= 0 || state != S_READY) return;
    for (int t = 0; t < T_COUNT; t++) {
        Track *k = &tracks[t];
        if (!k->want) continue;
        k->want = false;
        Out o;
        command(&o, k->command);
        if (k->object) {
            put_u32(&o, INVALID_INDEX);
            put_str(&o, k->object);
        }
        if (!send_packet(&o)) { disconnect(); return; }
        pending = t;
        return;
    }
}

static void want_info(TrackId t) {
    Track *k = &tracks[t];
    k->want = true;
    request_next();
}

static uint8_t get_u8(In *in) {
    if (!need(in, 2) || in->p[0] != 'B') { in->bad = true; return 0; }
    uint8_t v = in->p[1];
    in->p += 2;
    return v;
}

static void skip_s64(In *in) {  /* 'r' and eight bytes */
    if (!need(in, 9) || in->p[0] != 'r') { in->bad = true; return; }
    in->p += 9;
}

static void skip_usec(In *in) {  /* 'U' and eight bytes */
    if (!need(in, 9) || in->p[0] != 'U') { in->bad = true; return; }
    in->p += 9;
}

/* 'P', then key (string), length (u32) and value (arbitrary) until a null string */
static void skip_proplist(In *in) {
    if (!need(in, 1) || in->p[0] != 'P') { in->bad = true; return; }
    in->p++;
    while (!in->bad && need(in, 1) && *in->p != 'N') {
        char key[128];
        get_string(in, key, sizeof key);
        get_u32(in);
        if (!need(in, 5) || in->p[0] != 'x') { in->bad = true; return; }
        uint32_t n;
        memcpy(&n, in->p + 1, 4);
        n = ntohl(n);
        if (!need(in, 5 + (size_t)n)) return;
        in->p += 5 + n;
    }
    if (!in->bad) in->p++;   /* the null string that ends it */
}

/* ---- card ports: what a jack plug did, as the server sees it ---- */


typedef struct {
    char key[200];     /* card and port name */
    bool present;      /* something is plugged in, or the hardware cannot tell */
} PortState;

static PortState port_state[MAX_PORTS];
static int n_port_state;
static bool ports_known;            /* a first reading was taken: later ones can be compared with it */
static PortChanges changes;         /* what flipped since the last answer */
static void (*port_cb)(const PortChanges *);

static void report_ports(const PortChanges *c) {
    void (*cb)(const PortChanges *) = port_cb;
    if (!cb) return;      /* nobody is waiting: what flipped is kept for whoever asks next */
    port_cb = NULL;
    cb(c);
    if (c) memset(&changes, 0, sizeof changes);
}

/* One reading of every card: remember the ports that matter to a jack, and note which of them went
 * from absent to present or back. A port that is "unknown" counts as present: a codec that cannot
 * detect its headphones reports that, and moving from "no" to it still means something happened. */
static void handle_cards(In *in) {
    PortState now[MAX_PORTS];
    PortChanges found = {0};
    int n_now = 0;

    while (!in->bad && in->p < in->end) {
        get_u32(in);                       /* index */
        char card[96], text[128];
        get_string(in, card, sizeof card);
        get_u32(in);                       /* owner module */
        get_string(in, text, sizeof text); /* driver */
        uint32_t profiles = get_u32(in);
        for (uint32_t i = 0; i < profiles && !in->bad; i++) {
            get_string(in, text, sizeof text);   /* name */
            get_string(in, text, sizeof text);   /* description */
            get_u32(in); get_u32(in); get_u32(in); get_u32(in);   /* sinks, sources, priority, available */
        }
        get_string(in, text, sizeof text); /* active profile */
        skip_proplist(in);
        uint32_t ports = get_u32(in);
        for (uint32_t i = 0; i < ports && !in->bad; i++) {
            char port[96];
            get_string(in, port, sizeof port);
            get_string(in, text, sizeof text);   /* description */
            get_u32(in);                         /* priority */
            uint32_t available = get_u32(in);
            uint8_t direction = get_u8(in);
            skip_proplist(in);
            uint32_t n = get_u32(in);            /* the profiles it is in */
            for (uint32_t j = 0; j < n && !in->bad; j++) get_string(in, text, sizeof text);
            skip_s64(in);                        /* latency offset */
            get_string(in, text, sizeof text);   /* availability group */
            uint32_t type = get_u32(in);
            if (in->bad) break;

            int *kind = type == PORT_HEADPHONES ? &found.headphones : type == PORT_HEADSET ? &found.headset
                      : type == PORT_MIC ? &found.mic
                      : type == PORT_LINE && direction == DIRECTION_OUTPUT ? &found.lineout : NULL;
            if (!kind || n_now >= MAX_PORTS) continue;       /* HDMI, speakers, line in ...: not a jack */
            PortState *st = &now[n_now++];
            snprintf(st->key, sizeof st->key, "%s/%s", card, port);
            st->present = available != AVAILABLE_NO;
            for (int k = 0; k < n_port_state; k++) {
                if (strcmp(port_state[k].key, st->key) || port_state[k].present == st->present) continue;
                *kind = st->present ? 1 : -1;
                if (pac_setting("jacks_debug")) {
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    fprintf(stderr, "jacks %ld.%03ld port %s: %s (available=%u)\n", (long)ts.tv_sec % 1000,
                            ts.tv_nsec / 1000000, st->key, st->present ? "present" : "absent", available);
                }
                break;
            }
        }
    }
    if (in->bad) return;   /* keep the earlier reading rather than compare with half of this one */

    memcpy(port_state, now, sizeof(PortState) * n_now);
    n_port_state = n_now;
    ports_known = true;
    if (found.headphones) changes.headphones = found.headphones;
    if (found.headset) changes.headset = found.headset;
    if (found.mic) changes.mic = found.mic;
    if (found.lineout) changes.lineout = found.lineout;
}

void audio_port_changes(void (*cb)(const PortChanges *)) {
    if (state != S_READY || !ports_known) { cb(NULL); return; }
    port_cb = cb;
    want_info(T_CARDS);   /* answered once the reading after this call has been compared */
}

/* The active port, which follows the mute flag after fields this does not need: the monitor, the
 * latency, the driver, the flags, the property list, the base volume, the state, the volume steps,
 * the card and the ports with what each is. Read from a copy of the reply: a server that words
 * this differently (an older protocol) leaves the port unknown, not the volume unreadable. */
static void read_active_port(In in, char *port, size_t n) {
    char text[128];
    port[0] = '\0';
    get_u32(&in);                         /* monitor source (or the sink a source monitors) */
    get_string(&in, text, sizeof text);   /* its name */
    skip_usec(&in);                       /* latency */
    get_string(&in, text, sizeof text);   /* driver */
    get_u32(&in);                         /* flags */
    skip_proplist(&in);
    skip_usec(&in);                       /* configured latency */
    if (!need(&in, 5) || in.p[0] != 'V') return;
    in.p += 5;                            /* base volume */
    get_u32(&in); get_u32(&in); get_u32(&in);   /* state, volume steps, card */
    uint32_t ports = get_u32(&in);
    for (uint32_t i = 0; i < ports && !in.bad; i++) {
        get_string(&in, text, sizeof text);   /* name */
        get_string(&in, text, sizeof text);   /* description */
        get_u32(&in); get_u32(&in);           /* priority, available */
        get_string(&in, text, sizeof text);   /* availability group */
        get_u32(&in);                         /* type */
    }
    if (in.bad || !ports) return;
    get_string(&in, text, sizeof text);       /* the active one */
    if (!in.bad) snprintf(port, n, "%s", text);
}

/* ---- every sink and source, as devices ---- */

typedef struct {
    uint32_t index, monitor_of, volume;
    char name[128], description[128], bus[24], form[24], port[128];
    bool muted;
} Record;

/* The words of a property that go in an attribute: lower case, digits and hyphens, short. */
static void attr_word(const char *in, char *out, size_t n) {
    size_t j = 0;
    for (; *in && j + 1 < n && j < 16; in++) {
        char c = *in;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[j++] = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '-';
    }
    out[j] = '\0';
}

/* The property list, keeping what says how the device is connected (device.bus) and what it is (device.form_factor). */
static void read_proplist(In *in, Record *r) {
    if (!need(in, 1) || in->p[0] != 'P') { in->bad = true; return; }
    in->p++;
    while (!in->bad && need(in, 1) && *in->p != 'N') {
        char key[128];
        get_string(in, key, sizeof key);
        get_u32(in);
        if (!need(in, 5) || in->p[0] != 'x') { in->bad = true; return; }
        uint32_t n;
        memcpy(&n, in->p + 1, 4);
        n = ntohl(n);
        if (!need(in, 5 + (size_t)n)) return;
        char value[64];
        snprintf(value, sizeof value, "%.*s", (int)(n < sizeof value - 1 ? n : sizeof value - 1), (const char *)in->p + 5);
        if (!strcmp(key, "device.bus")) attr_word(value, r->bus, sizeof r->bus);
        else if (!strcmp(key, "device.form_factor")) attr_word(value, r->form, sizeof r->form);
        in->p += 5 + n;
    }
    if (!in->bad) in->p++;
}

/* One sink or source of a list, whole: what it is, and then the fields after it, so that the next one starts where it should. */
static bool read_record(In *in, Record *r, bool source) {
    char text[128];
    memset(r, 0, sizeof *r);
    r->index = get_u32(in);
    get_string(in, r->name, sizeof r->name);
    get_string(in, r->description, sizeof r->description);
    skip_sample_spec(in);
    skip_channel_map(in);
    get_u32(in);                              /* owner module */
    uint32_t volume = get_cvolume(in);
    r->muted = get_bool(in);
    uint32_t monitor = get_u32(in);           /* a sink's monitor source, or the sink a source monitors */
    get_string(in, text, sizeof text);        /* its name */
    skip_usec(in);                            /* latency */
    get_string(in, text, sizeof text);        /* driver */
    get_u32(in);                              /* flags */
    read_proplist(in, r);
    skip_usec(in);                            /* configured latency */
    if (!need(in, 5) || in->p[0] != 'V') { in->bad = true; return false; }
    in->p += 5;                               /* base volume */
    get_u32(in); get_u32(in); get_u32(in);    /* state, volume steps, card */
    uint32_t ports = get_u32(in);
    for (uint32_t i = 0; i < ports && !in->bad; i++) {
        get_string(in, text, sizeof text); get_string(in, text, sizeof text);
        get_u32(in); get_u32(in);
        get_string(in, text, sizeof text);
        get_u32(in);
    }
    if (ports && !in->bad) get_string(in, r->port, sizeof r->port);   /* the active one */
    uint8_t formats = get_u8(in);
    for (uint8_t i = 0; i < formats && !in->bad; i++) {
        if (!need(in, 1) || in->p[0] != 'f') { in->bad = true; break; }
        in->p++;
        get_u8(in);                           /* encoding */
        skip_proplist(in);
    }
    if (in->bad) return false;
    r->volume = volume;
    r->monitor_of = source ? monitor : INVALID_INDEX;
    return true;
}

static bool has(const char *text, const char *part) { return strcasestr(text, part) != NULL; }

/* What the device is for, as a person would say it: headphone, headset, speaker, lineout, earpiece, microphone, linein, radio, monitor
 * (the sound of a screen: HDMI or DisplayPort), beeper (the PC speaker) or bluetooth (when nothing says more); "" when nothing does.
 * What is plugged in now (the active port) says most, then what the device says it is, then its name. */
static const char *role_of(const Record *r, bool output) {
    if (has(r->name, "pcspkr") || has(r->description, "PC Speaker")) return "beeper";
    const char *port = r->port;
    if (has(port, "earpiece") || has(r->form, "handset")) return "earpiece";
    if (has(port, "-fm") || has(port, "tvtuner") || has(r->description, "radio") || has(r->description, "tuner")) return "radio";
    if (has(port, "hdmi") || has(port, "displayport") || has(r->name, "hdmi") || has(r->form, "tv") || has(r->form, "monitor")) return "monitor";
    if (output) {
        if (has(port, "headphone") || !strcmp(r->form, "headphone")) return "headphone";
        if (has(port, "headset") || !strcmp(r->form, "headset") || !strcmp(r->form, "hands-free")) return "headset";
        if (has(port, "lineout") || has(port, "line-out")) return "lineout";
        if (has(port, "speaker") || !strcmp(r->form, "speaker") || !strcmp(r->form, "hifi") || !strcmp(r->form, "portable")) return "speaker";
    } else {
        if (has(port, "linein") || has(port, "line-in")) return "linein";
        if (has(port, "mic") || !strcmp(r->form, "microphone") || !strcmp(r->form, "headset") || !strcmp(r->form, "webcam") || has(r->form, "hands-free")) return "microphone";
    }
    if (!strcmp(r->bus, "bluetooth")) return "bluetooth";
    return output ? "" : "microphone";   /* an input that says nothing is a microphone */
}

static struct { char name[128]; char id[12]; bool output, seen; } audio_dev[MAX_AUDIO_DEVICES];

static void audio_key(int i, const char *prop, char *out, size_t n) {
    if (prop) snprintf(out, n, "audio.%s.%s", audio_dev[i].id, prop);
    else snprintf(out, n, "audio.%s", audio_dev[i].id);
}

static void drop_device(int i) {
    char key[40];
    audio_key(i, NULL, key, sizeof key);
    bus_unset_tree(key);
    memset(&audio_dev[i], 0, sizeof audio_dev[i]);
}

static void forget_devices(void) {
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++)
        if (audio_dev[i].id[0]) drop_device(i);
}

/* An output or an input of the server is a device while it is there: it keeps its ID as long as the server keeps its name. */
static void publish_device(const Record *r, bool output) {
    int slot = -1, free_slot = -1;
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++) {
        if (audio_dev[i].id[0] && audio_dev[i].output == output && !strcmp(audio_dev[i].name, r->name)) slot = i;
        else if (!audio_dev[i].id[0] && free_slot < 0) free_slot = i;
    }
    if (slot < 0) {
        if (free_slot < 0 || !bus_new_id("audio", audio_dev[free_slot].id, sizeof audio_dev[free_slot].id)) return;
        slot = free_slot;
        snprintf(audio_dev[slot].name, sizeof audio_dev[slot].name, "%s", r->name);
        audio_dev[slot].output = output;
    }
    audio_dev[slot].seen = true;
    const Track *def = &tracks[output ? T_SINK : T_SOURCE];
    char attrs[96];
    int len = snprintf(attrs, sizeof attrs, "kind=%s", output ? "output" : "input");
    const char *role = role_of(r, output);   /* a value has 63 bytes for the name and these: only what tells something */
    if (*role) snprintf(attrs + len, sizeof attrs - (size_t)len, "\trole=%s", role);
    char key[40], text[16];
    bus_device("audio", audio_dev[slot].id, r->description[0] ? r->description : r->name, attrs);
    audio_key(slot, "volume", key, sizeof key);
    snprintf(text, sizeof text, "%d", (int)(((uint64_t)r->volume * 100 + VOLUME_NORM / 2) / VOLUME_NORM));
    bus_set(key, text);
    audio_key(slot, "mute", key, sizeof key);
    bus_set(key, r->muted ? "on" : "off");
    audio_key(slot, "default", key, sizeof key);
    bus_set(key, def->name[0] && !strcmp(def->name, r->name) ? "on" : "off");
}

/* The answer to a list of all the sinks or all the sources: those that are not there any more go whole. */
static void handle_list(TrackId t, In *in) {
    bool output = t == T_SINKS;
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++)
        if (audio_dev[i].output == output) audio_dev[i].seen = false;
    Record r;
    while (!in->bad && in->p < in->end) {
        if (!read_record(in, &r, !output)) return;      /* cut short: what is known stays as it was */
        if (!output && r.monitor_of != INVALID_INDEX) continue;   /* the monitor of an output is not an input */
        publish_device(&r, output);
    }
    for (int i = 0; i < MAX_AUDIO_DEVICES; i++)
        if (audio_dev[i].id[0] && audio_dev[i].output == output && !audio_dev[i].seen) drop_device(i);
}

/* A sink and a source reply start the same way: index, name, description, sample spec,
 * channel map, owner module, then the volume and the mute flag, which is all that is read. */
static void handle_list(TrackId t, In *in);

static void handle_info(TrackId t, In *in) {
    if (t == T_SINKS || t == T_SOURCES) {
        handle_list(t, in);
        return;
    }
    if (t == T_CARDS) {
        handle_cards(in);
        if (!tracks[T_CARDS].want) report_ports(in->bad ? NULL : &changes);
        return;
    }
    get_u32(in);      /* index */
    char name[128], description[128];
    get_string(in, name, sizeof name);
    get_string(in, description, sizeof description);
    skip_sample_spec(in);
    skip_channel_map(in);
    get_u32(in);      /* owner module */
    uint32_t volume = get_cvolume(in);
    bool muted = get_bool(in);
    if (in->bad) return;
    char port[sizeof tracks[t].port];
    read_active_port(*in, port, sizeof port);

    Track *k = &tracks[t];
    int pct = (int)(((uint64_t)volume * 100 + VOLUME_NORM / 2) / VOLUME_NORM);

    if (pac_setting("jacks_debug")) {
        fprintf(stderr, "jacks %lld.%03lld volume %s \"%s\" port \"%s\" %d%%%s (was %d%% on \"%s\" \"%s\")\n",
                now_ms() / 1000 % 1000, now_ms() % 1000, t == T_SINK ? "output" : "input", name, port, pct,
                muted ? " muted" : "", k->last_pct, k->name, k->port);
    }

    /* A different default device, or the same one on another port: the level that comes with it is
     * that device's or port's own, and the server may set it in a second step right after, so none of
     * it is the user changing the volume. It is said, but as the device's doing. A different default
     * output is said too, and as the jack's doing if a 3.5 mm jack moved it. */
    bool changed = false;
    if (name[0] && strcmp(name, k->name) != 0) {
        bool known = k->name[0] != '\0';   /* the first reading only learns the name */
        snprintf(k->name, sizeof k->name, "%s", name);
        if (t == T_SINK) publish_output(description[0] ? description : name, now_ms() < jack_until_ms);
        if (known) changed = true;
    }
    if (strcmp(port, k->port) != 0) {
        snprintf(k->port, sizeof k->port, "%s", port);
        if (k->last_pct >= 0) changed = true;
    }
    if (changed) k->settle_until_ms = now_ms() + SRC_AUDIO_SETTLE_MS;
    if (changed || now_ms() < k->settle_until_ms) {
        k->last_pct = pct;
        k->last_muted = muted;
        publish_level(t, pct, muted, true);
        return;
    }
    if (pct == k->last_pct && muted == k->last_muted) return;  /* e.g. a device state change */
    k->last_pct = pct;
    k->last_muted = muted;
    publish_level(t, pct, muted, false);
}

static void on_reply(In *in) {
    Out o;
    switch (state) {
    case S_AUTH:
        state = S_NAME;
        command(&o, CMD_SET_CLIENT_NAME);
        o.b[o.n++] = 'P';  /* property list: application.name = "pacreact" */
        put_str(&o, "application.name");
        put_u32(&o, 7);
        put_arbitrary(&o, "pacreact", 7);
        o.b[o.n++] = 'N';
        if (!send_packet(&o)) disconnect();
        break;
    case S_NAME:
        state = S_SUBSCRIBE;
        command(&o, CMD_SUBSCRIBE);
        put_u32(&o, MASK_SINK | MASK_SOURCE | MASK_SERVER | MASK_CARD);
        if (!send_packet(&o)) disconnect();
        break;
    case S_SUBSCRIBE:
        state = S_READY;
        retries = 0;
        want_info(T_SINK);  /* learn the current state; later changes compare against it */
        want_info(T_SOURCE);
        want_info(T_CARDS);  /* the ports as they are, before any plug */
        want_info(T_SINKS);  /* every output and input there is */
        want_info(T_SOURCES);
        break;
    case S_READY:
        if (pending < 0) break;
        int t = pending;
        pending = -1;
        handle_info(t, in);
        request_next();
        break;
    case S_DOWN:
        break;
    }
}

static void handle_frame(const uint8_t *payload, size_t len) {
    In in = { payload, payload + len, false };
    uint32_t cmd = get_u32(&in);
    get_u32(&in);  /* tag: requests are strictly sequential, so the state says what this answers */
    if (in.bad) return;

    if (cmd == CMD_REPLY) {
        on_reply(&in);
    } else if (cmd == CMD_ERROR) {
        if (state == S_READY) {  /* e.g. no default source yet: not fatal */
            if (pending == T_CARDS) report_ports(NULL);
            else if (pending == T_SINK || pending == T_SOURCE) retract_level((TrackId)pending);
            pending = -1;
            request_next();
        } else {
            disconnect();
        }
    } else if (cmd == CMD_SUBSCRIBE_EVENT) {
        uint32_t event = get_u32(&in);
        get_u32(&in);  /* index */
        uint32_t facility = event & FACILITY_MASK;
        if (in.bad) return;
        if (facility == FACILITY_SINK || facility == FACILITY_SERVER) tracks[T_SINK].dirty = true;
        if (facility == FACILITY_SOURCE || facility == FACILITY_SERVER) tracks[T_SOURCE].dirty = true;
        if (facility == FACILITY_CARD) tracks[T_CARDS].dirty = true;
        if (facility == FACILITY_SINK || facility == FACILITY_SERVER || facility == FACILITY_CARD) tracks[T_SINKS].dirty = true;
        if (facility == FACILITY_SOURCE || facility == FACILITY_SERVER || facility == FACILITY_CARD) tracks[T_SOURCES].dirty = true;
        for (int t = 0; t < T_COUNT; t++)
            if (tracks[t].dirty) { arm(debounce_fd, SRC_AUDIO_DEBOUNCE_MS); break; }
    }
}

static void on_data(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    ssize_t n = recv(sock, inbuf + inlen, sizeof inbuf - inlen, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { disconnect(); return; }
    if (n < 0) return;
    inlen += n;

    while (inlen >= DESCRIPTOR_LEN) {
        uint32_t len, channel;
        memcpy(&len, inbuf, 4);
        memcpy(&channel, inbuf + 4, 4);
        len = ntohl(len);
        channel = ntohl(channel);
        if (len > MAX_FRAME) { disconnect(); return; }
        if (inlen < DESCRIPTOR_LEN + len) break;

        if (channel == 0xFFFFFFFFu) handle_frame(inbuf + DESCRIPTOR_LEN, len);
        if (sock < 0) return;  /* the handler gave up on the connection */

        inlen -= DESCRIPTOR_LEN + len;
        memmove(inbuf, inbuf + DESCRIPTOR_LEN + len, inlen);
    }
}

/* True if a connection was made. A socket that exists and does not listen (yet) is looked at again soon, a few times: the
 * server makes it and then listens; one that does not exist is waited for by the watch. */
static bool connect_server(void) {
    struct sockaddr_un a;
    if (state != S_DOWN || !server_path(&a)) return false;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    /* Local connect finishes (or fails) at once, so do it blocking, then go non-blocking. */
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        int err = errno;
        close(fd);
        if (err == ECONNREFUSED && retries < SRC_AUDIO_RETRIES && retry_fd >= 0) {
            retries++;
            arm(retry_fd, SRC_AUDIO_RETRY_MS);
        }
        return false;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    sock = fd;
    inlen = 0;
    loop_add(sock, EPOLLIN, on_data, NULL);

    /* Over a local socket the server authenticates us by our uid; the cookie can be empty. */
    static const uint8_t cookie[COOKIE_LEN];
    Out o;
    state = S_AUTH;
    command(&o, CMD_AUTH);
    put_u32(&o, PROTOCOL_VERSION);
    put_arbitrary(&o, cookie, COOKIE_LEN);
    if (!send_packet(&o)) disconnect();
    return true;
}

static void on_debounce(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(debounce_fd, &n, sizeof n) <= 0 || state != S_READY) return;
    for (int t = 0; t < T_COUNT; t++) {
        if (!tracks[t].dirty) continue;
        tracks[t].dirty = false;
        want_info(t);
    }
}

static void on_retry(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(retry_fd, &n, sizeof n) > 0) connect_server();
}

/* Waits for the server's socket without looking at it on a timer: inotify on the folder it is made in (or, if that is not
 * there either, on the one above, for the folder). Called again when something is made, since a folder that was missing
 * now is not. */
static int watched[2] = { -1, -1 };

static void watch_server(void) {
    struct sockaddr_un a;
    if (watch_fd < 0 || !server_path(&a)) return;
    char path[sizeof a.sun_path], parent[sizeof a.sun_path];
    snprintf(path, sizeof path, "%s", a.sun_path);
    snprintf(parent, sizeof parent, "%s", dirname(path));             /* the folder of the socket */
    snprintf(path, sizeof path, "%s", parent);
    char up[sizeof a.sun_path];
    snprintf(up, sizeof up, "%s", dirname(path));                      /* and the one above it */
    int wd = inotify_add_watch(watch_fd, parent, IN_CREATE | IN_MOVED_TO);
    int wu = wd < 0 || strcmp(up, parent) ? inotify_add_watch(watch_fd, up, IN_CREATE | IN_MOVED_TO) : -1;
    watched[0] = wd;
    watched[1] = wu;
}

static void on_watch(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    if (read(watch_fd, buf, sizeof buf) <= 0) return;
    if (state != S_DOWN) return;
    watch_server();        /* the folder may be new */
    retries = 0;
    connect_server();
}

void audio_init(void) {
    debounce_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    retry_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (debounce_fd < 0) return;
    loop_add(debounce_fd, EPOLLIN, on_debounce, NULL);
    if (retry_fd >= 0) loop_add(retry_fd, EPOLLIN, on_retry, NULL);
    if (watch_fd >= 0) loop_add(watch_fd, EPOLLIN, on_watch, NULL);
    watch_server();     /* first, so that a server that starts in between is not missed */
    connect_server();
}
