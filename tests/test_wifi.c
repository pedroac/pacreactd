/* The wifi signal from NetworkManager or iwd, against a private bus and a forked fake that can be a NetworkManager and an iwd, come
 * and go, and move the signal on command. Skipped when dbus-daemon is not installed. src_wifi.c is included for its statics. */
#include "../src/loop.h"
#include "../src/sources.h"
#include <assert.h>
#include "setting.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* What the network source is told */
static int told[64], n_told, last_told = -2;
void net_signal_external(int percent) { if (n_told < 64) told[n_told++] = percent; last_told = percent; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }
int loop_remove(int fd) { (void)fd; return 0; }

#include "../src/src_wifi.c"

#define NM_AP "/org/freedesktop/NetworkManager/AccessPoint/7"
#define NM_DEV "/org/freedesktop/NetworkManager/Devices/9"
#define IWD_STATION "/net/connman/iwd/0/4"

/* ---- the fake (a child process) ---- */

static sd_bus *nm_bus, *iwd_bus, *impostor;
static int nm_strength_now = 62, nm_ap_active = 1;
static int iwd_rssi = -60;
static char iwd_state[16] = "connected";
static char agent_owner_path[128] = "";
static int agent_registered_count, agent_unregistered_count;

/* NetworkManager */
static int nm_get_devices(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    sd_bus_message *reply = NULL;
    assert(sd_bus_message_new_method_return(m, &reply) >= 0);
    assert(sd_bus_message_append(reply, "ao", 2, "/org/freedesktop/NetworkManager/Devices/1", NM_DEV) >= 0);   /* an ethernet card, and the wifi one */
    int r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return r;
}
static const sd_bus_vtable nm_vtable[] = { SD_BUS_VTABLE_START(0), SD_BUS_METHOD("GetAllDevices", "", "ao", nm_get_devices, 0), SD_BUS_VTABLE_END };

static int type_wifi(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)prop; (void)ud; (void)e;
    return sd_bus_message_append(reply, "u", p[strlen(p) - 1] == '9' ? 2u : 1u);
}
static int active_ap(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)prop; (void)ud; (void)e;
    return sd_bus_message_append(reply, "o", nm_ap_active ? NM_AP : "/");
}
static int strength(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)prop; (void)ud; (void)e;
    return sd_bus_message_append(reply, "y", (uint8_t)nm_strength_now);
}
static const sd_bus_vtable dev_vtable[] = { SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("DeviceType", "u", type_wifi, 0, 0), SD_BUS_VTABLE_END };
static const sd_bus_vtable wireless_vtable[] = { SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("ActiveAccessPoint", "o", active_ap, 0, 0), SD_BUS_VTABLE_END };
static const sd_bus_vtable ap_vtable[] = { SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("Strength", "y", strength, 0, 0), SD_BUS_VTABLE_END };

static void properties_changed(sd_bus *b, const char *path, const char *iface) {
    sd_bus_message *m = NULL;
    assert(sd_bus_message_new_signal(b, &m, path, "org.freedesktop.DBus.Properties", "PropertiesChanged") >= 0);
    assert(sd_bus_message_append(m, "s", iface) >= 0);
    assert(sd_bus_message_open_container(m, 'a', "{sv}") >= 0 && sd_bus_message_close_container(m) >= 0);
    assert(sd_bus_message_append(m, "as", 0) >= 0);
    assert(sd_bus_send(b, m, NULL) >= 0);
    sd_bus_message_unref(m);
    sd_bus_flush(b);
}

/* iwd */
static int iwd_register(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    const char *path;
    assert(sd_bus_message_read(m, "o", &path) >= 0);
    snprintf(agent_owner_path, sizeof agent_owner_path, "%s %s", sd_bus_message_get_sender(m), path);
    agent_registered_count++;
    return sd_bus_reply_method_return(m, "");
}
static int iwd_unregister(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    agent_unregistered_count++;
    agent_owner_path[0] = '\0';
    return sd_bus_reply_method_return(m, "");
}
static int iwd_state_get(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *e) {
    (void)b; (void)p; (void)i; (void)prop; (void)ud; (void)e;
    return sd_bus_message_append(reply, "s", iwd_state);
}
static int iwd_diagnostics(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    sd_bus_message *reply = NULL;
    assert(sd_bus_message_new_method_return(m, &reply) >= 0);
    assert(sd_bus_message_open_container(reply, 'a', "{sv}") >= 0);
    assert(sd_bus_message_append(reply, "{sv}", "Frequency", "u", 5180u) >= 0);
    assert(sd_bus_message_append(reply, "{sv}", "RSSI", "n", (int16_t)iwd_rssi) >= 0);
    assert(sd_bus_message_close_container(reply) >= 0);
    int r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return r;
}
static const sd_bus_vtable station_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("State", "s", iwd_state_get, 0, 0),
    SD_BUS_METHOD("RegisterSignalLevelAgent", "oan", "", iwd_register, 0),
    SD_BUS_METHOD("UnregisterSignalLevelAgent", "o", "", iwd_unregister, 0),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable diag_vtable[] = { SD_BUS_VTABLE_START(0), SD_BUS_METHOD("GetDiagnostics", "", "a{sv}", iwd_diagnostics, 0), SD_BUS_VTABLE_END };

