/* A stand-in for the bus and the loop: what a source publishes is kept, to be looked at. */
#include "loop.h"
#include "server.h"
#include "uevent.h"
#include <stdio.h>
#include <string.h>

#define REC_MAX 128
static struct { char key[40], value[80]; int count; } rec_table[REC_MAX];
static int rec_total;   /* every publish that changed something */

static const char *level_text[] = { "", "\tlevel=warning", "\tlevel=critical" };

bool bus_set(const char *key, const char *value) {
    int i = 0;
    while (i < REC_MAX - 1 && rec_table[i].key[0] && strcmp(rec_table[i].key, key)) i++;
    snprintf(rec_table[i].key, sizeof rec_table[i].key, "%s", key);
    if (!strcmp(rec_table[i].value, value)) return true;
    snprintf(rec_table[i].value, sizeof rec_table[i].value, "%s", value);
    rec_table[i].count++;
    rec_total++;
    return true;
}
bool bus_set_level(const char *key, const char *value, Level level) {
    char full[80];
    snprintf(full, sizeof full, "%s%s", value, level_text[level]);
    return bus_set(key, full);
}
void bus_unset(const char *key) {   /* gone: its value is empty, and that is a change */
    for (int i = 0; i < REC_MAX && rec_table[i].key[0]; i++)
        if (!strcmp(rec_table[i].key, key) && rec_table[i].value[0]) { rec_table[i].value[0] = '\0'; rec_table[i].count++; rec_total++; }
}

/* The latest value of a key, or "" if it was never published. */
__attribute__((unused)) static const char *rec(const char *key) {
    for (int i = 0; i < REC_MAX && rec_table[i].key[0]; i++)
        if (!strcmp(rec_table[i].key, key)) return rec_table[i].value;
    return "";
}
/* How many times the key changed. */
__attribute__((unused)) static int rec_changes(const char *key) {
    for (int i = 0; i < REC_MAX && rec_table[i].key[0]; i++)
        if (!strcmp(rec_table[i].key, key)) return rec_table[i].count;
    return 0;
}
__attribute__((unused)) static void rec_reset(void) { memset(rec_table, 0, sizeof rec_table); rec_total = 0; }

__attribute__((unused)) static int registered;
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; registered++; return 0; }
int loop_remove(int fd) { (void)fd; return 0; }
__attribute__((unused)) static UeventFn subscriber;
bool uevent_subscribe(UeventFn fn) { subscriber = fn; return true; }
