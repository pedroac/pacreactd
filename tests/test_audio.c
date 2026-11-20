/* The PulseAudio client: handshake packets, framing, the sink-info parser and what is published from it. The module is
 * included so its statics are reachable; the bus and the loop are stubbed. The sink-info reply below is a frame captured
 * from a real pipewire-pulse server.
 *
 * `slider_calls` and `toast_calls` count what a listener that shows a bar and a message would show (pacreact is one), by its
 * rules: a level or mute that changed from a value it already knew, and did not say `cause=device`, is the person's, and
 * so is a bar; an output that changed from a known one, and did not say `cause=jack`, is a message; and what first
 * appears is learned, not shown. */
#include "../src/loop.h"
#include "../src/server.h"
#include "../src/sources.h"
#include "../src/state.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

static int slider_calls;
static char slider_label[256];
static int slider_pct;
static bool slider_dim;
static int toast_calls;
static char toast_text[256];

/* What the listener knows: per track (output, input) the level, the mute, and for the output its description */
static struct { int pct, muted; } listener[2] = { { -1, -1 }, { -1, -1 } };
static char listener_output[64];
static int heard_pct[2];

static char published[16][2][64];
static const char *told_of(const char *key) {
    for (int i = 0; i < 16 && published[i][0][0]; i++)
        if (!strcmp(published[i][0], key)) return published[i][1];
    return "(gone)";
}

static void listener_reset(void) {
    listener[0].pct = listener[0].muted = listener[1].pct = listener[1].muted = -1;
    listener_output[0] = '\0';
    slider_calls = toast_calls = 0;
    memset(published, 0, sizeof published);
}

bool bus_set(const char *key, const char *value) {
    if (!strncmp(key, "audio.a", 7)) return state_set(key, value) >= 0;   /* a device (audio.a1) and what it has: kept as it is */
    int i = 0;
    while (i < 15 && published[i][0][0] && strcmp(published[i][0], key)) i++;
    snprintf(published[i][0], 64, "%s", key);
    snprintf(published[i][1], 64, "%s", value);

    bool device = strstr(value, "\tcause=device") != NULL, jack = strstr(value, "\tcause=jack") != NULL;
    int t = !strncmp(key, "audio.mic.", 10);
    if (!strcmp(key, "audio.volume") || !strcmp(key, "audio.mic.volume")) {
        heard_pct[t] = atoi(value);   /* the mute that follows decides: they are published together */
    } else if (!strcmp(key, "audio.mute") || !strcmp(key, "audio.mic.mute")) {
        int muted = !strncmp(value, "on", 2);
        bool changed = listener[t].pct != heard_pct[t] || listener[t].muted != muted;
        if (changed && listener[t].pct >= 0 && !device) {
            slider_calls++;
            snprintf(slider_label, sizeof slider_label, "%s%s", t ? "Microphone" : "Volume", muted ? " — muted" : "");
            slider_pct = heard_pct[t];
            slider_dim = muted;
        }
        listener[t].pct = heard_pct[t];
        listener[t].muted = muted;
    } else if (!strcmp(key, "audio.output")) {
        char name[64];
        snprintf(name, sizeof name, "%s", value);
        char *cause = strstr(name, "\tcause=jack");
        if (cause) *cause = '\0';
        if (listener_output[0] && strcmp(listener_output, name) && !jack) {
            toast_calls++;
            snprintf(toast_text, sizeof toast_text, "Audio output: %s", name);
        }
        snprintf(listener_output, sizeof listener_output, "%s", name);
    }
    return true;
}

void bus_unset(const char *key) {
    if (!strncmp(key, "audio.a", 7)) { state_unset(key); return; }
    int t = !strncmp(key, "audio.mic.", 10);
    if (!strcmp(key, "audio.volume") || !strcmp(key, "audio.mic.volume") || !strcmp(key, "audio.mute") || !strcmp(key, "audio.mic.mute")) listener[t].pct = listener[t].muted = -1;
    if (!strcmp(key, "audio.output")) listener_output[0] = '\0';
    for (int i = 0; i < 16; i++)
        if (!strcmp(published[i][0], key)) snprintf(published[i][1], 64, "(gone)");
}

bool bus_set_level(const char *key, const char *value, Level level) { (void)key; (void)value; (void)level; return true; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }
int loop_remove(int fd) { (void)fd; return 0; }

#include "../src/src_audio.c"

/* ---- fixtures ---- */

typedef struct { uint8_t b[4096]; size_t n; } Buf8;

static void raw(Buf8 *o, const void *d, size_t n) { memcpy(o->b + o->n, d, n); o->n += n; }
static void be_u32(Buf8 *o, uint32_t v) { o->b[o->n++] = 'L'; v = htonl(v); raw(o, &v, 4); }

/* A GET_SINK_INFO reply payload as pipewire-pulse sent it: volume `vol` on two channels. */
static Buf8 sink_info_named(uint32_t vol, char mute, const char *name, const char *desc) {
    Buf8 o = {0};
    be_u32(&o, 2);      /* REPLY */
    be_u32(&o, 4);      /* tag */
    be_u32(&o, 0x313);  /* sink index */
    o.b[o.n++] = 't'; raw(&o, name, strlen(name) + 1);
    o.b[o.n++] = 't'; raw(&o, desc, strlen(desc) + 1);
    raw(&o, "a\x07\x02\x00\x00\xbb\x80", 7);  /* sample spec: format 7, 2 channels, 48000 Hz */
    raw(&o, "m\x02\x01\x02", 4);               /* channel map: front-left, front-right */
    be_u32(&o, 0xFFFFFFFFu);                   /* owner module */
    o.b[o.n++] = 'v'; o.b[o.n++] = 2;
    uint32_t v = htonl(vol);
    raw(&o, &v, 4);
    raw(&o, &v, 4);
    o.b[o.n++] = mute;                         /* '1' muted, '0' not */
    be_u32(&o, 0x312);                         /* monitor source: the real frame continues */
    return o;
}

/* The same reply continued as pipewire-pulse sends it (protocol 35, captured from the real server):
 * the monitor, latency, driver, flags, an empty property list, base volume, state, volume steps, the
 * card, two ports and the active one. */
