/* USB devices: what a device is (from a sysfs tree the test builds), what it is called, and what is said
 * about it and when. src_usb.c is included for its statics; the bus, the loop and the clock are stubbed.
 *
 * `toasts`, `icons` and `last_sound` are what a listener that announces devices (pacreact) makes of each `usb.event`, so
 * that the assertions about what a plug is called and when still read as they did. */
#include "../src/loop.h"
#include "../src/server.h"
#include "../src/sources.h"
#include "../src/tunables.h"
#include "../src/state.h"
#include "../src/uevent.h"
#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static long long fake_ms = 1000000;
static int fake_clock_gettime(clockid_t c, struct timespec *ts) {
    (void)c;
    ts->tv_sec = fake_ms / 1000;
    ts->tv_nsec = (fake_ms % 1000) * 1000000L;
    return 0;
}
#define clock_gettime fake_clock_gettime

enum { SOUND_DEVICE_ADDED, SOUND_DEVICE_REMOVED, SOUND_KINDS };
static char toasts[16][160], icons[16][160];
static int ntoasts;
static int last_sound = SOUND_KINDS;
static int nsounds;
static char events[80][VALUE_LEN];    /* the value of each device key, as it first came */
static int nevents;
static bool listener_synced = true;   /* a listener learns what is there when it connects, and says nothing of it */

/* The devices as a listener (pacreact) has them: a key usb.ID or input.ID that appears is a device that connected, and one that
 * goes is one that disconnected; it says so by the kind it had. */
static struct { char key[24], name[96], kind[16]; bool builtin; } listed[80];

static void said(bool plugged, const char *kind, const char *name) {
    static const struct { const char *kind, *icon; } kinds[] = {
        { "camera", "camera-web,camera-video,camera-photo" }, { "printer", "printer" },
        { "storage", "drive-removable-media-usb,drive-removable-media" }, { "audio", "audio-headset,audio-card" },
        { "tablet", "input-tablet,input-mouse" }, { "keyboard", "input-keyboard" }, { "mouse", "input-mouse" },
        { "gamepad", "input-gaming,input-mouse" }, { "scanner", "scanner" }, { "other", "preferences-desktop-peripherals,drive-removable-media-usb" },
    };
    assert(ntoasts < 16);
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++) {
        if (strcmp(kind, kinds[i].kind)) continue;
        snprintf(toasts[ntoasts], sizeof toasts[0], "%s %s", *name ? name : "USB device", plugged ? "connected" : "disconnected");
        if (plugged) snprintf(icons[ntoasts], sizeof icons[0], "%s", kinds[i].icon);
        else {   /* the first name's "-off" glyph, then the names as they are */
            int first = (int)strcspn(kinds[i].icon, ",");
            snprintf(icons[ntoasts], sizeof icons[0], "%.*s-off,%s", first, kinds[i].icon, kinds[i].icon);
        }
        ntoasts++;
        last_sound = plugged ? SOUND_DEVICE_ADDED : SOUND_DEVICE_REMOVED;
        nsounds++;
        return;
    }
    assert(0 && "a kind nobody knows");
}

/* usb.ID and input.ID, and no key under them: a device */
static bool device_key(const char *key) {
    return (!strncmp(key, "usb.", 4) || !strncmp(key, "input.", 6)) && !strchr(strchr(key, '.') + 1, '.');
}

static const char *attr_of(const char *value, const char *name) {   /* a word, in a static buffer; "" if none */
    static char word[24];
    char want[24];
    snprintf(want, sizeof want, "\t%s=", name);
    const char *at = strstr(value, want);
    word[0] = '\0';
    if (at) snprintf(word, sizeof word, "%.*s", (int)strcspn(at + strlen(want), "\t"), at + strlen(want));
    return word;
}

