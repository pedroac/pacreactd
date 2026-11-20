#include "setting.h"
#include "loop.h"
#include "sources.h"
#include "tunables.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <systemd/sd-bus.h>

/* The wifi signal from the program that manages the connection, which knows when it moves and says so: NetworkManager if it runs
 * (the strength of the access point the wifi device is on), else iwd (which calls an agent of ours as the signal crosses a
 * level, and whose diagnostics give the exact RSSI). Neither is polled. What they say is handed to the network source, which
 * says it as net.signal; without either, that source reads the kernel's counter when the link changes.
 *   net_signal_source = auto (the default), networkmanager, iwd, or sysfs (neither of them: the counter only)
 * iwd lets only members of the wheel or network group ask it anything; for another user it is as if it were not there. */

#define NM "org.freedesktop.NetworkManager"
#define IWD "net.connman.iwd"
#define AGENT_PATH "/org/pacreactd/signal"

typedef enum { P_NONE, P_NM, P_IWD } Provider;

static sd_bus *bus;
static Provider provider = P_NONE;
static bool want_nm = true, want_iwd = true;
static char iwd_owner[64];          /* the unique name of iwd: the only sender the agent listens to */
static char station[160];           /* the iwd station that is connected, and has our agent */
static sd_bus_slot *agent_slot;

/* ---- the numbers ---- */

/* iwd says dBm. NetworkManager's own measure of the same: -100 or less is 0, -40 or more is 100, a straight line between, so that
 * the signal means the same on a machine with one manager as with the other. */
int wifi_percent_of_dbm(int dbm) {
    if (dbm <= -100) return 0;
    if (dbm >= -40) return 100;
    return (dbm + 100) * 100 / 60;
}

/* iwd calls the agent as the signal crosses one of these (dBm, strongest first): every 3 dB, which is about the step that is said. */
static void agent_levels(int16_t *levels, int *n) {
    *n = 0;
    for (int dbm = -40; dbm >= -94; dbm -= 3) levels[(*n)++] = (int16_t)dbm;
}

/* ---- NetworkManager ---- */

/* The strength, in percent, of the access point of the first wifi device that is on one, or -1. */
static int nm_strength(void) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int best = -1;
    if (sd_bus_call_method(bus, NM, "/org/freedesktop/NetworkManager", NM, "GetAllDevices", &err, &reply, "") < 0) goto done;
    if (sd_bus_message_enter_container(reply, 'a', "o") < 0) goto done;
    const char *device;
    while (sd_bus_message_read(reply, "o", &device) > 0) {
        uint32_t type = 0;
        if (sd_bus_get_property_trivial(bus, NM, device, NM ".Device", "DeviceType", NULL, 'u', &type) < 0 || type != 2) continue;   /* NM_DEVICE_TYPE_WIFI */
        /* The access point is a property of type "o", which the string getter does not take. */
        sd_bus_message *prop = NULL;
        const char *ap = NULL;
        if (sd_bus_get_property(bus, NM, device, NM ".Device.Wireless", "ActiveAccessPoint", NULL, &prop, "o") < 0) continue;
        uint8_t strength = 0;
        if (sd_bus_message_read(prop, "o", &ap) >= 0 && ap && strcmp(ap, "/") &&
            sd_bus_get_property_trivial(bus, NM, ap, NM ".AccessPoint", "Strength", NULL, 'y', &strength) >= 0 && strength <= 100)
            if ((int)strength > best) best = strength;
        sd_bus_message_unref(prop);
    }
done:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return best;
}

/* ---- iwd ---- */

/* The RSSI of the connection of a station, as a percent, or -1. */
static int iwd_percent(const char *path) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int percent = -1;
    if (sd_bus_call_method(bus, IWD, path, IWD ".StationDiagnostic", "GetDiagnostics", &err, &reply, "") < 0) goto done;
    if (sd_bus_message_enter_container(reply, 'a', "{sv}") < 0) goto done;
    while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
        const char *key;
        if (sd_bus_message_read(reply, "s", &key) < 0) goto done;
        if (!strcmp(key, "RSSI")) {
            int16_t dbm;
            if (sd_bus_message_read(reply, "v", "n", &dbm) < 0) goto done;
            percent = wifi_percent_of_dbm(dbm);
        } else if (sd_bus_message_skip(reply, "v") < 0) {
            goto done;
        }
        if (sd_bus_message_exit_container(reply) < 0) goto done;
    }
done:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return percent;
}

/* The path of the first station of iwd that is connected into `path`; false if none is. */
static bool iwd_connected_station(char *path, size_t n) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    bool found = false;
    if (sd_bus_call_method(bus, IWD, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &err, &reply, "") < 0) goto done;
    if (sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}") < 0) goto done;
    while (!found && sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
        const char *object;
        char candidate[160] = "";
        bool is_station = false;
        if (sd_bus_message_read(reply, "o", &object) < 0 || sd_bus_message_enter_container(reply, 'a', "{sa{sv}}") < 0) goto done;
        snprintf(candidate, sizeof candidate, "%s", object);
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char *iface;
            if (sd_bus_message_read(reply, "s", &iface) < 0) goto done;
            if (!strcmp(iface, IWD ".Station")) is_station = true;
            if (sd_bus_message_skip(reply, "a{sv}") < 0 || sd_bus_message_exit_container(reply) < 0) goto done;
        }
        if (sd_bus_message_exit_container(reply) < 0 || sd_bus_message_exit_container(reply) < 0) goto done;
        if (!is_station) continue;
        char *state = NULL;
        if (sd_bus_get_property_string(bus, IWD, candidate, IWD ".Station", "State", NULL, &state) >= 0 && state) {
            if (!strcmp(state, "connected")) { snprintf(path, n, "%s", candidate); found = true; }
            free(state);
        }
    }
done:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    return found;
}

static void agent_forget(bool tell_iwd) {
    if (tell_iwd && station[0])
        sd_bus_call_method(bus, IWD, station, IWD ".Station", "UnregisterSignalLevelAgent", NULL, NULL, "o", AGENT_PATH);
    station[0] = '\0';
}

/* iwd's calls: only iwd itself (root, by its policy, and by its name here) may make them. They only say that the signal moved: how much
 * is read from the diagnostics, so no convention of the level numbers is relied on. */
static void iwd_refresh(void);

static bool from_iwd(sd_bus_message *m) {
    const char *sender = sd_bus_message_get_sender(m);
    return sender && iwd_owner[0] && !strcmp(sender, iwd_owner);
}

static int agent_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)userdata; (void)err;
    if (!from_iwd(m)) return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_ACCESS_DENIED, "only iwd");
    iwd_refresh();
    return sd_bus_reply_method_return(m, "");
}

static int agent_release(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)userdata; (void)err;
    if (!from_iwd(m)) return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_ACCESS_DENIED, "only iwd");
    agent_forget(false);   /* iwd dropped us (it stopped, or the device went) */
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "o", "", agent_release, 0),
    SD_BUS_METHOD("Changed", "oy", "", agent_changed, 0),
    SD_BUS_VTABLE_END
};

/* The state of the connection, from iwd: the signal, and the agent that is told when it moves while connected. */
static void iwd_refresh(void) {
    char path[160];
    if (!iwd_connected_station(path, sizeof path)) {
        agent_forget(false);
        net_signal_external(-1);
        return;
    }
    if (strcmp(path, station)) {                        /* a station we have no agent with: make one */
        agent_forget(true);
        int16_t levels[32];
        int n;
        agent_levels(levels, &n);
        sd_bus_message *m = NULL;
        int r = sd_bus_message_new_method_call(bus, &m, IWD, path, IWD ".Station", "RegisterSignalLevelAgent");
        if (r >= 0) r = sd_bus_message_append(m, "o", AGENT_PATH);
        if (r >= 0) r = sd_bus_message_append_array(m, 'n', levels, (size_t)n * sizeof levels[0]);
        if (r >= 0) r = sd_bus_call(bus, m, 0, NULL, NULL);
        sd_bus_message_unref(m);
        if (r >= 0) snprintf(station, sizeof station, "%s", path);   /* if not (the policy says no), the signal is read this once and no more */
    }
    net_signal_external(iwd_percent(path));
}

