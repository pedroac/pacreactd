/* The server against real socket pairs; the loop is a stub that remembers each client's handler. */
#include "loop.h"
#include "server.h"
#include "state.h"
#include "tunables.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAXFD 64
static struct { LoopFn fn; void *ctx; } handlers[MAXFD];

int loop_add(int fd, uint32_t events, LoopFn fn, void *ctx) { (void)events; handlers[fd].fn = fn; handlers[fd].ctx = ctx; return 0; }
int loop_remove(int fd) { handlers[fd].fn = NULL; return 0; }

typedef struct { int fd, srv; } Peer;   /* fd: the program's end; srv: the daemon's */

static Peer connect_peer(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sv) == 0);
    server_add_client(sv[1]);
    return (Peer){ sv[0], sv[1] };
}

static void say(Peer p, const char *msg) {
    assert(send(p.fd, msg, strlen(msg), 0) >= 0);
    handlers[p.srv].fn(handlers[p.srv].ctx, 0);
}

/* The next datagram the daemon sent, or "" if there is none. */
static const char *hear(Peer p) {
    static char buf[MSG_MAX + 1];
    ssize_t n = recv(p.fd, buf, MSG_MAX, MSG_DONTWAIT);
    buf[n < 0 ? 0 : n] = '\0';
    return buf;
}

static void expect(Peer p, const char *want) {
    const char *got = hear(p);
    if (strcmp(got, want)) { fprintf(stderr, "wanted \"%s\", got \"%s\"\n", want, got); assert(0); }
}

static void drain(Peer p) { while (*hear(p)) {} }

static const char *ask(Peer p, const char *msg) { say(p, msg); return hear(p); }

