#include "setting.h"
#include "loop.h"
#include "server.h"
#include "sources.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <systemd/sd-bus.h>
#include "tunables.h"

/* Bluetooth devices connecting and disconnecting. The kernel offers nothing unprivileged for
 * this, so it listens to BlueZ's PropertiesChanged signals on the system bus (the bus
 * connection is one more fd in the loop: nothing to poll). It only works while bluetoothd runs;
 * if it starts later the signals simply begin. bluetooth = 0 turns it off.
 *   bt.ID      a device that is connected: its name, then kind=headset, headphones, speaker, keyboard, mouse, gamepad, phone,
 *              computer or other (from what BlueZ says its icon is), and uid=, its address (aa-bb-cc-dd-ee-ff), which is
 *              the same each time it connects while the ID is not. It is there from its connecting until its disconnecting. */


static sd_bus *bus;

/* A string property of a device from BlueZ; the caller frees it. */
static char *device_string(const char *path, const char *property) {
    char *value = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_get_property_string(bus, "org.bluez", path, "org.bluez.Device1", property, &err, &value) < 0) value = NULL;
    sd_bus_error_free(&err);
    return value;
}

/* The device's name from BlueZ: what the person called it (the alias), else the one it has; the caller frees it. */
static char *device_alias(const char *path) {
    char *alias = device_string(path, "Alias");
    return alias ? alias : device_string(path, "Name");
}

/* The same alias lookup, replaceable so the name can be tested without a bus. */
static char *(*alias_lookup)(const char *path) = device_alias;

/* What a person would call it, from the icon BlueZ gives it. */
static const char *kind_of(const char *icon) {
    static const struct { const char *icon, *kind; } kinds[] = {
        { "audio-headset", "headset" }, { "audio-headphones", "headphones" }, { "audio-card", "speaker" }, { "audio-speakers", "speaker" },
        { "input-keyboard", "keyboard" }, { "input-mouse", "mouse" }, { "input-gaming", "gamepad" }, { "input-tablet", "other" },
        { "phone", "phone" }, { "computer", "computer" },
    };
    for (size_t i = 0; icon && i < sizeof kinds / sizeof *kinds; i++)
        if (!strcmp(icon, kinds[i].icon)) return kinds[i].kind;
    return "other";
}

/* The devices that are connected, with the ID of the key each has. */
static struct { char path[128]; char id[12]; } connected[MAX_BT_DEVICES];

/* An address "AA:BB:CC:DD:EE:FF" as a word: aa-bb-cc-dd-ee-ff. */
static void address_word(const char *address, char *out, size_t n) {
    size_t o = 0;
    for (; address && *address && o < n - 1; address++) out[o++] = *address == ':' ? '-' : (*address >= 'A' && *address <= 'F') ? (char)(*address + 32) : *address;
    out[o] = '\0';
}

static void appear(const char *path) {
    int slot = -1;
    for (int i = 0; i < MAX_BT_DEVICES; i++) {
        if (!strcmp(connected[i].path, path)) return;           /* known: said twice */
        if (!connected[i].path[0] && slot < 0) slot = i;
    }
    if (slot < 0 || strlen(path) >= sizeof connected[0].path || !bus_new_id("bt", connected[slot].id, sizeof connected[slot].id)) return;
    char *name = alias_lookup(path), *icon = device_string(path, "Icon"), *address = device_string(path, "Address");
    char word[24], attrs[64];
    address_word(address, word, sizeof word);
    snprintf(attrs, sizeof attrs, "kind=%s%s%s", kind_of(icon), *word ? "\tuid=" : "", word);
    bus_device("bt", connected[slot].id, name, attrs);
    snprintf(connected[slot].path, sizeof connected[slot].path, "%s", path);
    free(name); free(icon); free(address);
}

static void vanish(const char *path) {
    for (int i = 0; i < MAX_BT_DEVICES; i++) {
        if (strcmp(connected[i].path, path)) continue;
        char key[32];
        snprintf(key, sizeof key, "bt.%s", connected[i].id);
        bus_unset_tree(key);
        memset(&connected[i], 0, sizeof connected[i]);
    }
}

/* org.freedesktop.DBus.Properties.PropertiesChanged(interface, changed, invalidated) of a device:
 * only the Connected property is of interest. */