bool bus_set(const char *key, const char *value) {
    state_set(key, value);
    if (!device_key(key)) return true;
    int i = 0;
    while (i < 80 && listed[i].key[0] && strcmp(listed[i].key, key)) i++;
    assert(i < 80);
    if (listed[i].key[0]) return true;                                    /* a change of what there is: not a connection */
    snprintf(listed[i].key, sizeof listed[i].key, "%s", key);
    snprintf(listed[i].name, sizeof listed[i].name, "%.*s", (int)strcspn(value, "\t"), value);
    snprintf(listed[i].kind, sizeof listed[i].kind, "%s", attr_of(value, "kind"));
    listed[i].builtin = !strcmp(attr_of(value, "builtin"), "yes");
    assert(nevents < 80);
    snprintf(events[nevents++], VALUE_LEN, "%s", value);
    if (listener_synced) said(true, listed[i].kind, listed[i].name);
    return true;
}
bool bus_set_level(const char *key, const char *value, Level level) { (void)key; (void)value; (void)level; return true; }
void bus_unset(const char *key) {
    state_unset(key);
    for (int i = 0; i < 80; i++) {
        if (strcmp(listed[i].key, key) || !listed[i].key[0]) continue;
        said(false, listed[i].kind, listed[i].name);
        memset(&listed[i], 0, sizeof listed[i]);
    }
}
/* Whether a device of that kind is there now (and, when `builtin`, one that is a laptop's own) */
static bool has_kind(const char *kind, bool builtin) {
    for (int i = 0; i < 80; i++)
        if (listed[i].key[0] && !strcmp(listed[i].kind, kind) && (!builtin || listed[i].builtin)) return true;
    return false;
}
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }
bool uevent_subscribe(UeventFn fn) { (void)fn; return true; }

#include "../src/src_usb.c"

/* ---- a sysfs of the test's own ---- */

static char root[200];

static void put(const char *dir, const char *attr, const char *value) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, attr);
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "%s\n", value);
    fclose(f);
}

static void mk(const char *path) {
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", path);
    assert(system(cmd) == 0);
}

typedef struct { const char *cls, *proto; } Ifc;

/* A device "1-N" under root/devices/pci/usb1, with interfaces; returns its DEVPATH */
static const char *make_device(const char *port, const char *removable, const char *devclass, const char *vid,
                               const char *pid, const char *vendor, const char *product, const Ifc *ifcs, int n) {
    static char devpath[192];
    snprintf(devpath, sizeof devpath, "/devices/pci/usb1/%s", port);
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s%s", root, devpath);
    mk(dir);
    put(dir, "removable", removable);
    put(dir, "bDeviceClass", devclass);
    put(dir, "idVendor", vid);
    put(dir, "idProduct", pid);
    if (vendor) put(dir, "manufacturer", vendor);
    if (product) put(dir, "product", product);
    for (int i = 0; i < n; i++) {
        char idir[PATH_MAX];
        assert(snprintf(idir, sizeof idir, "%s/%s:1.%d", dir, port, i) < (int)sizeof idir);
        mk(idir);
        put(idir, "bInterfaceClass", ifcs[i].cls);
        put(idir, "bInterfaceProtocol", ifcs[i].proto);
    }
    char link[PATH_MAX], target[PATH_MAX];
    snprintf(link, sizeof link, "%s/bus/usb/devices", root);
    mk(link);
    snprintf(link, sizeof link, "%s/bus/usb/devices/%s", root, port);
    snprintf(target, sizeof target, "../../../devices/pci/usb1/%s", port);
    unlink(link);
    assert(symlink(target, link) == 0);
    return devpath;
}

/* A kernel uevent for a USB device, built the way the kernel sends it */
static size_t uevent(char *buf, const char *action, const char *devpath, const char *product, const char *devtype,
                     const char *subsystem) {
    size_t n = 0;
    n += sprintf(buf + n, "%s@%s", action, devpath) + 1;
    n += sprintf(buf + n, "ACTION=%s", action) + 1;
    n += sprintf(buf + n, "DEVPATH=%s", devpath) + 1;
    n += sprintf(buf + n, "SUBSYSTEM=%s", subsystem) + 1;
    n += sprintf(buf + n, "DEVTYPE=%s", devtype) + 1;
    n += sprintf(buf + n, "PRODUCT=%s", product) + 1;
    return n;
}

static void event(const char *action, const char *devpath, const char *product) {
    char buf[1024];
    size_t n = uevent(buf, action, devpath, product, "usb_device", "usb");
    usb_uevent(buf, n);
}

