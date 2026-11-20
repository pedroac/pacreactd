#include "server.h"
#include "sources.h"
#include "tunables.h"
#include "uevent.h"
#include "util.h"
#include <dirent.h>
#include <limits.h>
#include <linux/input.h>
#include "setting.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The keyboards, mice, touchpads, drawing tablets and gamepads there are, whatever they are plugged into: the kernel's input
 * devices, which it announces as uevents (the same socket as the charger's) with their name, bus and capabilities, so there is
 * nothing to read and nothing to wait for. A program that wants to know whether a keyboard is available asks one question.
 * input = 0 turns this off.
 *   input.ID       a device: its name (maybe none), then kind=keyboard, mouse, touchpad, tablet or gamepad, bus=usb, bluetooth,
 *                  builtin (the machine's own) or other, and mouse=yes on a keyboard that is a pointer too. It is there from its
 *                  being plugged in until it is not; the key goes with whatever is under it.
 * Virtual devices (a program that makes keys, a remapper) are not devices here: nobody plugged them in. */

static const char *sysroot = "/sys";   /* a test points it at a tree of its own */
static bool off;

typedef struct { char path[192]; char id[12]; } Node;
static Node nodes[MAX_INPUT_DEVICES];

/* A capability as the kernel prints it (hex words of a long's width, the most significant first): is this bit set? */
static bool has_bit(const char *caps, unsigned bit) {
    if (!caps) return false;
    unsigned words = 1, per = sizeof(unsigned long) * 8;
    for (const char *p = caps; *p; p++) words += *p == ' ';
    unsigned want = bit / per;                       /* from the least significant word */
    if (want >= words) return false;
    unsigned skip = words - 1 - want;                /* words to pass from the start */
    const char *p = caps;
    while (skip--) {
        p = strchr(p, ' ');
        if (!p) return false;
        p++;
    }
    return (strtoul(p, NULL, 16) >> (bit % per) & 1) != 0;
}

typedef struct {
    char name[96];
    const char *kind, *bus;
    bool also_mouse;
} Found;

/* What a device is, from its bus and capabilities; false for what is no keyboard, pointer or gamepad (a power button, a lid). */
static bool classify(unsigned bustype, const char *key, const char *rel, const char *abs, Found *out) {
    switch (bustype) {
    case BUS_VIRTUAL: return false;
    case BUS_USB: out->bus = "usb"; break;
    case BUS_BLUETOOTH: out->bus = "bluetooth"; break;
    case BUS_I8042: case BUS_HOST: case BUS_I2C: out->bus = "builtin"; break;
    default: out->bus = "other"; break;
    }
    bool pointer_rel = has_bit(rel, REL_X) && has_bit(rel, REL_Y) && has_bit(key, BTN_LEFT);
    bool keyboard = has_bit(key, KEY_A) && has_bit(key, KEY_Z) && has_bit(key, KEY_ENTER);
    bool has_abs = has_bit(abs, ABS_X) && has_bit(abs, ABS_Y);
    out->also_mouse = false;
    if (has_abs && (has_bit(key, BTN_TOOL_PEN) || has_bit(key, BTN_STYLUS))) out->kind = "tablet";
    else if (has_bit(key, BTN_SOUTH) || has_bit(key, BTN_JOYSTICK)) out->kind = "gamepad";
    else if (has_abs && has_bit(key, BTN_TOOL_FINGER)) out->kind = "touchpad";
    else if (keyboard) { out->kind = "keyboard"; out->also_mouse = pointer_rel; }
    else if (pointer_rel) out->kind = "mouse";
    else return false;
    return true;
}

static Node *find(const char *path) {
    for (int i = 0; i < MAX_INPUT_DEVICES; i++)
        if (nodes[i].id[0] && !strcmp(nodes[i].path, path)) return &nodes[i];
    return NULL;
}

static void appear(const char *path, const char *name, unsigned bustype, const char *key, const char *rel, const char *abs) {
    Found f;
    if (find(path) || strlen(path) >= sizeof nodes[0].path || !classify(bustype, key, rel, abs, &f)) return;
    for (int i = 0; i < MAX_INPUT_DEVICES; i++) {
        if (nodes[i].id[0]) continue;
        if (!bus_new_id("input", nodes[i].id, sizeof nodes[i].id)) return;
        snprintf(nodes[i].path, sizeof nodes[i].path, "%s", path);
        char attrs[64];
        snprintf(attrs, sizeof attrs, "kind=%s\tbus=%s%s", f.kind, f.bus, f.also_mouse ? "\tmouse=yes" : "");
        bus_device("input", nodes[i].id, name, attrs);
        return;
    }
}

static void vanish(const char *path) {
    Node *n = find(path);
    if (!n) return;
    char key[32];
    snprintf(key, sizeof key, "input.%s", n->id);
    bus_unset_tree(key);
    memset(n, 0, sizeof *n);
}

/* "NAME=\"Logitech USB Receiver\"": the name without its quotes. */
static void unquote(const char *in, char *out, size_t n) {
    size_t len = strlen(in);
    if (len >= 2 && in[0] == '"' && in[len - 1] == '"') { in++; len -= 2; }
    snprintf(out, n, "%.*s", (int)(len < n - 1 ? len : n - 1), in);
}

/* The kernel's uevents: an input device came or went (its event nodes, which have no capabilities of their own, are passed over). */
static void on_uevent(const char *subsystem, const char *msg, size_t len) {
    if (strcmp(subsystem, "input")) return;
    const char *path = uevent_value(msg, len, "DEVPATH"), *product = uevent_value(msg, len, "PRODUCT");
    if (!path || !product) return;
    if (!strncmp(msg, "remove@", 7)) { vanish(path); return; }
    if (strncmp(msg, "add@", 4)) return;
    const char *name = uevent_value(msg, len, "NAME");
    char text[96];
    unquote(name ? name : "", text, sizeof text);
    appear(path, text, (unsigned)strtoul(product, NULL, 16), uevent_value(msg, len, "KEY"), uevent_value(msg, len, "REL"),
           uevent_value(msg, len, "ABS"));
}

/* What is there at start-up is told as it is: the same devices the uevents would have said. */
static void scan(void) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/class/input", sysroot);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "input", 5)) continue;
        char node[PATH_MAX], real[PATH_MAX], caps[PATH_MAX], name[96] = "", bus[16] = "", key[512] = "", rel[64] = "", abs[64] = "";
        size_t root_len = strlen(sysroot);
        if (snprintf(node, sizeof node, "%s/%s", dir, e->d_name) >= (int)sizeof node) continue;
        if (!realpath(node, real) || strncmp(real, sysroot, root_len)) continue;
        if (snprintf(caps, sizeof caps, "%s/id", real) >= (int)sizeof caps) continue;
        util_read_text(real, "name", name, sizeof name);
        if (!util_read_text(caps, "bustype", bus, sizeof bus)) continue;
        if (snprintf(caps, sizeof caps, "%s/capabilities", real) >= (int)sizeof caps) continue;
        util_read_text(caps, "key", key, sizeof key);
        util_read_text(caps, "rel", rel, sizeof rel);
        util_read_text(caps, "abs", abs, sizeof abs);
        appear(real + root_len, name, (unsigned)strtoul(bus, NULL, 16), key, rel, abs);
    }
    closedir(d);
}

void input_init(void) {
    const char *value = pac_setting("input");
    off = value && !strcmp(value, "0");
    if (off) return;
    scan();
    uevent_subscribe(on_uevent);
}
