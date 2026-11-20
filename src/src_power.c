#include "server.h"
#include "sources.h"
#include "tunables.h"
#include "uevent.h"
#include "loop.h"
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* The battery and the charger, from sysfs. A power_supply uevent says something changed, and we
 * read; while discharging a timer reads too, in case an event is missed.
 *   power.battery         percent, with a level while discharging and low
 *   power.battery.status  discharging, charging, full, not-charging, unknown
 *   power.ac              on if any mains or USB supply is online */

static const char *power_root = "/sys/class/power_supply";
static int timer_fd = -1;
static char bat_dir[300];   /* sysfs dir of the first battery, if any */
static Level latch;         /* the worst level since the battery last recovered */

/* The level of a battery. Once warned a level is kept until the battery is above SRC_POWER_REARM_PCT or
 * is not discharging, so that a level does not flicker as the percentage wavers. */
static Level battery_level(int capacity, bool discharging) {
    if (!discharging || capacity > SRC_POWER_REARM_PCT) {
        latch = LEVEL_NONE;
    } else {
        Level raw = capacity <= CRIT_PCT ? LEVEL_CRITICAL : capacity <= WARN_PCT ? LEVEL_WARNING : LEVEL_NONE;
        if (raw > latch) latch = raw;
    }
    return latch;
}

static bool read_attr(const char *dir, const char *attr, char *out, size_t n) {
    char path[360];
    snprintf(path, sizeof path, "%s/%s", dir, attr);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = '\0';
    return ok;
}

/* "Not charging" is "not-charging": a value has no reason to hold a space that a reader must split on. */
static void word(char *s) {
    for (; *s; s++) *s = *s == ' ' ? '-' : (char)tolower((unsigned char)*s);
}

/* Reads the battery and keeps the fallback timer armed only while discharging. */
static void check_battery(void) {
    char cap[16], status[32];
    bool discharging = false;
    if (read_attr(bat_dir, "capacity", cap, sizeof cap) && read_attr(bat_dir, "status", status, sizeof status)) {
        discharging = !strcmp(status, "Discharging");
        word(status);
        char value[16];
        int percent = atoi(cap);
        snprintf(value, sizeof value, "%d", percent);
        bus_set("power.battery.status", status);
        bus_set_level("power.battery", value, battery_level(percent, discharging));
    }
    else if (access(bat_dir, F_OK) != 0) {   /* the battery was taken out: what was said of it is gone */
        bus_unset("power.battery");
        bus_unset("power.battery.status");
        bat_dir[0] = '\0';
    }
    struct itimerspec its = { .it_value.tv_sec = discharging ? SRC_POWER_POLL_SECS : 0 };
    if (timer_fd >= 0) timerfd_settime(timer_fd, 0, &its, NULL);
}

/* The charger is on if any mains or USB supply is online. */
static void check_ac(void) {
    DIR *d = opendir(power_root);
    if (!d) return;
    struct dirent *e;
    bool any = false, online = false;
    while ((e = readdir(d))) {
        char dir[300], type[16], on[8];
        if (e->d_name[0] == '.') continue;
        snprintf(dir, sizeof dir, "%s/%s", power_root, e->d_name);
        if (!read_attr(dir, "type", type, sizeof type) || (strcmp(type, "Mains") && strncmp(type, "USB", 3))) continue;
        if (!read_attr(dir, "online", on, sizeof on)) continue;
        any = true;
        if (on[0] == '1') online = true;
    }
    closedir(d);
    if (any) bus_set("power.ac", online ? "on" : "off");
    else bus_unset("power.ac");   /* every supply went (a dock, an adapter): nothing is known */
}

static void on_timer_cb(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t expirations;
    if (read(timer_fd, &expirations, sizeof expirations) > 0) check_battery();
}

static bool find_battery(void);

static void start_battery(void) {
    if (timer_fd < 0) {
        timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (timer_fd >= 0) loop_add(timer_fd, EPOLLIN, on_timer_cb, NULL);
    }
    check_battery();
}

static void on_uevent(const char *subsystem, const char *msg, size_t len) {
    (void)msg; (void)len;
    if (strcmp(subsystem, "power_supply")) return;
    if (!*bat_dir && find_battery()) start_battery();   /* one that came after start-up (a dock's, a hot-swapped one) */
    else if (*bat_dir) check_battery();
    check_ac();
}


static bool find_battery(void) {
    DIR *d = opendir(power_root);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        char dir[300], type[16];
        if (e->d_name[0] == '.') continue;
        snprintf(dir, sizeof dir, "%s/%s", power_root, e->d_name);
        char scope[16] = "";
        read_attr(dir, "scope", scope, sizeof scope);
        if (read_attr(dir, "type", type, sizeof type) && !strcmp(type, "Battery") && strcmp(scope, "Device") &&
            read_attr(dir, "capacity", type, sizeof type)) {  /* a pen's or a mouse's has scope Device; some have no capacity */
            snprintf(bat_dir, sizeof bat_dir, "%s", dir);
            found = true;
        }
    }
    closedir(d);
    return found;
}

void power_init(void) {
    if (find_battery()) start_battery();
    check_ac();
    uevent_subscribe(on_uevent);
}
