/* Audio jacks: what a device name means, how plug events become one message, the event
 * handling (fed through a pipe, with other events mixed in) and a read-only probe of the real
 * devices. src_jack.c is included for its statics. */
#include "../src/loop.h"
#include "../src/server.h"
#include "../src/sources.h"
#include <assert.h>
#include "setting.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* What pacreact makes of an event (its toast, icon and sound), so that the older assertions about what a plug is called still
 * hold: the daemon says "headset plugged", and this says what that is shown as. */
enum { SOUND_DEVICE_ADDED, SOUND_DEVICE_REMOVED, SOUND_KINDS };
static char toasts[8][96];
static int ntoasts, registered, removed;
static int last_sound = SOUND_KINDS;
static int nsounds;
static char toast_icon[96];
static struct { char key[24], kind[16]; } listed[16];   /* the jack devices there are, as a listener has them */
static int n_events;
static bool listener_synced = true;

static void shown(const char *event) {
    static const struct { const char *kind, *on, *off, *on_icon, *off_icon; } kinds[] = {
        { "device", "Audio device connected", "Audio device disconnected", "audio-card,audio-headphones", "audio-card,audio-headphones" },
        { "headset", "Headset connected", "Headset disconnected", "audio-headset,audio-headphones", "audio-headset-off,audio-headset,audio-headphones" },
        { "headphones", "Headphones connected", "Headphones disconnected", "audio-headphones,audio-headset", "audio-headphones-off,audio-headphones,audio-headset" },
        { "lineout", "Line out connected", "Line out disconnected", "audio-speakers", "audio-speakers" },
        { "mic", "Microphone connected", "Microphone disconnected", "audio-input-microphone", "audio-input-microphone-off,audio-input-microphone" },
    };
    char kind[16], dir[16];
    assert(sscanf(event, "%15s %15s", kind, dir) == 2);
    bool plugged = !strcmp(dir, "plugged");
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++) {
        if (strcmp(kind, kinds[i].kind)) continue;
        snprintf(toasts[ntoasts++ % 8], sizeof toasts[0], "%s", plugged ? kinds[i].on : kinds[i].off);
        snprintf(toast_icon, sizeof toast_icon, "%s", plugged ? kinds[i].on_icon : kinds[i].off_icon);
        last_sound = plugged ? SOUND_DEVICE_ADDED : SOUND_DEVICE_REMOVED;
        nsounds++;
        return;
    }
    assert(0 && "an event of a kind nobody knows");
}

/* A jack.ID that appears is something plugged in, and one that goes is something unplugged, of the kind it had */
bool bus_set(const char *key, const char *value) {
    assert(!strncmp(key, "jack.", 5) && !strchr(key + 5, '.'));
    int i = 0;
    while (i < 16 && listed[i].key[0] && strcmp(listed[i].key, key)) i++;
    assert(i < 16);
    if (listed[i].key[0]) return true;
    const char *k = strstr(value, "kind=");
    assert(k && value[0] == '\t');                                  /* no name: the kind is what it is */
    snprintf(listed[i].key, sizeof listed[i].key, "%s", key);
    snprintf(listed[i].kind, sizeof listed[i].kind, "%s", k + 5);
    n_events++;
    if (listener_synced) { char event[32]; snprintf(event, sizeof event, "%s plugged", listed[i].kind); shown(event); }
    return true;
}
bool bus_set_level(const char *key, const char *value, Level level) { (void)key; (void)value; (void)level; return true; }
void bus_unset(const char *key) {
    for (int i = 0; i < 16; i++) {
        if (!listed[i].key[0] || strcmp(listed[i].key, key)) continue;
        char event[32];
        snprintf(event, sizeof event, "%s unplugged", listed[i].kind);
        shown(event);
        memset(&listed[i], 0, sizeof listed[i]);
    }
}
/* Whether a jack of that kind is plugged in now */
static bool there(const char *kind) {
    for (int i = 0; i < 16; i++) if (listed[i].key[0] && !strcmp(listed[i].kind, kind)) return true;
    return false;
}
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; registered++; return 0; }
int loop_remove(int fd) { (void)fd; removed++; return 0; }