static void forget(void) {
    memset(devs, 0, sizeof devs);
    state_clear();
    ntoasts = nsounds = nevents = 0;
    memset(listed, 0, sizeof listed);
    listener_synced = true;
    off = false;
}

/* ---- pure parts ---- */

static void test_names(void) {
    char out[96];
    make_name("Wacom Co.,Ltd.", "Intuos BT M", out, sizeof out);
    assert(!strcmp(out, "Wacom Intuos BT M"));                /* the company without its legal form */
    make_name("HP", "ENVY 4520 series", out, sizeof out);
    assert(!strcmp(out, "HP ENVY 4520 series"));
    make_name("Logitech", "Logitech C920", out, sizeof out);
    assert(!strcmp(out, "Logitech C920"));                    /* not said twice */
    make_name("", "USB Keyboard", out, sizeof out);
    assert(!strcmp(out, "USB Keyboard"));
    make_name("SanDisk Corp.", "", out, sizeof out);
    assert(!strcmp(out, "SanDisk"));
    make_name("", "", out, sizeof out);
    assert(!strcmp(out, ""));                                 /* nothing to call it: the listener says "USB device" */
    make_name("Evil", "name\x1b[31m\nred", out, sizeof out);   /* a device chooses its own strings */
    assert(!strchr(out, '\n') && !strchr(out, 0x1b));

    assert(same_product("56a/378/100", "056a/0378"));
    assert(!same_product("56a/378/100", "056a/0379"));
    assert(!same_product("", "056a/0378"));
}

/* ---- what a device is ---- */

static Found what(const char *devpath, bool *announced) {
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s%s", root, devpath);
    Found f = {0};
    *announced = classify(full, &f);
    return f;
}

static void test_classify(void) {
    bool yes;
    Found f;

    /* a webcam has a microphone too: still a webcam */
    const Ifc cam[] = { { "0e", "00" }, { "0e", "00" }, { "01", "00" }, { "01", "00" } };
    f = what(make_device("1-1", "removable", "ef", "046d", "082d", "Logitech", "HD Pro Webcam C920", cam, 4), &yes);
    assert(yes && !strcmp(f.kind, "camera") && !strcmp(f.name, "Logitech HD Pro Webcam C920"));

    const Ifc printer[] = { { "07", "04" }, { "ff", "01" }, { "07", "04" } };
    f = what(make_device("1-2", "removable", "00", "03f0", "d711", "HP", "ENVY 4520 series", printer, 3), &yes);
    assert(yes && !strcmp(f.kind, "printer") && !strcmp(f.name, "HP ENVY 4520 series"));

    const Ifc drive[] = { { "08", "50" } };
    f = what(make_device("1-3", "removable", "00", "0781", "5581", "SanDisk", "Ultra", drive, 1), &yes);
    assert(yes && !strcmp(f.kind, "storage"));

    const Ifc headset[] = { { "01", "00" }, { "01", "00" }, { "03", "00" } };
    f = what(make_device("1-4", "removable", "00", "1b3f", "0001", NULL, "USB Headset", headset, 3), &yes);
    assert(yes && !strcmp(f.kind, "audio"));

    /* keyboards, mice, tablets and gamepads are HID devices: the input source says so (input.ID), not this one */
    const Ifc kbd[] = { { "03", "01" }, { "03", "00" } };
    what(make_device("1-7", "removable", "00", "413c", "2113", NULL, "USB Keyboard", kbd, 2), &yes);
    assert(!yes);
    const Ifc tablet[] = { { "03", "00" } };
    what(make_device("1-5", "removable", "00", "1234", "5678", "Acme", "Sketch Pad", tablet, 1), &yes);
    assert(!yes);
    f = what(make_device("1-15", "removable", "00", "046d", "082e", "Logitech", "Cam", cam, 4), &yes);
    assert(yes && !strcmp(f.kind, "camera"));

    const Ifc phone[] = { { "ff", "42" } };
    f = what(make_device("1-9", "unknown", "00", "18d1", "4ee1", "Google", "Pixel", phone, 1), &yes);
    assert(yes && !strcmp(f.kind, "other"));                         /* "unknown" whether removable: announced */

    /* what is not announced: hubs, Bluetooth adapters, devices built in */
    const Ifc hub[] = { { "09", "00" } };
    what(make_device("1-10", "removable", "09", "05e3", "0610", NULL, "USB2.0 Hub", hub, 1), &yes);
    assert(!yes);
    const Ifc bt[] = { { "e0", "01" }, { "e0", "01" } };
    what(make_device("1-11", "removable", "e0", "8087", "0026", NULL, NULL, bt, 2), &yes);
    assert(!yes);
    what(make_device("1-12", "fixed", "00", "04f2", "b6d0", "Chicony", "Integrated Camera", cam, 4), &yes);
    assert(!yes);                                             /* a laptop's own webcam */
    what("/devices/pci/usb1/gone", &yes);
    assert(!yes);                                             /* nothing there */
}

