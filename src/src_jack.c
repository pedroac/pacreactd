#include "evdev.h"
#include "setting.h"
#include "loop.h"
#include "server.h"
#include "sources.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "tunables.h"

/* Audio jacks: the kernel reports a plugged connector as a switch on an input device (the
 * "HDA Intel PCH Front Headphone" device, ...). Events arrive only when something is plugged in
 * or out, so there is nothing to poll. HDMI and DisplayPort audio also show up as jacks but
 * mean "the monitor woke up", so they are left alone. jacks = 0 turns this off.
 *
 * Some codecs report one 3.5 mm connector now as a headphone jack and then as a microphone jack,
 * depending on how their impedance detection felt that time. That cannot be told from two real
 * jacks, by the kernel or by the sound server's ports, so jacks = generic says what the
 * machine has: a plug on either is "an audio device", and the announcement does not depend on the codec's
 * mood.
 *   jack.ID   something plugged into a connector, once the contacts and the sound server's ports have agreed what it is:
 *             kind=headset, headphones, mic, lineout or device (a connector that cannot say). It has no name of its own, since
 *             none is known: the kind is what it is. There is one of each kind that is plugged in, from its being plugged until
 *             it is unplugged; none is left when no connector has anything in it. */

#define DEBOUNCE_MS 500   /* a headset reports its headphone and microphone contacts separately, and the
                           * codec detects the microphone a while after the headphones */

typedef enum { K_NONE, K_HEADPHONES, K_MIC, K_LINEOUT, K_COUNT } JackKind;

typedef struct {
    int fd;               /* -1: slot unused */
    char path[64];
    JackKind kind;
    bool plugged;
    bool headset_mic;     /* the microphone contact of a headset, not a separate microphone jack */
} Jack;

static Jack jacks[MAX_JACKS];
static int pending[K_COUNT];    /* per kind: +1 plugged in, -1 plugged out, since the last event */
static int debounce_fd = -1;
static bool generic_names;         /* jacks = generic: headphone and microphone jacks are one */
static bool shared_socket;         /* both switches were seen to change together: one connector reports both */
static bool mic_seen;              /* the microphone contact said something in the pending window, whatever it ended as */
static bool pending_headset_mic;   /* the pending microphone change came from a headset's contact */
static bool started;

/* What a jack device is, from its name. */
JackKind jack_kind(const char *name) {
    if (!name || !*name) return K_NONE;
    if (strcasestr(name, "HDMI") || strcasestr(name, "DisplayPort") || strstr(name, "/DP")) return K_NONE;
    if (strcasestr(name, "Mic")) return K_MIC;                       /* "Headset Mic", "Front Mic" */
    if (strcasestr(name, "Headphone") || strcasestr(name, "Headset")) return K_HEADPHONES;
    if (strcasestr(name, "Line Out") || strcasestr(name, "Lineout")) return K_LINEOUT;
    return K_NONE;                                                   /* line in, speakers, ... */
}

static int switch_code(JackKind k) {
    return k == K_MIC ? SW_MICROPHONE_INSERT : k == K_LINEOUT ? SW_LINEOUT_INSERT : SW_HEADPHONE_INSERT;
}

/* jacks_debug = 1: what the switches and the ports said, and when, to find out why a plug is named as it is */
static void debug(const char *what, const char *detail, int value) {
    if (!pac_setting("jacks_debug")) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "jacks %ld.%03ld %s %s %d\n", (long)ts.tv_sec % 1000, ts.tv_nsec / 1000000, what, detail, value);
}

static void arm(long ms) {
    struct itimerspec its = { .it_value = { .tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000L } };
    if (debounce_fd >= 0) timerfd_settime(debounce_fd, 0, &its, NULL);
}

static void jack_check_none(void);

static void note(Jack *j, bool plugged) {
    debug("switch", j->path, plugged);
    if (plugged == j->plugged) return;
    j->plugged = plugged;
    audio_jack_plugged();
    pending[j->kind] = plugged ? 1 : -1;
    if (j->headset_mic) pending_headset_mic = true;
    if (j->kind == K_MIC) mic_seen = true;
    arm(DEBOUNCE_MS);
}

static bool headphones_plugged(void) {
    for (int i = 0; i < MAX_JACKS; i++)
        if (jacks[i].fd >= 0 && jacks[i].kind == K_HEADPHONES && jacks[i].plugged) return true;
    return false;
}

