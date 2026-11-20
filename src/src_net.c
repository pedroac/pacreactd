#include "server.h"
#include "setting.h"
#include "sources.h"
#include "tunables.h"
#include "util.h"
#include "loop.h"
#include <dirent.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* The network, from rtnetlink: link and route changes wake us, settle for SRC_NET_DEBOUNCE_MS (flaps and
 * resume bursts), then look once.
 *   net.online   on if there is a default route in the main table, else off
 *   net.device   the interface of that route (gone while offline)
 *   net.link     wired if a cable interface is up, else wifi if a wireless one is, else none
 *   net.signal   percent, the best of the wireless interfaces that are up (gone unless net.link is wifi)
 * The signal comes, best first, from NetworkManager or iwd over the system bus (src_wifi.c), which tell when it moves; failing
 * those, it is read here when the link changes, and no more, since the kernel sends an ordinary program no event when a signal
 * drifts. net_signal_interval = N then asks for a look every N seconds while on wifi (the one timer of this source, off by default
 * and unused while a manager tells). Whichever the source, it is said only when it has moved by SIGNAL_STEP_PCT. */

static const char *sys_net = "/sys/class/net";
static const char *proc_wireless = "/proc/net/wireless";
static int timer_fd = -1;

/* Returns the interface of the main-table default route (IPv4 or IPv6), 1 if it has none that is named, or 0 if none. */
static int default_route_dev(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;

    struct { struct nlmsghdr nh; struct rtmsg rt; } req = {
        .nh = { .nlmsg_len = sizeof req, .nlmsg_type = RTM_GETROUTE, .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP },
        .rt = { .rtm_family = AF_UNSPEC },
    };
    int dev = 0;
    if (send(fd, &req, sizeof req, 0) < 0) { close(fd); return -1; }

    char buf[16384];
    for (bool done = false; !done;) {
        ssize_t len = recv(fd, buf, sizeof buf, 0);
        if (len <= 0) break;
        for (struct nlmsghdr *nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, (size_t)len); nh = NLMSG_NEXT(nh, len)) {
            if (nh->nlmsg_type == NLMSG_DONE || nh->nlmsg_type == NLMSG_ERROR) { done = true; break; }
            if (nh->nlmsg_type != RTM_NEWROUTE) continue;
            struct rtmsg *rt = NLMSG_DATA(nh);
            if (rt->rtm_dst_len != 0 || rt->rtm_table != RT_TABLE_MAIN || rt->rtm_type != RTN_UNICAST) continue;
            int rlen = RTM_PAYLOAD(nh);
            for (struct rtattr *a = RTM_RTA(rt); RTA_OK(a, rlen); a = RTA_NEXT(a, rlen))
                if (a->rta_type == RTA_OIF) dev = *(int *)RTA_DATA(a);
            if (!dev) dev = -2;  /* a default route without an interface (multipath): still online */
        }
    }
    close(fd);
    return dev == -2 ? 1 : dev;
}

/* Link quality (out of 70) of a wireless interface, from /proc/net/wireless; -1 if not listed. */
static int wifi_quality(const char *name) {
    FILE *f = fopen(proc_wireless, "r");
    char line[256];
    int q = -1;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        double link;
        if (!colon) continue;
        *colon = '\0';
        char *iface = line;
        while (*iface == ' ') iface++;
        if (!strcmp(iface, name) && sscanf(colon + 1, "%*x %lf", &link) == 1) q = (int)link;
    }
    fclose(f);
    return q;
}

typedef enum { LINK_NONE, LINK_WIRED, LINK_WIFI } Link;

/* The interfaces that containers, virtual machines and bridges make are not a connection of the machine's own. */
static bool virtual_interface(const char *name) {
    static const char *const prefixes[] = { "veth", "docker", "br-", "virbr", "podman", "cni", "flannel", "vmnet", "vboxnet", "vnet", "lxc", "lxd", "tun", "tap", "wg", "zt" };
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return true;
    return false;
}

/* Connectivity from sysfs: a wired interface that is up wins, else the wifi, else none. Containers' and
 * bridges' interfaces are not a connection. The signal is a percent, or -1 when there is no wifi. */
static Link read_link(int *signal) {
    DIR *d = opendir(sys_net);
    Link link = LINK_NONE;
    *signal = -1;
    if (!d) return LINK_NONE;
    struct dirent *e;
    while ((e = readdir(d))) {
        char path[320], state[16] = "";
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
        snprintf(path, sizeof path, "%s/%.100s/operstate", sys_net, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(state, sizeof state, f)) state[0] = '\0';
        fclose(f);
        if (strncmp(state, "up", 2)) continue;
        snprintf(path, sizeof path, "%s/%.100s/wireless", sys_net, e->d_name);
        if (access(path, F_OK) != 0) {
            if (!virtual_interface(e->d_name)) {
                link = LINK_WIRED;
                break;
            }
            continue;
        }
        int q = wifi_quality(e->d_name);
        int pct = q < 0 ? 0 : q * 100 / 70;
        link = LINK_WIFI;
        if (pct > *signal) *signal = pct;
    }
    closedir(d);
    if (link != LINK_WIFI) *signal = -1;
    return link;
}

