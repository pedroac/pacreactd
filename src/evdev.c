#include "evdev.h"
#include "loop.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LONG_BITS (sizeof(unsigned long) * CHAR_BIT)
/* Words for the bits 0..max inclusive: that is max + 1 bits, so this is exactly their ceiling (adding LONG_BITS - 1 to max would be one short). */
#define BITS_LEN(max) (((max) + LONG_BITS) / LONG_BITS)

static bool test_bit(const unsigned long *bits, int bit) {
    return bits[bit / LONG_BITS] >> (bit % LONG_BITS) & 1;
}

int evdev_open(const char *path, char *name, size_t name_len) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (name && name_len) {
        name[0] = '\0';
        if (ioctl(fd, EVIOCGNAME((int)name_len - 1), name) < 0) name[0] = '\0';
        name[name_len - 1] = '\0';
    }
    return fd;
}

bool evdev_has_type(int fd, int type) {
    unsigned long bits[BITS_LEN(EV_MAX)] = {0};
    return ioctl(fd, EVIOCGBIT(0, sizeof bits), bits) >= 0 && test_bit(bits, type);
}

bool evdev_has_code(int fd, int type, int code) {
    unsigned long bits[BITS_LEN(KEY_MAX)] = {0};
    return ioctl(fd, EVIOCGBIT(type, sizeof bits), bits) >= 0 && code <= KEY_MAX && test_bit(bits, code);
}

int evdev_switch_state(int fd, int code) {
    unsigned long bits[BITS_LEN(SW_MAX)] = {0};
    if (ioctl(fd, EVIOCGSW(sizeof bits), bits) < 0 || code > SW_MAX) return -1;
    return test_bit(bits, code);
}

int evdev_led_state(int fd, int code) {
    unsigned long bits[BITS_LEN(LED_MAX)] = {0};
    if (ioctl(fd, EVIOCGLED(sizeof bits), bits) < 0 || code > LED_MAX) return -1;
    return test_bit(bits, code);
}

bool evdev_deliver_only(int fd, const int *types, int count) {
    unsigned long bits[BITS_LEN(EV_MAX)] = {0};
    bits[EV_SYN / LONG_BITS] |= 1UL << (EV_SYN % LONG_BITS);      /* frame markers are harmless */
    for (int i = 0; i < count; i++)
        if (types[i] <= EV_MAX) bits[types[i] / LONG_BITS] |= 1UL << (types[i] % LONG_BITS);
    struct input_mask mask = { .type = 0, .codes_size = sizeof bits, .codes_ptr = (uint64_t)(uintptr_t)bits };
    return ioctl(fd, EVIOCSMASK, &mask) == 0;                      /* type 0: the mask of event types */
}

bool evdev_delivers(int fd, int type) {
    unsigned long bits[BITS_LEN(EV_MAX)] = {0};
    struct input_mask mask = { .type = 0, .codes_size = sizeof bits, .codes_ptr = (uint64_t)(uintptr_t)bits };
    if (ioctl(fd, EVIOCGMASK, &mask) != 0 || type > EV_MAX) return false;
    return test_bit(bits, type);
}

int evdev_read(int fd, struct input_event *events, int max) {
    ssize_t n = read(fd, events, sizeof *events * (size_t)max);
    if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
    return (int)(n / (ssize_t)sizeof *events);
}

void evdev_scan(EvdevScanFn fn, void *ctx) {
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300];
        int len = snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        if (len > 0 && (size_t)len < sizeof path) fn(path, ctx);
    }
    closedir(d);
}

static void (*change_cb[4])(void);
static int change_count, watch_fd = -1;

static void on_inotify(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    char buf[1024];
    while (read(watch_fd, buf, sizeof buf) > 0) {}   /* the events themselves do not matter */
    for (int i = 0; i < change_count; i++) change_cb[i]();
}

void evdev_on_change(void (*fn)(void)) {
    if (change_count >= 4) return;
    change_cb[change_count++] = fn;
    if (watch_fd >= 0) return;
    watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watch_fd < 0) return;
    /* IN_ATTRIB too: a new node is often readable only once udev has set its permissions */
    if (inotify_add_watch(watch_fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        close(watch_fd);
        watch_fd = -1;
        return;
    }
    loop_add(watch_fd, EPOLLIN, on_inotify, NULL);
}
