#ifndef IPC_H
#define IPC_H

/* Daemon side. Returns the listening fd, -1 on error, -2 if another daemon already runs. If systemd started us for a socket it
 * holds (pacreactd.socket), that socket is the one: it is not made, and not removed at exit. */
int ipc_listen(void);
/* Removes the socket file; at exit (not one that was handed to us). */
void ipc_unlink(void);
/* The listening socket systemd handed over (LISTEN_PID is ours, LISTEN_FDS says there is one, and the descriptor `first` is a
 * listening SOCK_SEQPACKET Unix socket), made non-blocking, or -1. The variables are cleared, so that no child sees them. */
int ipc_activated(int first);

/* Client side: subscribes to the prefixes (none: everything) and prints each line until the daemon
 * goes away. Returns 0 then, 1 if it refused, -1 if no daemon is reachable. */
int ipc_watch(int n_prefixes, char **prefixes);
/* Client side: prints the value of a key. 0 on success, 1 if there is no such key, -1 if no daemon. */
int ipc_get(const char *key);

#endif
