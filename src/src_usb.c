#include "setting.h"
#include "loop.h"
#include "server.h"
#include "state.h"
#include "sources.h"
#include "uevent.h"
#include "util.h"
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#ifndef NO_SDBUS
#include <systemd/sd-hwdb.h>
#endif
#include <sys/timerfd.h>
#include <unistd.h>
#include "tunables.h"

/* USB devices the user plugs in: a webcam, a printer, a drive, a headset.
 * The kernel announces each one as a uevent (the same socket as the charger's), but with nothing a person
 * would call a name: that is in sysfs, which is read a moment later, once the device's interfaces are
 * there. Nothing is announced for what is found at start-up, nor for hubs, Bluetooth adapters and devices
 * the kernel says are built in (a laptop's own webcam), which are not things the user connects.
 * usb = 0 turns this off. Which kinds are announced is the listener's to say.
 *   usb.ID         a device that is connected, once it has settled: its name (maybe none), then kind=camera, printer, storage,
 *                  audio, scanner or other, and scanner=yes on a printer that scans too. It is there until its going is said: the key
 *                  goes with whatever is under it.
 *   usb.ID.busy    on a scanner (and a printer that scans): on while a program has it open, else off. Scanners are the devices SANE
 *                  knows (systemd's hardware database); one on the network is not seen here.
 * Keyboards, mice, tablets and gamepads are not here: they are input.ID (src_input.c), whatever they are plugged into. */


static const char *sysroot = "/sys";   /* a test points it at a tree of its own */
static const char *devroot = "/dev";
static const char *procroot = "/proc";
static bool off;
static int tfd = -1;

/* ---- what a device is, from sysfs ---- */

typedef struct {
    bool scanner;         /* it scans: a scanner, or a printer that is one too */
    char node[16];        /* its device node under /dev ("bus/usb/001/005"), for telling whether a program has it open */
    char name[96];
    const char *kind;     /* what it is: camera, printer, scanner, storage, audio or other */
} Found;

/* A device describes itself, so its strings are not to be trusted: no control characters in what is shown. */
static void clean(char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) *s = ' ';
}

/* "Wacom Co.,Ltd." is "Wacom": the company's name without its legal form. */
static void short_vendor(char *v) {
    char *cut = strchr(v, ',');
    if (cut) *cut = '\0';
    static const char *const forms[] = { " Co.", " Co ", " Inc", " Corp", " Ltd", " GmbH", " LLC", " Limited", " Technology" };
    for (size_t i = 0; i < sizeof forms / sizeof *forms; i++) {
        char *p = strcasestr(v, forms[i]);
        if (p && p != v) *p = '\0';
    }
    size_t len = strlen(v);
    while (len && v[len - 1] == ' ') v[--len] = '\0';
}

static void make_name(const char *vendor_in, const char *product, char *out, size_t n) {
    char vendor[64];
    snprintf(vendor, sizeof vendor, "%s", vendor_in);
    clean(vendor);
    short_vendor(vendor);
    char first[32];
    snprintf(first, sizeof first, "%.*s", (int)strcspn(vendor, " "), vendor);
    /* the brand in front, unless the product already says it */
    if (*product && *vendor && !(*first && strcasestr(product, first))) {
        if (snprintf(out, n, "%s %s", vendor, product) < 0) out[0] = '\0';   /* cut to fit, as intended */
    }
    else if (*product) snprintf(out, n, "%s", product);
    else if (*vendor) snprintf(out, n, "%s", vendor);
    else out[0] = '\0';   /* nothing to call it: the listener has its own words for that */
    state_text_clean(out);
}

/* Whether SANE knows the device (its entry in systemd's hardware database: libsane_matched=yes). It is a function pointer so that
 * a test can say which devices are scanners. */
#ifndef NO_SDBUS
static bool hwdb_scanner(const char *vid, const char *pid) {
    char modalias[48], up_v[8], up_p[8];
    snprintf(up_v, sizeof up_v, "%.4s", vid);
    snprintf(up_p, sizeof up_p, "%.4s", pid);
    for (char *c = up_v; *c; c++) if (*c >= 'a' && *c <= 'f') *c -= 32;
    for (char *c = up_p; *c; c++) if (*c >= 'a' && *c <= 'f') *c -= 32;
    snprintf(modalias, sizeof modalias, "usb:v%sp%s", up_v, up_p);
    static sd_hwdb *db;   /* opened on the first device that asks: mapping the database once, not for every device */
    if (!db && sd_hwdb_new(&db) < 0) { db = NULL; return false; }
    bool yes = false;
    const char *key, *value;
    if (sd_hwdb_seek(db, modalias) >= 0) {
        SD_HWDB_FOREACH_PROPERTY(db, modalias, key, value) {
            if (!strcmp(key, "libsane_matched") && !strcmp(value, "yes")) yes = true;
        }
    }
    return yes;
}
#else
static bool hwdb_scanner(const char *vid, const char *pid) { (void)vid; (void)pid; return false; }
#endif
static bool (*scanner_known)(const char *vid, const char *pid) = hwdb_scanner;