/* The sound server is not here: the test says what it would have answered */
static void (*ask_cb)(const PortChanges *);
void audio_jack_plugged(void) {}
void audio_port_changes(void (*cb)(const PortChanges *)) { ask_cb = cb; }

#include "../src/src_jack.c"

/* Starts over: nothing plugged in anywhere */
static void reset_jacks(void) {
    memset(listed, 0, sizeof listed);
    memset(plugged_in, 0, sizeof plugged_in);
}

static void test_kinds(void) {
    /* the real device names of an Intel HDA machine */
    assert(jack_kind("HDA Intel PCH Front Headphone") == K_HEADPHONES);
    assert(jack_kind("HDA Intel PCH Headset Mic") == K_MIC);
    assert(jack_kind("HDA Intel PCH Front Mic") == K_MIC);
    assert(jack_kind("HDA Intel PCH Line Out") == K_LINEOUT);
    assert(jack_kind("HDA Intel PCH Line") == K_NONE);                 /* line in */
    assert(jack_kind("HDA NVidia HDMI/DP,pcm=3") == K_NONE);           /* a monitor waking up is not a jack */
    assert(jack_kind("HDA ATI HDMI HDMI/DP,pcm=3") == K_NONE);
    assert(jack_kind("DisplayPort Audio") == K_NONE);
    assert(jack_kind("Logitech USB Headset") == K_HEADPHONES);
    assert(jack_kind("Sleep Button") == K_NONE && jack_kind("Power Button") == K_NONE);
    assert(jack_kind("AT Translated Set 2 keyboard") == K_NONE);
    assert(jack_kind("") == K_NONE && jack_kind(NULL) == K_NONE);
    assert(jack_kind("front headphone") == K_HEADPHONES);              /* case does not matter */
}

static void test_flush(void) {
    ntoasts = 0;
    memset(pending, 0, sizeof pending);
    jack_flush();
    assert(ntoasts == 0 && nsounds == 0);                              /* nothing happened: no sound either */

    pending[K_HEADPHONES] = 1;
    jack_flush();
    assert(ntoasts == 1 && !strcmp(toasts[0], "Headphones connected"));
    assert(!strncmp(toast_icon, "audio-headphones", 16));
    assert(nsounds == 1 && last_sound == SOUND_DEVICE_ADDED);
    assert(pending[K_HEADPHONES] == 0);                                /* consumed */
    pending[K_HEADPHONES] = -1;
    jack_flush();
    assert(!strcmp(toasts[1], "Headphones disconnected") && last_sound == SOUND_DEVICE_REMOVED);

    /* headphone and microphone contacts together are a headset */
    pending[K_HEADPHONES] = 1; pending[K_MIC] = 1;
    jack_flush();
    assert(ntoasts == 3 && !strcmp(toasts[2], "Headset connected") && last_sound == SOUND_DEVICE_ADDED);
    assert(!strncmp(toast_icon, "audio-headset", 13));            /* a headset is not headphones */
    pending[K_HEADPHONES] = -1; pending[K_MIC] = -1;
    jack_flush();
    assert(!strcmp(toasts[3], "Headset disconnected") && last_sound == SOUND_DEVICE_REMOVED);
    /* ...but not when they disagree */
    pending[K_HEADPHONES] = 1; pending[K_MIC] = -1;
    jack_flush();
    assert(!strcmp(toasts[4], "Headphones connected"));

    pending[K_MIC] = 1;
    jack_flush();
    assert(!strcmp(toasts[5], "Microphone connected"));
    pending[K_MIC] = -1;
    jack_flush();
    assert(!strcmp(toasts[6], "Microphone disconnected"));
    pending[K_LINEOUT] = 1;
    jack_flush();
    assert(!strcmp(toasts[7 % 8], "Line out connected"));
    pending[K_LINEOUT] = -1;
    jack_flush();
    assert(!strcmp(toasts[(ntoasts - 1) % 8], "Line out disconnected") && ntoasts == 9);
}

static void send(int fd, int type, int code, int value) {
    struct input_event ev = { .type = type, .code = code, .value = value };
    assert(write(fd, &ev, sizeof ev) == (ssize_t)sizeof ev);
}

