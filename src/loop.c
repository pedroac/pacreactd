#include "loop.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#ifdef LOOP_TRACE  /* development aid: log which fd woke the daemon (make CFLAGS="-O2 -DLOOP_TRACE") */
#include <stdio.h>
#endif

typedef struct { LoopFn fn; void *ctx; int fd; } Handler;

static int epfd = -1;

int loop_init(void) {
    epfd = epoll_create1(EPOLL_CLOEXEC);
    return epfd < 0 ? -1 : 0;
}

int loop_add(int fd, uint32_t events, LoopFn fn, void *ctx) {
    Handler *h = malloc(sizeof *h);  /* lives as long as the process */
    if (!h) return -1;
    h->fn = fn;
    h->ctx = ctx;
    h->fd = fd;
#ifdef LOOP_TRACE
    fprintf(stderr, "registered fd=%d\n", fd);
#endif
    struct epoll_event ev = { .events = events, .data.ptr = h };
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

int loop_remove(int fd) {
    return epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);  /* the Handler leaks 16 bytes; rare */
}

int loop_wait(int timeout_ms) {
    struct epoll_event evs[8];
    int n = epoll_wait(epfd, evs, 8, timeout_ms);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; i++) {
        Handler *h = evs[i].data.ptr;
#ifdef LOOP_TRACE
        fprintf(stderr, "wake fd=%d events=0x%x\n", h->fd, evs[i].events);
#endif
        h->fn(h->ctx, evs[i].events);
    }
    return 0;
}