/* ---- who is there ---- */

static bool name_owner(const char *name, char *owner, size_t n) {
    sd_bus_message *reply = NULL;
    bool found = false;
    if (sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner", NULL, &reply, "s", name) >= 0) {
        const char *o;
        if (sd_bus_message_read(reply, "s", &o) >= 0) { if (owner) snprintf(owner, n, "%s", o); found = true; }
    }
    sd_bus_message_unref(reply);
    return found;
}

/* Which manager is there now, and what it says. NetworkManager first: it has the whole picture, and works for any user. */
static void choose(void) {
    Provider now = want_nm && name_owner(NM, NULL, 0) ? P_NM : want_iwd && name_owner(IWD, iwd_owner, sizeof iwd_owner) ? P_IWD : P_NONE;
    if (now != P_IWD) iwd_owner[0] = '\0';
    if (provider == P_IWD && now != P_IWD) agent_forget(true);
    provider = now;
    if (provider == P_NM) net_signal_external(nm_strength());
    else if (provider == P_IWD) iwd_refresh();
    else net_signal_external(-1);
}

static int on_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)m; (void)userdata; (void)err;
    choose();
    return 0;
}

static int on_nm_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)m; (void)userdata; (void)err;
    if (provider == P_NM) net_signal_external(nm_strength());
    return 0;
}

static int on_iwd_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)m; (void)userdata; (void)err;
    if (provider == P_IWD) iwd_refresh();
    return 0;
}

static void on_bus(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    int r;
    while ((r = sd_bus_process(bus, NULL)) > 0) {}
    if (r < 0) {   /* the system bus went away */
        loop_remove(sd_bus_get_fd(bus));
        agent_forget(false);
        sd_bus_slot_unref(agent_slot);
        agent_slot = NULL;
        sd_bus_flush_close_unref(bus);
        bus = NULL;
        provider = P_NONE;
        net_signal_external(-1);
    }
}

void wifi_init(void) {
    const char *source = pac_setting("net_signal_source");
    if (source && !strcmp(source, "sysfs")) return;
    want_nm = !source || !strcmp(source, "auto") || !strcmp(source, "networkmanager");
    want_iwd = !source || !strcmp(source, "auto") || !strcmp(source, "iwd");
    if (sd_bus_open_system(&bus) < 0) { bus = NULL; return; }
    if (sd_bus_add_object_vtable(bus, &agent_slot, AGENT_PATH, IWD ".SignalLevelAgent", agent_vtable, NULL) < 0) agent_slot = NULL;

    static const char *const matches[] = {
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" NM "'",
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" IWD "'",
    };
    static const char *const nm_matches[] = {
        "type='signal',sender='" NM "',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='" NM ".AccessPoint'",
        "type='signal',sender='" NM "',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='" NM ".Device.Wireless'",
    };
    static const char *const iwd_matches[] = {
        "type='signal',sender='" IWD "',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',arg0='" IWD ".Station'",
        "type='signal',sender='" IWD "',interface='org.freedesktop.DBus.ObjectManager'",
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof matches / sizeof *matches; i++) ok &= sd_bus_add_match(bus, NULL, matches[i], on_owner_changed, NULL) >= 0;
    if (want_nm) for (size_t i = 0; i < sizeof nm_matches / sizeof *nm_matches; i++) ok &= sd_bus_add_match(bus, NULL, nm_matches[i], on_nm_changed, NULL) >= 0;
    if (want_iwd) for (size_t i = 0; i < sizeof iwd_matches / sizeof *iwd_matches; i++) ok &= sd_bus_add_match(bus, NULL, iwd_matches[i], on_iwd_changed, NULL) >= 0;
    if (!ok) {
        sd_bus_flush_close_unref(bus);
        bus = NULL;
        return;
    }
    loop_add(sd_bus_get_fd(bus), EPOLLIN, on_bus, NULL);
    choose();
    while (sd_bus_process(bus, NULL) > 0) {}
}

/* On the way out: iwd is told it need not call us any more. */
void wifi_shutdown(void) {
    if (bus && provider == P_IWD) agent_forget(true);
}
