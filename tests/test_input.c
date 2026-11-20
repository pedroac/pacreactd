/* Keyboards, mice, touchpads, tablets and gamepads: what each is (from the bus and capabilities the kernel gives, in a sysfs tree
 * the test builds, and in uevents), and that each comes and goes as a device. src_input.c is included for its statics. */
#include "../src/server.h"
#include "../src/sources.h"
#include "../src/state.h"
#include "../src/tunables.h"
#include "../src/uevent.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool bus_set(const char *key, const char *value) { return state_set(key, value) >= 0; }
bool bus_set_level(const char *key, const char *value, Level level) { (void)key; (void)value; (void)level; return true; }
void bus_unset(const char *key) { state_unset(key); }
bool uevent_subscribe(UeventFn fn) { (void)fn; return true; }
const char *uevent_value(const char *msg, size_t len, const char *key) {
    size_t n = strlen(key);
    for (const char *p = msg + strlen(msg) + 1; p < msg + len; p += strlen(p) + 1)
        if (!strncmp(p, key, n) && p[n] == '=') return p + n + 1;
    return NULL;
}

#include "../src/src_input.c"

/* Capabilities as the kernel prints them: 64-bit words, the highest first. */
#define KBD_KEYS "ffffffffffffffff ffffffffffffffff ffffffffffffffff fffffffffffffffe"   /* every key from 1 up, enter and letters among them */
#define KBD_AND_BUTTONS "1f0000 ffffffffffffffff ffffffffffffffff ffffffffffffffff fffffffffffffffe"   /* the same, and BTN_LEFT */
#define MOUSE_KEYS "1f0000 0 0 0 0"                 /* BTN_LEFT (0x110) .. */
#define PAD_KEYS "ffff000000000000 0 0 0 0"                 /* BTN_SOUTH (0x130) .. */
#define PEN_KEYS "1e01 0 0 0 0 0"                   /* BTN_TOOL_PEN (0x140) .. */
#define TOUCHPAD_KEYS "e420 0 0 0 0 0"              /* BTN_TOOL_FINGER (0x145) .. */

static void test_has_bit(void) {
    assert(has_bit("1e01 0 0 0 0 0", 0x140) && !has_bit("1e01 0 0 0 0 0", 0x141) && has_bit("1e01 0 0 0 0 0", 0x149));
    assert(!has_bit("1e01 0 0 0 0 0", 0x100) && !has_bit("", 0) && !has_bit(NULL, 0) && !has_bit("0", 5));
    assert(has_bit("3", 0) && has_bit("3", 1) && !has_bit("3", 2));
    assert(!has_bit("ff", 64));                                 /* a word that is not there */
}

static void test_classify(void) {
    Found f;
    assert(classify(BUS_USB, KBD_KEYS, "", "", &f) && !strcmp(f.kind, "keyboard") && !strcmp(f.bus, "usb") && !f.also_mouse);
    assert(classify(BUS_USB, KBD_AND_BUTTONS, "903", "", &f) && !strcmp(f.kind, "keyboard") && f.also_mouse);          /* a receiver: one node, both */
    assert(classify(BUS_BLUETOOTH, MOUSE_KEYS, "903", "", &f) && !strcmp(f.kind, "mouse") && !strcmp(f.bus, "bluetooth"));
    assert(classify(BUS_I8042, KBD_KEYS, "", "", &f) && !strcmp(f.bus, "builtin"));
    assert(classify(BUS_I2C, TOUCHPAD_KEYS, "", "3", &f) && !strcmp(f.kind, "touchpad") && !strcmp(f.bus, "builtin"));
    assert(classify(BUS_USB, PEN_KEYS, "", "3", &f) && !strcmp(f.kind, "tablet"));
    assert(classify(BUS_USB, PAD_KEYS, "", "3", &f) && !strcmp(f.kind, "gamepad"));
    assert(classify(BUS_USB, "0", "", "", &f) == false);                                                        /* a power button */
    assert(classify(BUS_VIRTUAL, KBD_KEYS, "", "", &f) == false);                                               /* a remapper: nobody plugged it in */
    assert(classify(BUS_USB, "100000 0 0 0 0", "903", "", &f) == false);                                        /* wheel without buttons */
}