static Buf8 sink_info_port(uint32_t vol, const char *name, const char *active) {
    Buf8 o = sink_info_named(vol, '0', name, "Built-in Audio");
    o.b[o.n++] = 't'; raw(&o, "monitor", 8);
    o.b[o.n++] = 'U'; raw(&o, "\0\0\0\0\0\0\0\0", 8);
    o.b[o.n++] = 't'; raw(&o, "PipeWire", 9);
    be_u32(&o, 183);                           /* flags */
    o.b[o.n++] = 'P'; o.b[o.n++] = 'N';        /* property list, empty */
    o.b[o.n++] = 'U'; raw(&o, "\0\0\0\0\0\0\0\0", 8);
    o.b[o.n++] = 'V'; raw(&o, "\0\1\0\0", 4);   /* base volume */
    be_u32(&o, 2); be_u32(&o, 0); be_u32(&o, 44);   /* state, volume steps, card */
    be_u32(&o, 2);                                  /* ports */
    const char *ports[2] = { "analog-output-lineout", "analog-output-headphones" };
    for (int i = 0; i < 2; i++) {
        o.b[o.n++] = 't'; raw(&o, ports[i], strlen(ports[i]) + 1);
        o.b[o.n++] = 't'; raw(&o, "Port", 5);
        be_u32(&o, 9000); be_u32(&o, 1);
        o.b[o.n++] = 't'; raw(&o, "Legacy 4", 9);
        be_u32(&o, 4);
    }
    o.b[o.n++] = 't'; raw(&o, active, strlen(active) + 1);
    return o;
}

static Buf8 sink_info(uint32_t vol, char mute) {
    return sink_info_named(vol, mute, "alsa_output.pci-0000_00_1f.3.analog-stereo", "Built-in Audio Analog Stereo");
}

static Buf8 framed(const Buf8 *payload) {
    Buf8 o = {0};
    uint32_t len = htonl(payload->n), ch = htonl(0xFFFFFFFFu), zero = 0;
    raw(&o, &len, 4);
    raw(&o, &ch, 4);
    raw(&o, &zero, 4);
    raw(&o, &zero, 4);
    raw(&o, &zero, 4);
    raw(&o, payload->b, payload->n);
    return o;
}

static int peer = -1;

static void connect_pair(State st) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    sock = sv[0];
    peer = sv[1];
    state = st;
    inlen = 0;
    pending = -1;
    for (int t = 0; t < T_COUNT; t++) {
        tracks[t].want = tracks[t].dirty = false;
        tracks[t].last_pct = tracks[t].last_muted = -1;
    }
    listener_reset();
    for (int t = 0; t < T_COUNT; t++) tracks[t].name[0] = tracks[t].port[0] = '\0', tracks[t].settle_until_ms = 0, jack_until_ms = 0;
}

static void drop_pair(void) {
    if (sock >= 0) close(sock);
    sock = -1;
    if (peer >= 0) close(peer);
    peer = -1;
}

/* Reads one framed packet the client wrote, returning its payload length. */
static size_t read_packet(uint8_t *payload, size_t cap) {
    uint8_t hdr[DESCRIPTOR_LEN];
    assert(recv(peer, hdr, sizeof hdr, MSG_WAITALL | MSG_DONTWAIT) == (ssize_t)sizeof hdr);
    uint32_t len, ch;
    memcpy(&len, hdr, 4);
    memcpy(&ch, hdr + 4, 4);
    len = ntohl(len);
    assert(ntohl(ch) == 0xFFFFFFFFu);  /* control channel */
    assert(len <= cap);
    assert(recv(peer, payload, len, MSG_WAITALL | MSG_DONTWAIT) == (ssize_t)len);
    return len;
}

static uint32_t u32_at(const uint8_t *p) { uint32_t v; memcpy(&v, p + 1, 4); return ntohl(v); }

static void feed_reply(void) {  /* a bare REPLY, as the server answers handshake steps */
    uint8_t payload[10] = { 'L', 0, 0, 0, 2, 'L', 0, 0, 0, 1 };
    In in = { payload + 10, payload + 10, false };
    on_reply(&in);
}

/* ---- tests ---- */

static void test_handshake_packets(void) {
    connect_pair(S_AUTH);
    uint8_t p[512];

    feed_reply();  /* AUTH accepted -> SET_CLIENT_NAME */
    assert(state == S_NAME);
    size_t n = read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_SET_CLIENT_NAME);
    assert(p[10] == 'P');
    assert(!strcmp((char *)p + 12, "application.name"));
    assert(n == 5 + 5 + 1 + 1 + 17 + 5 + 1 + 4 + 7 + 1);  /* the proplist is exactly this long */
    assert(p[n - 1] == 'N');

    feed_reply();  /* name accepted -> SUBSCRIBE sink|server */
    assert(state == S_SUBSCRIBE);
    n = read_packet(p, sizeof p);
    assert(n == 15 && u32_at(p) == CMD_SUBSCRIBE && u32_at(p + 10) == (MASK_SINK | MASK_SOURCE | MASK_SERVER | MASK_CARD));

    feed_reply();  /* subscribed -> ready, asks for the default sink silently */
    assert(state == S_READY && pending == T_SINK);
    n = read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SINK_INFO && u32_at(p + 10) == INVALID_INDEX);
    assert(p[15] == 't' && !strcmp((char *)p + 16, "@DEFAULT_SINK@"));
    assert(n == 15 + 1 + 15);

    /* Once the output is known, the input is asked for (one request at a time) */
    Buf8 reply = sink_info(9172, '0');
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(pending == T_SOURCE);
    n = read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SOURCE_INFO && u32_at(p + 10) == INVALID_INDEX);
    assert(p[15] == 't' && !strcmp((char *)p + 16, "@DEFAULT_SOURCE@"));
    assert(slider_calls == 0);                     /* learning the state is silent */
    drop_pair();
}

static void test_sink_info_parsing(void) {
    connect_pair(S_READY);

    /* The captured frame: 9172 on both channels, muted = 14%. Learning it is silent. */
    pending = T_SINK;
    Buf8 reply = sink_info(9172, '1');
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(pending == -1 && slider_calls == 0);
    assert(tracks[T_SINK].last_pct == 14 && tracks[T_SINK].last_muted == 1);
    /* Learned silently, but told to pacreactd all the same: a status bar shows it */
    assert(!strcmp(told_of("audio.volume"), "14") && !strcmp(told_of("audio.mute"), "on"));

    /* The same state again changes nothing, even when announcing (a sink state change). */
    pending = T_SINK;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 0);

    /* Unmuted at 25%. */
    reply = sink_info(0x4000, '0');
    pending = T_SINK;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 1 && !strcmp(slider_label, "Volume") && slider_pct == 25 && !slider_dim);

    /* Muting at the same level is a change. */
    reply = sink_info(0x4000, '1');
    pending = T_SINK;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 2 && !strcmp(slider_label, "Volume — muted") && slider_pct == 25 && slider_dim);

    /* Rounding, and above 100%. */
    struct { uint32_t raw; int pct; } cases[] = {
        { 0, 0 }, { 0x8000, 50 }, { 0x10000, 100 }, { 0x13333, 120 }, { 0x1, 0 }, { 0x7FFF, 50 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        tracks[T_SINK].last_pct = -1;
        reply = sink_info(cases[i].raw, '0');
        pending = T_SINK;
        in = (In){ reply.b + 10, reply.b + reply.n, false };
        on_reply(&in);
        assert(slider_pct == cases[i].pct);
    }
    drop_pair();
}