/* What is plugged in now, as words for `pacreact jacks`: "headset" (headphones with a microphone, or headphones
 * in a socket known to take both), "headphones", "microphone", "lineout"; "none" when nothing is. */
void jack_describe(char *out, size_t n) {
    bool hp = false, mic = false, line = false;
    for (int i = 0; i < MAX_JACKS; i++) {
        if (jacks[i].fd < 0 || !jacks[i].plugged) continue;
        if (jacks[i].kind == K_HEADPHONES) hp = true;
        else if (jacks[i].kind == K_MIC) mic = true;
        else if (jacks[i].kind == K_LINEOUT) line = true;
    }
    snprintf(out, n, "%s%s%s%s", hp && (mic || shared_socket) ? " headset" : hp ? " headphones" : "",
             mic && !hp ? " microphone" : "", line ? " lineout" : "", !hp && !mic && !line ? " none" : "");
}

/* The jacks that are plugged in, as devices. */
static struct { char id[12]; char kind[12]; } plugged_in[8];

static int plugged_slot(const char *kind) {
    for (int i = 0; i < 8; i++)
        if (plugged_in[i].id[0] && !strcmp(plugged_in[i].kind, kind)) return i;
    return -1;
}

static void jack_appear(const char *kind) {
    if (plugged_slot(kind) >= 0) return;                       /* it is there: said twice */
    for (int i = 0; i < 8; i++) {
        if (plugged_in[i].id[0]) continue;
        char attrs[24];
        if (!bus_new_id("jack", plugged_in[i].id, sizeof plugged_in[i].id)) return;
        snprintf(plugged_in[i].kind, sizeof plugged_in[i].kind, "%s", kind);
        snprintf(attrs, sizeof attrs, "kind=%s", kind);
        bus_device("jack", plugged_in[i].id, NULL, attrs);
        return;
    }
}

static void jack_gone(int slot) {
    char key[32];
    snprintf(key, sizeof key, "jack.%s", plugged_in[slot].id);
    bus_unset_tree(key);
    memset(&plugged_in[slot], 0, sizeof plugged_in[slot]);
}

/* Unplugged: the device of that kind. If the settling named it otherwise than it was named plugging it in, the oldest one that is left. */
static void jack_vanish(const char *kind) {
    int slot = plugged_slot(kind);
    for (int i = 0; slot < 0 && i < 8; i++)
        if (plugged_in[i].id[0]) slot = i;
    if (slot >= 0) jack_gone(slot);
}

/* The connectors have the last word: when none has anything in it, no jack is left, whatever was settled. */
static void jack_check_none(void) {
    bool known = false;
    for (int i = 0; i < MAX_JACKS; i++) known |= jacks[i].fd >= 0;
    if (!known) return;                                          /* no connector is watched: nothing to say that none has anything in it */
    char words[48];
    jack_describe(words, sizeof words);
    if (!strstr(words, " none")) return;
    for (int i = 0; i < 8; i++)
        if (plugged_in[i].id[0]) jack_gone(i);
}

/* What is plugged in at the start: known, and said by no one. */
static void jack_initial(void) {
    char words[48];
    jack_describe(words, sizeof words);
    if (generic_names && (strstr(words, "headset") || strstr(words, "headphones") || strstr(words, "microphone"))) jack_appear("device");
    else {
        if (strstr(words, "headset")) jack_appear("headset");
        if (strstr(words, "headphones")) jack_appear("headphones");
        if (strstr(words, "microphone")) jack_appear("mic");
    }
    if (strstr(words, "lineout")) jack_appear("lineout");
}

/* One event for what changed. `kernel` is what the jack switches said, per kind; `ports` is what
 * the sound server says changed on the card's ports, or NULL when it could not be asked.
 *
 * The switches say that something was plugged, not what: a codec may report the same headset as a
 * headphone jack one time and as a microphone jack the next. The ports are what the server will
 * route, so when they changed they name the event (headphones and a microphone together are a
 * headset). When the switches fired and no port moved, the hardware is not telling, and the event
 * says so instead of guessing. Without the server, the switches' names are used as they are. */
