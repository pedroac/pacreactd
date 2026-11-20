#include "server.h"
#include "state.h"
#include "tunables.h"
#include <stdio.h>
#include <string.h>

/* Devices: a key NS.ID for the device, NS.ID.PROP for what it has (see server.h). */

#define MAX_ID_NAMESPACES 16

/* An ID is the first letter of the namespace and a number that only grows, so that an ID that ended is not given to another
 * device: whoever still has the old one will never take the new one for it. */
bool bus_new_id(const char *ns, char *id, size_t n) {
    static struct { char ns[NS_LEN]; unsigned next; } counters[MAX_ID_NAMESPACES];
    int i = 0;
    while (i < MAX_ID_NAMESPACES && counters[i].ns[0] && strcmp(counters[i].ns, ns)) i++;
    if (i == MAX_ID_NAMESPACES || !*ns || strlen(ns) >= NS_LEN) return false;   /* a source with more kinds of device than we thought: a bug, not a crash */
    snprintf(counters[i].ns, sizeof counters[i].ns, "%s", ns);
    snprintf(id, n, "%c%u", ns[0], ++counters[i].next);
    return true;
}

bool bus_device(const char *ns, const char *id, const char *name, const char *attrs) {
    char key[KEY_LEN], text[VALUE_LEN * 2], value[VALUE_LEN];
    if (snprintf(key, sizeof key, "%s.%s", ns, id) >= (int)sizeof key) return false;
    snprintf(text, sizeof text, "%s", name ? name : "");
    state_text_clean(text);                                      /* what a device calls itself, or a person called it */
    size_t tail = attrs && *attrs ? strlen(attrs) + 1 : 0;
    if (tail >= sizeof value) return false;                      /* attributes too long for a value: a bug of the source */
    size_t room = sizeof value - 1 - tail;
    state_text_cut(text, room);
    snprintf(value, sizeof value, "%.*s%s%s", (int)room, text, tail ? "\t" : "", tail ? attrs : "");
    return bus_set(key, value);
}

struct under { const char *prefix; size_t n; char keys[STATE_MAX][KEY_LEN]; int count; };

static void collect(const char *key, const char *value, void *ctx) {
    (void)value;
    struct under *u = ctx;
    if (!strncmp(key, u->prefix, u->n) && key[u->n] == '.' && u->count < STATE_MAX) snprintf(u->keys[u->count++], KEY_LEN, "%s", key);
}

/* The properties first, and the device last: a program that drops a device on its going finds nothing left of it. */
void bus_unset_tree(const char *key) {
    struct under u;
    u.prefix = key;
    u.n = strlen(key);
    u.count = 0;
    state_each(collect, &u);
    for (int i = 0; i < u.count; i++) bus_unset(u.keys[i]);
    bus_unset(key);
}