static void read_reply(TrackId t, Buf8 reply, bool show) {
    tracks[t].settle_until_ms = 0;   /* a reply that comes later than the settling time */
    pending = t;
    (void)show;
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
}

/* One entry of a list of sinks or sources, as the server writes it: after the volume and mute, the monitor, the latency, the driver, the
 * flags, the properties (the bus and what the device is), the base volume, state, steps, card, ports and the formats it takes. */
static void list_entry(Buf8 *o, uint32_t index, const char *name, const char *desc, uint32_t vol, char mute, uint32_t monitor,
                       const char *bus, const char *form) {
    be_u32(o, index);
    o->b[o->n++] = 't'; raw(o, name, strlen(name) + 1);
    o->b[o->n++] = 't'; raw(o, desc, strlen(desc) + 1);
    raw(o, "a\x07\x02\x00\x00\xbb\x80", 7);
    raw(o, "m\x02\x01\x02", 4);
    be_u32(o, 0xFFFFFFFFu);
    o->b[o->n++] = 'v'; o->b[o->n++] = 2;
    uint32_t v = htonl(vol);
    raw(o, &v, 4); raw(o, &v, 4);
    o->b[o->n++] = mute;
    be_u32(o, monitor);
    o->b[o->n++] = 't'; raw(o, "monitor", 8);
    o->b[o->n++] = 'U'; raw(o, "\0\0\0\0\0\0\0\0", 8);
    o->b[o->n++] = 't'; raw(o, "PipeWire", 9);
    be_u32(o, 183);
    o->b[o->n++] = 'P';
    const char *props[2][2] = { { "device.bus", bus }, { "device.form_factor", form } };
    for (int i = 0; i < 2; i++) {
        if (!props[i][1]) continue;
        size_t len = strlen(props[i][1]) + 1;
        o->b[o->n++] = 't'; raw(o, props[i][0], strlen(props[i][0]) + 1);
        be_u32(o, (uint32_t)len);
        o->b[o->n++] = 'x';
        uint32_t n = htonl((uint32_t)len);
        raw(o, &n, 4);
        raw(o, props[i][1], len);
    }
    o->b[o->n++] = 'N';
    o->b[o->n++] = 'U'; raw(o, "\0\0\0\0\0\0\0\0", 8);
    o->b[o->n++] = 'V'; raw(o, "\0\1\0\0", 4);
    be_u32(o, 2); be_u32(o, 0); be_u32(o, 44);
    be_u32(o, 0);                                       /* no ports */
    o->b[o->n++] = 'B'; o->b[o->n++] = 1;               /* one format: */
    o->b[o->n++] = 'f'; o->b[o->n++] = 'B'; o->b[o->n++] = 1; o->b[o->n++] = 'P'; o->b[o->n++] = 'N';
}

static Buf8 list_start(void) {
    Buf8 o = {0};
    be_u32(&o, 2);
    be_u32(&o, 5);
    return o;
}