int main(void) {
    server_init();
    state_clear();
    assert(bus_set("daemon.version", "9"));

    Peer a = connect_peer();
    expect(a, "");
    assert(!strcmp(ask(a, "get daemon.version"), "ok 9"));
    assert(!strcmp(ask(a, "get nope.x"), "error unknown key"));
    assert(!strcmp(ask(a, "frobnicate"), "error unknown command"));

    /* A snapshot, then sync, then changes; the same value again says nothing. */
    say(a, "subscribe");
    expect(a, "ok"); expect(a, "daemon.version 9"); expect(a, "sync"); expect(a, "");
    bus_set("mem.pressure", "on\tlevel=warning");
    expect(a, "mem.pressure on\tlevel=warning"); expect(a, "");
    bus_set("mem.pressure", "on\tlevel=warning");
    expect(a, "");
    bus_set("mem.pressure", "off");
    expect(a, "mem.pressure off");
    bus_unset("mem.pressure");
    expect(a, "mem.pressure");
    bus_unset("mem.pressure");
    expect(a, "");
    assert(!bus_set("bad key", "x") && !bus_set("mem.x", ""));

    /* Prefixes: "mem" is mem.*, not memory.*. */
    Peer b = connect_peer();
    say(b, "subscribe mem temp");
    expect(b, "ok"); expect(b, "sync");
    bus_set("memory.x", "1"); bus_set("mem.pressure", "on"); bus_set("temp.cpu", "70"); bus_set("net.online", "on");
    expect(b, "mem.pressure on"); expect(b, "temp.cpu 70"); expect(b, "");
    assert(!strcmp(ask(b, "subscribe Mem"), "error bad prefix"));
    assert(!strcmp(ask(b, "subscribe a b c d e f g h i j k l m n o p q"), "error too many prefixes"));
    assert(!strcmp(ask(b, "subscribe a b c d e f g h i j k l m n o p"), "ok"));
    expect(b, "sync");
    say(b, "subscribe net");                                 /* again: a new filter and a new snapshot */
    expect(b, "ok"); expect(b, "net.online on"); expect(b, "sync");

    drain(a);   /* a heard all of that too */

    /* Namespaces: reserved ones are the daemon's; a free one goes to the first to ask. */
    Peer c = connect_peer();
    assert(!strcmp(ask(c, "own mem"), "error reserved"));
    assert(!strcmp(ask(c, "own net"), "error reserved") && !strcmp(ask(c, "own power"), "error reserved"));
    assert(!strcmp(ask(c, "own a.b"), "error bad namespace"));
    assert(!strcmp(ask(c, "publish notifications.count 3"), "error not owner"));
    assert(!strcmp(ask(c, "own notifications"), "ok"));
    assert(!strcmp(ask(c, "own notifications"), "ok"));
    assert(!strcmp(ask(c, "publish mem.pressure on"), "error not owner"));
    assert(!strcmp(ask(c, "publish notifications.count 3"), "ok"));
    expect(a, "notifications.count 3");
    assert(!strcmp(ask(c, "publish notifications.last two words"), "ok"));
    expect(a, "notifications.last two words");
    assert(!strcmp(ask(c, "publish notifications.count"), "error bad value"));
    assert(!strcmp(ask(c, "publish notifications."), "error bad key"));
    assert(!strcmp(ask(c, "retract notifications.last"), "ok"));
    expect(a, "notifications.last");

    /* A program publishes attributes like the daemon's own sources; free text cannot pose as one; a bad one is refused */
    assert(!strcmp(ask(c, "publish notifications.count 3\tlevel=warning"), "ok"));
    expect(a, "notifications.count 3\tlevel=warning");
    assert(!strcmp(ask(c, "publish notifications.count 3\tlevel"), "error bad value"));
    assert(!strcmp(ask(c, "publish notifications.count 3\t"), "error bad value"));
    assert(!strcmp(ask(c, "publish notifications.count 3 level=critical"), "ok"));         /* just text */
    expect(a, "notifications.count 3 level=critical");
    assert(!strcmp(ask(c, "publish notifications.count \xff"), "error bad value"));          /* not UTF-8 */
    assert(!strcmp(ask(c, "publish notifications.count  3"), "error bad value"));           /* a space at the start */
    assert(!strcmp(ask(c, "publish notifications.count 3"), "ok"));
    expect(a, "notifications.count 3");

    /* No one uses the table up: a program has a few keys, not as many as it likes (but changing one it has is fine) */
    for (int i = 0; i < MAX_CLIENT_KEYS - 1; i++) {      /* notifications.count and notifications.last are already two keys of c... */
        char msg[64];
        snprintf(msg, sizeof msg, "publish notifications.k%d 1", i);
        const char *r = ask(c, msg);
        if (strcmp(r, "ok")) { assert(!strcmp(r, "error too many keys")); break; }
    }
    assert(!strcmp(ask(c, "publish notifications.one-too-many 1"), "error too many keys"));
    assert(!strcmp(ask(c, "publish notifications.count 4"), "ok"));                           /* one it has: allowed */
    assert(!strcmp(ask(c, "retract notifications.k0"), "ok"));
    assert(!strcmp(ask(c, "publish notifications.one-more 1"), "ok"));                       /* room again */
    for (int i = 1; i < MAX_CLIENT_KEYS - 1; i++) {
        char msg[64];
        snprintf(msg, sizeof msg, "retract notifications.k%d", i);
        ask(c, msg);
    }
    assert(!strcmp(ask(c, "retract notifications.one-more"), "ok"));
    drain(a);

    /* A device published by a program: a key, with keys under it; retracting the device takes them all, the others stay */
    assert(!strcmp(ask(c, "publish notifications.n1 Backup done\tkind=info"), "ok"));
    expect(a, "notifications.n1 Backup done\tkind=info");
    assert(!strcmp(ask(c, "publish notifications.n1.age 3"), "ok") && !strcmp(ask(c, "publish notifications.n10 Other"), "ok"));
    expect(a, "notifications.n1.age 3"); expect(a, "notifications.n10 Other");
    assert(!strcmp(ask(c, "publish notifications.n2 \tkind=nameless"), "ok"));           /* attributes only: no name */
    expect(a, "notifications.n2 \tkind=nameless");
    assert(!strcmp(ask(c, "retract notifications.n1"), "ok"));
    expect(a, "notifications.n1.age"); expect(a, "notifications.n1");                      /* what is under it first, then the device */
    assert(!state_get("notifications.n1.age") && state_get("notifications.n10"));
    ask(c, "retract notifications.n10"); ask(c, "retract notifications.n2");
    drain(a);

    /* An event: told to those who listen now, kept nowhere, not in a snapshot */
    Peer e = connect_peer();
    say(e, "subscribe printer"); expect(e, "ok"); expect(e, "sync");
    assert(bus_event("printer.p1", "job-done", "  Quarterly\treport.pdf "));
    expect(e, "event printer.p1 job-done Quarterly report.pdf");
    expect(a, "event printer.p1 job-done Quarterly report.pdf");                          /* a listens to everything */
    assert(!state_get("printer.p1"));                                                      /* nothing was kept */
    assert(bus_event("printer.p1", "ready", NULL));
    expect(e, "event printer.p1 ready"); expect(a, "event printer.p1 ready");
    assert(bus_event("net.x", "gone", "not for e"));
    expect(a, "event net.x gone not for e"); expect(e, "");                                          /* e did not ask for net */
    assert(!bus_event("bad key", "k", "x") && !bus_event("a.b", "Bad Kind", "x") && !bus_event("a.b", "", "x"));
    Peer f = connect_peer();
    say(f, "subscribe"); expect(f, "ok");
    for (const char *l; *(l = hear(f)) && strcmp(l, "sync");) assert(strncmp(l, "event", 5));   /* the snapshot has no events */
    close(f.fd); handlers[f.srv].fn(handlers[f.srv].ctx, 0);
    close(e.fd); handlers[e.srv].fn(handlers[e.srv].ctx, 0);
    drain(a);

    /* A program that starts an hour after the devices were plugged in, and after the notifications came, is told what is there now: the
     * devices and the count are keys, so they are in its snapshot; what only happened once (an event) is not, and cannot be missed by
     * anything that needed it */
    assert(bus_device("usb", "u9", "SanDisk Ultra", "kind=storage"));
    bus_set("usb.u9.mounted", "on");
    assert(!strcmp(ask(c, "publish notifications.count 3"), "ok"));
    drain(a);
    bus_event("usb.u9", "ejected", "SanDisk Ultra");                    /* it happened before the program came */
    drain(a);
    Peer late = connect_peer();
    say(late, "subscribe usb notifications");
    expect(late, "ok");
    const char *seen[8];
    int n_seen = 0;
    static char lines[8][MSG_MAX];
    for (const char *l; *(l = hear(late)) && strcmp(l, "sync") && n_seen < 8;) { snprintf(lines[n_seen], MSG_MAX, "%s", l); seen[n_seen] = lines[n_seen]; n_seen++; }
    bool device = false, prop = false, count = false;
    for (int i = 0; i < n_seen; i++) {
        device |= !strcmp(seen[i], "usb.u9 SanDisk Ultra\tkind=storage");
        prop |= !strcmp(seen[i], "usb.u9.mounted on");
        count |= !strcmp(seen[i], "notifications.count 3");
        assert(strncmp(seen[i], "event", 5));
    }
    assert(device && prop && count);
    close(late.fd); handlers[late.srv].fn(handlers[late.srv].ctx, 0);
    bus_unset_tree("usb.u9");
    drain(a);

    Peer d = connect_peer();
    assert(!strcmp(ask(d, "own notifications"), "error taken"));

    /* The owner leaves: its keys go, and subscribers are told. */
    close(c.fd);
    handlers[c.srv].fn(handlers[c.srv].ctx, 0);
    expect(a, "notifications.count");
    expect(a, "");
    assert(state_get("notifications.count") == NULL);
    assert(!strcmp(ask(d, "own notifications"), "ok"));

    /* A program that vanishes without being read is dropped on the next send, never waited for. */
    close(a.fd);
    bus_set("mem.pressure", "off");
    assert(handlers[a.srv].fn == NULL);

    /* A message that is too long is refused, not cut. */
    char big[MSG_MAX + 40];
    memset(big, 'x', sizeof big); memcpy(big, "get ", 4); big[sizeof big - 1] = '\0';
    assert(!strcmp(ask(d, big), "error message too long"));

    /* No room for one more client: it is told, and closed. */
    Peer many[MAX_CLIENTS + 1];
    int n = 0;
    for (; n < MAX_CLIENTS + 1; n++) {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, sv) == 0);
        server_add_client(sv[1]);
        many[n] = (Peer){ sv[0], sv[1] };
        if (!handlers[sv[1]].fn) break;
    }
    assert(n < MAX_CLIENTS + 1);
    expect(many[n], "error too many clients");
    puts("server: ok");
    return 0;
}
