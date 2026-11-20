#ifndef EVDEV_H
#define EVDEV_H

#include <linux/input.h>
#include <stdbool.h>
#include <stddef.h>

/* A thin layer over the kernel's input devices (/dev/input/event*), for the sources that watch a
 * switch (headphone jack) or an LED (Caps Lock). Reading needs membership of the `input` group,
 * which is the case for the desktop user on most distributions. */

/* Opens a device read-only and non-blocking. Fills name (if not NULL). Returns the fd or -1. */
int evdev_open(const char *path, char *name, size_t name_len);

bool evdev_has_type(int fd, int type);
bool evdev_has_code(int fd, int type, int code);
/* Current state of a switch (EV_SW) or LED (EV_LED): 1, 0, or -1 if it cannot be read. */
int evdev_switch_state(int fd, int code);
int evdev_led_state(int fd, int code);

/* Asks the kernel to deliver only these event types to this file descriptor (plus EV_SYN), so
 * the others are never even queued for us: a reader of LED events cannot see a keystroke. */
bool evdev_deliver_only(int fd, const int *types, int count);

/* Whether the kernel would still deliver events of this type to this file descriptor (reads the
 * mask back). False when it is filtered out, and false if the mask cannot be read. */
bool evdev_delivers(int fd, int type);

/* Reads whatever events are waiting. Returns how many, 0 if none, -1 if the device is gone. */
int evdev_read(int fd, struct input_event *events, int max);

/* Calls fn for the path of every /dev/input/event* node. */
typedef void (*EvdevScanFn)(const char *path, void *ctx);
void evdev_scan(EvdevScanFn fn, void *ctx);

/* Calls fn whenever a node appears or its permissions change, so a source can look again. */
void evdev_on_change(void (*fn)(void));

#endif