static void test_roles(void) {
    static const struct { const char *name, *desc, *bus, *form, *port; bool output; const char *role; } cases[] = {
        { "alsa_output.pci.analog-stereo", "Built-in Audio", "pci", "internal", "analog-output-headphones", true, "headphone" },
        { "alsa_output.pci.analog-stereo", "Built-in Audio", "pci", "internal", "analog-output-speaker", true, "speaker" },
        { "alsa_output.pci.analog-stereo", "Built-in Audio", "pci", "internal", "analog-output-lineout", true, "lineout" },
        { "alsa_output.pci.analog-stereo", "Built-in Audio", "pci", "internal", "analog-output-earpiece", true, "earpiece" },
        { "alsa_output.usb.analog", "USB Headset", "usb", "headset", "", true, "headset" },
        { "alsa_output.usb.analog", "USB Headset", "usb", "hands-free", "", true, "headset" },
        { "alsa_output.pci.hdmi-stereo", "HDMI / DisplayPort", "pci", "", "hdmi-output-0", true, "monitor" },
        { "alsa_output.platform-pcspkr.analog-mono", "Beeper", "", "", "", true, "beeper" },
        { "bluez_output.AA", "Thing", "bluetooth", "", "", true, "bluetooth" },
        { "bluez_output.AA", "WH", "bluetooth", "headphone", "", true, "headphone" },
        { "alsa_output.x", "Mystery", "pci", "", "", true, "" },
        { "alsa_input.pci.analog-stereo", "Built-in Audio", "pci", "internal", "analog-input-linein", false, "linein" },
        { "alsa_input.pci.analog-stereo", "Built-in Audio", "pci", "internal", "analog-input-mic", false, "microphone" },
        { "alsa_input.pci.analog-stereo", "Built-in Audio", "pci", "internal", "analog-input-fm", false, "radio" },
        { "alsa_input.usb.cam", "Webcam", "usb", "webcam", "", false, "microphone" },
        { "alsa_input.x", "Mystery", "pci", "", "", false, "microphone" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        Record r = {0};
        snprintf(r.name, sizeof r.name, "%s", cases[i].name);
        snprintf(r.description, sizeof r.description, "%s", cases[i].desc);
        snprintf(r.bus, sizeof r.bus, "%s", cases[i].bus);
        snprintf(r.form, sizeof r.form, "%s", cases[i].form);
        snprintf(r.port, sizeof r.port, "%s", cases[i].port);
        assert(!strcmp(role_of(&r, cases[i].output), cases[i].role));
    }
}

static void test_devices(void) {
    connect_pair(S_READY);
    state_clear();
    memset(audio_dev, 0, sizeof audio_dev);
    const char *speakers = "alsa_output.pci.analog-stereo", *bt = "bluez_output.AA_BB.1", *mic = "alsa_input.pci.analog-stereo";
    snprintf(tracks[T_SINK].name, sizeof tracks[T_SINK].name, "%s", speakers);
    snprintf(tracks[T_SOURCE].name, sizeof tracks[T_SOURCE].name, "%s", mic);

    Buf8 sinks = list_start();
    list_entry(&sinks, 10, speakers, "Built-in Audio", 0x8000, '0', 11, "pci", "internal");
    list_entry(&sinks, 12, bt, "WH-1000XM4", 0x4000, '1', 13, "bluetooth", "headset");
    read_reply(T_SINKS, sinks, false);
    char id1[12], id2[12], key[40];
    snprintf(id1, sizeof id1, "%s", audio_dev[0].id);
    snprintf(id2, sizeof id2, "%s", audio_dev[1].id);
    assert(id1[0] && id2[0] && strcmp(id1, id2));
    snprintf(key, sizeof key, "audio.%s", id1);
    assert(!strcmp(state_get(key), "Built-in Audio\tkind=output"));                         /* internal and pci: nothing to add */
    snprintf(key, sizeof key, "audio.%s", id2);
    assert(!strcmp(state_get(key), "WH-1000XM4\tkind=output\trole=headset"));
    snprintf(key, sizeof key, "audio.%s.volume", id1); assert(!strcmp(state_get(key), "50"));
    snprintf(key, sizeof key, "audio.%s.default", id1); assert(!strcmp(state_get(key), "on"));
    snprintf(key, sizeof key, "audio.%s.default", id2); assert(!strcmp(state_get(key), "off"));
    snprintf(key, sizeof key, "audio.%s.mute", id2); assert(!strcmp(state_get(key), "on"));

    /* an input; the monitor of an output is not one */
    Buf8 sources = list_start();
    list_entry(&sources, 20, mic, "Built-in Microphone", 0x10000, '0', 0xFFFFFFFFu, "pci", "internal");
    list_entry(&sources, 21, "alsa_output.pci.analog-stereo.monitor", "Monitor of Built-in Audio", 0x10000, '0', 10, "pci", NULL);
    read_reply(T_SOURCES, sources, false);
    assert(audio_dev[2].id[0] && !audio_dev[3].id[0]);
    snprintf(key, sizeof key, "audio.%s", audio_dev[2].id);
    assert(!strcmp(state_get(key), "Built-in Microphone\tkind=input\trole=microphone"));

    /* the default moves: the same devices, the other one is the default */
    snprintf(tracks[T_SINK].name, sizeof tracks[T_SINK].name, "%s", bt);
    Buf8 again = list_start();
    list_entry(&again, 10, speakers, "Built-in Audio", 0x8000, '0', 11, "pci", "internal");
    list_entry(&again, 12, bt, "WH-1000XM4", 0x4000, '0', 13, "bluetooth", "headset");
    read_reply(T_SINKS, again, false);
    assert(!strcmp(audio_dev[0].id, id1) && !strcmp(audio_dev[1].id, id2));                 /* the same IDs */
    snprintf(key, sizeof key, "audio.%s.default", id2); assert(!strcmp(state_get(key), "on"));
    snprintf(key, sizeof key, "audio.%s.default", id1); assert(!strcmp(state_get(key), "off"));
    snprintf(key, sizeof key, "audio.%s.mute", id2); assert(!strcmp(state_get(key), "off"));

    /* the Bluetooth headset goes: whole, with its level; the input is not touched */
    Buf8 one = list_start();
    list_entry(&one, 10, speakers, "Built-in Audio", 0x8000, '0', 11, "pci", "internal");
    read_reply(T_SINKS, one, false);
    snprintf(key, sizeof key, "audio.%s", id2);
    assert(!state_get(key));
    snprintf(key, sizeof key, "audio.%s.volume", id2); assert(!state_get(key));
    snprintf(key, sizeof key, "audio.%s", audio_dev[2].id); assert(state_get(key));
    snprintf(key, sizeof key, "audio.%s", id1); assert(state_get(key));

    /* back again: a new ID, never the one that went */
    Buf8 back = list_start();
    list_entry(&back, 10, speakers, "Built-in Audio", 0x8000, '0', 11, "pci", "internal");
    list_entry(&back, 12, bt, "WH-1000XM4", 0x4000, '0', 13, "bluetooth", "headset");
    read_reply(T_SINKS, back, false);
    assert(strcmp(audio_dev[1].id, id2) && audio_dev[1].id[0]);

    /* a list cut short changes nothing; the server going takes them all */
    Buf8 cut = list_start();
    list_entry(&cut, 10, speakers, "Built-in Audio", 0x8000, '0', 11, "pci", "internal");
    cut.n -= 5;
    char before[40];
    snprintf(before, sizeof before, "audio.%s", audio_dev[1].id);
    read_reply(T_SINKS, cut, false);
    assert(state_get(before));
    forget_devices();
    assert(!state_get(before) && !audio_dev[0].id[0]);
    drop_pair();
}

static void test_output_change(void) {
    connect_pair(S_READY);
    const char *a = "alsa_output.pci-0000_00_1f.3.analog-stereo";
    const char *b = "bluez_output.AA_BB_CC_DD_EE_FF.1";

    /* Learning the output at start-up says nothing, and remembers its name */
    read_reply(T_SINK, sink_info_named(0x4000, '0', a, "Built-in Audio"), false);
    assert(toast_calls == 0 && slider_calls == 0 && !strcmp(tracks[T_SINK].name, a));

    /* The same output again: just the volume, as before */
    read_reply(T_SINK, sink_info_named(0x8000, '0', a, "Built-in Audio"), true);
    assert(slider_calls == 1 && slider_pct == 50);

    /* A different default output: one message naming it, no volume bar: that volume is the new device's */
    read_reply(T_SINK, sink_info_named(0x6000, '0', b, "WH-1000XM4"), true);
    assert(slider_calls == 1 && toast_calls == 1 && !strcmp(toast_text, "Audio output: WH-1000XM4"));
    assert(tracks[T_SINK].last_pct == 38 && !strcmp(tracks[T_SINK].name, b));
    /* ...and it is not repeated, nor does the same volume come back as a change */
    read_reply(T_SINK, sink_info_named(0x6000, '0', b, "WH-1000XM4"), true);
    assert(slider_calls == 1 && toast_calls == 1);
    read_reply(T_SINK, sink_info_named(0x4000, '0', b, "WH-1000XM4"), true);
    assert(slider_calls == 2 && !strcmp(slider_label, "Volume"));

    /* Back again; without a description the name is used */
    read_reply(T_SINK, sink_info_named(0x4000, '0', a, ""), true);
    assert(slider_calls == 2 && toast_calls == 2 && !strcmp(toast_text, "Audio output: alsa_output.pci-0000_00_1f.3.analog-stereo"));

    /* Re-learning after a reconnect: everything was withdrawn when the connection went, so what comes back is new, not a change */
    retract_level(T_SINK);
    read_reply(T_SINK, sink_info_named(0x4000, '0', b, "WH-1000XM4"), false);
    assert(slider_calls == 2 && toast_calls == 2 && !strcmp(tracks[T_SINK].name, b));

    /* The default microphone changing (a headset's jack moves it) is learned, not announced: the
     * bar would be about a device whose volume the user did not touch */
    read_reply(T_SOURCE, sink_info_named(0x8000, '0', "alsa_input.pci-a", "Built-in mic"), false);   /* start-up */
    read_reply(T_SOURCE, sink_info_named(0x4000, '0', "alsa_input.usb-a", "USB mic"), true);
    assert(slider_calls == 2 && !strcmp(tracks[T_SOURCE].name, "alsa_input.usb-a"));
    read_reply(T_SOURCE, sink_info_named(0x2000, '0', "alsa_input.usb-a", "USB mic"), true);
    assert(slider_calls == 3 && !strcmp(slider_label, "Microphone"));   /* a real change still is */

    drop_pair();
}

static void test_microphone_track(void) {
    connect_pair(S_READY);

    /* A source reply has the same layout; it is the microphone's keys, and what first appears is learned, not shown */
    pending = T_SOURCE;
    Buf8 reply = sink_info(0x8000, '0');
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 0);
    assert(tracks[T_SOURCE].last_pct == 50 && tracks[T_SINK].last_pct == -1);   /* kept apart */
    assert(!strcmp(told_of("audio.mic.volume"), "50") && !strcmp(told_of("audio.mic.mute"), "off"));
    assert(!strcmp(told_of("audio.volume"), "(gone)"));                          /* nothing of the output */

    pending = T_SOURCE;
    reply = sink_info(0x8000, '1');
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 1 && !strcmp(slider_label, "Microphone — muted") && slider_dim && slider_pct == 50);
    assert(!strcmp(told_of("audio.mic.mute"), "on"));

    /* The same input state again: nothing, while the output has its own memory */
    pending = T_SOURCE;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 1);
    pending = T_SINK;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 1 && !strcmp(told_of("audio.volume"), "50") && !strcmp(told_of("audio.mute"), "on"));   /* first of the output */

    /* No default input any more (or no server): what was said of it is gone, and so is the output's description */
    retract_level(T_SOURCE);
    assert(!strcmp(told_of("audio.mic.volume"), "(gone)") && !strcmp(told_of("audio.mic.mute"), "(gone)"));
    assert(!strcmp(told_of("audio.volume"), "50"));
    retract_level(T_SINK);
    assert(!strcmp(told_of("audio.volume"), "(gone)") && !strcmp(told_of("audio.output"), "(gone)"));
    drop_pair();
}

