#include "evdev.h"
#include "setting.h"
#include "loop.h"
#include "server.h"
#include "sources.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>
#include "tunables.h"

/* Caps Lock and Num Lock. A keyboard reports its lock LEDs as events on its input device, so
 * nothing is polled. Reading a keyboard device normally means being able to see every key press,
 * so this is opt-in (lock_keys = 1), and it takes a precaution: the device is opened with a
 * kernel event filter that lets only LED events through, so key events are never even delivered
 * to this process. If the kernel cannot apply the filter, the device is not used at all.
 *   lock.caps, lock.num   on or off: once a keyboard has been read, and whenever one changes */


typedef struct {
    int fd;           /* -1: slot unused */
    char path[64];
} Keyboard;

static Keyboard keyboards[MAX_KEYBOARDS];
static int caps = -1, num = -1;        /* the last known state, -1 until a keyboard has been read */
static bool started, warned;

/* A lock key changed: say so, unless it is what we already believed. */
void lock_changed(int led, bool on) {
    int *state = led == LED_CAPSL ? &caps : led == LED_NUML ? &num : NULL;
    if (!state) return;                              /* scroll lock and the others: not interesting */
    if (*state == on) return;                        /* another keyboard reporting the same change */
    *state = on;
    bus_set(led == LED_CAPSL ? "lock.caps" : "lock.num", on ? "on" : "off");
}

static void drop(Keyboard *k) {
    if (k->fd >= 0) {
        loop_remove(k->fd);
        close(k->fd);
    }
    memset(k, 0, sizeof *k);
    k->fd = -1;
    for (int i = 0; i < MAX_KEYBOARDS; i++)
        if (keyboards[i].fd >= 0) return;
    caps = num = -1;                                 /* the last one went: nobody can say what the locks are */
    bus_unset("lock.caps");
    bus_unset("lock.num");
}

static void on_events(void *ctx, uint32_t ev) {
    (void)ev;
    Keyboard *k = ctx;
    struct input_event events[16];
    int n = evdev_read(k->fd, events, 16);
    if (n < 0) { drop(k); return; }                  /* unplugged */
    for (int i = 0; i < n; i++)
        if (events[i].type == EV_LED) lock_changed(events[i].code, events[i].value != 0);
}

static void consider(const char *path, void *ctx) {
    (void)ctx;
    for (int i = 0; i < MAX_KEYBOARDS; i++)
        if (keyboards[i].fd >= 0 && !strcmp(keyboards[i].path, path)) return;
    Keyboard *slot = NULL;
    for (int i = 0; i < MAX_KEYBOARDS && !slot; i++)
        if (keyboards[i].fd < 0) slot = &keyboards[i];
    if (!slot) return;

    int fd = evdev_open(path, NULL, 0);
    if (fd < 0) return;
    if (!evdev_has_type(fd, EV_KEY) || !evdev_has_code(fd, EV_LED, LED_CAPSL)) { close(fd); return; }

    /* Only now that it is a keyboard: make sure its key events can never reach us. */
    static const int only_leds[] = { EV_LED };
    if (!evdev_deliver_only(fd, only_leds, 1) || evdev_delivers(fd, EV_KEY)) {
        if (!warned) {
            warned = true;
            fprintf(stderr, "pacreactd: this kernel cannot filter keyboard events; lock keys are off\n");
        }
        close(fd);
        return;
    }

    slot->fd = fd;
    snprintf(slot->path, sizeof slot->path, "%s", path);
    int c = evdev_led_state(fd, LED_CAPSL), n = evdev_led_state(fd, LED_NUML);
    if (caps < 0 && c >= 0) lock_changed(LED_CAPSL, c);   /* the starting state */
    if (num < 0 && n >= 0) lock_changed(LED_NUML, n);
    loop_add(fd, EPOLLIN, on_events, slot);
}

static void rescan(void) { evdev_scan(consider, NULL); }

void locks_init(void) {
    const char *on = pac_setting("lock_keys");
    if (!on || strcmp(on, "1") != 0) return;          /* opt-in */
    if (!started) {
        for (int i = 0; i < MAX_KEYBOARDS; i++) keyboards[i].fd = -1;
        started = true;
    }
    rescan();
    evdev_on_change(rescan);                           /* a keyboard plugged in later */
}