/* ---- scanners ---- */

static bool fake_scanner(const char *vid, const char *pid) {   /* what SANE's database would say */
    return (!strcmp(vid, "04a9") && !strcmp(pid, "1909")) || (!strcmp(vid, "03f0") && !strcmp(pid, "d711"));
}

static void test_scanners(void) {
    scanner_known = fake_scanner;
    bool yes;
    Found f;
    /* a scanner is a vendor-specific device: told apart by SANE knowing it */
    const Ifc vendor[] = { { "ff", "ff" } };
    char path[192];   /* make_device gives a buffer it reuses */
    snprintf(path, sizeof path, "%s", make_device("5-1", "removable", "ff", "04a9", "1909", "Canon", "CanoScan LiDE 110", vendor, 1));
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s%s", root, path);
    put(dir, "busnum", "1"); put(dir, "devnum", "7");
    f = what(path, &yes);
    assert(yes && !strcmp(f.kind, "scanner") && f.scanner && !strcmp(f.node, "bus/usb/001/007"));
    /* the same vendor's other devices are not */
    f = what(make_device("5-2", "removable", "ff", "04a9", "0001", "Canon", "Thing", vendor, 1), &yes);
    assert(yes && !strcmp(f.kind, "other") && !f.scanner);
    /* a printer that scans too is a printer, and says so */
    const Ifc mfp[] = { { "07", "04" }, { "ff", "01" } };
    f = what(make_device("5-3", "removable", "00", "03f0", "d711", "HP", "ENVY 4520 series", mfp, 2), &yes);
    assert(yes && !strcmp(f.kind, "printer") && f.scanner);
    const Ifc plain[] = { { "07", "04" } };
    f = what(make_device("5-4", "removable", "00", "03f0", "1111", "HP", "LaserJet", plain, 1), &yes);
    assert(yes && !strcmp(f.kind, "printer") && !f.scanner);

    /* it comes as a device with scanner state, and a printer that scans says so */
    forget();
    event("add", path, "4a9/1909/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 1 && !strcmp(toasts[0], "Canon CanoScan LiDE 110 connected"));
    Dev *d = find(path);
    assert(d && d->id[0] && !strcmp(state_get(strcat(strcpy(dir, "usb."), d->id)), "Canon CanoScan LiDE 110\tkind=scanner"));
    char key[32];
    snprintf(key, sizeof key, "usb.%s.busy", d->id);
    assert(!strcmp(state_get(key), "off"));

    /* busy while a program has the node open (a made-up /proc), idle again when it lets go */
    char proc[PATH_MAX + 32], fddir[PATH_MAX + 64], link[PATH_MAX + 96];
    snprintf(proc, sizeof proc, "%s/proc", root);
    snprintf(fddir, sizeof fddir, "%s/4242/fd", proc);
    mk(fddir);
    snprintf(link, sizeof link, "%s/3", fddir);
    assert(symlink("/dev/bus/usb/001/007", link) == 0);
    procroot = proc;
    d->busy_due = fake_ms;
    settle();
    assert(!strcmp(state_get(key), "on") && d->busy_due == 0);
    unlink(link);
    assert(symlink("/dev/bus/usb/001/008", link) == 0);               /* another device's node: not ours */
    d->busy_due = fake_ms;
    settle();
    assert(!strcmp(state_get(key), "off"));
    procroot = "/proc";

    /* and it goes with its state */
    event("remove", path, "4a9/1909/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 2 && !state_get(key));
    forget();

    /* a printer that scans */
    event("add", make_device("5-3", "removable", "00", "03f0", "d711", "HP", "ENVY 4520 series", mfp, 2), "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    d = find("/devices/pci/usb1/5-3");
    assert(d && strstr(state_get(strcat(strcpy(dir, "usb."), d->id)), "scanner=yes"));
    forget();
    scanner_known = hwdb_scanner;
}

/* ---- what a status bar is told ---- */

static void test_watch(void) {
    forget();
    const Ifc drive[] = { { "08", "50" } };
    const char *path = make_device("4-1", "removable", "00", "30fa", "0500", NULL, "Watched Drive", drive, 1);
    event("add", path, "30fa/500/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(has_kind("storage", false));
    event("remove", path, "30fa/500/100");
    assert(has_kind("storage", false));                          /* still here until the removal is said */
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(!has_kind("storage", false));

    /* a printer is a USB device like the others, of its kind (CUPS's own state is printer.state) */
    const Ifc printer[] = { { "07", "04" } };
    const char *ppath = make_device("4-2", "removable", "00", "03f0", "d711", "HP", "ENVY", printer, 1);
    event("add", ppath, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(has_kind("printer", false));
    forget();

}

/* ---- what is said, and when ---- */

static void test_announcements(void) {
    forget();
    const Ifc printer[] = { { "07", "04" } };
    const char *path = make_device("2-1", "removable", "00", "03f0", "d711", "HP", "ENVY 4520 series", printer, 1);

    event("add", path, "3f0/d711/100");
    assert(ntoasts == 0);                                     /* its names are not there yet */
    fake_ms += SRC_USB_SETTLE_MS - 1;
    settle();
    assert(ntoasts == 0);
    fake_ms += 1;
    settle();
    assert(ntoasts == 1 && !strcmp(toasts[0], "HP ENVY 4520 series connected") && !strcmp(icons[0], "printer"));
    assert(nsounds == 1 && last_sound == SOUND_DEVICE_ADDED);

    event("remove", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 2 && !strcmp(toasts[1], "HP ENVY 4520 series disconnected") && last_sound == SOUND_DEVICE_REMOVED);
    assert(!find(path));                                      /* the slot is free again */

    /* the device was a key with its name and what it is; plugged again it is another device, and not the same ID */
    assert(nevents == 1 && !strcmp(events[0], "HP ENVY 4520 series\tkind=printer"));
    char first_id[12] = "";
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(find(path) && find(path)->id[0]);
    snprintf(first_id, sizeof first_id, "%s", find(path)->id);
    event("remove", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(strcmp(find(path)->id, first_id) && strlen(find(path)->id) >= 2);   /* an ID that ended is not given again */

    /* the same event twice is one device */
    forget();
    event("add", path, "3f0/d711/100");
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 1);

    /* out and straight back (a resume, a loose cable): nothing to say */
    forget();
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    ntoasts = 0;
    event("remove", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS / 2;
    settle();
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0 && find(path)->state == D_SHOWN);
    event("remove", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 1 && strstr(toasts[0], "disconnected"));  /* and it can still be taken out */

    /* a different device in the same port at once: the first is gone for good, then the new one appears */
    forget();
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    ntoasts = 0;
    event("remove", path, "3f0/d711/100");
    const Ifc drive[] = { { "08", "50" } };
    make_device("2-1", "removable", "00", "0781", "5581", "SanDisk", "Ultra", drive, 1);
    event("add", path, "781/5581/100");
    assert(ntoasts == 1 && !strcmp(toasts[0], "HP ENVY 4520 series disconnected"));
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 2 && !strcmp(toasts[1], "SanDisk Ultra connected") && !strcmp(icons[1], "drive-removable-media-usb,drive-removable-media"));

    /* out again before it was read: it was never announced, so there is nothing to take back */
    forget();
    event("add", path, "3f0/d711/100");
    event("remove", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0 && !find(path));

    /* a name too long for an event is cut to fit, and never in the middle of a character; with no name, none is said */
    forget();
    char long_name[200] = "";
    for (int i = 0; i < 60; i++) strcat(long_name, "\xc3\xa9");        /* 60 times an e with an acute: two bytes each */
    const Ifc stick[] = { { "08", "50" } };
    const char *lpath = make_device("2-3", "removable", "00", "0781", "5581", NULL, long_name, stick, 1);
    event("add", lpath, "781/5581/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(nevents == 1 && strlen(events[0]) < VALUE_LEN);
    const char *name_at = events[0];
    const char *c = name_at;
    for (; *c && strncmp(c, "…", 3); c += 2) assert((unsigned char)c[0] == 0xc3 && (unsigned char)c[1] == 0xa9);   /* whole characters only */
    assert(!strcmp(c, "…\tkind=storage") && c > name_at);               /* an ellipsis where it was cut, and the attributes whole */
    forget();
    const char *npath = make_device("2-4", "removable", "00", "0781", "5582", NULL, NULL, stick, 1);
    event("add", npath, "781/5582/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(nevents == 1 && ntoasts == 1 && !strcmp(toasts[0], "USB device connected") && !strcmp(events[0], "\tkind=storage"));   /* no name: attributes only */

    /* hubs and the like are known, never announced, and leave quietly */
    forget();
    const Ifc hub[] = { { "09", "00" } };
    const char *hpath = make_device("2-2", "removable", "09", "05e3", "0610", NULL, "USB2.0 Hub", hub, 1);
    event("add", hpath, "5e3/610/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 0 && find(hpath)->state == D_QUIET);
    event("remove", hpath, "5e3/610/100");
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0 && !find(hpath));

    /* other events are not ours: interfaces, other subsystems, other actions */
    forget();
    char buf[1024];
    size_t n = uevent(buf, "add", path, "3f0/d711/100", "usb_interface", "usb");
    usb_uevent(buf, n);
    n = uevent(buf, "add", path, "3f0/d711/100", "usb_device", "block");
    usb_uevent(buf, n);
    n = uevent(buf, "change", path, "3f0/d711/100", "usb_device", "usb");
    usb_uevent(buf, n);
    n = uevent(buf, "bind", path, "3f0/d711/100", "usb_device", "usb");
    usb_uevent(buf, n);
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0 && !find(path));

    /* turned off */
    forget();
    off = true;
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0 && !find(path));
    off = false;

    /* a full table drops the newcomer instead of crashing */
    forget();
    for (int i = 0; i < MAX_DEVICES; i++) {
        char p[64];
        snprintf(p, sizeof p, "/devices/pci/usb1/fill-%d", i);
        event("add", p, "1/2/3");
    }
    event("add", path, "3f0/d711/100");
    assert(!find(path));
    forget();
}

/* ---- start-up: what is already there is known, and not announced ---- */

static void test_start_up(void) {
    forget();
    const Ifc drive[] = { { "08", "50" } };
    const char *mpath = make_device("3-1", "removable", "00", "30fa", "0400", NULL, "Fast Drive", drive, 1);
    listener_synced = false;                                  /* what is there at start-up is the snapshot: learned, not said */
    scan();
    listener_synced = true;
    assert(ntoasts == 0 && find(mpath) && find(mpath)->state == D_SHOWN);
    assert(!strcmp(find(mpath)->found.name, "Fast Drive"));

    /* so taking it out is announced, though it was never announced coming in */
    event("remove", mpath, "30fa/400/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 1 && !strcmp(toasts[0], "Fast Drive disconnected") && !strcmp(icons[0], "drive-removable-media-usb-off,drive-removable-media-usb,drive-removable-media"));
    forget();

    /* and the same device is recognised coming back, whichever way the number is written */
    listener_synced = false;
    scan();
    listener_synced = true;
    assert(find(mpath));
    event("remove", mpath, "30fa/400/100");
    fake_ms += SRC_USB_SETTLE_MS / 2;
    event("add", mpath, "30fa/400/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 0 && find(mpath)->state == D_SHOWN);
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/pacreact-usb-XXXXXX");
    assert(mkdtemp(root));
    char *real = realpath(root, NULL);                       /* the tree is found through its real path */
    assert(real && strlen(real) < sizeof root);
    snprintf(root, sizeof root, "%s", real);
    free(real);
    sysroot = root;

    test_names();
    test_classify();
    test_scanners();
    test_watch();
    test_announcements();
    test_start_up();

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
    puts("usb: ok");
    return 0;
}
