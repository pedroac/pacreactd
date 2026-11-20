#include "ipc.h"
#include "tunables.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* A directory only this user can enter: made if wanted, and refused when it exists but is not ours
 * and private (so nobody else can have planted or can reach what is in it). */
static int private_dir(const char *path, bool create) {
    if (create && mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) return -1;
    return 0;
}

/* $XDG_RUNTIME_DIR is private to the user by definition. Without it the socket goes in a private
 * directory of its own: a socket straight in the shared /tmp could be squatted by another user. */
static int sock_path(struct sockaddr_un *a, bool create) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    a->sun_family = AF_UNIX;
    if (dir && *dir) {
        if (snprintf(a->sun_path, sizeof a->sun_path, "%s/pacreactd.sock", dir) >= (int)sizeof a->sun_path) return -1;
        return 0;
    }
    char priv[64];
    snprintf(priv, sizeof priv, "/tmp/pacreactd-%u", (unsigned)geteuid());
    if (private_dir(priv, create) < 0) return -1;
    snprintf(a->sun_path, sizeof a->sun_path, "%s/pacreactd.sock", priv);
    return 0;
}

static int connect_daemon(void) {
    struct sockaddr_un a = {0};
    if (sock_path(&a, false) < 0) return -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

static bool handed_over;   /* the socket is systemd's: not ours to make or to remove */

int ipc_activated(int first) {
    const char *pid = getenv("LISTEN_PID"), *fds = getenv("LISTEN_FDS");
    int fd = -1;
    if (pid && fds && atol(pid) == (long)getpid() && atoi(fds) >= 1) {
        int type = 0, listening = 0;
        socklen_t len = sizeof type, llen = sizeof listening;
        struct sockaddr_un a;
        socklen_t alen = sizeof a;
        if (getsockopt(first, SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_SEQPACKET &&
            getsockopt(first, SOL_SOCKET, SO_ACCEPTCONN, &listening, &llen) == 0 && listening &&
            getsockname(first, (struct sockaddr *)&a, &alen) == 0 && a.sun_family == AF_UNIX) {
            fd = first;
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        }
    }
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    return fd;
}

/* The socket is made under another name, listening, and renamed into place: so the name never exists
 * without a daemon behind it, and a program that waits for the name to appear (inotify, IN_MOVED_TO)
 * can connect the moment it does. */
int ipc_listen(void) {
    int given = ipc_activated(3);   /* SD_LISTEN_FDS_START */
    if (given >= 0) { handed_over = true; return given; }
    struct sockaddr_un a = {0}, tmp = {0};
    if (sock_path(&a, true) < 0) return -1;
    tmp = a;
    if (snprintf(tmp.sun_path, sizeof tmp.sun_path, "%s.new", a.sun_path) >= (int)sizeof tmp.sun_path) return -1;

    int probe = connect_daemon();
    if (probe >= 0) { close(probe); return -2; }
    unlink(a.sun_path);  /* stale socket from a dead daemon */
    unlink(tmp.sun_path);

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    /* No one but this user may connect, whatever the umask. */
    mode_t old = umask(0177);
    int bound = bind(fd, (struct sockaddr *)&tmp, sizeof tmp);
    umask(old);
    if (bound < 0 || listen(fd, 8) < 0 || rename(tmp.sun_path, a.sun_path) < 0) {
        close(fd);
        unlink(tmp.sun_path);
        return -1;
    }
    return fd;
}

void ipc_unlink(void) {
    if (handed_over) return;
    struct sockaddr_un a = {0};
    if (sock_path(&a, false) == 0) unlink(a.sun_path);
}

/* Sends one message and reads the reply. Returns the length, or -1. */
static ssize_t ask(int fd, const char *msg, char *reply, size_t n) {
    if (send(fd, msg, strlen(msg), MSG_NOSIGNAL) < 0) return -1;
    ssize_t len = recv(fd, reply, n - 1, 0);
    if (len >= 0) reply[len] = '\0';
    return len;
}

static bool is_ok(const char *reply) {
    return !strncmp(reply, "ok", 2) && (reply[2] == '\0' || reply[2] == ' ');
}

int ipc_watch(int n, char **prefixes) {
    int fd = connect_daemon();
    if (fd < 0) return -1;
    char msg[MSG_MAX] = "subscribe", reply[MSG_MAX + 1];
    for (int i = 0; i < n; i++) {
        size_t used = strlen(msg);
        snprintf(msg + used, sizeof msg - used, " %s", prefixes[i]);
    }
    if (ask(fd, msg, reply, sizeof reply) < 0) { close(fd); return -1; }
    if (!is_ok(reply)) { fprintf(stderr, "pacreactd: %s\n", reply); close(fd); return 1; }
    ssize_t len;
    while ((len = recv(fd, reply, sizeof reply - 1, 0)) > 0) {
        reply[len] = '\0';
        puts(reply);
        fflush(stdout);
    }
    close(fd);
    return 0;
}

int ipc_get(const char *key) {
    int fd = connect_daemon();
    if (fd < 0) return -1;
    char msg[MSG_MAX], reply[MSG_MAX + 1];
    snprintf(msg, sizeof msg, "get %s", key);
    ssize_t len = ask(fd, msg, reply, sizeof reply);
    close(fd);
    if (len < 0) return -1;
    if (is_ok(reply)) { puts(reply[2] ? reply + 3 : ""); return 0; }
    fprintf(stderr, "pacreactd: %s\n", reply);
    return 1;
}
