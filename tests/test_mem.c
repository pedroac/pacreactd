/* Pressure keys: a level that is kept until the calm, and what a trigger that fires too soon does.
 * src_mem.c is included for its statics. */
#include "recorder.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

#include "../src/src_mem.c"

static void set_avg10(const char *path, double some, double full) {
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "some avg10=%.2f avg60=0.00 avg300=0.00 total=100\nfull avg10=%.2f avg60=0.00 avg300=0.00 total=50\n", some, full);
    fclose(f);
}

/* Makes a timer run out now. */
static void expire(int tfd) {
    struct itimerspec its = { .it_value = { .tv_nsec = 1 } };
    timerfd_settime(tfd, 0, &its, NULL);
    usleep(2000);
}

static void reset(void) {
    rec_reset();
    for (size_t i = 0; i < sizeof psi / sizeof *psi; i++) psi[i].level = LEVEL_NONE;
}

int main(void) {
    Psi *mem = &psi[0], *cpu = &psi[1];

    /* A warning, then a worse level, which a milder one does not undo */
    reset();
    fire(mem, LEVEL_WARNING);
    assert(!strcmp(rec("mem.pressure"), "on\tlevel=warning") && *rec("mem.available"));
    fire(mem, LEVEL_CRITICAL);
    assert(!strcmp(rec("mem.pressure"), "on\tlevel=critical"));
    fire(mem, LEVEL_WARNING);
    assert(!strcmp(rec("mem.pressure"), "on\tlevel=critical"));
    int n = rec_changes("mem.pressure");
    fire(mem, LEVEL_CRITICAL);
    assert(rec_changes("mem.pressure") == n);                   /* a trigger that repeats says nothing new */

    /* The calm, and the next pressure is a new one */
    calm(mem);
    assert(!strcmp(rec("mem.pressure"), "off"));
    fire(mem, LEVEL_WARNING);
    assert(!strcmp(rec("mem.pressure"), "on\tlevel=warning"));

    /* Each resource has a key of its own, and no available memory comes with the others */
    fire(cpu, LEVEL_WARNING);
    assert(!strcmp(rec("cpu.pressure"), "on\tlevel=warning") && !strcmp(rec("mem.pressure"), "on\tlevel=warning"));
    publish(&psi[2]);
    assert(!strcmp(rec("io.pressure"), "off"));

    /* The kernel fires a new trigger with next to no stall: it counts only if the kernel's own average agrees */
    char file[64] = "/tmp/pacreactd-psi-XXXXXX";
    int fd = mkstemp(file);
    assert(fd >= 0);
    close(fd);
    mem->path = file;
    set_avg10(file, 0.00, 0.00);
    mem->confirm_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    assert(mem->confirm_fd >= 0);
    reset();
    Trigger t = { mem, LEVEL_CRITICAL, "full" }, w = { mem, LEVEL_WARNING, "some" };
    assert(avg10(mem, "some") == 0.0 && avg10(mem, "full") == 0.0);
    on_trigger(&t, EPOLLPRI);
    assert(!strcmp(rec("mem.pressure"), "") && mem->pending == &t);   /* doubtful: a second look is waiting */
    on_confirm(mem, EPOLLIN);                                           /* (the timer has not run out) */
    assert(mem->pending == &t);
    expire(mem->confirm_fd);
    on_confirm(mem, EPOLLIN);
    assert(!strcmp(rec("mem.pressure"), "") && !mem->pending);          /* still nothing: it was the kernel's mistake */

    /* A real burst fires before the average has caught up: seen on the second look */
    on_trigger(&t, EPOLLPRI);
    on_trigger(&w, EPOLLPRI);
    assert(mem->pending == &t);                                         /* the worse one is the one kept */
    set_avg10(file, 4.50, 2.20);
    expire(mem->confirm_fd);
    on_confirm(mem, EPOLLIN);
    assert(!strcmp(rec("mem.pressure"), "on\tlevel=critical"));
    calm(mem);

    /* One the average already agrees with is believed at once, on the line it watches */
    on_trigger(&w, EPOLLPRI);
    assert(!strcmp(rec("mem.pressure"), "on\tlevel=warning") && !mem->pending);
    calm(mem);
    set_avg10(file, 0.20, 12.0);
    on_trigger(&w, EPOLLPRI);                                           /* "some" is low even though "full" is high */
    assert(!strcmp(rec("mem.pressure"), "off") && mem->pending == &w);
    mem->pending = NULL;
    on_trigger(&t, EPOLLIN);                                            /* only a priority event is a trigger */
    assert(!strcmp(rec("mem.pressure"), "off") && !mem->pending);

    /* A file that cannot be read is no confirmation */
    mem->path = "/nonexistent-psi";
    assert(avg10(mem, "some") < 0);
    on_trigger(&t, EPOLLPRI);
    assert(!strcmp(rec("mem.pressure"), "off"));
    unlink(file);
    mem->path = "/proc/pressure/memory";

    puts("mem: ok");
    return 0;
}
