/* Caps Lock and Num Lock: the change logic, event handling fed through a pipe (with key events
 * mixed in that must be ignored), the opt-in, and, on a real keyboard, proof that the kernel
 * filter keeps key events from ever being delivered. src_locks.c is included for its statics. */
#include "../src/loop.h"
#include "../src/server.h"
#include <assert.h>
#include "setting.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

/* `toasts` are what a listener that announces the lock keys (pacreact) makes of lock.caps and lock.num: a change from a
 * state it knew; the first reading is the starting point, not news. */
static char toasts[16][64];
static int ntoasts, registered, removed;
static int listener_caps = -1, listener_num = -1;
static const char *last_value[2];

bool bus_set(const char *key, const char *value) {
    bool caps_key = !strcmp(key, "lock.caps");
    int *known = caps_key ? &listener_caps : &listener_num;
    int on = !strcmp(value, "on");
    if (*known >= 0 && *known != on) snprintf(toasts[ntoasts++ % 16], sizeof toasts[0], "%s %s", caps_key ? "Caps Lock" : "Num Lock", value);
    *known = on;
    last_value[!caps_key] = value[1] == 'n' ? "on" : "off";
    return true;
}
bool bus_set_level(const char *key, const char *value, Level level) { (void)key; (void)value; (void)level; return true; }
void bus_unset(const char *key) { (void)key; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; registered++; return 0; }
int loop_remove(int fd) { (void)fd; removed++; return 0; }

#include "../src/src_locks.c"

static void reset(void) { caps = num = -1; ntoasts = 0; listener_caps = listener_num = -1; }

static void test_changes(void) {
    reset();
    /* The first reading is the starting point, not news */
    lock_changed(LED_CAPSL, true);
    assert(ntoasts == 0 && caps == 1);
    lock_changed(LED_NUML, false);
    assert(ntoasts == 0 && num == 0);

    lock_changed(LED_CAPSL, false);
    assert(ntoasts == 1 && !strcmp(toasts[0], "Caps Lock off"));
    lock_changed(LED_CAPSL, true);
    assert(!strcmp(toasts[1], "Caps Lock on"));
    lock_changed(LED_NUML, true);
    assert(!strcmp(toasts[2], "Num Lock on"));
    lock_changed(LED_NUML, false);
    assert(!strcmp(toasts[3], "Num Lock off") && ntoasts == 4);

    /* the same state again (a second keyboard reporting the same change) is not a change */
    lock_changed(LED_NUML, false);
    lock_changed(LED_CAPSL, true);
    assert(ntoasts == 4);

    /* scroll lock and compose/kana LEDs are not interesting */
    lock_changed(LED_SCROLLL, true);
    lock_changed(LED_COMPOSE, true);
    lock_changed(99, true);
    assert(ntoasts == 4);
}

static void send(int fd, int type, int code, int value) {
    struct input_event ev = { .type = type, .code = code, .value = value };
    assert(write(fd, &ev, sizeof ev) == (ssize_t)sizeof ev);
}

static Keyboard make_keyboard(int *write_end) {
    int p[2];
    assert(pipe2(p, O_NONBLOCK) == 0);
    *write_end = p[1];
    return (Keyboard){ .fd = p[0] };
}

static void test_events(void) {
    reset();
    int w1, w2;
    Keyboard a = make_keyboard(&w1), b = make_keyboard(&w2);

    /* establish the starting state */
    send(w1, EV_LED, LED_CAPSL, 0);
    on_events(&a, EPOLLIN);
    assert(caps == 0 && ntoasts == 0);

    /* key presses (which the real filter would never deliver) and other LEDs are ignored */
    send(w1, EV_KEY, KEY_CAPSLOCK, 1);
    send(w1, EV_KEY, KEY_A, 1);
    send(w1, EV_MSC, MSC_SCAN, 58);
    send(w1, EV_LED, LED_SCROLLL, 1);
    send(w1, EV_SYN, SYN_REPORT, 0);
    on_events(&a, EPOLLIN);
    assert(caps == 0 && ntoasts == 0);

    /* Caps Lock on, reported by both keyboards: one message */
    send(w1, EV_LED, LED_CAPSL, 1);
    send(w2, EV_LED, LED_CAPSL, 1);
    on_events(&a, EPOLLIN);
    on_events(&b, EPOLLIN);
    assert(ntoasts == 1 && !strcmp(toasts[0], "Caps Lock on"));

    /* an empty read changes nothing */
    on_events(&a, EPOLLIN);
    assert(ntoasts == 1);

    /* a keyboard that disappears releases its slot */
    int w3;
    Keyboard gone = make_keyboard(&w3);
    close(gone.fd);
    for (int i = 1; i < MAX_KEYBOARDS; i++) keyboards[i].fd = -1;
    keyboards[0] = gone;
    removed = 0;
    on_events(&keyboards[0], EPOLLIN);
    assert(keyboards[0].fd == -1 && removed == 1);
    assert(caps == -1 && num == -1);                 /* the last one went: the locks are not known any more */
    close(w1); close(w2); close(w3); close(a.fd); close(b.fd);
}

static void test_opt_in_and_real_keyboards(void) {
    for (int i = 0; i < MAX_KEYBOARDS; i++) keyboards[i].fd = -1;
    started = true;
    reset();

    /* off unless lock_keys is exactly 1 */
    const char *no[] = { NULL, "", "0", "true", "yes", "11" };
    for (size_t i = 0; i < sizeof no / sizeof *no; i++) {
        if (no[i]) pac_setting_set("lock_keys", no[i]); else pac_setting_set("lock_keys", NULL);
        registered = 0;
        locks_init();
        assert(registered == 0);
        for (int k = 0; k < MAX_KEYBOARDS; k++) assert(keyboards[k].fd < 0);
    }

    pac_setting_set("lock_keys", "1");
    registered = 0;
    locks_init();
    int opened = 0;
    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i].fd < 0) continue;
        opened++;
        /* The privacy guarantee, checked on the real device: the kernel will deliver LED events
         * to us and will not deliver key events at all. */
        assert(evdev_has_code(keyboards[i].fd, EV_LED, LED_CAPSL));
        assert(evdev_delivers(keyboards[i].fd, EV_LED));
        assert(!evdev_delivers(keyboards[i].fd, EV_KEY));
        assert(!evdev_delivers(keyboards[i].fd, EV_REL) && !evdev_delivers(keyboards[i].fd, EV_ABS));
    }
    assert(ntoasts == 0);                                   /* the state at start-up is not news */
    assert(registered >= opened);
    printf("locks: %d real keyboard(s) opened, all with key events filtered out by the kernel\n", opened);

    /* looking again does not open them twice */
    int before = registered;
    rescan();
    assert(registered == before);
    pac_setting_set("lock_keys", NULL);
}

int main(void) {
    test_changes();
    test_events();
    test_opt_in_and_real_keyboards();
    puts("locks: ok");
    return 0;
}
