#include "loop.h"
#include "server.h"
#include "sources.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

/* The screen's brightness, from the sysfs attribute, which the kernel signals when it changes.
 *   backlight.level   0 to 100 */

static int fd = -1;
static long max_brightness = 1, last = -1;

static long read_long(int f) {
    char buf[32];
    ssize_t n = pread(f, buf, sizeof buf - 1, 0);  /* sysfs attributes are re-read from 0 */
    if (n <= 0) return -1;
    buf[n] = '\0';
    return atol(buf);
}

static void publish(long v) {
    char text[32];
    snprintf(text, sizeof text, "%ld", v * 100 / max_brightness);
    bus_set("backlight.level", text);
}

static void on_change(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    long v = read_long(fd);  /* reading also re-arms the sysfs notification */
    if (v < 0 || v == last) return;
    last = v;
    publish(v);
}

void backlight_init(void) {
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && fd < 0) {
        if (e->d_name[0] == '.') continue;
        char path[300];
        snprintf(path, sizeof path, "/sys/class/backlight/%s/max_brightness", e->d_name);
        int mf = open(path, O_RDONLY | O_CLOEXEC);
        if (mf < 0) continue;
        long max = read_long(mf);
        close(mf);
        if (max <= 0) continue;

        snprintf(path, sizeof path, "/sys/class/backlight/%s/brightness", e->d_name);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        max_brightness = max;
    }
    closedir(d);
    if (fd < 0) return;

    last = read_long(fd);
    if (last >= 0) publish(last);
    loop_add(fd, EPOLLPRI | EPOLLERR, on_change, NULL);
}
