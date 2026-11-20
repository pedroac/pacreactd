#include "setting.h"
#include "loop.h"
#include "server.h"
#include "sources.h"
#include "state.h"
#include "util.h"
#include <arpa/inet.h>
#include <netinet/ip_icmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include "tunables.h"

/* Opt-in: probe_host = <IPv4 address> sends one ICMP echo every probe_interval
 * seconds (default 30), and only while the network is up. It needs an unprivileged ping
 * socket (net.ipv4.ping_group_range). Nothing runs, and nothing wakes, without the variable.
 *   net.probe        ok, or slow (answers, but late) or down (no answer), with the level warning, after BAD_TO_ALERT bad probes
 *                    in a row, and ok again after GOOD_TO_CLEAR good ones; said when it changes, not at each probe
 *   net.latency      the milliseconds of the probe that made it slow (said first)
 *   net.probe.host   the address probed */


static char host_name[64] = "probe host";   /* the address, for the message */
static int bad, good;
static bool degraded;

/* Pure decision logic: one probe outcome in, at most one change of what is said out. */
void probe_result(bool ok, int rtt_ms) {
    if (!ok || rtt_ms > SLOW_MS) {
        good = 0;
        if (++bad == BAD_TO_ALERT && !degraded) {
            degraded = true;
            if (ok) {
                char text[16];
                snprintf(text, sizeof text, "%d", rtt_ms);
                bus_set("net.latency", text);
            }
            bus_set_level("net.probe", ok ? "slow" : "down", LEVEL_WARNING);
        }
    } else {
        bad = 0;
        if (degraded && ++good >= GOOD_TO_CLEAR) {
            degraded = false;
            good = 0;
            bus_set("net.probe", "ok");
        }
    }
}

static int sock = -1, timer_fd = -1;
static struct sockaddr_in target;
static int interval_s = SRC_PROBE_DEFAULT_INTERVAL_S;
static bool waiting;
static unsigned short seq;
static long long sent_ms;

static void arm(long ms) {
    struct itimerspec its = { .it_value = { .tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000L } };
    timerfd_settime(timer_fd, 0, &its, NULL);
}

/* Whether the network source sees a default route. */
static bool online(void) {
    const char *v = state_get("net.online");
    return v && !strncmp(v, "on", 2);
}

static void on_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(timer_fd, &n, sizeof n) < 0) return;

    if (waiting) {  /* no reply in time */
        waiting = false;
        probe_result(false, 0);
        arm(interval_s * 1000L);
        return;
    }
    if (!online()) { arm(interval_s * 1000L); return; }

    struct icmphdr echo = { .type = ICMP_ECHO, .code = 0 };
    echo.un.echo.sequence = ++seq;
    if (sendto(sock, &echo, sizeof echo, 0, (struct sockaddr *)&target, sizeof target) < 0) {
        arm(interval_s * 1000L);  /* e.g. network unreachable right now: the net source handles it */
        return;
    }
    waiting = true;
    sent_ms = now_ms();
    arm(TIMEOUT_MS);
}

static void on_reply(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    char buf[128];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
        struct icmphdr *h = (struct icmphdr *)buf;
        if ((size_t)n < sizeof *h || h->type != ICMP_ECHOREPLY || ntohs(h->un.echo.sequence) != seq)
            continue;
        if (!waiting) continue;
        waiting = false;
        probe_result(true, (int)(now_ms() - sent_ms));
        arm(interval_s * 1000L);
    }
}

void probe_init(void) {
    const char *host = pac_setting("probe_host"), *every = pac_setting("probe_interval");
    if (!host || !*host) return;
    target.sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &target.sin_addr) != 1) {
        fprintf(stderr, "pacreactd: probe_host must be an IPv4 address; probe disabled\n");
        return;
    }
    snprintf(host_name, sizeof host_name, "%s", host);
    bus_set("net.probe.host", host_name);
    bus_set("net.probe", "ok");
    interval_s = (int)int_value("probe_interval", every, interval_s, 5, 3600, "seconds");

    sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (sock < 0) {
        perror("pacreactd: probe disabled (ICMP socket; see net.ipv4.ping_group_range)");
        return;
    }
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) { close(sock); return; }
    loop_add(sock, EPOLLIN, on_reply, NULL);
    loop_add(timer_fd, EPOLLIN, on_timer, NULL);
    arm(interval_s * 1000L);
}
