#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <stddef.h>

/* The clients of the daemon and the bus the sources publish on (docs/protocol.md). */

void server_init(void);
/* Accepts the clients waiting on the listening socket; for the loop. */
void server_accept(int listen_fd);
/* Adds an already connected client socket (the listening socket's accept does this; so do the tests). */
void server_add_client(int fd);

/* What the daemon's own sources call. A key's value changes: subscribers are told, once, and only if
 * it is different. `value` is a text and then, each after a TAB, attributes (see state_value_valid()). False, and said once on
 * standard error, if the key or value is not valid or the table is full. */
bool bus_set(const char *key, const char *value);
/* The severity of a value: it is sent as the attribute level=warning or level=critical. */
typedef enum { LEVEL_NONE, LEVEL_WARNING, LEVEL_CRITICAL } Level;
/* bus_set() of `value` carrying `level`. */
bool bus_set_level(const char *key, const char *value, Level level);
/* Something happened once: told to those who listen to the key now, and kept nowhere (a program that connects later is not told, and
 * a snapshot has none). The KIND is a word, and the ARG a name that a device, a document or a person chose: cleaned, and cut to fit
 * with an ellipsis. ARG may be NULL or empty. A thing that is, and was and will be (a device, a condition) is a key, not an event. */
bool bus_event(const char *key, const char *kind, const char *arg);

/* ---- devices (event.c) ----
 * A device is the key NS.ID, whose value is its name (cleaned, cut to fit) and then attributes: kind=headset, scanner=yes... Its
 * properties are the keys NS.ID.PROP. It connects by the key's appearing, and ends by its going: that takes every key under it, and
 * the ID is never given again by this daemon. An ID is the namespace's first letter and a number that only grows: u7. */
bool bus_new_id(const char *ns, char *id, size_t n);
/* Sets NS.ID. `attrs` are "kind=storage" or several separated by a TAB, or NULL. */
bool bus_device(const char *ns, const char *id, const char *name, const char *attrs);
/* The key and everything under it is gone (a device that ended). */
void bus_unset_tree(const char *key);
/* The key is gone: subscribers are told. */
void bus_unset(const char *key);

#endif
