/* The table of keys and values: what is a key, what is a value, set, unset, and namespaces. */
#include "state.h"
#include "tunables.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int seen;
static void count_one(const char *k, const char *v, void *ctx) { (void)k; (void)v; (void)ctx; seen++; }

int main(void) {
    assert(state_key_valid("mem.pressure") && state_key_valid("audio.jack.headset") && state_key_valid("disk.root-1.x_y"));
    assert(!state_key_valid("") && !state_key_valid("mem") && !state_key_valid(".mem") && !state_key_valid("mem."));
    assert(!state_key_valid("mem..x") && !state_key_valid("Mem.x") && !state_key_valid("mem.x y"));
    assert(!state_key_valid("a123456789012345.x"));          /* a namespace of NS_LEN is too long */
    assert(state_key_valid("a12345678901234.x"));
    char big[KEY_LEN + 2];
    memset(big, 'a', sizeof big); big[0] = 'm'; big[1] = '.'; big[sizeof big - 1] = '\0';
    assert(!state_key_valid(big));

    /* A value: text, then attributes, each after a TAB */
    assert(state_value_valid("on") && state_value_valid("87\tlevel=warning") && state_value_valid("caf\xc3\xa9"));
    assert(state_value_valid("40\tcause=device\tlevel=warning") && state_value_valid("a b c"));
    assert(!state_value_valid("") && !state_value_valid("a\nb") && !state_value_valid("a\x1b" "b") && !state_value_valid("a\x7f"));
    assert(state_value_valid("\tkind=mouse") && state_value_valid("\tkind=mouse\tmouse=yes"));   /* a device with no name: attributes only */
    assert(!state_value_valid("\t") && !state_value_valid("\tkind") && !state_value_valid("87\t") && !state_value_valid("87\tlevel") && !state_value_valid("87\tlevel=") && !state_value_valid("87\t=x"));
    assert(!state_value_valid("87\tLevel=warning") && !state_value_valid("87\tlevel=Warning") && !state_value_valid("87\tlevel=a b"));
    assert(!state_value_valid("87\tlevel=warning\t"));
    assert(!state_value_valid("87\tlevel=aaaaaaaaaaaaaaaaa") && !state_value_valid("87\taaaaaaaaaaaaaaaaa=x"));   /* 17 long */
    /* Free text cannot pose as an attribute: a space is not a separator of them, only a TAB is, and a TAB cannot be in text */
    assert(state_value_valid("Foo level=critical") && state_value_valid("Foo cause=jack"));
    assert(!state_text_valid("Foo\tlevel=critical"));
    /* No space at either end, valid UTF-8 only */
    assert(!state_value_valid(" on") && !state_value_valid("on ") && !state_text_valid(" ") && !state_text_valid(""));
    assert(state_text_valid("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"));                 /* 2, 3 and 4 bytes */
    assert(!state_text_valid("\xc3") && !state_text_valid("\x80") && !state_text_valid("a\xff"));  /* cut short, a continuation, not UTF-8 */
    assert(!state_text_valid("\xc0\xaf") && !state_text_valid("\xe0\x80\xaf"));                  /* overlong forms */
    assert(!state_text_valid("\xed\xa0\x80") && !state_text_valid("\xf4\x90\x80\x80"));          /* a surrogate, above U+10FFFF */
    char v[VALUE_LEN + 1];
    memset(v, 'x', sizeof v); v[sizeof v - 1] = '\0';
    assert(!state_value_valid(v));
    v[VALUE_LEN - 1] = '\0';
    assert(state_value_valid(v));
    snprintf(v, sizeof v, "%0*d\tlevel=warning", VALUE_LEN - 15, 0);   /* the attributes count in the length */
    assert(state_value_valid(v));
    snprintf(v, sizeof v, "%0*d\tlevel=warning", VALUE_LEN - 14, 0);
    assert(!state_value_valid(v));

    /* What a device calls itself is made to fit */
    char t[128];
    snprintf(t, sizeof t, "  Wacom \t Intuos\n BT  ");               state_text_clean(t); assert(!strcmp(t, "Wacom Intuos BT"));
    snprintf(t, sizeof t, "evil\x1b[31mname\x7f");                    state_text_clean(t); assert(!strcmp(t, "evil?[31mname?") && state_text_valid(t));
    snprintf(t, sizeof t, "bad \xff\xfe bytes \xc3");               state_text_clean(t); assert(!strcmp(t, "bad ?? bytes ?") && state_text_valid(t));
    snprintf(t, sizeof t, "caf\xc3\xa9");                            state_text_clean(t); assert(!strcmp(t, "caf\xc3\xa9"));   /* good text is kept */
    snprintf(t, sizeof t, " \t\n ");                                  state_text_clean(t); assert(!strcmp(t, ""));
    t[0] = 0;                                                         state_text_clean(t); assert(!strcmp(t, ""));
    snprintf(t, sizeof t, "ok level=critical");                       state_text_clean(t); assert(!strcmp(t, "ok level=critical"));   /* text, not a command */
    /* and cut to fit, whole characters only, with an ellipsis */
    snprintf(t, sizeof t, "short");                                   state_text_cut(t, 10); assert(!strcmp(t, "short"));
    snprintf(t, sizeof t, "0123456789");                              state_text_cut(t, 10); assert(!strcmp(t, "0123456789"));
    snprintf(t, sizeof t, "abcdef");                                  state_text_cut(t, 2); assert(!strcmp(t, "ab"));      /* no room for the ellipsis */
    snprintf(t, sizeof t, "abcdef");                                  state_text_cut(t, 0); assert(!strcmp(t, ""));
    snprintf(t, sizeof t, "\xc3\xa9\xc3\xa9\xc3\xa9");                  state_text_cut(t, 3); assert(!strcmp(t, "\xe2\x80\xa6") || strlen(t) <= 3);
    snprintf(t, sizeof t, "a\xc3\xa9\xc3\xa9");                          state_text_cut(t, 2); assert(!strcmp(t, "a"));       /* not inside a character */
    snprintf(t, sizeof t, "0123456789ab");                            state_text_cut(t, 10); assert(!strcmp(t, "0123456\xe2\x80\xa6"));
    snprintf(t, sizeof t, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");   state_text_cut(t, 8);
    assert(strlen(t) <= 8 && state_text_valid(t) && !strcmp(t + strlen(t) - 3, "\xe2\x80\xa6"));
    snprintf(t, sizeof t, "ab        cd");                           state_text_cut(t, 8);   assert(state_text_valid(t) && strlen(t) <= 8);   /* no space before the ellipsis */

    state_clear();
    assert(state_set("mem.pressure", "on") == 1);
    assert(state_set("mem.pressure", "on") == 0);            /* nothing changed: nothing to tell */
    assert(state_set("mem.pressure", "off") == 1);
    assert(!strcmp(state_get("mem.pressure"), "off"));
    assert(state_get("nope.x") == NULL);
    assert(state_set("bad key", "x") == -1 && state_set("mem.x", "") == -1);

    state_set("mem.other", "1"); state_set("temp.cpu", "50");
    state_each(count_one, NULL);
    assert(seen == 3);
    char key[KEY_LEN];
    assert(state_take_namespace("mem", key) && !strncmp(key, "mem.", 4));
    assert(state_take_namespace("mem", key) && !strncmp(key, "mem.", 4));
    assert(!state_take_namespace("mem", key));
    assert(state_get("temp.cpu"));                           /* "me" is not "mem": only whole namespaces */
    assert(!state_take_namespace("tem", key));
    assert(state_count_namespace("temp") == 1 && state_count_namespace("tem") == 0 && state_count_namespace("nothing") == 0);
    assert(state_unset("temp.cpu") && !state_unset("temp.cpu"));

    for (int i = 0; i < STATE_MAX; i++) {                    /* a full table refuses, and says so */
        char k[KEY_LEN];
        snprintf(k, sizeof k, "fill.k%d", i);
        assert(state_set(k, "1") == 1);
    }
    assert(state_set("fill.more", "1") == -1);
    assert(state_set("fill.k0", "2") == 1);                  /* changing one that is there still works */
    puts("state: ok");
    return 0;
}