/* Calls the agent we were given, as iwd (or, from the impostor's connection, as someone else) */
static void call_agent(sd_bus *from) {
    char owner[64], path[64];
    if (sscanf(agent_owner_path, "%63s %63s", owner, path) != 2) return;
    sd_bus_call_method_async(from, NULL, owner, path, "net.connman.iwd.SignalLevelAgent", "Changed", NULL, NULL, "oy", IWD_STATION, 2);   /* not waited for: iwd has other things to answer meanwhile */
    sd_bus_flush(from);
}

static void iwd_up(void) {
    if (iwd_bus) return;
    assert(sd_bus_open_system(&iwd_bus) >= 0 && sd_bus_request_name(iwd_bus, "net.connman.iwd", 0) >= 0);
    assert(sd_bus_add_object_manager(iwd_bus, NULL, "/") >= 0);
    assert(sd_bus_add_object_vtable(iwd_bus, NULL, IWD_STATION, "net.connman.iwd.Station", station_vtable, NULL) >= 0);
    assert(sd_bus_add_object_vtable(iwd_bus, NULL, IWD_STATION, "net.connman.iwd.StationDiagnostic", diag_vtable, NULL) >= 0);
    assert(sd_bus_emit_object_added(iwd_bus, IWD_STATION) >= 0);
}
static void iwd_down(void) {
    if (!iwd_bus) return;
    sd_bus_flush_close_unref(iwd_bus);
    iwd_bus = NULL;
    agent_owner_path[0] = '\0';
}
static void nm_up(void) {
    if (nm_bus) return;
    assert(sd_bus_open_system(&nm_bus) >= 0 && sd_bus_request_name(nm_bus, NM, 0) >= 0);
    assert(sd_bus_add_object_vtable(nm_bus, NULL, "/org/freedesktop/NetworkManager", NM, nm_vtable, NULL) >= 0);
    assert(sd_bus_add_object_vtable(nm_bus, NULL, NM_DEV, NM ".Device", dev_vtable, NULL) >= 0);
    assert(sd_bus_add_object_vtable(nm_bus, NULL, "/org/freedesktop/NetworkManager/Devices/1", NM ".Device", dev_vtable, NULL) >= 0);
    assert(sd_bus_add_object_vtable(nm_bus, NULL, NM_DEV, NM ".Device.Wireless", wireless_vtable, NULL) >= 0);
    assert(sd_bus_add_object_vtable(nm_bus, NULL, NM_AP, NM ".AccessPoint", ap_vtable, NULL) >= 0);
}
static void nm_down(void) {
    if (!nm_bus) return;
    sd_bus_flush_close_unref(nm_bus);
    nm_bus = NULL;
}