static void test_port_change_is_not_a_volume_change(void) {
    connect_pair(S_READY);
    const char *a = "alsa_output.pci-0000_00_1f.3.analog-stereo";
    read_reply(T_SINK, sink_info_port(0x4000, a, "analog-output-lineout"), false);
    assert(!strcmp(tracks[T_SINK].port, "analog-output-lineout"));

    /* Plugging headphones: the same output, another port, with that port's own volume. Not shown */
    read_reply(T_SINK, sink_info_port(0xA000, a, "analog-output-headphones"), true);
    assert(slider_calls == 0 && tracks[T_SINK].last_pct == 63 && !strcmp(tracks[T_SINK].port, "analog-output-headphones"));

    /* The user changing it afterwards is a volume change, as ever */
    read_reply(T_SINK, sink_info_port(0x8000, a, "analog-output-headphones"), true);
    assert(slider_calls == 1 && !strcmp(slider_label, "Volume") && slider_pct == 50);

    /* A reply this parser cannot take apart past the mute flag leaves the port unknown, the volume fine */
    read_reply(T_SINK, sink_info(0x4000, '0'), true);
    assert(slider_calls == 1 && !strcmp(tracks[T_SINK].port, "") && tracks[T_SINK].last_pct == 25);
    read_reply(T_SINK, sink_info(0x2000, '0'), true);
    assert(slider_calls == 2 && slider_pct == 13);
    drop_pair();
}

static void test_volume_settling(void) {
    connect_pair(S_READY);
    const char *a = "alsa_input.pci-0000_00_1f.3.analog-stereo", *m = "alsa_output.hdmi-stereo.monitor";
    read_reply(T_SOURCE, sink_info_named(0x8000, '0', a, "Built-in"), false);
    read_reply(T_SOURCE, sink_info_named(0x10000, '0', m, "Monitor"), true);   /* the default moved to the monitor */
    int before = slider_calls;

    /* Back to the microphone: the server first reports the level it carried over, then sets the real one
     * a moment later. Neither is the user's doing */
    tracks[T_SOURCE].settle_until_ms = 0;
    pending = T_SOURCE;
    Buf8 reply = sink_info_named(0x10000, '0', a, "Built-in");
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    pending = T_SOURCE;
    reply = sink_info_named(0x8000, '0', a, "Built-in");
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == before && tracks[T_SOURCE].last_pct == 50);

    /* Once it has settled, a change is the user's */
    read_reply(T_SOURCE, sink_info_named(0x4000, '0', a, "Built-in"), true);
    assert(slider_calls == before + 1);
    drop_pair();
}

static void test_jack_output_change_is_quiet(void) {
    connect_pair(S_READY);
    const char *a = "alsa_output.analog", *h = "alsa_output.hdmi";
    read_reply(T_SINK, sink_info_named(0x8000, '0', a, "Built-in"), false);

    /* The output moved because a jack was plugged: the jack's toast said it */
    audio_jack_plugged();
    read_reply(T_SINK, sink_info_named(0x8000, '0', h, "HDMI"), true);
    assert(toast_calls == 0 && slider_calls == 0 && !strcmp(tracks[T_SINK].name, h));

    /* Without one (a USB or Bluetooth device) it is said */
    jack_until_ms = 0;
    read_reply(T_SINK, sink_info_named(0x8000, '0', a, "Built-in"), true);
    assert(toast_calls == 1 && slider_calls == 0);
    drop_pair();
}

