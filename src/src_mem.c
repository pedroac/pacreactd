#include "server.h"
#include "sources.h"
#include "tunables.h"
#include "loop.h"
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* Pressure stall information: the kernel wakes us when tasks have waited on a resource for too long
 * in a window, and says nothing when it passes, so a key that went "on" goes back to "off" after
 * SRC_PSI_CLEAR_SECS without another trigger. Without PSI, memory is polled at a low rate instead.
 *   mem.pressure, cpu.pressure, io.pressure   on or off, with a level while on
 *   mem.available                             MB, as of the last time the pressure changed */

/* Unprivileged triggers need a window that is a multiple of 2 s. */
typedef struct Trigger Trigger;
typedef struct {
    const char *name, *path, *warn, *crit;
    int level, clear_fd, confirm_fd;   /* the timers: back to "off" after the calm, and a second look at a doubtful trigger */
    const Trigger *pending;            /* the doubtful trigger waiting for that second look */
} Psi;

static Psi psi[] = {
    /* Stalling for 500 ms of 2 s means tasks are waiting on memory; 250 ms with everything stalled is thrashing. */
    { "mem", "/proc/pressure/memory", "some 500000 2000000", "full 250000 2000000", 0, -1, -1, NULL },
    /* Runnable tasks waiting for a CPU for most of the window. A warning only: a build does this on purpose. */
    { "cpu", "/proc/pressure/cpu", "some 1500000 2000000", NULL, 0, -1, -1, NULL },
    /* Everything stalled on disk for half the window. */
    { "io", "/proc/pressure/io", NULL, "full 1000000 2000000", 0, -1, -1, NULL },
};

static long mem_available_mb(long *total_mb) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    long avail = -1, total = -1;
    while (fgets(line, sizeof line, f)) {
        sscanf(line, "MemAvailable: %ld kB", &avail);
        sscanf(line, "MemTotal: %ld kB", &total);
    }
    fclose(f);
    if (total_mb) *total_mb = total / 1024;
    return avail < 0 ? -1 : avail / 1024;
}

/* What the key says now. The available memory goes first, so that whoever reads the pressure finds it current. */
static void publish(Psi *p) {
    if (!strcmp(p->name, "mem")) {
        long avail = mem_available_mb(NULL);
        char v[24];
        if (avail >= 0) { snprintf(v, sizeof v, "%ld", avail); bus_set("mem.available", v); }
    }
    char key[KEY_LEN];
    snprintf(key, sizeof key, "%s.pressure", p->name);
    if (p->level == LEVEL_NONE) bus_set(key, "off");
    else bus_set_level(key, "on", (Level)p->level);
}

static void arm_clear(Psi *p) {
    struct itimerspec its = { .it_value.tv_sec = SRC_PSI_CLEAR_SECS };
    timerfd_settime(p->clear_fd, 0, &its, NULL);
}

/* A trigger of the given level fired. A worse level is kept over a milder one until the calm. */
static void fire(Psi *p, Level level) {
    if ((int)level > p->level) p->level = level;
    publish(p);
    if (p->clear_fd >= 0) arm_clear(p);
}

static void calm(Psi *p) {
    p->level = LEVEL_NONE;
    publish(p);
}

struct Trigger { Psi *psi; Level level; const char *kind; };   /* kind: the line of the file the trigger watches, "some" or "full" */

/* The kernel's own average of the stall over the last 10 s, in percent, of the line `kind`; -1 if it cannot be read. */
static double avg10(const Psi *p, const char *kind) {
    FILE *f = fopen(p->path, "r");
    if (!f) return -1;
    char line[160];
    double v = -1;
    size_t n = strlen(kind);
    while (fgets(line, sizeof line, f)) {
        const char *at;
        if (!strncmp(line, kind, n) && line[n] == ' ' && (at = strstr(line, "avg10="))) v = strtod(at + 6, NULL);
    }
    fclose(f);
    return v;
}