enum { C_AUDIO = 1 << 0, C_HID = 1 << 1, C_PRINTER = 1 << 2, C_STORAGE = 1 << 3, C_HUB = 1 << 4, C_VIDEO = 1 << 5,
       C_WIRELESS = 1 << 6, C_OTHER = 1 << 7 };

/* What the device at syspath is, or false for what is not announced: hubs, Bluetooth adapters, and
 * devices that are built in. */
static bool classify(const char *syspath, Found *out) {
    char text[128], removable[16] = "", vid[16] = "", vendor[64] = "", product[64] = "";
    util_read_text(syspath, "removable", removable, sizeof removable);
    if (!strcmp(removable, "fixed")) return false;

    unsigned classes = 0;
    if (util_read_text(syspath, "bDeviceClass", text, sizeof text) && strtol(text, NULL, 16) == 0x09) return false;

    const char *base = strrchr(syspath, '/');
    base = base ? base + 1 : syspath;
    size_t blen = strlen(base);
    DIR *d = opendir(syspath);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, base, blen) || e->d_name[blen] != ':') continue;   /* an interface: "1-2:1.0" */
        char idir[PATH_MAX];
        if (snprintf(idir, sizeof idir, "%s/%s", syspath, e->d_name) >= (int)sizeof idir) continue;
        if (!util_read_text(idir, "bInterfaceClass", text, sizeof text)) continue;
        long cls = strtol(text, NULL, 16);
        switch (cls) {
        case 0x01: classes |= C_AUDIO; break;
        case 0x03:
            classes |= C_HID;
            break;
        case 0x07: classes |= C_PRINTER; break;
        case 0x08: classes |= C_STORAGE; break;
        case 0x09: classes |= C_HUB; break;
        case 0x0e: classes |= C_VIDEO; break;
        case 0xe0: classes |= C_WIRELESS; break;
        default: classes |= C_OTHER; break;
        }
    }
    closedir(d);
    if (classes & C_HUB && !(classes & ~C_HUB)) return false;
    if (classes & C_WIRELESS && !(classes & ~C_WIRELESS)) return false;    /* a Bluetooth adapter: its devices are reported by bluez */

    util_read_text(syspath, "idVendor", vid, sizeof vid);
    util_read_text(syspath, "manufacturer", vendor, sizeof vendor);
    util_read_text(syspath, "product", product, sizeof product);
    clean(product);
    make_name(vendor, product, out->name, sizeof out->name);

    char pid[16] = "", busnum[16] = "", devnum[16] = "";
    util_read_text(syspath, "idProduct", pid, sizeof pid);
    util_read_text(syspath, "busnum", busnum, sizeof busnum);
    util_read_text(syspath, "devnum", devnum, sizeof devnum);
    out->scanner = scanner_known && vid[0] && pid[0] && !(classes & (C_VIDEO | C_STORAGE | C_AUDIO)) && scanner_known(vid, pid);
    out->node[0] = '\0';
    if (busnum[0] && devnum[0]) snprintf(out->node, sizeof out->node, "bus/usb/%03d/%03d", atoi(busnum), atoi(devnum));
    if (classes & C_VIDEO) out->kind = "camera";
    else if (classes & C_PRINTER) out->kind = "printer";
    else if (out->scanner) out->kind = "scanner";
    else if (classes & C_STORAGE) out->kind = "storage";
    else if (classes & C_AUDIO) out->kind = "audio";
    else if (classes & C_HID) return false;   /* a keyboard, a mouse, a tablet, a gamepad: the input source says so */
    else out->kind = "other";
    return true;
}

/* ---- the devices, and what is waiting to be said about them ---- */

typedef enum {
    D_FREE,
    D_ADDING,     /* appeared; its names are read once things have settled */
    D_SHOWN,      /* here, and announced */
    D_QUIET,      /* here, and not announced: a hub, a built-in device */
    D_REMOVING,   /* gone; the announcement waits to see whether it comes straight back */
} DevState;