static Jack make_jack(JackKind kind, bool plugged, int *write_end) {
    int p[2];
    assert(pipe2(p, O_NONBLOCK) == 0);                     /* like a real evdev descriptor */
    *write_end = p[1];
    return (Jack){ .fd = p[0], .kind = kind, .plugged = plugged };
}

static void test_generic_names(void) {
    generic_names = true;
    for (int dir = 1; dir >= -1; dir -= 2) {
        int kinds[] = { K_HEADPHONES, K_MIC };
        for (int i = 0; i < 2; i++) {          /* whichever switch the codec picked: the same toast */
            reset_jacks();
            if (dir < 0) { listener_synced = false; jack_appear("device"); listener_synced = true; }   /* it was plugged in */
            ntoasts = 0;
            memset(pending, 0, sizeof pending);
            pending[kinds[i]] = dir;
            jack_flush();
            assert(ntoasts == 1 && !strcmp(toasts[0], dir > 0 ? "Audio device connected" : "Audio device disconnected"));
        }
    }
    reset_jacks();
    ntoasts = 0;
    memset(pending, 0, sizeof pending);
    pending[K_HEADPHONES] = pending[K_MIC] = 1;   /* both switches at once: still one toast */
    jack_flush();
    assert(ntoasts == 1 && !strcmp(toasts[0], "Audio device connected"));
    reset_jacks();
    ntoasts = 0;
    memset(pending, 0, sizeof pending);
    pending[K_LINEOUT] = 1;                    /* line out keeps its name */
    jack_flush();
    assert(ntoasts == 1 && !strcmp(toasts[0], "Line out connected"));
    generic_names = false;
}

/* What one plug looks like to the toast, from the switches and from the server's ports */
static const char *plug_contact(int hp, int mic, const PortChanges *ports, bool mic_contact) {
    int kernel[K_COUNT] = { 0 };
    kernel[K_HEADPHONES] = hp;
    kernel[K_MIC] = mic;
    reset_jacks();                                      /* each plug here is the first */
    ntoasts = 0;
    announce(kernel, ports, mic_contact);
    if (!ntoasts) {                                     /* nothing was plugged to take away: so this was an unplug; plug everything, and again */
        static const char *const all[] = { "headset", "headphones", "mic", "lineout", "device" };
        listener_synced = false;
        for (size_t i = 0; i < sizeof all / sizeof *all; i++) jack_appear(all[i]);
        listener_synced = true;
        announce(kernel, ports, mic_contact);
    }
    return ntoasts ? toasts[0] : "";
}

static const char *plug(int hp, int mic, const PortChanges *ports) { return plug_contact(hp, mic, ports, false); }

static void test_ports_name_the_toast(void) {
    PortChanges none = {0};
    PortChanges both = { .headphones = 1, .mic = 1 };           /* this machine: headphones and front mic */
    PortChanges hp_only = { .headphones = 1 };
    PortChanges headset = { .headset = 1 };
    PortChanges mic_only = { .mic = 1 };
    PortChanges gone = { .headphones = -1, .mic = -1 };

    /* the same plug, whichever switch the codec picked: the ports decide */
    assert(!strcmp(plug(1, 0, &both), "Headset connected"));
    assert(!strcmp(plug(0, 1, &both), "Headset connected"));
    assert(!strcmp(plug(0, -1, &gone), "Headset disconnected"));
    assert(!strcmp(plug(1, 1, &hp_only), "Headphones connected"));   /* the switches over-reported a mic */
    assert(!strcmp(plug(1, 0, &headset), "Headset connected"));
    assert(!strcmp(plug(0, 1, &mic_only), "Microphone connected"));  /* a real front-panel microphone */

    /* the switches fired and no port moved: the hardware is not saying what it is */
    assert(!strcmp(plug(1, 0, &none), "Audio device connected"));
    assert(!strcmp(plug(0, 1, &none), "Audio device connected"));
    assert(!strcmp(plug(-1, 0, &none), "Audio device disconnected"));
    assert(!strcmp(toast_icon, "audio-card,audio-headphones"));

    /* no server: the switches' own names, as before */
    assert(!strcmp(plug(1, 0, NULL), "Headphones connected"));
    assert(!strcmp(plug(0, 1, NULL), "Microphone connected"));
    assert(!strcmp(plug(1, 1, NULL), "Headset connected"));

    /* the setting forces the neutral name whatever the ports say */
    generic_names = true;
    assert(!strcmp(plug(1, 0, &both), "Audio device connected"));
    generic_names = false;

    /* line out is its own switch, and the ports do not rename it */
    ntoasts = 0;
    int line[K_COUNT] = { [K_LINEOUT] = 1 };
    announce(line, &none, false);
    assert(ntoasts == 1 && !strcmp(toasts[0], "Line out connected"));
}

