#include "settings.h"
#include "config_file.h"
#include "setting.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The integer settings and the range each is read with (see the call sites). Checking here gives
 * the file and line; the reader then has nothing to clamp. */
static const struct { const char *key; long lo, hi; } numbers[] = {
    { "temp_interval", 0, 3600 },
    { "temp_warn", 20, 150 },
    { "temp_crit", 20, 150 },
    { "net_signal_interval", 0, 3600 },
    { "disk_warn", 2, 95 },
    { "disk_crit", 1, 94 },
    { "disk_interval", 0, 86400 },
    { "probe_interval", 5, 3600 },
};

/* The settings with only yes or no for a value. */
static const char *const switches[] = { "temp", "camera", "usb", "print", "disk", "bluetooth", "lock_keys" };

bool settings_apply(const char *key, const char *value, void *data) {
    (void)data;
    char name[64];
    size_t i;
    for (i = 0; key[i]; i++) {
        if (i + 1 >= sizeof name) return false;
        name[i] = key[i] == '-' ? '_' : (char)tolower((unsigned char)key[i]);
    }
    name[i] = '\0';
    if (!strcmp(name, "net_signal_source")) {
        return (!strcmp(value, "auto") || !strcmp(value, "networkmanager") || !strcmp(value, "iwd") || !strcmp(value, "sysfs")) &&
               pac_setting_set(name, value);
    }
    if (!strcmp(name, "probe_host")) {
        struct in_addr addr;
        return (!*value || inet_pton(AF_INET, value, &addr) == 1) && pac_setting_set(name, value);
    }
    if (!strcmp(name, "jacks"))
        return (!*value || !strcmp(value, "auto") || !strcmp(value, "generic") || !strcmp(value, "0")) && pac_setting_set(name, value);
    if (!strcmp(name, "print_debug")) {   /* only its being set counts: off is not set */
        bool on;
        return pac_config_bool(value, &on) && pac_setting_set(name, on ? "1" : NULL);
    }
    if (!strcmp(name, "jacks_debug")) {   /* only its being set counts: off is not set */
        bool on;
        return pac_config_bool(value, &on) && pac_setting_set(name, on ? "1" : NULL);
    }
    for (i = 0; i < sizeof switches / sizeof *switches; i++)
        if (!strcmp(name, switches[i])) {
            bool on;
            return pac_config_bool(value, &on) && pac_setting_set(name, on ? "1" : "0");
        }
    for (i = 0; i < sizeof numbers / sizeof *numbers; i++) {
        if (strcmp(name, numbers[i].key)) continue;
        char *end;
        errno = 0;
        long n = strtol(value, &end, 10);
        return *value && !*end && !errno && n >= numbers[i].lo && n <= numbers[i].hi && pac_setting_set(name, value);
    }
    return false;
}

void settings_load(void) {
    pac_config_load("pacreactd", settings_apply, NULL);
}