static void test_uevents(void) {
    state_clear();
    memset(nodes, 0, sizeof nodes);
    char msg[512];
    size_t n = (size_t)snprintf(msg, sizeof msg, "add@/devices/x/input/input9%cSUBSYSTEM=input%cPRODUCT=3/46d/c52b/111%cNAME=\"Logitech USB Receiver\"%cKEY=%s%cREL=903%c",
                                0, 0, 0, 0, MOUSE_KEYS, 0, 0);
    on_uevent("input", msg, n);
    /* DEVPATH is not in this message: nothing is made of it */
    assert(!state_get("input.i1"));
    n = (size_t)snprintf(msg, sizeof msg, "add@/devices/x/input/input9%cSUBSYSTEM=input%cDEVPATH=/devices/x/input/input9%cPRODUCT=3/46d/c52b/111%cNAME=\"Logitech USB Receiver\"%cKEY=%s%cREL=903%c",
                         0, 0, 0, 0, 0, MOUSE_KEYS, 0, 0);
    on_uevent("input", msg, n);
    assert(!strcmp(state_get("input.i1"), "Logitech USB Receiver\tkind=mouse\tbus=usb"));
    on_uevent("input", msg, n);                                              /* said twice: still one device */
    assert(!state_get("input.i2"));
    on_uevent("block", msg, n);

    /* an event node, which has no PRODUCT, is not a device */
    n = (size_t)snprintf(msg, sizeof msg, "add@/devices/x/input/input9/event4%cSUBSYSTEM=input%cDEVPATH=/devices/x/input/input9/event4%c", 0, 0, 0);
    on_uevent("input", msg, n);
    assert(!state_get("input.i2"));

    state_set("input.i1.note", "x");
    n = (size_t)snprintf(msg, sizeof msg, "remove@/devices/x/input/input9%cSUBSYSTEM=input%cDEVPATH=/devices/x/input/input9%cPRODUCT=3/46d/c52b/111%c", 0, 0, 0, 0);
    on_uevent("input", msg, n);
    assert(!state_get("input.i1") && !state_get("input.i1.note"));            /* the device goes with what is under it */
    on_uevent("input", msg, n);                                              /* gone twice: nothing */
    n = (size_t)snprintf(msg, sizeof msg, "add@/devices/x/input/input9%cSUBSYSTEM=input%cDEVPATH=/devices/x/input/input9%cPRODUCT=3/46d/c52b/111%cNAME=\"Again\"%cKEY=%s%cREL=903%c",
                         0, 0, 0, 0, 0, MOUSE_KEYS, 0, 0);
    on_uevent("input", msg, n);
    assert(!state_get("input.i1") && state_get("input.i2"));                  /* plugged again: a new ID, never the old one */
}

static void put(const char *dir, const char *attr, const char *value) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, attr);
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "%s\n", value);
    fclose(f);
}

static void test_scan(void) {
    char root[] = "/tmp/pacreactd-input-XXXXXX", cmd[2 * PATH_MAX + 64], dir[PATH_MAX];
    assert(mkdtemp(root));
    static const struct { const char *name, *bus, *key, *rel, *abs; } nodes_here[] = {
        { "AT Translated Set 2 keyboard", "0011", KBD_KEYS, "", "" },
        { "Power Button", "0019", "0", "", "" },
        { "Logitech MX Keys", "0005", KBD_KEYS, "", "" },
        { "ydotoold virtual device", "0006", KBD_KEYS, "", "" },
    };
    for (int i = 0; i < 4; i++) {
        snprintf(dir, sizeof dir, "%s/devices/platform/input/input%d", root, i);
        snprintf(cmd, sizeof cmd, "mkdir -p %s/id %s/capabilities", dir, dir);
        assert(system(cmd) == 0);
        put(dir, "name", nodes_here[i].name);
        snprintf(cmd, sizeof cmd, "%s/id", dir);
        put(cmd, "bustype", nodes_here[i].bus);
        snprintf(cmd, sizeof cmd, "%s/capabilities", dir);
        put(cmd, "key", nodes_here[i].key); put(cmd, "rel", nodes_here[i].rel); put(cmd, "abs", nodes_here[i].abs);
    }
    snprintf(dir, sizeof dir, "%s/class/input", root);
    assert(system((snprintf(cmd, sizeof cmd, "mkdir -p %s", dir), cmd)) == 0);
    for (int i = 0; i < 4; i++) {
        snprintf(cmd, sizeof cmd, "ln -s %s/devices/platform/input/input%d %s/input%d", root, i, dir, i);
        assert(system(cmd) == 0);
    }
    state_clear();
    memset(nodes, 0, sizeof nodes);
    sysroot = root;
    scan();
    int built_in = 0, blue = 0, total = 0;
    for (int i = 1; i < 8; i++) {
        char key[16];
        snprintf(key, sizeof key, "input.i%d", i);
        const char *v = state_get(key);
        if (!v) continue;
        total++;
        built_in += strstr(v, "bus=builtin") != NULL;
        blue += strstr(v, "bus=bluetooth") != NULL && strstr(v, "Logitech MX Keys") == v;
    }
    assert(total == 2 && built_in == 1 && blue == 1);                       /* the keyboards: not the power button, not the virtual one */
    sysroot = "/sys";
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
}

int main(void) {
    test_has_bit();
    test_classify();
    test_uevents();
    test_scan();
    puts("input: ok");
    return 0;
}
