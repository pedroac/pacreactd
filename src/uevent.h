#ifndef UEVENT_H
#define UEVENT_H

#include <stdbool.h>
#include <stddef.h>

/* The kernel's uevents (NETLINK_KOBJECT_UEVENT), one socket for every source that wants them. */

/* Called for each uevent: its SUBSYSTEM ("" if it has none) and the whole message, which is
 * "action@path\0KEY=VALUE\0KEY=VALUE\0...". */
typedef void (*UeventFn)(const char *subsystem, const char *msg, size_t len);

/* Asks for the uevents; false if there is no room. Call before uevent_init(). */
bool uevent_subscribe(UeventFn fn);
/* Opens the socket, if anyone subscribed. Quietly does nothing where the kernel has none. */
void uevent_init(void);
/* The value of KEY in a message, as a pointer into it, or NULL. */
const char *uevent_value(const char *msg, size_t len, const char *key);

#endif
