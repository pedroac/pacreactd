/* The socket systemd hands over (socket activation): taken only if it is meant for us and is what we need. */
#include "../src/ipc.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int listener(int type, bool listen_too, const char *path) {
    int fd = socket(AF_UNIX, type, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    unlink(path);
    assert(fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof a) == 0);
    if (listen_too) assert(listen(fd, 4) == 0);
    return fd;
}

static void hand_over(int fds, long pid) {
    char text[32];
    snprintf(text, sizeof text, "%ld", pid);
    setenv("LISTEN_PID", text, 1);
    snprintf(text, sizeof text, "%d", fds);
    setenv("LISTEN_FDS", text, 1);
    setenv("LISTEN_FDNAMES", "pacreactd", 1);
}

int main(void) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/pacreactd-ipc-%d.sock", (int)getpid());

    /* Nothing handed over */
    unsetenv("LISTEN_PID"); unsetenv("LISTEN_FDS");
    int fd = listener(SOCK_SEQPACKET, true, path);
    assert(ipc_activated(fd) == -1);

    /* Handed to us: taken, made non-blocking, and no child sees the variables */
    hand_over(1, getpid());
    assert(ipc_activated(fd) == fd);
    assert(fcntl(fd, F_GETFL) & O_NONBLOCK);
    assert(!getenv("LISTEN_PID") && !getenv("LISTEN_FDS") && !getenv("LISTEN_FDNAMES"));

    /* Meant for another process (a parent's, left in the environment): not ours */
    hand_over(1, getpid() + 1);
    assert(ipc_activated(fd) == -1 && !getenv("LISTEN_PID"));
    hand_over(0, getpid());                                      /* none handed over */
    assert(ipc_activated(fd) == -1);
    hand_over(1, getpid());
    assert(ipc_activated(-1) == -1 && ipc_activated(1000) == -1);   /* not a socket at all */
    close(fd);

    /* Not what we need: a stream socket, or one that does not listen */
    fd = listener(SOCK_STREAM, true, path);
    hand_over(1, getpid());
    assert(ipc_activated(fd) == -1);
    close(fd);
    fd = listener(SOCK_SEQPACKET, false, path);
    hand_over(1, getpid());
    assert(ipc_activated(fd) == -1);
    close(fd);

    unlink(path);
    puts("ipc: ok");
    return 0;
}