static int on_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)userdata; (void)err;
    const char *iface;
    if (sd_bus_message_read(m, "s", &iface) < 0 || strcmp(iface, "org.bluez.Device1") != 0) return 0;

    int connected_now = -1;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
    int r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *key, *contents;
        char type;
        if (sd_bus_message_read(m, "s", &key) < 0 || sd_bus_message_peek_type(m, &type, &contents) < 0) return 0;
        if (!strcmp(key, "Connected") && contents && !strcmp(contents, "b")) {
            int value;
            if (sd_bus_message_read(m, "v", "b", &value) < 0) return 0;
            connected_now = value != 0;
        } else if (sd_bus_message_skip(m, "v") < 0) {
            return 0;
        }
        if (sd_bus_message_exit_container(m) < 0) return 0;
    }
    if (connected_now < 0) return 0;     /* some other property of the device changed */

    const char *path = sd_bus_message_get_path(m);
    if (!path) return 0;
    if (connected_now) appear(path);
    else vanish(path);
    return 0;
}

/* A device that BlueZ forgot (unpaired while connected) is gone. */
static int on_removed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)userdata; (void)err;
    const char *path;
    if (sd_bus_message_read(m, "o", &path) >= 0) vanish(path);
    return 0;
}

/* The devices that are connected now: when bluetoothd is there at the start, or starts later. */
static void enumerate(void) {
    sd_bus_message *reply = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_call_method(bus, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &err, &reply, "") < 0) goto done;
    if (sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}") < 0) goto done;
    while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
        const char *object;
        char path[128] = "";
        bool is_device = false, is_connected = false;
        if (sd_bus_message_read(reply, "o", &object) < 0 || sd_bus_message_enter_container(reply, 'a', "{sa{sv}}") < 0) goto done;
        snprintf(path, sizeof path, "%s", object);
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char *iface;
            if (sd_bus_message_read(reply, "s", &iface) < 0) goto done;
            if (!strcmp(iface, "org.bluez.Device1")) {
                is_device = true;
                if (sd_bus_message_enter_container(reply, 'a', "{sv}") < 0) goto done;
                while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                    const char *key;
                    if (sd_bus_message_read(reply, "s", &key) < 0) goto done;
                    if (!strcmp(key, "Connected")) {
                        int value;
                        if (sd_bus_message_read(reply, "v", "b", &value) < 0) goto done;
                        is_connected = value != 0;
                    } else if (sd_bus_message_skip(reply, "v") < 0) {
                        goto done;
                    }
                    if (sd_bus_message_exit_container(reply) < 0) goto done;
                }
                if (sd_bus_message_exit_container(reply) < 0) goto done;
            } else if (sd_bus_message_skip(reply, "a{sv}") < 0) {
                goto done;
            }
            if (sd_bus_message_exit_container(reply) < 0) goto done;
        }
        if (sd_bus_message_exit_container(reply) < 0 || sd_bus_message_exit_container(reply) < 0) goto done;
        if (is_device && is_connected) appear(path);
    }
done:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
}

static int on_owner(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)m; (void)userdata; (void)err;
    enumerate();
    return 0;
}

static void on_bus(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    int r;
    while ((r = sd_bus_process(bus, NULL)) > 0) {}
    if (r < 0) {   /* the system bus went away */
        loop_remove(sd_bus_get_fd(bus));
        sd_bus_flush_close_unref(bus);
        bus = NULL;
    }
}

void bt_init(void) {
    const char *off = pac_setting("bluetooth");
    if (off && !strcmp(off, "0")) return;
    if (sd_bus_open_system(&bus) < 0) { bus = NULL; return; }
    int r = sd_bus_add_match(bus, NULL,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',arg0='org.bluez.Device1'", on_changed, NULL);
    if (r < 0) {
        sd_bus_flush_close_unref(bus);
        bus = NULL;
        return;
    }
    if (sd_bus_add_match(bus, NULL, "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'", on_removed, NULL) < 0 ||
        sd_bus_add_match(bus, NULL, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'", on_owner, NULL) < 0) {
        sd_bus_flush_close_unref(bus);
        bus = NULL;
        return;
    }
    loop_add(sd_bus_get_fd(bus), EPOLLIN, on_bus, NULL);
    enumerate();
    while (sd_bus_process(bus, NULL) > 0) {}
}
