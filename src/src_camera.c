#include "loop.h"
#include "server.h"
#include "setting.h"
#include "util.h"
#include "sources.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include "tunables.h"

/* Is a webcam in use? The kernel does not say, so: an inotify watch on each /dev/videoN node wakes this up when one is
 * opened or closed, and then the files the user's programs hold open (/proc/PID/fd) say whether one still is. Nothing
 * is read while no camera is touched. Only the user's own programs can be seen, which is the point: it is their camera.
 * camera = 0 turns it off.
 *   camera.ID           a camera: its name, then node=video0 (the node that captures). It is there from its being plugged until it is not.
 *   camera.ID.active    on while a program of the user's has it open, else off */

static int notify_fd = -1, debounce_fd = -1, dev_wd = -1;
static char nodes[MAX_CAMERAS][16];   /* "video0": the capture nodes being watched */
static int node_wd[MAX_CAMERAS], n_nodes;

/* "video0", "video12": a video node, whatever it is for. */
bool camera_node_name(const char *name) {
    if (strncmp(name, "video", 5) || !name[5] || strlen(name) >= sizeof nodes[0]) return false;
    for (const char *c = name + 5; *c; c++)
        if (*c < '0' || *c > '9') return false;
    return true;
}

/* A camera has one node that captures and others for its metadata, which a program opens on its own account: only the
 * first, whose index is 0, counts. A node with no index to read is counted. */
static bool capture_node(const char *sys_root, const char *name) {
    char path[PATH_MAX], text[16] = "";
    snprintf(path, sizeof path, "%s/%s/index", sys_root, name);
    FILE *f = fopen(path, "r");
    if (!f) return true;
    bool ok = fgets(text, sizeof text, f) && atoi(text) == 0;
    fclose(f);
    return ok;
}

static const char *sys_video = "/sys/class/video4linux";   /* a test points it at a tree of its own */
static struct { char node[16]; char id[12]; } cameras[MAX_CAMERAS];   /* the cameras there are, by the node that captures */

/* What the camera calls itself: the name its driver gives the node. */
static void camera_name(const char *node, char *out, size_t n) {
    char dir[PATH_MAX];
    out[0] = '\0';
    if (snprintf(dir, sizeof dir, "%s/%s", sys_video, node) < (int)sizeof dir) util_read_text(dir, "name", out, n);
}

static bool in_nodes(const char *node) {
    for (int i = 0; i < n_nodes; i++)
        if (!strcmp(nodes[i], node)) return true;
    return false;
}

/* The cameras as devices, for the nodes there are: one that is gone goes whole, one that is new comes with an ID of its own. */
static void sync_cameras(void) {
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (!cameras[i].id[0] || in_nodes(cameras[i].node)) continue;
        char key[32];
        snprintf(key, sizeof key, "camera.%s", cameras[i].id);
        bus_unset_tree(key);
        memset(&cameras[i], 0, sizeof cameras[i]);
    }
    for (int n = 0; n < n_nodes; n++) {
        bool known = false;
        int slot = -1;
        for (int i = 0; i < MAX_CAMERAS; i++) {
            if (cameras[i].id[0] && !strcmp(cameras[i].node, nodes[n])) known = true;
            if (!cameras[i].id[0] && slot < 0) slot = i;
        }
        if (known || slot < 0 || !bus_new_id("camera", cameras[slot].id, sizeof cameras[slot].id)) continue;
        snprintf(cameras[slot].node, sizeof cameras[slot].node, "%.15s", nodes[n]);
        char name[96], attrs[32], key[40];
        camera_name(nodes[n], name, sizeof name);
        snprintf(attrs, sizeof attrs, "node=%.15s", nodes[n]);
        bus_device("camera", cameras[slot].id, name, attrs);
        snprintf(key, sizeof key, "camera.%s.active", cameras[slot].id);
        bus_set(key, "off");
    }
}

/* Which cameras a program of the user's holds open. */
static void look(void) {
    char names[MAX_CAMERAS][16];
    int which[MAX_CAMERAS], n = 0;
    for (int i = 0; i < MAX_CAMERAS; i++) {
        if (!cameras[i].id[0]) continue;
        snprintf(names[n], sizeof names[n], "%.15s", cameras[i].node);
        which[n++] = i;
    }
    if (!n) return;
    unsigned held = dev_in_use("/proc", n, names, getpid());   /* one walk of the processes for every camera */
    for (int k = 0; k < n; k++) {
        char key[40];
        snprintf(key, sizeof key, "camera.%s.active", cameras[which[k]].id);
        bus_set(key, held >> k & 1 ? "on" : "off");
    }
}

static void arm(void) {
    struct itimerspec its = { .it_value = { .tv_nsec = CAMERA_DEBOUNCE_MS * 1000000L } };
    timerfd_settime(debounce_fd, 0, &its, NULL);
}

static void forget_nodes(void) {
    for (int i = 0; i < n_nodes; i++) inotify_rm_watch(notify_fd, node_wd[i]);
    n_nodes = 0;
}

/* The capture nodes there are now, each with a watch of its own. */
static void scan(void) {
    forget_nodes();
    DIR *d = opendir("/dev");
    if (!d) { sync_cameras(); return; }
    struct dirent *e;
    while (n_nodes < MAX_CAMERAS && (e = readdir(d))) {
        char path[PATH_MAX];
        if (!camera_node_name(e->d_name) || !capture_node(sys_video, e->d_name)) continue;
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        int wd = inotify_add_watch(notify_fd, path, IN_OPEN | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE);
        if (wd < 0) continue;
        snprintf(nodes[n_nodes], sizeof nodes[0], "%.15s", e->d_name);
        node_wd[n_nodes++] = wd;
    }
    closedir(d);
    sync_cameras();
}

static void on_notify(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len;
    bool changed = false;
    while ((len = read(notify_fd, buf, sizeof buf)) > 0) {
        for (char *p = buf; p < buf + len;) {
            struct inotify_event *e = (struct inotify_event *)p;
            if (e->wd == dev_wd) changed = true;   /* a node came or went */
            p += sizeof *e + e->len;
        }
    }
    if (changed) scan();
    arm();   /* an opening and a closing come in bursts (a program probing the device): one look, a moment after */
}

static void on_debounce(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    (void)!read(debounce_fd, &n, sizeof n);
    look();
}

void camera_init(void) {
    const char *off = pac_setting("camera");
    if (off && !strcmp(off, "0")) return;
    notify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    debounce_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (notify_fd < 0 || debounce_fd < 0) return;
    dev_wd = inotify_add_watch(notify_fd, "/dev", IN_CREATE | IN_DELETE);   /* only a camera's coming and going */
    scan();
    loop_add(notify_fd, EPOLLIN, on_notify, NULL);
    loop_add(debounce_fd, EPOLLIN, on_debounce, NULL);
    look();
}