/* A new trigger fires with hardly any stall at all, on kernels seen so far (7.2): once at the first look and
 * again a window or two later, whatever the pressure (a stall of 164 us for a threshold of 1.5 s). So a trigger is
 * only believed when the average of the kernel agrees. The average moves every 2 s, so one that fires before it has
 * caught up is looked at again a little later; a real burst is still in the average then. */
static void on_trigger(void *ctx, uint32_t ev) {
    if (!(ev & EPOLLPRI)) return;
    Trigger *t = ctx;
    Psi *p = t->psi;
    if (avg10(p, t->kind) >= SRC_PSI_MIN_AVG10) { fire(p, t->level); return; }
    if (p->confirm_fd < 0) return;
    if (!p->pending || t->level > p->pending->level) p->pending = t;
    struct itimerspec its = { .it_value = { .tv_sec = SRC_PSI_CONFIRM_MS / 1000, .tv_nsec = (SRC_PSI_CONFIRM_MS % 1000) * 1000000L } };
    timerfd_settime(p->confirm_fd, 0, &its, NULL);
}

static void on_confirm(void *ctx, uint32_t ev) {
    (void)ev;
    Psi *p = ctx;
    uint64_t n;
    if (read(p->confirm_fd, &n, sizeof n) <= 0 || !p->pending) return;
    const Trigger *t = p->pending;
    p->pending = NULL;
    if (avg10(p, t->kind) >= SRC_PSI_MIN_AVG10) fire(p, t->level);
}

static void on_clear(void *ctx, uint32_t ev) {
    (void)ev;
    Psi *p = ctx;
    uint64_t n;
    if (read(p->clear_fd, &n, sizeof n) > 0) calm(p);
}

static bool add_trigger(Psi *p, const char *spec, Level level) {
    static Trigger triggers[8];
    static int n;
    if (!spec || n == 8) return false;
    int fd = open(p->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    if (write(fd, spec, strlen(spec) + 1) < 0) { close(fd); return false; }
    triggers[n] = (Trigger){ p, level, !strncmp(spec, "full", 4) ? "full" : "some" };
    loop_add(fd, EPOLLPRI, on_trigger, &triggers[n++]);
    return true;
}

/* Without PSI: MemAvailable, read at a low rate. */
static void on_poll(void *ctx, uint32_t ev) {
    (void)ev;
    uint64_t n;
    if (read(*(int *)ctx, &n, sizeof n) < 0) return;
    long total, avail = mem_available_mb(&total);
    if (avail < 0 || total <= 0) return;
    Psi *p = &psi[0];
    if (!p->level && avail * 100 < total * LOW_PCT) { p->level = LEVEL_CRITICAL; publish(p); }
    else if (p->level && avail * 100 > total * SRC_MEM_REARM_PCT) calm(p);
}

void mem_init(void) {
    bool mem_psi = false;
    for (size_t i = 0; i < sizeof psi / sizeof *psi; i++) {
        Psi *p = &psi[i];
        bool w = add_trigger(p, p->warn, LEVEL_WARNING);
        bool c = add_trigger(p, p->crit, LEVEL_CRITICAL);
        if (!w && !c) continue;
        p->clear_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (p->clear_fd >= 0) loop_add(p->clear_fd, EPOLLIN, on_clear, p);
        p->confirm_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (p->confirm_fd >= 0) loop_add(p->confirm_fd, EPOLLIN, on_confirm, p);
        publish(p);   /* "off", so the source is known to be there */
        if (i == 0) mem_psi = true;
    }
    if (mem_psi) return;

    /* No PSI for memory (a kernel without it, or denied): poll MemAvailable at a low rate instead. */
    static int fd;
    fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) return;
    struct itimerspec its = { .it_value.tv_sec = SRC_MEM_POLL_SECS, .it_interval.tv_sec = SRC_MEM_POLL_SECS };
    timerfd_settime(fd, 0, &its, NULL);
    loop_add(fd, EPOLLIN, on_poll, &fd);
    publish(&psi[0]);
}