static void test_mic_dropping_again(void) {
    PortChanges settled = { .headphones = 1, .mic = -1 }, gone = { .headphones = -1, .mic = 1 };
    /* the headset plugged in and its microphone contact dropped again inside the window */
    assert(!strcmp(plug_contact(1, -1, &settled, true), "Headset connected"));
    assert(!strcmp(plug_contact(1, -1, NULL, true), "Headset connected"));
    assert(!strcmp(plug_contact(-1, 1, &gone, true), "Headset disconnected"));
    /* headphones alone, the microphone contact silent: still headphones */
    assert(!strcmp(plug_contact(1, 0, &(PortChanges){ .headphones = 1 }, false), "Headphones connected"));
}

static void test_flaky_headphone_contact(void) {
    PortChanges mic_only = { .mic = 1 }, mic_gone = { .mic = -1 }, both = { .headphones = 1, .mic = 1 };
    for (int i = 0; i < MAX_JACKS; i++) { memset(&jacks[i], 0, sizeof jacks[i]); jacks[i].fd = -1; }
    shared_socket = false;

    /* a microphone alone, and nothing says it shares a socket with the headphones: a microphone */
    jacks[0].fd = 0; jacks[0].kind = K_HEADPHONES; jacks[0].plugged = true;
    assert(!strcmp(plug(0, 1, &mic_only), "Microphone connected"));

    /* one plug that both contacts noticed teaches that they are one socket */
    assert(!strcmp(plug(1, 1, &both), "Headset connected") && shared_socket);

    /* now the headphone contact lags: the microphone alone, with it engaged, is the headset */
    assert(!strcmp(plug(0, 1, &mic_only), "Headset connected"));
    assert(!strcmp(plug(0, -1, &mic_gone), "Headset disconnected"));
    assert(!strcmp(plug(0, -1, NULL), "Headset disconnected"));        /* also without the server */

    /* with the headphones out, a microphone is a microphone again */
    jacks[0].plugged = false;
    assert(!strcmp(plug(0, 1, &mic_only), "Microphone connected"));

    for (int i = 0; i < MAX_JACKS; i++) { memset(&jacks[i], 0, sizeof jacks[i]); jacks[i].fd = -1; }
    shared_socket = false;
}

static void test_event_asks_the_server(void) {
    for (int i = 0; i < MAX_JACKS; i++) { memset(&jacks[i], 0, sizeof jacks[i]); jacks[i].fd = -1; }   /* no connector is watched: the plugs here are all there is */
    reset_jacks();
    PortChanges both = { .headphones = 1, .mic = 1 };
    ntoasts = 0;
    ask_cb = NULL;
    memset(pending, 0, sizeof pending);
    awaiting = false;

    int p[2];
    assert(pipe(p) == 0);
    debounce_fd = p[0];
    uint64_t tick = 1;

    pending[K_MIC] = 1;                                /* a plug, the switches only */
    assert(write(p[1], &tick, sizeof tick) == sizeof tick);
    on_debounce(NULL, 0);
    assert(ntoasts == 0 && ask_cb && awaiting);        /* nothing is said until the server has answered */
    assert(pending[K_MIC] == 0);                       /* and the switches' state is kept aside */
    ask_cb(&both);
    assert(ntoasts == 1 && !strcmp(toasts[0], "Headset connected") && !awaiting);

    /* a second answer to the same question is ignored */
    ask_cb(&both);
    assert(ntoasts == 1);

    /* an answer that never comes does not stop the next toast: the old one is said with the switches' words */
    ntoasts = 0;
    pending[K_HEADPHONES] = 1;
    assert(write(p[1], &tick, sizeof tick) == sizeof tick);
    on_debounce(NULL, 0);
    assert(ntoasts == 0 && awaiting);
    pending[K_HEADPHONES] = -1;
    assert(write(p[1], &tick, sizeof tick) == sizeof tick);
    on_debounce(NULL, 0);
    assert(ntoasts == 1 && !strcmp(toasts[0], "Headphones connected") && awaiting);
    ask_cb(NULL);                                      /* the server went away: the switches' words */
    assert(ntoasts == 2 && !strcmp(toasts[1], "Headphones disconnected"));

    close(p[0]);
    close(p[1]);
    debounce_fd = -1;
}