typedef struct {
    DevState state;
    char path[192];       /* DEVPATH: "/devices/pci0000:00/.../usb1/1-11" */
    char product[48];     /* PRODUCT= of the uevent: vendor, product and revision, to tell a return from a swap */
    long long due;        /* when the wait is over, for ADDING and REMOVING */
    Found found;
    int wd;               /* the inotify watch on its device node, for a scanner (0 if none) */
    long long busy_due;   /* when to look at whether a program has it open (0: not waiting) */
    char id[12];          /* the ID of its key (usb.ID), while it has one: from its being shown to its being taken away */
} Dev;

static Dev devs[MAX_DEVICES];

/* "56a/378/100" (a uevent: hex without leading zeros, then the revision) and "056a/0378" (read from sysfs) are
 * the same device: compare the vendor and product numbers. */
static bool same_product(const char *a, const char *b) {
    char *ea, *eb;
    unsigned long va = strtoul(a, &ea, 16), vb = strtoul(b, &eb, 16);
    if (*ea != '/' || *eb != '/') return false;
    return va == vb && strtoul(ea + 1, NULL, 16) == strtoul(eb + 1, NULL, 16);
}

static Dev *find(const char *path) {
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devs[i].state != D_FREE && !strcmp(devs[i].path, path)) return &devs[i];
    return NULL;
}

static Dev *take(const char *path, const char *product) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devs[i].state != D_FREE) continue;
        memset(&devs[i], 0, sizeof devs[i]);
        snprintf(devs[i].path, sizeof devs[i].path, "%s", path);
        snprintf(devs[i].product, sizeof devs[i].product, "%s", product ? product : "");
        return &devs[i];
    }
    return NULL;
}

static void rearm(void) {
    if (tfd < 0) return;
    long long first = 0;
    for (int i = 0; i < MAX_DEVICES; i++) {
        if ((devs[i].state == D_ADDING || devs[i].state == D_REMOVING) && (!first || devs[i].due < first)) first = devs[i].due;
        if (devs[i].busy_due && (!first || devs[i].busy_due < first)) first = devs[i].busy_due;
    }
    struct itimerspec its = {0};
    if (first) {
        long long wait = first - now_ms();
        if (wait < 1) wait = 1;
        its.it_value.tv_sec = wait / 1000;
        its.it_value.tv_nsec = (wait % 1000) * 1000000L;
    }
    timerfd_settime(tfd, 0, &its, NULL);
}

/* ---- a scanner is busy while a program has it open ---- */

static int nfd = -1;

static void watch(Dev *d) {
    if (nfd < 0 || !d->found.node[0]) return;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", devroot, d->found.node);
    int wd = inotify_add_watch(nfd, path, IN_OPEN | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE);
    if (wd > 0) d->wd = wd;
}

/* Whether a program (not us) has the node open now: said when it changes. */
static void check_busy(Dev *d) {
    d->busy_due = 0;
    if (!d->id[0] || !d->found.scanner) return;
    char names[1][16], key[32];
    snprintf(names[0], sizeof names[0], "%.15s", d->found.node);
    snprintf(key, sizeof key, "usb.%s.busy", d->id);
    bus_set(key, dev_in_use(procroot, 1, names, getpid()) ? "on" : "off");
}

/* The device, as a key: usb.ID is its name, then what it is (kind=storage). It is
 * there from its being shown until its going is said, so that one that comes straight back does not blink. */
static void appear(Dev *d) {
    char attrs[48];
    if (!bus_new_id("usb", d->id, sizeof d->id)) return;
    snprintf(attrs, sizeof attrs, "kind=%s%s", d->found.kind, d->found.scanner && !strcmp(d->found.kind, "printer") ? "\tscanner=yes" : "");
    bus_device("usb", d->id, d->found.name, attrs);
    if (d->found.scanner) {
        char key[32];
        snprintf(key, sizeof key, "usb.%s.busy", d->id);
        bus_set(key, "off");
        watch(d);
    }
}

static void vanish(Dev *d) {
    if (!d->id[0]) return;
    char key[32];
    snprintf(key, sizeof key, "usb.%s", d->id);
    bus_unset_tree(key);
    if (d->wd > 0 && nfd >= 0) inotify_rm_watch(nfd, d->wd);
    d->wd = 0;
    d->busy_due = 0;
    d->id[0] = '\0';
}

/* A device has been here long enough to be read: what is it, and is it one that is shown? */
static void settle_adding(Dev *d) {
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s%s", sysroot, d->path);
    if (!classify(full, &d->found)) {
        d->state = D_QUIET;
        return;
    }
    d->state = D_SHOWN;
    appear(d);
}