static void test_request_queue(void) {
    connect_pair(S_READY);
    uint8_t p[256];

    /* Two wanted at once: the first goes out, the second waits for its answer */
    want_info(T_SINK);
    want_info(T_SOURCE);
    assert(pending == T_SINK && tracks[T_SOURCE].want);
    read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SINK_INFO);
    assert(recv(peer, p, sizeof p, MSG_DONTWAIT) < 0);          /* nothing else was sent yet */

    Buf8 reply = sink_info(0x4000, '0');
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(pending == T_SOURCE && !tracks[T_SOURCE].want);
    read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SOURCE_INFO);

    /* A request for a track already waiting is not sent twice */
    tracks[T_SINK].want = false;
    want_info(T_SINK);
    want_info(T_SINK);
    assert(tracks[T_SINK].want);

    /* An error for the outstanding request (e.g. no default input) moves on, it is not fatal */
    uint8_t err[15] = { 'L', 0, 0, 0, 0, 'L', 0, 0, 0, 2, 'L', 0, 0, 0, 3 };
    handle_frame(err, sizeof err);
    assert(state == S_READY && sock >= 0 && pending == T_SINK);   /* the waiting sink request went out */
    read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SINK_INFO);
    drop_pair();
}

static void event_frame(uint32_t event) {
    uint8_t f[20];
    uint32_t cmd = htonl(CMD_SUBSCRIBE_EVENT), tag = htonl(0xFFFFFFFFu), ev = htonl(event), idx = htonl(7);
    size_t n = 0;
    f[n++] = 'L'; memcpy(f + n, &cmd, 4); n += 4;
    f[n++] = 'L'; memcpy(f + n, &tag, 4); n += 4;
    f[n++] = 'L'; memcpy(f + n, &ev, 4); n += 4;
    f[n++] = 'L'; memcpy(f + n, &idx, 4); n += 4;
    handle_frame(f, n);
}

static void test_events(void) {
    connect_pair(S_READY);
    uint8_t p[256];

    /* Output events dirty the output only; input events the input only */
    event_frame(0x10 | FACILITY_SINK);
    assert(tracks[T_SINK].dirty && !tracks[T_SOURCE].dirty);
    tracks[T_SINK].dirty = false;
    event_frame(0x10 | FACILITY_SOURCE);
    assert(!tracks[T_SINK].dirty && tracks[T_SOURCE].dirty);
    tracks[T_SOURCE].dirty = false;

    /* A server event (the default device changed) concerns both */
    event_frame(0x10 | FACILITY_SERVER);
    assert(tracks[T_SINK].dirty && tracks[T_SOURCE].dirty);
    tracks[T_SINK].dirty = tracks[T_SOURCE].dirty = false;

    /* Anything else (clients, cards, streams) is none of our business */
    event_frame(0x10 | 5);
    event_frame(0x00 | 2);
    assert(!tracks[T_SINK].dirty && !tracks[T_SOURCE].dirty);

    /* After the debounce, what is dirty is requested */
    event_frame(0x10 | FACILITY_SERVER);
    debounce_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    assert(debounce_fd >= 0);
    arm(debounce_fd, 1);
    struct pollfd pf = { .fd = debounce_fd, .events = POLLIN };
    assert(poll(&pf, 1, 1000) == 1);
    on_debounce(NULL, 0);
    assert(pending == T_SINK && tracks[T_SOURCE].want);
    assert(!tracks[T_SINK].dirty && !tracks[T_SOURCE].dirty);
    read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SINK_INFO);
    close(debounce_fd);
    debounce_fd = -1;
    drop_pair();
}

static void test_loudest_channel(void) {
    connect_pair(S_READY);
    Buf8 reply = sink_info(0x4000, '0');
    /* Make the second channel louder: the HUD shows the loudest one. */
    uint32_t louder = htonl(0x8000);
    size_t vol2 = reply.n - 5 - 1 - 4;  /* before: mute (1) and the monitor 'L'+u32 (5) */
    memcpy(reply.b + vol2, &louder, 4);
    pending = T_SINK;
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(!strcmp(told_of("audio.volume"), "50"));   /* the loudest channel */
    drop_pair();
}

static void test_malformed_replies(void) {
    connect_pair(S_READY);
    Buf8 reply = sink_info(9172, '1');

    /* Every truncation of a valid reply must be rejected without announcing anything. */
    for (size_t cut = 10; cut < reply.n - 5; cut++) {
        pending = T_SINK;
        tracks[T_SINK].last_pct = tracks[T_SINK].last_muted = -1;
        In in = { reply.b + 10, reply.b + cut, false };
        on_reply(&in);
        assert(slider_calls == 0);
    }

    /* A wrong tag where a number belongs. */
    reply = sink_info(9172, '1');
    reply.b[10] = 'x';
    pending = T_SINK;
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 0);

    /* A boolean that is neither. */
    reply = sink_info(9172, '?');
    pending = T_SINK;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 0);
    drop_pair();
}

static void test_framing(void) {
    connect_pair(S_READY);
    pending = T_SINK;
    Buf8 payload = sink_info(0x8000, '0');
    Buf8 frame = framed(&payload);

    /* Delivered in pieces: nothing happens until the frame is complete. */
    assert(send(peer, frame.b, 7, 0) == 7);
    on_data(NULL, 0);
    assert(slider_calls == 0 && inlen == 7);
    assert(send(peer, frame.b + 7, 30, 0) == 30);
    on_data(NULL, 0);
    assert(slider_calls == 0);
    assert(send(peer, frame.b + 37, frame.n - 37, 0) == (ssize_t)(frame.n - 37));
    on_data(NULL, 0);
    assert(!strcmp(told_of("audio.volume"), "50") && inlen == 0);

    /* Two frames in one read, and an audio-channel frame in between that must be ignored. */
    tracks[T_SINK].last_pct = -1;
    pending = T_SINK;
    Buf8 two = {0};
    raw(&two, frame.b, frame.n);
    uint32_t len = htonl(4), ch = htonl(7), zero = 0;  /* a data frame on channel 7 */
    raw(&two, &len, 4); raw(&two, &ch, 4); raw(&two, &zero, 4); raw(&two, &zero, 4); raw(&two, &zero, 4);
    raw(&two, "abcd", 4);
    Buf8 p2 = sink_info(0x4000, '0');
    Buf8 f2 = framed(&p2);
    raw(&two, f2.b, f2.n);
    assert(send(peer, two.b, two.n, 0) == (ssize_t)two.n);
    on_data(NULL, 0);
    assert(!strcmp(told_of("audio.volume"), "50") && inlen == 0);   /* the first reply answered the request; the second was unsolicited */
    drop_pair();
}

