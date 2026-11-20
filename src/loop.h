#ifndef LOOP_H
#define LOOP_H

#include <stdint.h>

/* The one multiplexer. Sources register their fds and sleep until one fires. */
typedef void (*LoopFn)(void *ctx, uint32_t events);

int loop_init(void);
int loop_add(int fd, uint32_t events, LoopFn fn, void *ctx);
int loop_remove(int fd);
/* Waits up to timeout_ms (-1: forever) and runs the handlers of ready fds.
 * Returns -1 on error. */
int loop_wait(int timeout_ms);

#endif
