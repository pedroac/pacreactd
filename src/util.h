#ifndef UTIL_H
#define UTIL_H

/* A whole number from a setting, strictly: all of VALUE must be digits (with a sign, and leading
 * space forgiven), or it is not a number. Unset or empty gives `def`, quietly. Anything else wrong is
 * said on stderr, by NAME, and then: not a number gives `def`; a number outside lo..hi is brought to
 * the nearest end of it. `unit` ("seconds") is for the message. */
long int_value(const char *name, const char *value, long def, long lo, long hi, const char *unit);

/* int_value() for the setting NAME (a key of the settings file) */
long setting_int(const char *name, long def, long lo, long hi, const char *unit);

#include <stdbool.h>
#include <stddef.h>

/* The text of the file ATTR in directory DIR, one line, without the newline and the padding some devices put after it. False if it is
 * not there or empty. */
bool util_read_text(const char *dir, const char *attr, char *out, size_t n);

/* Which of the nodes (names relative to /dev, up to 15 characters; at most 32) does a process other than `skip` of the tree under proc_root
 * hold open: bit i is set for names[i]. One walk of the processes answers for all of them. A program that has a camera or a scanner
 * open is using it. */
unsigned dev_in_use(const char *proc_root, int n, char names[][16], int skip);

#include <time.h>

/* The monotonic clock in milliseconds. Inline, and calling clock_gettime directly, so that a test that defines that name
 * as a fake before including a source file gets its clock. */
static inline long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

#endif
