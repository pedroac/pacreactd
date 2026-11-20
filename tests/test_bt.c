/* Bluetooth connect/disconnect: against a private dbus-daemon, with a forked fake BlueZ that owns
 * org.bluez, answers Alias reads and emits real PropertiesChanged signals on command. Skipped when
 * dbus-daemon is not installed. src_bt.c is included for its statics. */
#include "../src/loop.h"
#include "../src/server.h"
#include "../src/tunables.h"
#include <assert.h>
#include "setting.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* `toasts`, `toast_icon` and `last_sound` are what a listener that announces Bluetooth devices (pacreact) makes of them: a key bt.ID that
 * appears is a device that connected, and one that goes is one that disconnected, called by the name it had. */
enum { SOUND_DEVICE_ADDED, SOUND_DEVICE_REMOVED, SOUND_KINDS };
enum { LEVEL_INFO };
static char toasts[16][160];
static int ntoasts, removed, registered;
static int last_level = LEVEL_INFO;
static int last_sound = SOUND_KINDS;
static char toast_icon[96];
static bool listener_synced = true;
static struct { char key[24], name[96], value[VALUE_LEN]; } listed[16];

static void said(bool connected, const char *name) {
    snprintf(toasts[ntoasts++ % 16], sizeof toasts[0], connected ? "Bluetooth: %s connected" : "Bluetooth: %s disconnected", *name ? name : "?");
    snprintf(toast_icon, sizeof toast_icon, "%s", connected ? "bluetooth-active,bluetooth" : "bluetooth-disconnected,bluetooth-disabled,bluetooth");
    last_sound = connected ? SOUND_DEVICE_ADDED : SOUND_DEVICE_REMOVED;
}

bool bus_set(const char *key, const char *value) {
    assert(!strncmp(key, "bt.", 3) && !strchr(key + 3, '.'));       /* a device, and nothing else is said */
    int i = 0;
    while (i < 16 && listed[i].key[0] && strcmp(listed[i].key, key)) i++;
    assert(i < 16);
    bool known = listed[i].key[0] != '\0';
    snprintf(listed[i].key, sizeof listed[i].key, "%s", key);
    snprintf(listed[i].value, sizeof listed[i].value, "%s", value);
    snprintf(listed[i].name, sizeof listed[i].name, "%.*s", (int)strcspn(value, "\t"), value);
    if (!known && listener_synced) said(true, listed[i].name);
    return true;
}
bool bus_set_level(const char *key, const char *value, Level level) { (void)key; (void)value; (void)level; return true; }
void bus_unset(const char *key) {
    for (int i = 0; i < 16; i++)
        if (listed[i].key[0] && !strcmp(listed[i].key, key)) { said(false, listed[i].name); memset(&listed[i], 0, sizeof listed[i]); }
}
/* What is said of the device that is there now, or "" */
static const char *device_value(void) {
    for (int i = 0; i < 16; i++) if (listed[i].key[0]) return listed[i].value;
    return "";
}
static int devices_there(void) { int n = 0; for (int i = 0; i < 16; i++) n += listed[i].key[0] != '\0'; return n; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; registered++; return 0; }
int loop_remove(int fd) { (void)fd; removed++; return 0; }

#include "../src/src_bt.c"

#define DEVICE "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"

/* ---- the fake BlueZ (a child process) ---- */

static char fake_alias[256] = "WH-1000XM4";

static int get_alias(sd_bus *b, const char *path, const char *iface, const char *prop,
                     sd_bus_message *reply, void *ud, sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    return sd_bus_message_append(reply, "s", fake_alias);
}

static int fake_connected;
static int get_connected(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    return sd_bus_message_append(reply, "b", fake_connected);
}
static int get_icon(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    return sd_bus_message_append(reply, "s", "audio-headset");
}
static int get_address(sd_bus *b, const char *path, const char *iface, const char *prop, sd_bus_message *reply, void *ud, sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    return sd_bus_message_append(reply, "s", "AA:BB:CC:DD:EE:FF");
}

static const sd_bus_vtable device_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Alias", "s", get_alias, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Icon", "s", get_icon, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Address", "s", get_address, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Connected", "b", get_connected, 0, 0),
    SD_BUS_VTABLE_END
};