static void announce(const int *kernel, const PortChanges *ports, bool mic_contact) {
    int dir = 0;   /* +1 connected, -1 disconnected */
    int hp = kernel[K_HEADPHONES], mic = kernel[K_MIC], lineout = kernel[K_LINEOUT];
    bool neutral = generic_names && (hp || mic);
    if (hp && hp == mic) shared_socket = true;   /* a plug that both contacts noticed: they are one socket */

    /* A headset's microphone contact arriving alone is the other half of a headset, not a microphone:
     * with the headphones in, it completes one; with them out, the headphones' event covered it. */
    if (mic && !hp && pending_headset_mic) {
        if (headphones_plugged() && mic > 0) hp = mic;
        else if (!headphones_plugged() && mic < 0) mic = 0;
    }
    pending_headset_mic = false;

    if ((hp || mic) && ports && !neutral) {
        if (!ports->headphones && !ports->headset && !ports->mic) {
            neutral = true;
        } else {
            hp = ports->headphones;
            mic = ports->headset ? ports->headset : ports->mic;
            if (ports->headset && !hp) hp = mic;   /* a headset's contacts: the headset */
        }
    }

    /* The microphone contact spoke in the same window as the headphone one but ended up elsewhere
     * (plugged and dropped again within it): still one headset being plugged, not headphones. */
    if (hp && mic_contact && !neutral) mic = hp;

    /* A codec that notices the microphone contact but lets the headphone contact lag (it keeps saying
     * "plugged" after the plug is out) reports a headset's plug and unplug as a microphone alone.
     * With the headphone contact engaged on a socket known to report both, that is the headset. */
    if (mic && !hp && !neutral && shared_socket && headphones_plugged()) hp = mic;

    const char *kind = NULL;
    if (neutral) {
        dir = hp ? hp : mic;
        kind = "device";
    } else if (hp && hp == mic) {
        dir = hp;
        kind = "headset";
    } else if (hp) {
        dir = hp;
        kind = "headphones";
    } else if (lineout) {
        dir = lineout;
        kind = "lineout";
    } else if (mic) {
        dir = mic;
        kind = "mic";
    }
    if (!kind) { jack_check_none(); return; }
    char event[32];
    snprintf(event, sizeof event, "%s %s", kind, dir > 0 ? "plugged" : "unplugged");
    if (pac_setting("jacks_debug"))
        fprintf(stderr, "jacks switches hp=%d mic=%d line=%d, ports %s -> \"%s\"\n", kernel[K_HEADPHONES],
                kernel[K_MIC], kernel[K_LINEOUT], ports ? "answered" : "unknown", event);
    if (dir > 0) jack_appear(kind);
    else jack_vanish(kind);
    jack_check_none();
}

/* The event for what is pending, from the switches alone. */
void jack_flush(void) {
    int kernel[K_COUNT];
    memcpy(kernel, pending, sizeof kernel);
    memset(pending, 0, sizeof pending);
    bool contact = mic_seen;
    mic_seen = false;
    announce(kernel, NULL, contact);
}

static int held[K_COUNT];   /* what the switches said, while the sound server is asked what it means */
static bool held_mic_seen;
static bool awaiting;

static void on_ports(const PortChanges *ports) {
    if (!awaiting) return;   /* an answer to a question that was given up on */
    awaiting = false;
    announce(held, ports, held_mic_seen);
}

static void on_debounce(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(debounce_fd, &n, sizeof n) <= 0) return;
    if (awaiting) {   /* the server never answered: do not wait on it again */
        awaiting = false;
        announce(held, NULL, held_mic_seen);
    }
    memcpy(held, pending, sizeof held);
    memset(pending, 0, sizeof pending);
    held_mic_seen = mic_seen;
    mic_seen = false;
    awaiting = true;
    audio_port_changes(on_ports);
}

static void drop(Jack *j) {
    if (j->fd >= 0) {
        loop_remove(j->fd);
        close(j->fd);
    }
    memset(j, 0, sizeof *j);
    j->fd = -1;
    jack_check_none();
}

static void rescan(void);

static void on_events(void *ctx, uint32_t ev) {
    (void)ev;
    Jack *j = ctx;
    struct input_event events[16];
    int n = evdev_read(j->fd, events, 16);
    if (n < 0) {                                  /* the sound card went away, or its codec was reconfigured */
        drop(j);
        rescan();   /* its replacement may already exist under the same name, which looked "already watched" */
        return;
    }
    for (int i = 0; i < n; i++)
        if (events[i].type == EV_SW && events[i].code == switch_code(j->kind))
            note(j, events[i].value != 0);
}