static void fake(int cmd_fd, int ready_fd) {
    if (sd_bus_open_system(&impostor) < 0) _exit(1);
    (void)!write(ready_fd, "ready\n", 6);
    char buf[256];
    size_t have = 0;
    for (;;) {
        struct pollfd p[3] = { { .fd = cmd_fd, .events = POLLIN }, { .fd = nm_bus ? sd_bus_get_fd(nm_bus) : -1, .events = POLLIN },
                               { .fd = iwd_bus ? sd_bus_get_fd(iwd_bus) : -1, .events = POLLIN } };
        if (nm_bus) while (sd_bus_process(nm_bus, NULL) > 0) {}
        if (iwd_bus) while (sd_bus_process(iwd_bus, NULL) > 0) {}
        if (poll(p, 3, 50) <= 0) continue;
        if (p[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(cmd_fd, buf + have, sizeof buf - 1 - have);
            if (n <= 0) _exit(0);
            have += (size_t)n;
            buf[have] = '\0';
            char *nl;
            while ((nl = strchr(buf, '\n'))) {
                *nl = '\0';
                int v;
                if (!strcmp(buf, "nm up")) nm_up();
                else if (!strcmp(buf, "nm down")) nm_down();
                else if (!strcmp(buf, "iwd up")) iwd_up();
                else if (!strcmp(buf, "iwd down")) iwd_down();
                else if (sscanf(buf, "strength %d", &v) == 1) { nm_strength_now = v; properties_changed(nm_bus, NM_AP, NM ".AccessPoint"); }
                else if (!strcmp(buf, "ap none")) { nm_ap_active = 0; properties_changed(nm_bus, NM_DEV, NM ".Device.Wireless"); }
                else if (!strcmp(buf, "ap back")) { nm_ap_active = 1; properties_changed(nm_bus, NM_DEV, NM ".Device.Wireless"); }
                else if (sscanf(buf, "rssi %d", &v) == 1) { iwd_rssi = v; call_agent(iwd_bus); }
                else if (sscanf(buf, "quietrssi %d", &v) == 1) iwd_rssi = v;                  /* changes, and tells no one */
                else if (!strcmp(buf, "impostor")) call_agent(impostor);
                else if (!strcmp(buf, "disconnect")) { snprintf(iwd_state, sizeof iwd_state, "disconnected"); properties_changed(iwd_bus, IWD_STATION, "net.connman.iwd.Station"); }
                else if (!strcmp(buf, "connect")) { snprintf(iwd_state, sizeof iwd_state, "connected"); properties_changed(iwd_bus, IWD_STATION, "net.connman.iwd.Station"); }
                else if (!strcmp(buf, "report")) { char line[160]; snprintf(line, sizeof line, "%d %d %s\n", agent_registered_count, agent_unregistered_count, agent_owner_path[0] ? "agent" : "none"); (void)!write(ready_fd, line, strlen(line)); }
                memmove(buf, nl + 1, have - (size_t)(nl + 1 - buf) + 1);
                have -= (size_t)(nl + 1 - buf);
            }
        }
    }
}

/* ---- driving it ---- */

static int to_fake = -1, from_fake = -1;
static void step(const char *what) { if (getenv("WIFI_TRACE")) fprintf(stderr, "wifi: %s\n", what); }

static void command(const char *cmd) {
    char line[64];
    snprintf(line, sizeof line, "%s\n", cmd);
    assert(write(to_fake, line, strlen(line)) == (ssize_t)strlen(line));
}

/* Processes our bus until the network source was last told `want`, or `ms` pass. */
static bool wait_for(int want, int ms) {
    for (int waited = 0; waited < ms; waited += 25) {
        while (bus && sd_bus_process(bus, NULL) > 0) {}
        if (last_told == want) return true;
        if (bus) sd_bus_wait(bus, 25 * 1000); else usleep(25000);
    }
    return last_told == want;
}

static void settle(void) {                           /* what is in flight arrives */
    for (int i = 0; i < 8; i++) { while (bus && sd_bus_process(bus, NULL) > 0) {} usleep(15000); }
}

static void stop_ours(void) {
    if (!bus) return;
    wifi_shutdown();
    loop_remove(sd_bus_get_fd(bus));
    sd_bus_slot_unref(agent_slot);
    agent_slot = NULL;
    sd_bus_flush_close_unref(bus);
    bus = NULL;
    provider = P_NONE;
    station[0] = '\0';
    iwd_owner[0] = '\0';
}

int main(void) {
    /* The numbers, and where an agent is told */
    assert(wifi_percent_of_dbm(-100) == 0 && wifi_percent_of_dbm(-120) == 0 && wifi_percent_of_dbm(-40) == 100 && wifi_percent_of_dbm(-30) == 100);
    assert(wifi_percent_of_dbm(-70) == 50 && wifi_percent_of_dbm(-60) == 66 && wifi_percent_of_dbm(-85) == 25);
    int16_t levels[32];
    int n;
    agent_levels(levels, &n);
    assert(n > 10 && n <= 32 && levels[0] == -40 && levels[n - 1] >= -100);
    for (int i = 1; i < n; i++) assert(levels[i] < levels[i - 1]);        /* strongest first, as iwd wants them */

    if (system("command -v dbus-daemon >/dev/null 2>&1") != 0) {
        puts("wifi: (skipped: dbus-daemon is not installed)");
        return 0;
    }
    FILE *p = popen("dbus-daemon --session --fork --print-address --print-pid", "r");
    assert(p);
    char address[512] = "", pidline[32] = "";
    assert(fgets(address, sizeof address, p) && fgets(pidline, sizeof pidline, p));
    pclose(p);
    address[strcspn(address, "\n")] = '\0';
    pid_t daemon_pid = (pid_t)atoi(pidline);
    assert(daemon_pid > 1 && address[0]);

    /* A system bus that is not there: quietly nothing */
    setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/bus", 1);
    wifi_init();
    assert(bus == NULL && n_told == 0);
    /* Told to use the counter only: not even the bus */
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);
    pac_setting_set("net_signal_source", "sysfs");
    wifi_init();
    assert(bus == NULL);

    int cmd[2], ready[2];
    assert(pipe(cmd) == 0 && pipe(ready) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) { close(cmd[1]); close(ready[0]); fake(cmd[0], ready[1]); _exit(0); }
    close(cmd[0]); close(ready[1]);
    to_fake = cmd[1]; from_fake = ready[0];
    char line[160];
    assert(read(from_fake, line, sizeof line) > 0);                        /* ready */

    step("Neither is there");
    /* ---- Neither is there: the counter is left to say it ---- */
    pac_setting_set("net_signal_source", "auto");
    wifi_init();
    assert(bus != NULL && provider == P_NONE && last_told == -1);

    step("NetworkManager comes:");
    /* ---- NetworkManager comes: its strength; and it is told of each change, with no timer ---- */
    command("nm up");
    assert(wait_for(62, 3000) && provider == P_NM);
    command("strength 80");
    assert(wait_for(80, 3000));
    command("strength 35");
    assert(wait_for(35, 3000));
    command("ap none");                                                    /* it is on no access point any more */
    assert(wait_for(-1, 3000));
    command("ap back");
    assert(wait_for(35, 3000));

    step("iwd comes too");
    /* ---- iwd comes too: NetworkManager is the one that is asked ---- */
    command("iwd up");
    settle();
    assert(provider == P_NM && !station[0]);
    command("report");
    assert(read(from_fake, line, sizeof line) > 0 && !strncmp(line, "0 0 none", 8));   /* no agent was made with iwd */

    step("NetworkManager goes:");
    /* ---- NetworkManager goes: iwd takes over, with an agent, and the signal as a percent of its RSSI ---- */
    command("nm down");
    assert(wait_for(66, 3000) && provider == P_IWD);                       /* -60 dBm */
    assert(!strcmp(station, IWD_STATION));
    command("report");
    assert(read(from_fake, line, sizeof line) > 0 && !strncmp(line, "1 0 agent", 9));

    step("iwd calls the agent");
    /* iwd calls the agent as the signal moves: the RSSI is read again */
    command("rssi -70");
    assert(wait_for(50, 3000));
    command("rssi -85");
    assert(wait_for(25, 3000));

    step("Not iwd calling the agent");
    /* Not iwd calling the agent: refused, and nothing is read */
    command("quietrssi -50");
    command("impostor");
    settle();
    assert(last_told == 25);
    command("rssi -50");                                                   /* the real iwd: now it is read */
    assert(wait_for(83, 3000));

    step("The connection ends and begins");
    /* The connection ends and begins: no signal, no agent; then an agent again */
    command("disconnect");
    assert(wait_for(-1, 3000) && !station[0]);
    command("connect");
    assert(wait_for(83, 3000) && !strcmp(station, IWD_STATION));
    command("report");
    assert(read(from_fake, line, sizeof line) > 0 && !strncmp(line, "2 ", 2));

    step("NetworkManager comes back");
    /* NetworkManager comes back and is preferred again; iwd is told it need not call any more */
    command("nm up");
    assert(wait_for(35, 3000) && provider == P_NM);
    command("report");
    assert(read(from_fake, line, sizeof line) > 0 && !strncmp(line, "2 1 none", 8));

    step("Both go");
    /* Both go: nothing to tell it */
    command("nm down");
    command("iwd down");
    assert(wait_for(-1, 3000));
    settle();
    assert(provider == P_NONE);

    step("Told to ask only");
    /* Told to ask only iwd, or only NetworkManager */
    stop_ours();
    command("nm up");
    pac_setting_set("net_signal_source", "iwd");
    wifi_init();
    settle();
    assert(provider == P_NONE);                                            /* NetworkManager is there; iwd is not, and was the only one wanted */
    stop_ours();
    command("iwd up");
    pac_setting_set("net_signal_source", "networkmanager");
    wifi_init();
    settle();
    assert(provider == P_NM);                                              /* iwd is there too, and not asked */
    stop_ours();

    close(to_fake);
    waitpid(child, NULL, 0);
    kill(daemon_pid, SIGTERM);
    puts("wifi: ok");
    return 0;
}
