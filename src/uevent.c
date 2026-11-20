#include "uevent.h"
#include "loop.h"
#include <linux/netlink.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_UEVENT_SUBSCRIBERS 4

static UeventFn subscribers[MAX_UEVENT_SUBSCRIBERS];
static int n_subscribers;

bool uevent_subscribe(UeventFn fn) {
    if (n_subscribers == MAX_UEVENT_SUBSCRIBERS) return false;
    subscribers[n_subscribers++] = fn;
    return true;
}

const char *uevent_value(const char *msg, size_t len, const char *key) {
    size_t n = strlen(key);
    for (const char *p = msg + strlen(msg) + 1; p < msg + len; p += strlen(p) + 1)
        if (!strncmp(p, key, n) && p[n] == '=') return p + n + 1;
    return NULL;
}

static void on_uevent(void *ctx, uint32_t ev) {
    (void)ev;
    char buf[8192];
    ssize_t n;
    struct sockaddr_nl from;
    socklen_t from_len;
    for (;;) {
        from_len = sizeof from;
        n = recvfrom(*(int *)ctx, buf, sizeof buf - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &from_len);
        if (n <= 0) break;
        if (from_len < sizeof from || from.nl_pid != 0) continue;   /* only the kernel's: a program can send to this socket too, and would be believed */
        buf[n] = '\0';
        const char *subsystem = uevent_value(buf, (size_t)n, "SUBSYSTEM");
        for (int i = 0; i < n_subscribers; i++) subscribers[i](subsystem ? subsystem : "", buf, (size_t)n);
    }
}

void uevent_init(void) {
    static int fd;
    if (!n_subscribers) return;
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };  /* kernel events */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return; }
    loop_add(fd, EPOLLIN, on_uevent, &fd);
}
