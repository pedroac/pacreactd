/* CPU and SSD temperature: the thresholds, hysteresis, polling interval, and the whole check
 * against a mock hwmon tree. src_temp.c is included for its statics. */
#include "recorder.h"
#include "setting.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/src_temp.c"

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

static void set_temp(const char *rel, int celsius) {
    char v[16];
    snprintf(v, sizeof v, "%d\n", celsius * 1000);
    put(rel, v);
}

static void test_limits(void) {
    Limits l = temp_limits(CLASS_CPU, 100);   assert(l.warn == 85 && l.hot == 95);
    l = temp_limits(CLASS_CPU, 110);          assert(l.warn == 85 && l.hot == 95);    /* never above the defaults */
    l = temp_limits(CLASS_CPU, 90);           assert(l.warn == 75 && l.hot == 85);    /* a lower own limit wins */
    l = temp_limits(CLASS_CPU, 75);           assert(l.warn == 60 && l.hot == 70);
    l = temp_limits(CLASS_SSD, 79);           assert(l.warn == 73 && l.hot == 77);
    l = temp_limits(CLASS_SSD, 80);           assert(l.warn == 74 && l.hot == 78);

    Limits c = { 85, 95 };
    assert(temp_level(60, c, 0) == 0 && temp_level(84, c, 0) == 0);
    assert(temp_level(85, c, 0) == 1 && temp_level(94, c, 0) == 1);
    assert(temp_level(95, c, 0) == 2 && temp_level(120, c, 0) == 2);
    /* hysteresis: down only once 5 degrees under the threshold */
    assert(temp_level(81, c, 1) == 1 && temp_level(80, c, 1) == 0);
    assert(temp_level(91, c, 2) == 2 && temp_level(90, c, 2) == 1);
    assert(temp_level(80, c, 2) == 0 && temp_level(81, c, 2) == 1);
    assert(temp_level(84, c, 0) == 0);                          /* but from fine, 84 is fine */

    /* the polling interval follows the margin to the warning level */
    assert(temp_interval(40, 0, 120) == 120 && temp_interval(25, 0, 120) == 120);
    assert(temp_interval(24, 0, 120) == 30 && temp_interval(10, 0, 120) == 30);
    assert(temp_interval(9, 0, 120) == 10 && temp_interval(-3, 0, 120) == 10);
    assert(temp_interval(40, 1, 120) == 10);                    /* already high: keep watching closely */
    assert(temp_interval(40, 0, 60) == 60 && temp_interval(15, 0, 20) == 20);   /* never slower than asked */
    assert(temp_interval(5, 0, 5) == 5);                         /* a very short maximum is honoured */
}

static void build_tree(void) {
    snprintf(root, sizeof root, "/tmp/pacreact-temp-XXXXXX");
    assert(mkdtemp(root));
    put("hwmon0/name", "coretemp\n");
    set_temp("hwmon0/temp1_input", 46);  set_temp("hwmon0/temp1_crit", 100);
    set_temp("hwmon0/temp2_input", 45);  set_temp("hwmon0/temp2_crit", 100);
    put("hwmon1/name", "nvme\n");
    set_temp("hwmon1/temp1_input", 27);  set_temp("hwmon1/temp1_crit", 79);
    put("hwmon2/name", "iwlwifi_1\n");  set_temp("hwmon2/temp1_input", 99);   /* not watched */
    put("hwmon3/name", "acpitz\n");     set_temp("hwmon3/temp1_input", 99);   /* not watched */
    put("hwmon4/name", "k10temp\n");    set_temp("hwmon4/temp1_input", 40);   /* no limit: the default */
    hwmon_root = root;
}

