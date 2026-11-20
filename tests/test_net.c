/* What the network keys are made of: the link and the signal, from a mock sysfs. src_net.c is included for its statics. */
#include "recorder.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/src_net.c"

static char root[64], wireless[96];

static void put(const char *path, const char *content) {
    char full[512], cmd[600];
    snprintf(full, sizeof full, "%s/%s", root, path);
    snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname %s)\"", full);
    assert(system(cmd) == 0);
    FILE *f = fopen(full, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static void quality(const char *iface, int link) {
    char text[256];
    snprintf(text, sizeof text, "Inter-| sta-|   Quality\n face | tus | link level noise\n%6s: 0000   %d.  -50.  -256.\n", iface, link);
    put("proc-wireless", text);
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/pacreactd-net-XXXXXX");
    assert(mkdtemp(root));
    char net[96];
    snprintf(net, sizeof net, "%s/net", root);
    snprintf(wireless, sizeof wireless, "%s/proc-wireless", root);
    sys_net = net;
    proc_wireless = wireless;
    int signal;

    /* Nothing up: none */
    put("net/lo/operstate", "unknown\n");
    put("net/eth0/operstate", "down\n");
    put("net/wlan0/operstate", "down\n"); put("net/wlan0/wireless/x", "");
    quality("wlan0", 50);
    assert(read_link(&signal) == LINK_NONE && signal == -1);

    /* Wifi up: its link quality out of 70, as a percent */
    put("net/wlan0/operstate", "up\n");
    assert(read_link(&signal) == LINK_WIFI && signal == 71);
    quality("wlan0", 70);
    assert(read_link(&signal) == LINK_WIFI && signal == 100);
    put("proc-wireless", "");                                    /* up but not listed: connected, signal unknown (0) */
    assert(read_link(&signal) == LINK_WIFI && signal == 0);

    /* A cable that is up wins, and has no signal */
    put("net/eth0/operstate", "up\n");
    assert(read_link(&signal) == LINK_WIRED && signal == -1);

    /* Containers, virtual machines, bridges and tunnels are not a connection of the machine's own */
    assert(virtual_interface("veth12ab") && virtual_interface("docker0") && virtual_interface("br-abc") && virtual_interface("virbr0"));
    assert(virtual_interface("podman1") && virtual_interface("cni0") && virtual_interface("vmnet8") && virtual_interface("vboxnet0") && virtual_interface("wg0"));
    assert(!virtual_interface("enp2s0") && !virtual_interface("wlan0") && !virtual_interface("eth0") && !virtual_interface("wlp3s0"));
    /* A container's or a bridge's interface is not a connection */
    put("net/eth0/operstate", "down\n");
    put("net/veth1234/operstate", "up\n"); put("net/docker0/operstate", "up\n"); put("net/br-abc/operstate", "up\n");
    quality("wlan0", 35);
    assert(read_link(&signal) == LINK_WIFI && signal == 50);
    put("net/wlan0/operstate", "down\n");
    assert(read_link(&signal) == LINK_NONE);

    /* The opt-in look at the signal: said only when it has moved by a step, and a link that changed is said whole */
    put("net/wlan0/operstate", "up\n");
    put("net/eth0/operstate", "down\n");
    quality("wlan0", 49);                                        /* 70% */
    signal_interval = 30;
    rec_reset();
    publish_link();
    assert(!strcmp(rec("net.link"), "wifi") && !strcmp(rec("net.signal"), "70") && signal_said == 70);
    quality("wlan0", 47);                                        /* 67%: under the step */
    signal_check();
    assert(!strcmp(rec("net.signal"), "70"));
    quality("wlan0", 45);                                        /* 64%: 6 points from what was said */
    signal_check();
    assert(!strcmp(rec("net.signal"), "64") && signal_said == 64);
    int n = rec_changes("net.signal");
    signal_check();                                              /* nothing moved: nothing said */
    assert(rec_changes("net.signal") == n);
    quality("wlan0", 63);                                        /* 90%: a big move up */
    signal_check();
    assert(!strcmp(rec("net.signal"), "90"));
    put("net/eth0/operstate", "up\n");                           /* a cable: the link changed, and it is said whole */
    signal_check();
    assert(!strcmp(rec("net.link"), "wired") && link_said == LINK_WIRED);
    put("net/eth0/operstate", "down\n");
    put("net/wlan0/operstate", "down\n");
    signal_check();
    assert(!strcmp(rec("net.link"), "none") && signal_said == -1);
    signal_check();                                              /* off wifi: nothing to follow */
    signal_interval = 0;

    /* A network manager tells the signal: it takes the place of the counter, is said when it moves a step, and leaves when it goes */
    put("net/wlan0/operstate", "up\n");
    quality("wlan0", 49);                                        /* the counter says 70 */
    signal_interval = 30;
    external = -1;
    rec_reset();
    publish_link();
    assert(!strcmp(rec("net.signal"), "70"));
    net_signal_external(40);                                     /* it begins: said from the manager now */
    assert(!strcmp(rec("net.signal"), "40") && external == 40);
    net_signal_external(43);                                     /* 3 points: not said */
    assert(!strcmp(rec("net.signal"), "40"));
    net_signal_external(46);                                     /* 6 from what was said */
    assert(!strcmp(rec("net.signal"), "46"));
    quality("wlan0", 7);                                         /* the counter moves meanwhile: not heard while a manager tells */
    signal_check();
    assert(!strcmp(rec("net.signal"), "46"));
    net_signal_external(-1);                                     /* the manager goes: the counter again (10%) */
    assert(!strcmp(rec("net.signal"), "10") && external == -1);
    net_signal_external(-1);                                     /* nothing new: nothing said */
    n = rec_changes("net.signal");
    net_signal_external(-1);
    assert(rec_changes("net.signal") == n);
    put("net/wlan0/operstate", "down\n");
    publish_link();
    external = -1; signal_interval = 0;

    /* No sysfs at all */
    sys_net = "/nonexistent-net";
    assert(read_link(&signal) == LINK_NONE && signal == -1);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
    puts("net: ok");
    return 0;
}