static void consider(const char *path, void *ctx) {
    (void)ctx;
    for (int i = 0; i < MAX_JACKS; i++)
        if (jacks[i].fd >= 0 && !strcmp(jacks[i].path, path)) return;       /* already watching it */
    Jack *slot = NULL;
    for (int i = 0; i < MAX_JACKS && !slot; i++)
        if (jacks[i].fd < 0) slot = &jacks[i];
    if (!slot) return;

    char name[96];
    int fd = evdev_open(path, name, sizeof name);
    if (fd < 0) return;
    JackKind kind = jack_kind(name);
    if (kind == K_NONE || !evdev_has_code(fd, EV_SW, switch_code(kind))) { close(fd); return; }

    slot->fd = fd;
    snprintf(slot->path, sizeof slot->path, "%s", path);
    slot->kind = kind;
    slot->headset_mic = kind == K_MIC && strcasestr(name, "Headset");
    slot->plugged = evdev_switch_state(fd, switch_code(kind)) == 1;       /* the state now: no event for it */
    loop_add(fd, EPOLLIN, on_events, slot);
}

static void rescan(void) { evdev_scan(consider, NULL); }

/* A combo jack from the codec's pin configuration. The codec describes every pin as a 32-bit word
 * (HDA spec 7.3.3.31): connectivity in bits 31-30 (0 jack, 1 none, 2 fixed, 3 both), location in
 * 29-24, default device in 23-20 (2 headphone out, 0xa microphone in). A headphone-out pin and a
 * microphone-in pin on the same panel position, both with a jack, are one headset connector that
 * the codec reports as two. `init` is the BIOS's table, `fixups` the driver's overrides of it (both
 * are "0xNN 0xCONFIG" lines, as sysfs prints them); the fixups win. */

static void read_pins(const char *text, uint32_t *nid, uint32_t *cfg, int *n) {
    for (const char *p = text; p && *p;) {
        unsigned a, c;
        if (sscanf(p, "%x %x", &a, &c) == 2) {
            int i = 0;
            while (i < *n && nid[i] != a) i++;
            if (i < MAX_PINS) { nid[i] = a; cfg[i] = c; if (i == *n) (*n)++; }
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
}

bool jack_combo_pins(const char *init, const char *fixups) {
    uint32_t nid[MAX_PINS], cfg[MAX_PINS];
    int n = 0;
    read_pins(init, nid, cfg, &n);
    read_pins(fixups, nid, cfg, &n);
    for (int i = 0; i < n; i++) {
        if ((cfg[i] >> 20 & 0xf) != 0x2 || (cfg[i] >> 30) != 0) continue;
        for (int j = 0; j < n; j++)
            if ((cfg[j] >> 20 & 0xf) == 0xa && (cfg[j] >> 30) == 0 && (cfg[j] >> 24 & 0x3f) == (cfg[i] >> 24 & 0x3f))
                return true;
    }
    return false;
}

static char *slurp(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    size_t len = f ? fread(buf, 1, size - 1, f) : 0;
    if (f) fclose(f);
    buf[len] = 0;
    return buf;
}

/* Any codec with a combo jack: the socket is known to report both contacts before the first plug. */
static void detect_combo(void) {
    for (int card = 0; card < 8 && !shared_socket; card++) {
        char path[80], init[2048], fixups[2048];
        snprintf(path, sizeof path, "/sys/class/sound/hwC%dD0/init_pin_configs", card);
        slurp(path, init, sizeof init);
        snprintf(path, sizeof path, "/sys/class/sound/hwC%dD0/driver_pin_configs", card);
        slurp(path, fixups, sizeof fixups);
        if (jack_combo_pins(init, fixups)) {
            shared_socket = true;
            debug("combo jack", path, 1);
        }
    }
}

void jack_init(void) {
    const char *off = pac_setting("jacks");
    if (off && !strcmp(off, "0")) return;
    generic_names = off && !strcmp(off, "generic");
    if (!started) {
        for (int i = 0; i < MAX_JACKS; i++) jacks[i].fd = -1;
        started = true;
    }
    debounce_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (debounce_fd >= 0) loop_add(debounce_fd, EPOLLIN, on_debounce, NULL);
    detect_combo();
    rescan();
    jack_initial();
    evdev_on_change(rescan);   /* a sound card that appears later */
}