static void test_check(void) {
    build_tree();
    memset(levels, 0, sizeof levels);
    rec_reset();

    /* Everything cool: published, with no level, and a relaxed timer (closest margin: 85 - 46) */
    assert(check() == 120);
    assert(!strcmp(rec("temp.cpu"), "46") && !strcmp(rec("temp.ssd"), "27"));

    /* A hot CPU: high, then critical */
    set_temp("hwmon0/temp1_input", 86);
    assert(check() == 10);
    assert(!strcmp(rec("temp.cpu"), "86\tlevel=warning"));
    int n = rec_total;
    check();
    assert(rec_total == n);                                      /* the same again says nothing */
    set_temp("hwmon0/temp1_input", 96);
    check();
    assert(!strcmp(rec("temp.cpu"), "96\tlevel=critical"));

    /* Cooling down: the level is kept until 5 degrees under its threshold */
    set_temp("hwmon0/temp1_input", 91);
    check();
    assert(!strcmp(rec("temp.cpu"), "91\tlevel=critical"));
    set_temp("hwmon0/temp1_input", 70);
    assert(check() == 30);                                       /* 15 degrees below the warning level */
    assert(!strcmp(rec("temp.cpu"), "70"));

    /* The hottest of a class's sensors is what is reported (a core, not the package) */
    set_temp("hwmon0/temp1_input", 50);
    set_temp("hwmon0/temp2_input", 97);
    check();
    assert(!strcmp(rec("temp.cpu"), "97\tlevel=critical"));
    set_temp("hwmon0/temp2_input", 45);
    check();

    /* An SSD has its own, lower, limits (79: high at 73, critical at 77) */
    set_temp("hwmon1/temp1_input", 74);
    check();
    assert(!strcmp(rec("temp.ssd"), "74\tlevel=warning"));
    set_temp("hwmon1/temp1_input", 78);
    check();
    assert(!strcmp(rec("temp.ssd"), "78\tlevel=critical"));
    set_temp("hwmon1/temp1_input", 30);
    set_temp("hwmon0/temp1_input", 40);
    check();
    assert(!strcmp(rec("temp.ssd"), "30") && !strcmp(rec("temp.cpu"), "45"));   /* core 2 is the hottest now */

    /* Sensors that are not watched, and unreadable files, change nothing */
    set_temp("hwmon2/temp1_input", 120);
    put("hwmon4/temp2_input", "not a number\n");
    put("hwmon4/temp3_input", "");
    set_temp("hwmon0/temp1_input", 40);
    n = rec_total;
    check();
    assert(!strstr(rec("temp.cpu"), "level"));

    /* A device's own limit counts: 90 makes this CPU high at 75 */
    set_temp("hwmon0/temp1_input", 30);
    set_temp("hwmon4/temp1_crit", 90);
    set_temp("hwmon4/temp1_input", 76);
    check();
    assert(!strcmp(rec("temp.cpu"), "76\tlevel=warning"));
    set_temp("hwmon4/temp1_input", 40);
    check();
    /* a bogus limit (under 20 degrees) is ignored in favour of the default; "max" is the fallback */
    set_temp("hwmon4/temp1_crit", 5);
    set_temp("hwmon4/temp1_input", 80);
    check();
    assert(!strstr(rec("temp.cpu"), "level"));                   /* 80 is fine under the default 100 */
    put("hwmon4/temp1_crit", "");
    set_temp("hwmon4/temp1_max", 85);                            /* an unreadable crit falls back to max: high from 70 */
    set_temp("hwmon4/temp1_input", 75);
    check();
    assert(!strcmp(rec("temp.cpu"), "75\tlevel=warning"));

    /* No sensors at all (or no hwmon directory): a rare look, no crash */
    hwmon_root = "/nonexistent-hwmon";
    memset(levels, 0, sizeof levels);
    assert(check() == max_interval_s);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
}

static void test_uevent(void) {
    /* Thermal and hwmon events look at once; others do not. */
    build_tree();
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    memset(levels, 0, sizeof levels);
    rec_reset();
    on_uevent("block", "", 0);
    assert(rec_total == 0);
    on_uevent("thermal", "", 0);
    assert(rec_total > 0);
    int n = rec_total;
    set_temp("hwmon0/temp1_input", 90);
    on_uevent("hwmon", "", 0);
    assert(rec_total > n);
    close(timer_fd);
    timer_fd = -1;
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
}

int main(void) {
    test_limits();
    test_check();
    test_uevent();
    puts("temp: ok");
    return 0;
}