static void test_events(void) {
    reset_jacks();
    ntoasts = 0;
    memset(pending, 0, sizeof pending);
    int w;
    Jack j = make_jack(K_HEADPHONES, false, &w);

    /* other devices' events, other switches and key presses are all ignored */
    send(w, EV_KEY, KEY_A, 1);
    send(w, EV_SW, SW_LID, 1);
    send(w, EV_SW, SW_MICROPHONE_INSERT, 1);                  /* not this jack's switch */
    send(w, EV_SYN, SYN_REPORT, 0);
    on_events(&j, EPOLLIN);
    assert(!j.plugged && pending[K_HEADPHONES] == 0 && pending[K_MIC] == 0);

    /* plugging in */
    send(w, EV_SW, SW_HEADPHONE_INSERT, 1);
    send(w, EV_SYN, SYN_REPORT, 0);
    on_events(&j, EPOLLIN);
    assert(j.plugged && pending[K_HEADPHONES] == 1);
    jack_flush();
    assert(ntoasts == 1 && !strcmp(toasts[0], "Headphones connected"));

    /* the same state again is not a change */
    send(w, EV_SW, SW_HEADPHONE_INSERT, 1);
    on_events(&j, EPOLLIN);
    assert(pending[K_HEADPHONES] == 0);

    /* plugging out, and in and out again quickly within one debounce window: the last state wins */
    send(w, EV_SW, SW_HEADPHONE_INSERT, 0);
    send(w, EV_SW, SW_HEADPHONE_INSERT, 1);
    send(w, EV_SW, SW_HEADPHONE_INSERT, 0);
    on_events(&j, EPOLLIN);
    assert(!j.plugged && pending[K_HEADPHONES] == -1);
    jack_flush();
    assert(!strcmp(toasts[1], "Headphones disconnected"));

    /* a headset: two devices, one message */
    int w2;
    Jack mic = make_jack(K_MIC, false, &w2);
    send(w, EV_SW, SW_HEADPHONE_INSERT, 1);
    send(w2, EV_SW, SW_MICROPHONE_INSERT, 1);
    on_events(&j, EPOLLIN);
    on_events(&mic, EPOLLIN);
    jack_flush();
    assert(ntoasts == 3 && !strcmp(toasts[2], "Headset connected"));

    /* an empty read changes nothing */
    on_events(&j, EPOLLIN);
    assert(ntoasts == 3);

    /* the device disappearing: it is released, and the devices are looked at again, because its
     * replacement may already exist under the same name (a codec reconfigured, a card replugged) */
    Jack gone = make_jack(K_HEADPHONES, true, &w);
    close(gone.fd);                                           /* reading a closed fd fails like a vanished device */
    snprintf(gone.path, sizeof gone.path, "/dev/input/vanished");
    jacks[0] = gone;
    removed = 0;
    on_events(&jacks[0], EPOLLIN);
    assert(removed == 1);
    for (int i = 0; i < MAX_JACKS; i++) {
        assert(strcmp(jacks[i].path, "/dev/input/vanished"));   /* released; what the rescan found is the machine's own */
        if (jacks[i].fd >= 0) { close(jacks[i].fd); jacks[i].fd = -1; }
    }
    close(w); close(w2); close(j.fd); close(mic.fd);
}

