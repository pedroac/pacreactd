#include "server.h"
#include "loop.h"
#include "state.h"
#include "tunables.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;                         /* -1 for a free slot */
    bool subscribed;
    int n_filters, n_owned;
    char filters[MAX_FILTERS][KEY_LEN];
    char owned[MAX_OWNED][NS_LEN];
} Client;

static Client clients[MAX_CLIENTS];

/* The namespaces of the daemon's own sources: no program can own them. The others are free, and
 * pacreact publishes the ones whose source has not moved here yet; when one does, it goes on this list. */
static const char *const reserved[] = { "daemon", "power", "mem", "cpu", "io", "temp", "net", "audio", "camera", "usb", "printer", "backlight", "lock", "disk", "bt", "input", "jack" };

static void drop(Client *c);

/* One datagram; a client that cannot take it (gone, or not reading) is dropped and false is returned. */
static bool send_text(Client *c, const char *text) {
    if (send(c->fd, text, strlen(text), MSG_NOSIGNAL | MSG_DONTWAIT) >= 0) return true;
    drop(c);
    return false;
}

/* "key value", or "key" alone when the key is gone. */
static bool send_state(Client *c, const char *key, const char *value) {
    char line[MSG_MAX];
    snprintf(line, sizeof line, value ? "%s %s" : "%s", key, value);
    return send_text(c, line);
}

/* Whether a key is under one of the prefixes: "mem" is "mem" and "mem.*", not "memory.*". */
static bool wanted(const Client *c, const char *key) {
    if (!c->n_filters) return true;
    for (int i = 0; i < c->n_filters; i++) {
        size_t n = strlen(c->filters[i]);
        if (!strncmp(key, c->filters[i], n) && (key[n] == '\0' || key[n] == '.')) return true;
    }
    return false;
}

static void broadcast(const char *key, const char *value) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].subscribed && wanted(&clients[i], key))
            send_state(&clients[i], key, value);
}

/* A source gave a key or a value that is not allowed (or the table is full): it is a bug of the source, and it says so, a few times. */
bool bus_set(const char *key, const char *value) {
    int r = state_set(key, value);
    if (r == 1) broadcast(key, value);
    if (r < 0) {
        static int said;
        if (said++ < 16) fprintf(stderr, "pacreactd: refused %s: not a valid key or value, or no room\n", key);
    }
    return r >= 0;
}

/* "event KEY KIND [ARG]" to the programs listening to KEY. Nothing is kept. */
bool bus_event(const char *key, const char *kind, const char *arg) {
    if (!state_key_valid(key)) return false;
    for (const char *k = kind; *k; k++)
        if (!((*k >= 'a' && *k <= 'z') || (*k >= '0' && *k <= '9') || *k == '-')) return false;
    if (!*kind || strlen(kind) > 24) return false;
    char name[VALUE_LEN * 2], line[MSG_MAX];
    snprintf(name, sizeof name, "%s", arg ? arg : "");
    state_text_clean(name);
    size_t head = (size_t)snprintf(line, sizeof line, "event %s %s", key, kind);
    size_t room = sizeof line - head - 2;
    if (room > VALUE_LEN - 1) room = VALUE_LEN - 1;
    state_text_cut(name, room);
    if (*name) snprintf(line + head, sizeof line - head, " %.*s", (int)room, name);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].subscribed && wanted(&clients[i], key)) send_text(&clients[i], line);
    return true;
}

bool bus_set_level(const char *key, const char *value, Level level) {
    static const char *const suffix[] = { "", "\tlevel=warning", "\tlevel=critical" };
    char full[VALUE_LEN];
    snprintf(full, sizeof full, "%s%s", value, suffix[level]);
    return bus_set(key, full);
}

void bus_unset(const char *key) {
    if (state_unset(key)) broadcast(key, NULL);
}

/* The program is gone: what it published goes with it. */
static void drop(Client *c) {
    if (c->fd < 0) return;
    loop_remove(c->fd);
    close(c->fd);
    c->fd = -1;
    c->subscribed = false;
    char key[KEY_LEN];
    for (int i = 0; i < c->n_owned; i++)
        while (state_take_namespace(c->owned[i], key)) broadcast(key, NULL);
    c->n_owned = 0;
}

static bool owns(const Client *c, const char *ns) {
    for (int i = 0; i < c->n_owned; i++)
        if (!strcmp(c->owned[i], ns)) return true;
    return false;
}

static bool owned_by_anyone(const char *ns) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && owns(&clients[i], ns)) return true;
    return false;
}

static bool name_ok(const char *s) {   /* what a prefix or a namespace may be made of */
    if (!*s) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '-' || *s == '_')) return false;
    return true;
}

struct snapshot { Client *c; bool ok; };