static int external = -1;           /* the signal as a network manager says it (src_wifi.c), or -1 */
static Link link_said = LINK_NONE;   /* the link as last said */
static int signal_said = -1;        /* the signal as last said */
static long signal_interval;        /* net_signal_interval: seconds between looks at the signal while on wifi; 0 is never */
static int signal_fd = -1;

/* The link and the signal, as keys. */
static void publish_link(void) {
    int signal;
    Link link = read_link(&signal);
    if (link == LINK_WIFI && external >= 0) signal = external;   /* a manager knows better than a kernel counter read once */
    link_said = link;
    bus_set("net.link", link == LINK_WIRED ? "wired" : link == LINK_WIFI ? "wifi" : "none");
    signal_said = signal;
    if (signal >= 0) {
        char v[16];
        snprintf(v, sizeof v, "%d", signal);
        bus_set("net.signal", v);
    } else {
        bus_unset("net.signal");
    }
    /* The timer runs only while there is a wifi signal to follow */
    long every = link == LINK_WIFI && external < 0 ? signal_interval : 0;
    struct itimerspec its = { .it_value = { .tv_sec = every }, .it_interval = { .tv_sec = every } };
    if (signal_fd >= 0) timerfd_settime(signal_fd, 0, &its, NULL);
}

static void publish(void) {
    int dev = default_route_dev();
    if (dev >= 0) {
        char name[IF_NAMESIZE] = "";
        if (dev > 1 && if_indextoname((unsigned)dev, name)) bus_set("net.device", name);
        else bus_unset("net.device");   /* offline, or a route that does not say its interface */
        bus_set("net.online", dev > 0 ? "on" : "off");
    }
    publish_link();
}

/* A look at the signal (net_signal_interval): said when it has moved by SIGNAL_STEP_PCT or more since it was, so that a bar is not woken
 * for each point it drifts; a link that changed meanwhile is said whole. */
static void say_signal(int signal) {
    int moved = signal > signal_said ? signal - signal_said : signal_said - signal;
    if (signal_said >= 0 && moved < SIGNAL_STEP_PCT) return;
    char v[16];
    snprintf(v, sizeof v, "%d", signal);
    signal_said = signal;
    bus_set("net.signal", v);
}

static void signal_check(void) {
    int signal;
    Link link = read_link(&signal);
    if (link != link_said) { publish_link(); return; }
    if (link != LINK_WIFI || signal < 0) return;
    if (external >= 0) return;               /* a manager is telling it */
    say_signal(signal);
}

/* The signal as NetworkManager or iwd say it (src_wifi.c): percent, or -1 when they have none (no manager, not connected). It
 * takes the place of what the kernel's counter says, and is said when it has moved by a step. */
void net_signal_external(int percent) {
    bool changed_source = (external >= 0) != (percent >= 0);
    external = percent;
    if (changed_source) { publish_link(); return; }       /* the timer for the counter starts or stops; the signal is said from the right place */
    if (percent >= 0 && link_said == LINK_WIFI) say_signal(percent);
}

static void on_signal_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(signal_fd, &n, sizeof n) > 0) signal_check();
}

static void on_event(void *ctx, uint32_t ev) {
    (void)ev;
    char buf[8192];
    while (recv(*(int *)ctx, buf, sizeof buf, MSG_DONTWAIT) > 0) {}
    struct itimerspec its = { .it_value = { .tv_sec = SRC_NET_DEBOUNCE_MS / 1000, .tv_nsec = (SRC_NET_DEBOUNCE_MS % 1000) * 1000000L } };
    timerfd_settime(timer_fd, 0, &its, NULL);  /* re-arming restarts the debounce */
}

static void on_settled(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(timer_fd, &n, sizeof n) > 0) publish();
}

void net_init(void) {
    static int fd;
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (fd < 0) return;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE };
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return; }
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) { close(fd); return; }
    signal_interval = setting_int("net_signal_interval", 0, 0, 3600, "seconds");
    if (signal_interval > 0 && signal_interval < 5) signal_interval = 5;
    if (signal_interval > 0) {
        signal_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (signal_fd >= 0) loop_add(signal_fd, EPOLLIN, on_signal_timer, NULL);
    }
    publish();
    loop_add(fd, EPOLLIN, on_event, &fd);
    loop_add(timer_fd, EPOLLIN, on_settled, NULL);
}