static void test_bad_connections(void) {
    /* A frame length beyond anything we expect: drop the connection. */
    connect_pair(S_READY);
    uint8_t hdr[DESCRIPTOR_LEN] = { 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    assert(send(peer, hdr, sizeof hdr, 0) == (ssize_t)sizeof hdr);
    on_data(NULL, 0);
    assert(sock == -1 && state == S_DOWN);
    drop_pair();

    /* The server closes: same. */
    connect_pair(S_READY);
    close(peer);
    peer = -1;
    on_data(NULL, 0);
    assert(sock == -1 && state == S_DOWN);
    drop_pair();

    /* An ERROR reply during the handshake is fatal, but not once ready. */
    connect_pair(S_NAME);
    uint8_t err[15] = { 'L', 0, 0, 0, 0, 'L', 0, 0, 0, 2, 'L', 0, 0, 0, 3 };
    handle_frame(err, sizeof err);
    assert(state == S_DOWN);
    drop_pair();

    connect_pair(S_READY);
    pending = T_SINK;
    handle_frame(err, sizeof err);
    assert(state == S_READY && pending == -1 && sock >= 0);
    drop_pair();
}

/* Making a timer run out now. */
static void expire(int tfd) {
    struct itimerspec its = { .it_value = { .tv_nsec = 1 } };
    timerfd_settime(tfd, 0, &its, NULL);
    usleep(2000);
}

/* Waiting for the server without looking on a timer: its folder, then its socket, then a socket that does not listen yet. */
static void test_waiting_for_the_server(void) {
    char dir[64] = "/tmp/pacreactd-audio-XXXXXX";
    assert(mkdtemp(dir));
    char folder[96], path[104];
    snprintf(folder, sizeof folder, "%s/pulse", dir);
    snprintf(path, sizeof path, "%s/native", folder);
    setenv("PULSE_SERVER", path, 1);

    sock = -1; state = S_DOWN; retries = 0;
    watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    retry_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    assert(watch_fd >= 0 && retry_fd >= 0);

    /* Not even its folder: the folder above is watched, and nothing is tried on a timer */
    watch_server();
    assert(watched[0] < 0 && watched[1] >= 0 && !connect_server() && retries == 0);

    /* The folder appears: now it is the folder that is watched */
    assert(mkdir(folder, 0700) == 0);
    on_watch(NULL, 0);
    assert(watched[0] >= 0 && sock < 0);

    /* The socket is made and does not listen yet: looked at again soon (once, by a timer that is set only now) */
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    assert(strlen(path) < sizeof a.sun_path);
    memcpy(a.sun_path, path, strlen(path) + 1);
    assert(bind(lfd, (struct sockaddr *)&a, sizeof a) == 0);
    on_watch(NULL, 0);
    assert(sock < 0 && retries == 1);
    struct itimerspec armed;
    timerfd_gettime(retry_fd, &armed);
    assert(armed.it_value.tv_sec || armed.it_value.tv_nsec);

    /* It listens: the second look connects, and the handshake begins */
    assert(listen(lfd, 1) == 0);
    expire(retry_fd);
    on_retry(NULL, 0);
    assert(sock >= 0 && state == S_AUTH);
    int served = accept(lfd, NULL, NULL);
    assert(served >= 0);
    close(served);

    /* The server goes: not connected, and waiting again */
    disconnect();
    assert(sock < 0 && state == S_DOWN);
    close(lfd);

    /* A socket that never listens is looked at a few times and then left to the watch */
    retries = 0;
    for (int i = 0; i < 3 * SRC_AUDIO_RETRIES; i++) connect_server();
    assert(retries == SRC_AUDIO_RETRIES && sock < 0);

    close(watch_fd); watch_fd = -1;
    close(retry_fd); retry_fd = -1;
    unsetenv("PULSE_SERVER");
    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    assert(system(cmd) == 0);
}

static void test_server_path(void) {
    struct sockaddr_un a;
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    unsetenv("PULSE_SERVER");
    assert(server_path(&a) && !strcmp(a.sun_path, "/run/user/1000/pulse/native"));

    setenv("PULSE_SERVER", "unix:/tmp/pa.sock", 1);
    assert(server_path(&a) && !strcmp(a.sun_path, "/tmp/pa.sock"));
    setenv("PULSE_SERVER", "/tmp/other", 1);
    assert(server_path(&a) && !strcmp(a.sun_path, "/tmp/other"));
    setenv("PULSE_SERVER", "tcp:example.org", 1);
    assert(!server_path(&a));  /* TCP is not supported */

    unsetenv("PULSE_SERVER");
    unsetenv("XDG_RUNTIME_DIR");
    assert(!server_path(&a));
}

/* What is said of the output and of a change that is the device's, or a jack's, doing: the keys themselves */
static void test_what_is_said(void) {
    connect_pair(S_READY);
    const char *a = "alsa_output.a", *b = "alsa_output.b";

    /* The first reading: the level, the mute and the output, plain */
    read_reply(T_SINK, sink_info_named(0x4000, '0', a, "Speakers"), false);
    assert(!strcmp(told_of("audio.volume"), "25") && !strcmp(told_of("audio.mute"), "off") && !strcmp(told_of("audio.output"), "Speakers"));

    /* The person's change: plain */
    read_reply(T_SINK, sink_info_named(0x8000, '1', a, "Speakers"), true);
    assert(!strcmp(told_of("audio.volume"), "50") && !strcmp(told_of("audio.mute"), "on"));

    /* Another output: its own level comes with it, and says so */
    read_reply(T_SINK, sink_info_named(0x6000, '1', b, "Headset"), true);
    assert(!strcmp(told_of("audio.output"), "Headset") && !strcmp(told_of("audio.volume"), "38\tcause=device"));
    assert(!strcmp(told_of("audio.mute"), "on\tcause=device"));

    /* The same, because a jack was plugged: the output says that, so that the jack's event is the only message */
    audio_jack_plugged();
    read_reply(T_SINK, sink_info_named(0x6000, '1', a, "Speakers"), true);
    assert(!strcmp(told_of("audio.output"), "Speakers\tcause=jack"));
    jack_until_ms = 0;

    /* A long description is cut to fit, never overrunning, and with its cause intact */
    char long_name[200];
    memset(long_name, 'x', sizeof long_name);
    long_name[sizeof long_name - 1] = '\0';
    audio_jack_plugged();
    read_reply(T_SINK, sink_info_named(0x6000, '1', "n", long_name), true);
    assert(strlen(told_of("audio.output")) < 64 && strstr(told_of("audio.output"), "\tcause=jack"));
    jack_until_ms = 0;
    read_reply(T_SINK, sink_info_named(0x6000, '1', "m", long_name), true);
    assert(strlen(told_of("audio.output")) == 63 && strstr(told_of("audio.output"), "…") && !strstr(told_of("audio.output"), "cause"));   /* cut to fit, with an ellipsis */
    drop_pair();
}

/* ---- card ports ---- */

static void put_t(Buf8 *o, const char *str) { o->b[o->n++] = 't'; raw(o, str, strlen(str) + 1); }

/* One port of a GET_CARD_INFO_LIST reply, in the layout pipewire-pulse sends */
static void put_port(Buf8 *o, const char *name, uint32_t available, uint8_t direction, uint32_t type) {
    put_t(o, name);
    put_t(o, "A port");
    be_u32(o, 8000);                           /* priority */
    be_u32(o, available);                      /* 0 unknown, 1 no, 2 yes */
    o->b[o->n++] = 'B'; o->b[o->n++] = direction;
    raw(o, "PN", 2);                           /* an empty property list */
    be_u32(o, 1);                              /* the profiles it is in */
    put_t(o, "output:analog-stereo");
    o->b[o->n++] = 'r'; raw(o, "\0\0\0\0\0\0\0\0", 8);   /* latency offset */
    put_t(o, "Legacy 1");                      /* availability group */
    be_u32(o, type);
}

/* A reply with one card: front mic and headphones as an Intel HDA codec has them, plus the kinds of
 * port that are not a jack (HDMI, line in) */
static Buf8 card_info(uint32_t headphones, uint32_t front_mic) {
    Buf8 o = {0};
    be_u32(&o, 2);
    be_u32(&o, 5);
    be_u32(&o, 8176);                          /* index */
    put_t(&o, "alsa_card.pci-0000_00_1f.3");
    be_u32(&o, 0xFFFFFFFFu);                   /* owner module */
    put_t(&o, "alsa");
    be_u32(&o, 1);                             /* one profile */
    put_t(&o, "output:analog-stereo");
    put_t(&o, "Analog Stereo Output");
    be_u32(&o, 1); be_u32(&o, 0); be_u32(&o, 6500); be_u32(&o, 1);
    put_t(&o, "output:analog-stereo");         /* active profile */
    raw(&o, "PN", 2);                          /* its property list */
    be_u32(&o, 4);                             /* ports */
    put_port(&o, "analog-input-front-mic", front_mic, DIRECTION_INPUT, PORT_MIC);
    put_port(&o, "analog-input-linein", 1, DIRECTION_INPUT, PORT_LINE);
    put_port(&o, "analog-output-headphones", headphones, DIRECTION_OUTPUT, PORT_HEADPHONES);
    put_port(&o, "hdmi-output-0", 1, DIRECTION_OUTPUT, 10);
    return o;
}

static int ports_answers;
static PortChanges ports_answer;
static void on_ports_answer(const PortChanges *c) {
    ports_answers++;
    if (c) ports_answer = *c; else memset(&ports_answer, 0xFF, sizeof ports_answer);
}

static void test_card_ports(void) {
    connect_pair(S_READY);
    memset(&changes, 0, sizeof changes);
    n_port_state = 0;
    ports_known = false;

    /* Nothing can be said before the first reading: the server is not asked, the answer is "unknown" */
    ports_answers = 0;
    audio_port_changes(on_ports_answer);
    assert(ports_answers == 1 && ports_answer.headphones == -1 && !tracks[T_CARDS].want);

    /* The first reading is the baseline: the jack's ports, not the HDMI port or line in */
    read_reply(T_CARDS, card_info(1, 1), false);
    assert(ports_known && n_port_state == 2 && changes.headphones == 0 && changes.mic == 0);
    assert(slider_calls == 0 && toast_calls == 0);

    /* Headphones plugged in: "no" to "unknown", and the front mic goes to "yes", as on the real card */
    read_reply(T_CARDS, card_info(0, 2), false);
    assert(changes.headphones == 1 && changes.mic == 1 && changes.headset == 0 && changes.lineout == 0);

    /* Asking: a new reading is requested and the answer carries what flipped, once */
    ports_answers = 0;
    audio_port_changes(on_ports_answer);
    assert(pending == T_CARDS && ports_answers == 0);            /* sent at once, not answered yet */
    read_reply(T_CARDS, card_info(0, 2), false);
    assert(ports_answers == 1 && ports_answer.headphones == 1 && ports_answer.mic == 1);
    assert(changes.headphones == 0 && changes.mic == 0);          /* taken */
    read_reply(T_CARDS, card_info(0, 2), false);
    assert(ports_answers == 1);                                   /* not asked again */

    /* Unplugged: both go back; a reading that changes nothing reports no change */
    read_reply(T_CARDS, card_info(1, 1), false);
    assert(changes.headphones == -1 && changes.mic == -1);
    memset(&changes, 0, sizeof changes);
    read_reply(T_CARDS, card_info(1, 1), false);
    assert(changes.headphones == 0 && changes.mic == 0);

    /* A reply cut short is not compared, and does not replace the baseline */
    Buf8 cut = card_info(0, 2);
    cut.n -= 20;
    read_reply(T_CARDS, cut, false);
    assert(changes.headphones == 0 && changes.mic == 0 && port_state[1].present == false);

    /* The server going away answers anyone waiting, with "unknown" */
    ports_answers = 0;
    audio_port_changes(on_ports_answer);
    assert(port_cb);
    report_ports(NULL);
    assert(ports_answers == 1 && ports_answer.headphones == -1 && !port_cb);

    drop_pair();
}

int main(void) {
    test_handshake_packets();
    test_sink_info_parsing();
    test_roles();
    test_devices();
    test_output_change();
    test_what_is_said();
    test_microphone_track();
    test_port_change_is_not_a_volume_change();
    test_volume_settling();
    test_jack_output_change_is_quiet();
    test_card_ports();
    test_request_queue();
    test_events();
    test_loudest_channel();
    test_malformed_replies();
    test_framing();
    test_bad_connections();
    test_server_path();
    test_waiting_for_the_server();
    puts("audio: ok");
    return 0;
}
