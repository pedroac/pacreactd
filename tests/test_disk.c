/* Low disk space: thresholds with hysteresis, the mount table parser, what is published, and
 * a real pass over this machine's own mounts. src_disk.c is included for its statics. */
#include "recorder.h"
#include <assert.h>
#include "setting.h"
#include <stdio.h>
#include <string.h>

#include "../src/src_disk.c"

#define GB (1ULL << 30)
#define MB (1ULL << 20)

static void test_levels(void) {
    /* A big disk: percentages decide */
    assert(disk_level(50 * GB, 100 * GB, 0) == 0);
    assert(disk_level(10 * GB, 100 * GB, 0) == 0);          /* exactly 10% is not low */
    assert(disk_level(9 * GB, 100 * GB, 0) == 1);
    assert(disk_level(5 * GB, 100 * GB, 0) == 1);           /* exactly 5% is low, not critical */
    assert(disk_level(4 * GB, 100 * GB, 0) == 2);
    assert(disk_level(45 * GB, 500 * GB, 0) == 1);          /* 9% of a big disk is still 45 GB: not critical */

    /* A small partition: nearly empty in bytes is fine while it is nearly empty in use too */
    assert(disk_level(856 * MB, 1000 * MB, 0) == 0);        /* a boot partition that is 14% used */
    assert(disk_level(90 * MB, 1000 * MB, 0) == 2);         /* low in percent and in bytes: critical */
    assert(disk_level(900 * MB, 10 * GB, 0) == 2);          /* 8.8% of 10 GB, under 1 GB left */
    assert(disk_level(2 * GB, 10 * GB, 0) == 0);

    /* Hysteresis: a level is left only after recovering past a margin */
    assert(disk_level(11 * GB, 100 * GB, 1) == 1);          /* 11%: not yet recovered from "low" */
    assert(disk_level(13 * GB, 100 * GB, 1) == 0);
    assert(disk_level(6 * GB, 100 * GB, 2) == 2);           /* 6% after critical: still critical */
    assert(disk_level(7 * GB, 100 * GB, 2) == 2);
    assert(disk_level(9 * GB, 100 * GB, 2) == 1);           /* 9%: recovered from critical, still low */
    assert(disk_level(30 * GB, 100 * GB, 2) == 0);
    assert(disk_level(9 * GB, 100 * GB, 0) == 1);           /* ...but from fine, 9% is a fresh warning */

    /* Odd inputs */
    assert(disk_level(0, 0, 0) == 0);                       /* an empty filesystem reports nothing */
    assert(disk_level(0, 100 * GB, 0) == 2);
}

static void test_mountinfo(void) {
    const char *text =
        "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
        "23 22 259:1 / /boot rw,relatime shared:2 - vfat /dev/nvme0n1p1 rw,fmask=0022\n"
        "24 22 0:21 / /run rw,nosuid shared:3 - tmpfs tmpfs rw\n"
        "25 22 7:0 / /var/lib/snapd/snap/core/1 ro,nodev - squashfs /dev/loop0 ro\n"
        "26 22 259:2 /home /home rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
        "27 22 8:17 / /mnt/my\\040drive rw,relatime - exfat /dev/sdb1 rw\n"
        "28 22 8:33 / /mnt/cd ro,relatime - iso9660 /dev/sr0 ro\n"
        "29 22 8:49 / /mnt/rodisk ro,relatime - ext4 /dev/sdd1 ro\n"
        "30 22 0:40 / /mnt/net rw - nfs4 server:/export rw\n"
        "31 22 0:41 / /sys rw - sysfs sysfs rw\n"
        "this line is not a mount\n"
        "32 22 8:65 / /mnt/optional rw,relatime shared:9 master:2 - xfs /dev/sde1 rw\n"
        "33 22 8:81 / /mnt/roword rw,errors=remount-ro - ext4 /dev/sdf1 rw\n";
    Mount m[16];
    int n = disk_parse_mountinfo(text, m, 16);
    assert(n == 5);
    assert(!strcmp(m[0].mount, "/") && !strcmp(m[0].dev, "259:2"));
    assert(!strcmp(m[1].mount, "/boot"));
    assert(!strcmp(m[2].mount, "/mnt/my drive"));            /* \040 is a space */
    assert(!strcmp(m[3].mount, "/mnt/optional") && !strcmp(m[3].dev, "8:65"));   /* optional fields skipped */
    assert(!strcmp(m[4].mount, "/mnt/roword"));              /* "remount-ro" in the options is not "ro" */

    assert(disk_parse_mountinfo(text, m, 2) == 2);           /* capped */
    assert(disk_parse_mountinfo("", m, 16) == 0);
    assert(disk_parse_mountinfo("garbage - \n- - -\n", m, 16) == 0);

    char esc[] = "a\\134b\\012c\\040d\\05x";
    unescape(esc);
    assert(!strcmp(esc, "a\\b\nc d\\05x"));                   /* backslash, newline, space; junk left alone */
}