/* Acts on whatever has waited long enough. */
static void settle(void) {
    long long now = now_ms();
    for (int i = 0; i < MAX_DEVICES; i++) {
        Dev *d = &devs[i];
        if (d->busy_due && d->busy_due <= now) check_busy(d);
        if (!d->due || d->due > now) continue;
        if (d->state == D_ADDING) {
            settle_adding(d);
        } else if (d->state == D_REMOVING) {
            vanish(d);
            d->state = D_FREE;
        }
    }
    rearm();
}

static void on_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(tfd, &n, sizeof n) > 0) settle();
}

/* A scanner's node was opened or closed: look at who has it, once the burst is over. */
static void on_notify(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len;
    while ((len = read(nfd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + len;) {
            struct inotify_event *e = (struct inotify_event *)p;
            for (int i = 0; i < MAX_DEVICES; i++)
                if (devs[i].wd == e->wd && e->wd > 0) devs[i].busy_due = now_ms() + SRC_USB_BUSY_MS;
            p += sizeof *e + e->len;
        }
    }
    rearm();
}

static void device_added(const char *path, const char *product) {
    Dev *d = find(path);
    if (d && d->state == D_REMOVING) {
        if (same_product(d->product, product)) {   /* the same device straight back (a resume, a loose cable): nothing happened */
            d->state = D_SHOWN;
            rearm();
            return;
        }
        vanish(d);                            /* a different one in the same place: the first is gone for good */
        d->state = D_FREE;
        d = NULL;
    }
    if (d) return;                            /* already known: an event twice */
    d = take(path, product);
    if (!d) return;
    d->state = D_ADDING;
    d->due = now_ms() + SRC_USB_SETTLE_MS;
    rearm();
}

static void device_removed(const char *path) {
    Dev *d = find(path);
    if (!d) return;
    if (d->state == D_SHOWN) {
        d->state = D_REMOVING;
        d->due = now_ms() + SRC_USB_SETTLE_MS;
    } else {
        d->state = D_FREE;                    /* never shown: nothing to take back */
    }
    rearm();
}

/* A kernel uevent is "action@path\0KEY=VALUE\0KEY=VALUE\0...". */
void usb_uevent(const char *msg, size_t len) {
    if (off) return;
    const char *end = msg + len;
    const char *action = NULL, *subsystem = NULL, *type = NULL, *path = NULL, *product = "";
    for (const char *p = msg + strlen(msg) + 1; p < end; p += strlen(p) + 1) {
        if (!strncmp(p, "ACTION=", 7)) action = p + 7;
        else if (!strncmp(p, "SUBSYSTEM=", 10)) subsystem = p + 10;
        else if (!strncmp(p, "DEVTYPE=", 8)) type = p + 8;
        else if (!strncmp(p, "DEVPATH=", 8)) path = p + 8;
        else if (!strncmp(p, "PRODUCT=", 8)) product = p + 8;
    }
    if (!action || !subsystem || strcmp(subsystem, "usb") || !type || strcmp(type, "usb_device") || !path) return;
    if (strlen(path) >= sizeof devs[0].path) return;
    if (!strcmp(action, "add")) device_added(path, product);
    else if (!strcmp(action, "remove")) device_removed(path);
}

/* The kernel's uevents: a device of the usb subsystem came or went. */
static void on_uevent(const char *subsystem, const char *msg, size_t len) {
    if (!strcmp(subsystem, "usb")) usb_uevent(msg, len);
}

/* The devices that are here at start-up are known, so that taking one out is announced, but not announced themselves. */
static void scan(void) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/bus/usb/devices", sysroot);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    size_t root_len = strlen(sysroot);
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;   /* interfaces are looked at with their device */
        char link[PATH_MAX], real[PATH_MAX];
        if (snprintf(link, sizeof link, "%s/%s", dir, e->d_name) >= (int)sizeof link) continue;
        if (!realpath(link, real) || strncmp(real, sysroot, root_len) || strlen(real + root_len) >= sizeof devs[0].path) continue;
        char product[48] = "", vid[16] = "", pid[16] = "";
        util_read_text(real, "idVendor", vid, sizeof vid);
        util_read_text(real, "idProduct", pid, sizeof pid);
        snprintf(product, sizeof product, "%s/%s", vid, pid);
        Dev *dev = take(real + root_len, product);
        if (!dev) break;
        settle_adding(dev);
    }
    closedir(d);
}

void usb_init(void) {
    const char *value = pac_setting("usb");
    off = value && !strcmp(value, "0");
    if (off) return;
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd >= 0) loop_add(tfd, EPOLLIN, on_timer, NULL);
    nfd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (nfd >= 0) loop_add(nfd, EPOLLIN, on_notify, NULL);
    scan();
    uevent_subscribe(on_uevent);
}
