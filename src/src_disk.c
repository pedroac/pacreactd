#include "setting.h"
#include "loop.h"
#include "server.h"
#include "state.h"
#include "sources.h"
#include "util.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/statvfs.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include "tunables.h"

/* Low disk space. The kernel has no "this filesystem is filling up" event, so this is the one
 * source that cannot be fully event-driven: a slow timer (default every 5 minutes) checks the
 * filesystems that sit on a block device, and a change in the mount table (a drive plugged in)
 * checks at once. disk = 0 turns it off.
 *   disk.NAME         percent free, with the level warning when it is low and critical when it is almost full. NAME is the
 *                     mount point as a word: root, home, mnt-data
 *   disk.NAME.mount   the mount point
 *   disk.NAME.free    the bytes available, which is what the message wants to say, and the percentage does not */


static int warn_pct = DEFAULT_WARN_PCT, crit_pct = DEFAULT_CRIT_PCT;

typedef struct {
    char mount[256];
    char dev[24];       /* "major:minor": the same device mounted twice is one filesystem */
    char name[24];      /* the NAME of its keys */
    int level;          /* 0 fine, 1 low, 2 almost full */
    bool seen;
} Fs;

static Fs filesystems[MAX_FS];
static int mounts_fd = -1, timer_fd = -1;

/* ---- decisions (no I/O) ---- */

static double free_pct(uint64_t avail, uint64_t total) {
    return total ? (double)avail * 100.0 / (double)total : 100.0;
}

/* The level for the space left, with hysteresis: a level is only left once the filesystem has
 * recovered past a margin, so a value hovering at a threshold does not flap. `total` is what
 * `df` calls used plus available. */
int disk_level(uint64_t avail, uint64_t total, int current) {
    double pct = free_pct(avail, total);

    /* Small filesystems have little room in bytes even when they are nearly empty in percent:
     * an almost-empty 1 GB boot partition is not critical, so the byte limit only counts when
     * the percentage is low too. */
    int level = 0;
    if (pct < crit_pct || (pct < warn_pct && avail < CRIT_BYTES)) level = 2;
    else if (pct < warn_pct) level = 1;

    if (current == 2 && level < 2 && !(pct >= crit_pct + REARM_MARGIN_PCT && avail >= 2 * CRIT_BYTES))
        level = 2;
    if (current >= 1 && level < 1 && pct < warn_pct + REARM_MARGIN_PCT) level = 1;
    return level;
}

/* ---- the mount table ---- */

typedef struct {
    char mount[256];
    char dev[24];
} Mount;