/* What is published of a filesystem: the percent free with its level, and the mount and the bytes beside it */
static void test_published(void) {
    memset(filesystems, 0, sizeof filesystems);
    rec_reset();

    disk_evaluate("8:1", "/data", 50 * GB, 100 * GB);
    assert(!strcmp(rec("disk.data"), "50") && !strcmp(rec("disk.data.mount"), "/data") && !strcmp(rec("disk.data.free"), "53687091200"));

    disk_evaluate("8:1", "/data", 8 * GB, 100 * GB);                         /* 8%: low */
    assert(!strcmp(rec("disk.data"), "8\tlevel=warning") && !strcmp(rec("disk.data.free"), "8589934592"));
    int n = rec_changes("disk.data");
    disk_evaluate("8:1", "/data", 8 * GB, 100 * GB);                         /* nothing changed: nothing said */
    assert(rec_changes("disk.data") == n);

    disk_evaluate("8:1", "/data", 3 * GB, 100 * GB);                         /* 3%: almost full */
    assert(!strcmp(rec("disk.data"), "3\tlevel=critical"));
    disk_evaluate("8:1", "/data", 30 * GB, 100 * GB);                        /* recovered */
    assert(!strcmp(rec("disk.data"), "30") && filesystems[0].level == 0);

    /* Between the thresholds a level is kept (hysteresis), as the value says */
    disk_evaluate("8:1", "/data", 3 * GB, 100 * GB);
    disk_evaluate("8:1", "/data", 7 * GB, 100 * GB);                         /* 7%: still critical, 3% past the threshold not reached */
    assert(!strcmp(rec("disk.data"), "7\tlevel=critical") || !strcmp(rec("disk.data"), "7\tlevel=warning"));

    /* Another filesystem has its own keys; a mount point is a word; one moved is the same filesystem */
    disk_evaluate("8:2", "/home", 9 * GB, 100 * GB);
    assert(!strcmp(rec("disk.home"), "9\tlevel=warning"));
    disk_evaluate("8:2", "/mnt/My Disk", 9 * GB, 100 * GB);
    assert(!strcmp(rec("disk.home.mount"), "/mnt/My Disk") && !strcmp(filesystems[1].name, "home"));
    disk_evaluate("8:3", "/", 9 * GB, 100 * GB);
    assert(!strcmp(rec("disk.root"), "9\tlevel=warning"));
    disk_evaluate("8:4", "/mnt/My Disk", 9 * GB, 100 * GB);                  /* the same word: told apart */
    assert(!strcmp(rec("disk.mnt-my-disk"), "9\tlevel=warning") || !strcmp(rec("disk.mnt-my-disk-2"), "9\tlevel=warning"));
    disk_evaluate("8:5", "/Mnt/my disk", 9 * GB, 100 * GB);
    assert(*rec("disk.mnt-my-disk-2") || *rec("disk.mnt-my-disk-3"));

    /* An unmounted filesystem is forgotten: it can be told anew */
    memset(filesystems, 0, sizeof filesystems);
    disk_evaluate("8:6", "/gone", 3 * GB, 100 * GB);
    assert(!strcmp(rec("disk.gone"), "3\tlevel=critical"));
    forget(&filesystems[0]);
    assert(filesystems[0].dev[0] == '\0');

    /* More filesystems than slots: the extra ones are ignored, not a crash */
    memset(filesystems, 0, sizeof filesystems);
    char dev[24], mnt[32];
    for (int i = 0; i < MAX_FS + 4; i++) {
        snprintf(dev, sizeof dev, "9:%d", i);
        snprintf(mnt, sizeof mnt, "/m%d", i);
        disk_evaluate(dev, mnt, 50 * GB, 100 * GB);
    }
}

static void test_real_system(void) {
    /* The real mount table and statvfs: this machine's root filesystem must be found */
    memset(filesystems, 0, sizeof filesystems);
    pac_setting_set("disk_interval", "0");
    pac_setting_set("disk", NULL);
    pac_setting_set("disk_warn", NULL);
    pac_setting_set("disk_crit", NULL);
    registered = 0;
    disk_init();
    assert(registered == 1 && mounts_fd >= 0 && timer_fd < 0);   /* only the mount table: interval 0 */
    int tracked = 0;
    for (int i = 0; i < MAX_FS; i++) tracked += filesystems[i].dev[0] != '\0';
    assert(tracked >= 1);

    /* With an interval the timer is registered too */
    pac_setting_set("disk_interval", "300");
    registered = 0;
    disk_init();
    assert(registered == 2 && timer_fd >= 0);

    /* Switched off: nothing registered */
    pac_setting_set("disk", "0");
    registered = 0;
    disk_init();
    assert(registered == 0);
    pac_setting_set("disk", NULL);

    /* Thresholds from the environment, and nonsense falls back */
    pac_setting_set("disk_warn", "20");
    pac_setting_set("disk_crit", "8");
    disk_init();
    assert(warn_pct == 20 && crit_pct == 8);
    pac_setting_set("disk_warn", "abc");
    pac_setting_set("disk_crit", "abc");                         /* not a number: the default */
    disk_init();
    assert(warn_pct == 10 && crit_pct == 5);
    pac_setting_set("disk_crit", "99");                          /* out of range: the highest allowed, then kept below warn */
    disk_init();
    assert(warn_pct == 10 && crit_pct == 9);
    pac_setting_set("disk_warn", "6");
    pac_setting_set("disk_crit", "30");                          /* in range but not below warn: pulled down */
    disk_init();
    assert(warn_pct == 6 && crit_pct == 5);
}

int main(void) {
    test_levels();
    test_mountinfo();
    test_published();
    test_real_system();
    puts("disk: ok");
    return 0;
}
