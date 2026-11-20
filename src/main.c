#include "ipc.h"
#include "loop.h"
#include "server.h"
#include "settings.h"
#include "sources.h"
#include "uevent.h"
#include "version.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>

#define USAGE \
    "usage: pacreactd                     run the daemon\n" \
    "       pacreactd watch [prefix...]   print the state, then each change (see docs/protocol.md)\n" \
    "       pacreactd get <key>           print the value of one key\n" \
    "       pacreactd print               the print jobs that have not finished (asks CUPS)\n" \
    "       pacreactd --version\n"

static volatile sig_atomic_t quit;

static void on_signal(int sig) {
    (void)sig;
    quit = 1;
}

static void accept_clients(void *ctx, uint32_t events) {
    (void)events;
    server_accept((int)(long)ctx);
}

static int run(void) {
    settings_load();
    int lfd = ipc_listen();
    if (lfd == -2) { fprintf(stderr, "pacreactd: already running\n"); return 1; }
    if (lfd == -1) { fprintf(stderr, "pacreactd: cannot make its socket (is $XDG_RUNTIME_DIR short enough and yours?)\n"); return 1; }
    if (loop_init() < 0 || loop_add(lfd, EPOLLIN, accept_clients, (void *)(long)lfd) < 0) {
        perror("pacreactd");
        return 1;
    }
    server_init();
    bus_set("daemon.version", VERSION);
    bus_set("daemon.protocol", PROTOCOL_VERSION);
    /* The sources first, so that what they find is in the first snapshot; then the uevents they share. */
    power_init();
    mem_init();
    temp_init();
    net_init();
    camera_init();
    usb_init();
    input_init();
    disk_init();
    backlight_init();
    locks_init();
    probe_init();
#ifndef NO_SDBUS
    print_init();
    bt_init();
    wifi_init();
#endif
    jack_init();
    audio_init();
    uevent_init();

    struct sigaction sa = { .sa_handler = on_signal };   /* no SA_RESTART: the wait must return */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    while (!quit)
        if (loop_wait(-1) < 0) { perror("pacreactd"); break; }
#ifndef NO_SDBUS
    print_shutdown();
    wifi_shutdown();
#endif
    ipc_unlink();
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return run();
    if (!strcmp(argv[1], "--version")) { puts("pacreactd " VERSION); return 0; }
    if (!strcmp(argv[1], "--help")) { fputs(USAGE, stdout); return 0; }
    int r;
    if (!strcmp(argv[1], "watch")) r = ipc_watch(argc - 2, argv + 2);
    else if (!strcmp(argv[1], "get") && argc == 3) r = ipc_get(argv[2]);
#ifndef NO_SDBUS
    else if (!strcmp(argv[1], "print") && argc == 2) return print_queue();
#endif
    else { fputs(USAGE, stderr); return 2; }
    if (r < 0) fprintf(stderr, "pacreactd: not running\n");
    return r < 0 ? 1 : r;
}