/* "\040" and friends: mountinfo escapes spaces, tabs, newlines and backslashes in octal. */
static void unescape(char *s) {
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 3;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static bool has_option(const char *options, const char *want) {
    size_t len = strlen(want);
    for (const char *p = options; p && *p;) {
        if (!strncmp(p, want, len) && (p[len] == ',' || p[len] == '\0')) return true;
        p = strchr(p, ',');
        if (p) p++;
    }
    return false;
}

/* The filesystems worth watching in /proc/self/mountinfo text: on a block device, writable, not
 * a read-only image format, once per device. Returns how many were found. */
int disk_parse_mountinfo(const char *text, Mount *out, int max) {
    int n = 0;
    char copy[8192];
    snprintf(copy, sizeof copy, "%s", text);

    char *save_line, *line = strtok_r(copy, "\n", &save_line);
    for (; line && n < max; line = strtok_r(NULL, "\n", &save_line)) {
        /* id parent maj:min root mountpoint options [optional fields...] - fstype source superopts */
        char *sep = strstr(line, " - ");
        if (!sep) continue;
        char fstype[32] = "", source[256] = "";
        if (sscanf(sep + 3, "%31s %255s", fstype, source) != 2) continue;

        char head[1024];
        snprintf(head, sizeof head, "%.*s", (int)(sep - line), line);
        char *tok[7] = {0}, *save;
        int t = 0;
        for (char *p = strtok_r(head, " ", &save); p && t < 7; p = strtok_r(NULL, " ", &save)) tok[t++] = p;
        if (t < 6) continue;

        if (strncmp(source, "/dev/", 5) != 0) continue;                 /* tmpfs, proc, overlay, ... */
        if (!strcmp(fstype, "squashfs") || !strcmp(fstype, "iso9660") || !strcmp(fstype, "udf") ||
            !strcmp(fstype, "erofs"))
            continue;                                                    /* images: always full */
        if (has_option(tok[5], "ro")) continue;                          /* cannot fill up */

        Mount m;
        snprintf(m.dev, sizeof m.dev, "%s", tok[2]);
        snprintf(m.mount, sizeof m.mount, "%s", tok[4]);
        unescape(m.mount);

        bool dup = false;
        for (int i = 0; i < n; i++)
            if (!strcmp(out[i].dev, m.dev)) dup = true;                  /* bind mounts, subvolumes */
        if (!dup) out[n++] = m;
    }
    return n;
}

/* The mount point as a word for a key: "/" is root, "/mnt/My Disk" is mnt-my-disk; two that come to the same are told apart. */
static void key_name(const char *mount, const Fs *self, char *out, size_t n) {
    char base[24];
    size_t o = 0;
    for (const char *c = mount + (*mount == '/'); *c && o < sizeof base - 1; c++) {
        char x = (*c >= 'A' && *c <= 'Z') ? (char)(*c + 32) : *c;
        bool word = (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9');
        if (word) base[o++] = x;
        else if (o && base[o - 1] != '-') base[o++] = '-';
    }
    while (o && base[o - 1] == '-') o--;
    base[o] = '\0';
    if (!o) snprintf(base, sizeof base, "%s", "root");
    for (int tries = 1;; tries++) {
        if (tries == 1) snprintf(out, n, "%s", base);
        else snprintf(out, n, "%.14s-%d", base, tries % 1000);
        bool taken = false;
        for (int i = 0; i < MAX_FS; i++)
            if (&filesystems[i] != self && filesystems[i].dev[0] && !strcmp(filesystems[i].name, out)) taken = true;
        if (!taken) return;
    }
}

/* ---- checking ---- */

static Fs *fs_for(const char *dev, const char *mount) {
    Fs *free_slot = NULL;
    for (int i = 0; i < MAX_FS; i++) {
        if (filesystems[i].dev[0] && !strcmp(filesystems[i].dev, dev)) {
            snprintf(filesystems[i].mount, sizeof filesystems[i].mount, "%s", mount);
            return &filesystems[i];
        }
        if (!filesystems[i].dev[0] && !free_slot) free_slot = &filesystems[i];
    }
    if (free_slot) {
        snprintf(free_slot->dev, sizeof free_slot->dev, "%s", dev);
        snprintf(free_slot->mount, sizeof free_slot->mount, "%s", mount);
        free_slot->level = 0;
        key_name(mount, free_slot, free_slot->name, sizeof free_slot->name);
    }
    return free_slot;
}

/* One filesystem's numbers in; what is true of it out. */
void disk_evaluate(const char *dev, const char *mount, uint64_t avail, uint64_t total) {
    Fs *fs = fs_for(dev, mount);
    if (!fs) return;
    fs->seen = true;
    fs->level = disk_level(avail, total, fs->level);

    char key[48], text[VALUE_LEN];
    snprintf(key, sizeof key, "disk.%s.mount", fs->name);
    snprintf(text, sizeof text, "%s", mount);
    state_text_clean(text);                              /* a mount point can be called anything */
    state_text_cut(text, VALUE_LEN - 1);
    if (*text) bus_set(key, text);
    snprintf(key, sizeof key, "disk.%s.free", fs->name);
    snprintf(text, sizeof text, "%llu", (unsigned long long)avail);
    bus_set(key, text);                                  /* these two first, so that whoever reads the level finds them current */
    snprintf(key, sizeof key, "disk.%s", fs->name);
    snprintf(text, sizeof text, "%d", (int)free_pct(avail, total));
    bus_set_level(key, text, fs->level == 2 ? LEVEL_CRITICAL : fs->level == 1 ? LEVEL_WARNING : LEVEL_NONE);
}

/* An unmounted filesystem: what was said of it is gone, so that it can be told anew if it comes back. */
static void forget(Fs *fs) {
    char key[48];
    snprintf(key, sizeof key, "disk.%s", fs->name);
    bus_unset(key);
    snprintf(key, sizeof key, "disk.%s.mount", fs->name);
    bus_unset(key);
    snprintf(key, sizeof key, "disk.%s.free", fs->name);
    bus_unset(key);
    memset(fs, 0, sizeof *fs);
}

static void check_all(void) {
    char text[8192];
    ssize_t len = 0;
    if (mounts_fd >= 0) {  /* re-reading from the start also re-arms the change notification */
        if (lseek(mounts_fd, 0, SEEK_SET) < 0) return;
        ssize_t r;
        while (len < (ssize_t)sizeof text - 1 && (r = read(mounts_fd, text + len, sizeof text - 1 - len)) > 0) len += r;
    }
    text[len > 0 ? len : 0] = '\0';

    Mount mounts[MAX_FS];
    int n = disk_parse_mountinfo(text, mounts, MAX_FS);

    for (int i = 0; i < MAX_FS; i++) filesystems[i].seen = false;
    for (int i = 0; i < n; i++) {
        struct statvfs sv;
        if (statvfs(mounts[i].mount, &sv) != 0 || sv.f_blocks == 0) continue;
        uint64_t unit = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        uint64_t avail = (uint64_t)sv.f_bavail * unit;
        uint64_t used = (uint64_t)(sv.f_blocks - sv.f_bfree) * unit;
        disk_evaluate(mounts[i].dev, mounts[i].mount, avail, used + avail);
    }
    for (int i = 0; i < MAX_FS; i++)             /* unmounted: forget it, so it can be told anew */
        if (filesystems[i].dev[0] && !filesystems[i].seen) forget(&filesystems[i]);
}

static void on_mounts(void *ctx, uint32_t ev) { (void)ctx; (void)ev; check_all(); }

static void on_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(timer_fd, &n, sizeof n) > 0) check_all();
}

void disk_init(void) {
    const char *off = pac_setting("disk");
    if (off && !strcmp(off, "0")) return;

    warn_pct = (int)setting_int("disk_warn", DEFAULT_WARN_PCT, 2, 95, "percent");
    crit_pct = (int)setting_int("disk_crit", DEFAULT_CRIT_PCT, 1, 94, "percent");
    if (crit_pct >= warn_pct) crit_pct = warn_pct - 1;
    long interval = setting_int("disk_interval", SRC_DISK_DEFAULT_INTERVAL_S, 0, 86400, "seconds");  /* 0: mount changes only */

    mounts_fd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mounts_fd >= 0) loop_add(mounts_fd, EPOLLPRI | EPOLLERR, on_mounts, NULL);
    if (interval > 0) {
        timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (timer_fd >= 0) {
            struct itimerspec its = { .it_value.tv_sec = interval, .it_interval.tv_sec = interval };
            timerfd_settime(timer_fd, 0, &its, NULL);
            loop_add(timer_fd, EPOLLIN, on_timer, NULL);
        }
    }
    check_all();
}