static void snapshot_one(const char *key, const char *value, void *ctx) {
    struct snapshot *s = ctx;
    if (s->ok && s->c->fd >= 0 && wanted(s->c, key)) s->ok = send_state(s->c, key, value);
}

static void cmd_subscribe(Client *c, char *args) {
    int n = 0;
    char filters[MAX_FILTERS][KEY_LEN];
    for (char *w = strtok(args, " "); w; w = strtok(NULL, " ")) {
        if (n == MAX_FILTERS) { send_text(c, "error too many prefixes"); return; }
        if (!name_ok(w) || strlen(w) >= KEY_LEN) { send_text(c, "error bad prefix"); return; }
        snprintf(filters[n++], KEY_LEN, "%s", w);
    }
    memcpy(c->filters, filters, sizeof filters);
    c->n_filters = n;
    c->subscribed = true;
    if (!send_text(c, "ok")) return;
    struct snapshot s = { c, true };
    state_each(snapshot_one, &s);
    if (s.ok) send_text(c, "sync");
}

static void cmd_own(Client *c, const char *ns) {
    char key[KEY_LEN + 8];
    snprintf(key, sizeof key, "%s.x", ns);
    char checked[NS_LEN];
    if (!name_ok(ns) || strchr(ns, '.') || !state_namespace(key, checked)) { send_text(c, "error bad namespace"); return; }
    for (size_t i = 0; i < sizeof reserved / sizeof *reserved; i++)
        if (!strcmp(ns, reserved[i])) { send_text(c, "error reserved"); return; }
    if (owns(c, ns)) { send_text(c, "ok"); return; }
    if (owned_by_anyone(ns)) { send_text(c, "error taken"); return; }
    if (c->n_owned == MAX_OWNED) { send_text(c, "error too many namespaces"); return; }
    snprintf(c->owned[c->n_owned++], NS_LEN, "%s", ns);
    send_text(c, "ok");
}

/* True if c may publish under key (which must be valid): says why if not. */
static bool may_publish(Client *c, const char *key) {
    char ns[NS_LEN];
    if (!state_key_valid(key) || !state_namespace(key, ns)) { send_text(c, "error bad key"); return false; }
    if (!owns(c, ns)) { send_text(c, "error not owner"); return false; }
    return true;
}

static void handle(Client *c, char *msg) {
    char none[1] = "";
    char *args = strchr(msg, ' ');
    if (args) *args++ = '\0'; else args = none;

    if (!strcmp(msg, "subscribe")) {
        cmd_subscribe(c, args);
    } else if (!strcmp(msg, "get")) {
        const char *v = state_get(args);
        char reply[MSG_MAX + 8];
        if (v) snprintf(reply, sizeof reply, "ok %s", v);
        send_text(c, v ? reply : "error unknown key");
    } else if (!strcmp(msg, "own")) {
        cmd_own(c, args);
    } else if (!strcmp(msg, "publish")) {
        char *value = strchr(args, ' ');
        if (value) *value++ = '\0';
        if (!may_publish(c, args)) return;
        if (!value || !state_value_valid(value)) { send_text(c, "error bad value"); return; }
        char ns[NS_LEN];
        state_namespace(args, ns);
        int mine = 0;
        for (int i = 0; i < c->n_owned; i++) mine += state_count_namespace(c->owned[i]);
        if (!state_get(args) && mine >= MAX_CLIENT_KEYS) { send_text(c, "error too many keys"); return; }
        if (!bus_set(args, value)) { send_text(c, "error no room"); return; }
        send_text(c, "ok");
    } else if (!strcmp(msg, "retract")) {
        if (!may_publish(c, args)) return;
        bus_unset_tree(args);                  /* a device goes whole */
        send_text(c, "ok");
    } else {
        send_text(c, "error unknown command");
    }
}

static void client_event(void *ctx, uint32_t events) {
    (void)events;   /* a hang-up shows as a read that returns 0 */
    Client *c = ctx;
    char buf[MSG_MAX + 1];
    while (c->fd >= 0) {
        ssize_t n = recv(c->fd, buf, MSG_MAX, MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        if (n <= 0) { drop(c); return; }
        if (n > MSG_MAX) { send_text(c, "error message too long"); continue; }
        buf[n] = '\0';
        handle(c, buf);
    }
}

void server_init(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
}

void server_add_client(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) continue;
        clients[i] = (Client){ .fd = fd };
        if (loop_add(fd, EPOLLIN, client_event, &clients[i]) < 0) { clients[i].fd = -1; break; }
        return;
    }
    static const char full[] = "error too many clients";
    (void)!send(fd, full, sizeof full - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    close(fd);
}

void server_accept(int listen_fd) {
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        /* Only this user's own processes, even if the socket's permissions are ever wrong. */
        struct ucred cred;
        socklen_t len = sizeof cred;
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || cred.uid != geteuid()) { close(fd); continue; }
        server_add_client(fd);
    }
}