/* PropertiesChanged on `path` for `iface`, with Connected (if >= 0) and an unrelated property. */
static void emit(sd_bus *b, const char *path, const char *iface, int connection, bool with_other) {
    sd_bus_message *m = NULL;
    assert(sd_bus_message_new_signal(b, &m, path, "org.freedesktop.DBus.Properties", "PropertiesChanged") >= 0);
    assert(sd_bus_message_append(m, "s", iface) >= 0);
    assert(sd_bus_message_open_container(m, 'a', "{sv}") >= 0);
    if (with_other) assert(sd_bus_message_append(m, "{sv}", "RSSI", "n", (int16_t)-60) >= 0);
    if (connection >= 0) assert(sd_bus_message_append(m, "{sv}", "Connected", "b", connection) >= 0);
    assert(sd_bus_message_close_container(m) >= 0);
    assert(sd_bus_message_append(m, "as", 0) >= 0);
    assert(sd_bus_send(b, m, NULL) >= 0);
    sd_bus_message_unref(m);
    sd_bus_flush(b);
}

static void fake_bluez(int cmd_fd, int ready_fd) {
    sd_bus *b = NULL, *impostor = NULL;
    if (sd_bus_open_system(&b) < 0 || sd_bus_request_name(b, "org.bluez", 0) < 0) _exit(1);
    if (sd_bus_add_object_vtable(b, NULL, DEVICE, "org.bluez.Device1", device_vtable, NULL) < 0) _exit(1);
    if (sd_bus_add_object_manager(b, NULL, "/") < 0) _exit(1);
    if (sd_bus_open_system(&impostor) < 0) _exit(1);   /* a connection that does not own org.bluez */
    (void)!write(ready_fd, "ready\n", 6);

    char buf[256];
    size_t have = 0;
    for (;;) {
        struct pollfd p[2] = { { .fd = sd_bus_get_fd(b), .events = POLLIN }, { .fd = cmd_fd, .events = POLLIN } };
        sd_bus_process(b, NULL);
        if (poll(p, 2, 100) <= 0) continue;
        if (p[1].revents & POLLIN) {
            ssize_t n = read(cmd_fd, buf + have, sizeof buf - 1 - have);
            if (n <= 0) _exit(0);
            have += n;
            buf[have] = '\0';
            char *nl;
            while ((nl = strchr(buf, '\n'))) {
                *nl = '\0';
                if (!strcmp(buf, "connect")) { fake_connected = 1; emit(b, DEVICE, "org.bluez.Device1", 1, false); }
                else if (!strcmp(buf, "disconnect")) { fake_connected = 0; emit(b, DEVICE, "org.bluez.Device1", 0, false); }
                else if (!strcmp(buf, "both")) { fake_connected = 1; emit(b, DEVICE, "org.bluez.Device1", 1, true); }
                else if (!strcmp(buf, "remove")) { sd_bus_emit_object_removed(b, DEVICE); }
                else if (!strcmp(buf, "ghost")) { fake_connected = 1; }
                else if (!strcmp(buf, "rssi")) emit(b, DEVICE, "org.bluez.Device1", -1, true);
                else if (!strcmp(buf, "adapter")) emit(b, "/org/bluez/hci0", "org.bluez.Adapter1", 1, false);
                else if (!strcmp(buf, "unknown")) emit(b, "/org/bluez/hci0/dev_NO_SUCH", "org.bluez.Device1", 1, false);
                else if (!strcmp(buf, "spoof")) emit(impostor, DEVICE, "org.bluez.Device1", 1, false);
                else if (!strncmp(buf, "alias ", 6)) snprintf(fake_alias, sizeof fake_alias, "%s", buf + 6);
                memmove(buf, nl + 1, have - (size_t)(nl + 1 - buf) + 1);
                have -= (size_t)(nl + 1 - buf);
            }
        }
        sd_bus_process(b, NULL);
    }
}

/* ---- driving it ---- */

static int to_fake = -1;

static void command(const char *cmd) {
    char line[64];
    snprintf(line, sizeof line, "%s\n", cmd);
    assert(write(to_fake, line, strlen(line)) == (ssize_t)strlen(line));
}

