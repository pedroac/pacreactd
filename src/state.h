#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stddef.h>

/* The table of keys and values (docs/protocol.md). Fixed size, searched in order: there are few. */

/* A key the protocol allows: lower-case letters, digits and . - _, at least one dot, no empty part. */
bool state_key_valid(const char *key);
/* Text the protocol allows as a value (without its attributes): 1 to VALUE_LEN-1 bytes of valid UTF-8, no control characters
 * (a TAB included, which is what sets an attribute off), and no space at either end. */
bool state_text_valid(const char *text);
/* A whole value as it is sent: that text (which may be left out when there are attributes: a device that has no name), then any
 * number of attributes, each a TAB and name=word (a lower-case name of up to
 * 16 letters, a word of up to 16 of a-z 0-9 and -). The whole is at most VALUE_LEN-1 bytes. */
bool state_value_valid(const char *value);
/* Makes text that a device, a document or a person named fit state_text_valid(): control characters and bytes that are not UTF-8
 * become '?', and spaces at the ends and doubled ones go. Every string a source took from outside goes through this. */
void state_text_clean(char *text);
/* Cuts text to at most max bytes, never inside a character, with an ellipsis where it was cut. */
void state_text_cut(char *text, size_t max);
/* The namespace of a valid key (the part before the first dot) into out, which holds NS_LEN. False
 * if it is too long for that. */
bool state_namespace(const char *key, char *out);

/* Sets a key. 1 if it changed, 0 if it already had that value, -1 if the key or value is not valid
 * or the table is full. */
int state_set(const char *key, const char *value);
/* Removes a key; false if it was not there. */
bool state_unset(const char *key);
/* The value of a key, or NULL. Valid until the next change. */
const char *state_get(const char *key);
/* Calls cb for each key. cb must not change the table. */
void state_each(void (*cb)(const char *key, const char *value, void *ctx), void *ctx);
/* How many keys the namespace has. */
int state_count_namespace(const char *ns);
/* Removes one key of the namespace and copies its name into key (KEY_LEN); false if none is left. */
bool state_take_namespace(const char *ns, char *key);
void state_clear(void);

#endif