static void test_real_devices(void) {
    for (int i = 0; i < MAX_JACKS; i++) jacks[i].fd = -1;
    started = true;
    ntoasts = registered = 0;
    memset(pending, 0, sizeof pending);
    jack_init();

    int open_jacks = 0;
    for (int i = 0; i < MAX_JACKS; i++) {
        if (jacks[i].fd < 0) continue;
        open_jacks++;
        assert(jacks[i].kind != K_NONE && strncmp(jacks[i].path, "/dev/input/event", 16) == 0);
    }
    assert(ntoasts == 0);                                     /* what is plugged in now is not news */
    printf("jack: %d real jack device(s) found and opened\n", open_jacks);

    /* looking again does not open them twice */
    int before = registered;
    rescan();
    assert(registered == before);

    pac_setting_set("jacks", "0");
    registered = 0;
    jack_init();
    assert(registered == 0);
    pac_setting_set("jacks", NULL);
}

static void test_combo_pins(void) {
    /* this machine: the BIOS table has a front headphone out; the driver makes pin 0x19 a front mic */
    const char *init = "0x14 0x01014010\n0x18 0x01a19040\n0x19 0x411111f0\n0x21 0x02211020\n";
    assert(jack_combo_pins(init, "0x19 0x02a11040\n0x1b 0x0181304f\n"));
    assert(!jack_combo_pins(init, ""));                                /* rear mic, front headphones: two jacks */
    assert(!jack_combo_pins("0x18 0x01a19040\n0x21 0x02211020\n", NULL));
    assert(!jack_combo_pins("0x19 0x42a11040\n0x21 0x02211020\n", ""));  /* a fixed (internal) mic is no jack */
    assert(!jack_combo_pins("", "") && !jack_combo_pins(NULL, NULL));
}

static void test_describe(void) {
    char out[48];
    for (int i = 0; i < MAX_JACKS; i++) { memset(&jacks[i], 0, sizeof jacks[i]); jacks[i].fd = -1; }
    shared_socket = false;
    jack_describe(out, sizeof out);
    assert(!strcmp(out, " none"));
    jacks[0].fd = 0; jacks[0].kind = K_HEADPHONES; jacks[0].plugged = true;
    jack_describe(out, sizeof out);
    assert(!strcmp(out, " headphones"));                /* the contact says nothing about a microphone */
    shared_socket = true;
    jack_describe(out, sizeof out);
    assert(!strcmp(out, " headset"));                   /* a socket that takes both: a headset */
    shared_socket = false;
    jacks[1].fd = 0; jacks[1].kind = K_MIC; jacks[1].plugged = true;
    jack_describe(out, sizeof out);
    assert(!strcmp(out, " headset"));
    jacks[0].plugged = false;
    jack_describe(out, sizeof out);
    assert(!strcmp(out, " microphone"));
    jacks[2].fd = 0; jacks[2].kind = K_LINEOUT; jacks[2].plugged = true;
    jack_describe(out, sizeof out);
    assert(!strcmp(out, " microphone lineout"));

    /* The same, as devices: what is plugged in at the start is there, and said by no one */
    reset_jacks();
    listener_synced = false;
    jack_initial();
    listener_synced = true;
    assert(there("mic") && there("lineout") && !there("headset") && !there("headphones"));
    reset_jacks();
    jacks[0].plugged = true; shared_socket = true;
    listener_synced = false;
    jack_initial();
    listener_synced = true;
    assert(there("headset") && !there("headphones") && !there("mic"));
    jacks[0].plugged = false; shared_socket = false;
    generic_names = true;                                        /* a connector that cannot say */
    reset_jacks();
    jacks[0].plugged = true;
    listener_synced = false;
    jack_initial();
    listener_synced = true;
    assert(there("device") && !there("headphones"));
    generic_names = false;
    jacks[0].plugged = false;
    reset_jacks();
    jacks[1].plugged = false;
    jack_describe(out, 4);                              /* too small: cut, never overrun */
    assert(!strcmp(out, " li"));
    for (int i = 0; i < MAX_JACKS; i++) { memset(&jacks[i], 0, sizeof jacks[i]); jacks[i].fd = -1; }
}

int main(void) {
    test_kinds();
    test_describe();
    test_combo_pins();
    test_flush();
    test_generic_names();
    test_ports_name_the_toast();
    test_flaky_headphone_contact();
    test_mic_dropping_again();
    test_event_asks_the_server();
    test_events();
    test_real_devices();
    puts("jack: ok");
    return 0;
}