/* Processes our bus for up to `ms`; returns whether a toast arrived. */
static bool wait_for_toast(int before, int ms) {
    for (int waited = 0; waited < ms; waited += 50) {
        while (sd_bus_process(bus, NULL) > 0) {}
        if (ntoasts > before) return true;
        sd_bus_wait(bus, 50 * 1000);
    }
    return false;
}

int main(void) {
    if (system("command -v dbus-daemon >/dev/null 2>&1") != 0) {
        puts("bt: (skipped: dbus-daemon is not installed)");
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
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);     /* the "system" bus is the private one */

    /* a system bus that is not there: quietly nothing */
    setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/bus", 1);
    registered = 0;
    bt_init();
    assert(bus == NULL && registered == 0);

    pac_setting_set("bluetooth", "0");
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);
    bt_init();
    assert(bus == NULL && registered == 0);
    pac_setting_set("bluetooth", NULL);

    int cmd[2], ready[2];
    assert(pipe(cmd) == 0 && pipe(ready) == 0);
    pid_t fake = fork();
    assert(fake >= 0);
    if (fake == 0) {
        close(cmd[1]);
        close(ready[0]);
        fake_bluez(cmd[0], ready[1]);
    }
    close(cmd[0]);
    close(ready[1]);
    to_fake = cmd[1];
    char r[8];
    assert(read(ready[0], r, 6) == 6);                  /* the fake owns org.bluez now */

    /* A device that is connected before we look: it is there at the start, and nothing is said of it */
    command("ghost");
    listener_synced = false;
    bt_init();
    listener_synced = true;
    assert(bus != NULL && registered == 1);
    assert(ntoasts == 0 && devices_there() == 1);
    assert(!strcmp(device_value(), "WH-1000XM4\tkind=headset\tuid=aa-bb-cc-dd-ee-ff"));
    int before = ntoasts;
    command("disconnect");
    assert(wait_for_toast(before, 3000) && devices_there() == 0 && !strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: WH-1000XM4 disconnected"));

    /* connecting and disconnecting, with the name BlueZ gives the device */
    before = ntoasts;
    command("connect");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: WH-1000XM4 connected") && last_level == LEVEL_INFO);
    assert(last_sound == SOUND_DEVICE_ADDED);
    assert(!strncmp(toast_icon, "bluetooth-active", 16));
    before = ntoasts;
    command("disconnect");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: WH-1000XM4 disconnected"));
    assert(last_sound == SOUND_DEVICE_REMOVED);
    assert(!strncmp(toast_icon, "bluetooth-disconnected", 22));

    /* Connected together with other changed properties still counts */
    before = ntoasts;
    command("both");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: WH-1000XM4 connected"));

    /* what it was called when it connected is what it is called when it goes; the next time it connects, the name is read again */
    command("alias Kitchen speaker");
    before = ntoasts;
    command("disconnect");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: WH-1000XM4 disconnected"));
    before = ntoasts;
    command("connect");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: Kitchen speaker connected"));
    before = ntoasts;
    command("remove");                                   /* unpaired while connected: gone */
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: Kitchen speaker disconnected") && devices_there() == 0);

    /* a device BlueZ cannot name still gets a message */
    before = ntoasts;
    command("unknown");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: ? connected"));
    assert(!strcmp(device_value(), "\tkind=other"));                    /* no name, no icon, no address: what there is */

    /* things that are not a connection: other properties, other interfaces, other senders */
    before = ntoasts;
    command("rssi");
    command("adapter");
    command("spoof");
    assert(!wait_for_toast(before, 800));
    assert(ntoasts == before);

    /* and it is still listening afterwards */
    command("connect");
    assert(wait_for_toast(before, 3000));

    /* the system bus going away: the connection is released */
    kill(fake, SIGKILL);
    waitpid(fake, NULL, 0);
    kill(daemon_pid, SIGTERM);
    for (int i = 0; i < 40 && bus; i++) {
        removed = 0;
        on_bus(NULL, 0);
        if (bus) usleep(50000);
    }
    assert(bus == NULL && removed == 1);

    close(cmd[1]);
    close(ready[0]);
    puts("bt: ok");
    return 0;
}
