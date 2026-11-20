/* The battery and the charger: the level logic, and the whole check against a mock sysfs tree.
 * src_power.c is included for its statics. */
#include "recorder.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/src_power.c"

static char root[64];

static void put(const char *rel, const char *content) {
    char path[512], cmd[600];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname %s)\"", path);
    assert(system(cmd) == 0);
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static void test_levels(void) {
    latch = LEVEL_NONE;
    assert(battery_level(50, true) == LEVEL_NONE);
    assert(battery_level(15, true) == LEVEL_WARNING);
    assert(battery_level(14, true) == LEVEL_WARNING);
    assert(battery_level(5, true) == LEVEL_CRITICAL);
    assert(battery_level(12, true) == LEVEL_CRITICAL);          /* a level is kept while the percentage wavers */
    assert(battery_level(10, false) == LEVEL_NONE);             /* charging re-arms */
    assert(battery_level(15, true) == LEVEL_WARNING);
    assert(battery_level(20, true) == LEVEL_WARNING);           /* still at or below the re-arm percentage */
    assert(battery_level(21, true) == LEVEL_NONE);              /* recovered */
    assert(battery_level(30, true) == LEVEL_NONE);
    assert(battery_level(16, true) == LEVEL_NONE);              /* and above the warning threshold, from fine, is fine */
}

static void test_check(void) {
    snprintf(root, sizeof root, "/tmp/pacreactd-power-XXXXXX");
    assert(mkdtemp(root));
    power_root = root;
    put("BAT0/type", "Battery\n"); put("BAT0/capacity", "80\n"); put("BAT0/status", "Discharging\n");
    put("AC/type", "Mains\n");     put("AC/online", "0\n");
    put("wacom_battery_0/type", "Battery\n");                    /* no capacity: not ours */
    put("pen/type", "Battery\n"); put("pen/capacity", "90\n"); put("pen/scope", "Device\n");   /* a pen's: not the machine's */
    put("ps/type", "USB\n"); put("ps/online", "0\n");

    assert(find_battery() && strstr(bat_dir, "BAT0"));
    latch = LEVEL_NONE;
    rec_reset();
    check_battery();
    check_ac();
    assert(!strcmp(rec("power.battery"), "80") && !strcmp(rec("power.battery.status"), "discharging"));
    assert(!strcmp(rec("power.ac"), "off"));

    put("BAT0/capacity", "15\n");
    check_battery();
    assert(!strcmp(rec("power.battery"), "15\tlevel=warning"));
    put("BAT0/capacity", "4\n");
    check_battery();
    assert(!strcmp(rec("power.battery"), "4\tlevel=critical"));
    int n = rec_total;
    check_battery();
    assert(rec_total == n);                                     /* nothing changed: nothing said */

    put("BAT0/status", "Not charging\n");
    check_battery();
    assert(!strcmp(rec("power.battery.status"), "not-charging") && !strcmp(rec("power.battery"), "4"));
    put("BAT0/status", "Charging\n");
    check_battery();
    assert(!strcmp(rec("power.battery.status"), "charging") && !strstr(rec("power.battery"), "level"));

    put("AC/online", "1\n");                                    /* any mains or USB supply online is "on" */
    check_ac();
    assert(!strcmp(rec("power.ac"), "on"));
    put("AC/online", "0\n");
    put("ps/online", "1\n");
    check_ac();
    assert(!strcmp(rec("power.ac"), "on"));
    put("ps/online", "0\n");
    check_ac();
    assert(!strcmp(rec("power.ac"), "off"));

    /* Only a power_supply uevent makes it look. */
    put("BAT0/status", "Discharging\n"); put("BAT0/capacity", "60\n");
    on_uevent("block", "", 0);
    assert(!strcmp(rec("power.battery"), "4"));
    on_uevent("power_supply", "", 0);
    assert(!strcmp(rec("power.battery"), "60") && !strcmp(rec("power.battery.status"), "discharging"));

    /* Every supply goes (a dock, an adapter): the charger is not known any more; the battery taken out is gone too */
    char rm[600];
    snprintf(rm, sizeof rm, "rm -rf %s/AC %s/ps", root, root);
    assert(system(rm) == 0);
    check_ac();
    assert(!strcmp(rec("power.ac"), ""));
    snprintf(rm, sizeof rm, "rm -rf %s/BAT0", root);
    assert(system(rm) == 0);
    check_battery();
    assert(!strcmp(rec("power.battery"), "") && !strcmp(rec("power.battery.status"), "") && !*bat_dir);
    /* ...and one that comes later (no battery at start-up, a power_supply uevent when it is there) is found */
    put("BAT1/type", "Battery\n"); put("BAT1/capacity", "77\n"); put("BAT1/status", "Charging\n");
    on_uevent("power_supply", "", 0);
    assert(strstr(bat_dir, "BAT1") && !strcmp(rec("power.battery"), "77") && !strcmp(rec("power.battery.status"), "charging"));

    /* No power supplies at all (a desktop): nothing is published. */
    power_root = "/nonexistent-power";
    bat_dir[0] = '\0';
    rec_reset();
    assert(!find_battery());
    check_ac();
    assert(rec_total == 0);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
}

int main(void) {
    test_levels();
    test_check();
    puts("power: ok");
    return 0;
}
