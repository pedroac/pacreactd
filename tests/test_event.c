/* Devices: an ID that only grows, a key for the device with its name made to fit, and the whole of it going together. */
#include "../src/server.h"
#include "../src/state.h"
#include "../src/tunables.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The table, as the server keeps it: bus_set and bus_unset on the real state */
static char said[32][VALUE_LEN + 48];
static int n_said;
bool bus_set(const char *key, const char *value) {
    int r = state_set(key, value);
    if (r == 1) snprintf(said[n_said++ % 32], sizeof said[0], "%s=%s", key, value);
    return r >= 0;
}
bool bus_set_level(const char *key, const char *value, Level level) { (void)key; (void)value; (void)level; return true; }
void bus_unset(const char *key) { if (state_unset(key)) snprintf(said[n_said++ % 32], sizeof said[0], "%s gone", key); }
bool bus_event(const char *key, const char *kind, const char *arg) { (void)key; (void)kind; (void)arg; return true; }
static const char *last(void) { return said[(n_said - 1) % 32]; }

int main(void) {
    /* IDs: the first letter and a number that grows, per namespace; never the same twice */
    char a[16], b[16], c[16];
    assert(bus_new_id("usb", a, sizeof a) && !strcmp(a, "u1"));
    assert(bus_new_id("usb", b, sizeof b) && !strcmp(b, "u2"));
    assert(bus_new_id("printer", c, sizeof c) && !strcmp(c, "p1"));
    assert(bus_new_id("usb", c, sizeof c) && !strcmp(c, "u3"));
    assert(state_key_valid("usb.u3") && state_key_valid("usb.u3.volume"));
    for (int i = 0; i < 200; i++) { assert(bus_new_id("usb", c, sizeof c)); assert(state_key_valid("usb.u7") && strlen(c) < NS_LEN); }
    char many[16];
    int ok = 0;
    for (int i = 0; i < 30; i++) { char ns[16]; snprintf(ns, sizeof ns, "n%d", i); ok += bus_new_id(ns, many, sizeof many); }
    assert(ok < 30 && ok >= 10);                                 /* more kinds of device than room: refused, not a crash */

    /* A device: its name cleaned, then attributes; a name that is only a number of attributes */
    assert(bus_device("usb", "u1", "  SanDisk \tUltra\n", "kind=storage") && !strcmp(last(), "usb.u1=SanDisk Ultra\tkind=storage"));
    assert(bus_device("usb", "u2", "", "kind=mouse\tstatus=ok") && !strcmp(last(), "usb.u2=\tkind=mouse\tstatus=ok") && state_value_valid("\tkind=mouse"));
    assert(bus_device("usb", "u3", "Plain", NULL) && !strcmp(last(), "usb.u3=Plain"));
    assert(bus_device("usb", "u4", "level=critical", "kind=other") && !strcmp(last(), "usb.u4=level=critical\tkind=other"));   /* a name is only a name */
    assert(!bus_device("usb", "u5", "", NULL));                   /* nothing at all is no device: the table refuses it */
    char name[300] = "";
    for (int i = 0; i < 100; i++) strcat(name, "\xc3\xa9");
    assert(bus_device("usb", "u6", name, "kind=storage"));
    const char *v = state_get("usb.u6");
    assert(strlen(v) <= VALUE_LEN - 1 && state_value_valid(v) && strstr(v, "\xe2\x80\xa6\tkind=storage"));   /* cut to fit, the attributes whole */

    /* Its properties, and then the whole of it going: the properties first, the device last, nothing else touched */
    bus_set("usb.u1.size", "16");
    bus_set("usb.u1.mounted", "on");
    bus_set("usb.u10.size", "8");                                /* another device, whose ID starts the same */
    bus_set("usb.u10", "Other");
    bus_unset_tree("usb.u1");
    assert(!strcmp(last(), "usb.u1 gone"));
    assert(!state_get("usb.u1") && !state_get("usb.u1.size") && !state_get("usb.u1.mounted"));
    assert(state_get("usb.u10") && state_get("usb.u10.size") && state_get("usb.u2"));
    bus_unset_tree("usb.nothing");                               /* nothing there: nothing */
    puts("event: ok");
    return 0;
}
