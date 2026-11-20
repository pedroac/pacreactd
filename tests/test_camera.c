/* The webcam: which names are video nodes, which of them capture, and who holds one open (a made-up /proc, so no
 * camera is needed). src_camera.c is included for its statics; the loop is stubbed. */
#include "../src/loop.h"
#include "../src/server.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/state.h"
static int n_sets;
bool bus_set(const char *key, const char *value) { n_sets++; return state_set(key, value) >= 0; }
bool bus_set_level(const char *key, const char *value, Level level) { (void)key; (void)value; (void)level; return true; }
void bus_unset(const char *key) { state_unset(key); }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }
int loop_remove(int fd) { (void)fd; return 0; }

#include "../src/src_camera.c"

static void test_names(void) {
    assert(camera_node_name("video0") && camera_node_name("video12"));
    assert(!camera_node_name("video") && !camera_node_name("video0x") && !camera_node_name("videoX"));
    assert(!camera_node_name("vcs1") && !camera_node_name("") && !camera_node_name("xvideo0"));
}

static void make_dir(const char *path) { assert(mkdir(path, 0700) == 0); }

static void test_capture_node(void) {
    char root[] = "/tmp/pacreact-camera-XXXXXX";
    assert(mkdtemp(root));
    char path[256];
    snprintf(path, sizeof path, "%s/video0", root); make_dir(path);
    snprintf(path, sizeof path, "%s/video0/index", root);
    FILE *f = fopen(path, "w"); fputs("0\n", f); fclose(f);
    snprintf(path, sizeof path, "%s/video1", root); make_dir(path);
    snprintf(path, sizeof path, "%s/video1/index", root);
    f = fopen(path, "w"); fputs("1\n", f); fclose(f);
    assert(capture_node(root, "video0"));
    assert(!capture_node(root, "video1"));       /* the metadata node */
    assert(capture_node(root, "video7"));        /* no index to read: counted */
    snprintf(path, sizeof path, "%s/video0/index", root); unlink(path);
    snprintf(path, sizeof path, "%s/video0", root); rmdir(path);
    snprintf(path, sizeof path, "%s/video1/index", root); unlink(path);
    snprintf(path, sizeof path, "%s/video1", root); rmdir(path);
    rmdir(root);
}

static void test_in_use(void) {
    char root[] = "/tmp/pacreact-proc-XXXXXX";
    assert(mkdtemp(root));
    char path[256], watched[2][16] = { "video0", "video2" };
    snprintf(path, sizeof path, "%s/100", root); make_dir(path);
    snprintf(path, sizeof path, "%s/100/fd", root); make_dir(path);
    snprintf(path, sizeof path, "%s/100/fd/3", root); assert(symlink("/dev/null", path) == 0);
    snprintf(path, sizeof path, "%s/100/fd/4", root); assert(symlink("socket:[123]", path) == 0);
    snprintf(path, sizeof path, "%s/self", root); make_dir(path);   /* not a process id: skipped */
    assert(!dev_in_use(root, 2, watched, -1));                   /* nothing holds a camera */

    snprintf(path, sizeof path, "%s/100/fd/5", root); assert(symlink("/dev/video2", path) == 0);
    assert(dev_in_use(root, 2, watched, -1));
    assert(!dev_in_use(root, 2, watched, 100));                  /* the program asking is not counted */
    assert(!dev_in_use(root, 1, watched, -1));                   /* video2 is not one of the nodes */
    snprintf(path, sizeof path, "%s/100/fd/6", root); assert(symlink("/dev/video0", path) == 0);
    assert(dev_in_use(root, 1, watched, -1));
    assert(dev_in_use(root, 2, watched, -1) == 3);               /* one walk says which: video0 and video2 are both held */
    snprintf(path, sizeof path, "%s/100/fd/6", root); unlink(path);
    assert(dev_in_use(root, 2, watched, -1) == 2);               /* only video2 (bit 1) */
    snprintf(path, sizeof path, "%s/100/fd/6", root); assert(symlink("/dev/video0", path) == 0);
    assert(!dev_in_use("/tmp/pacreact-no-such-proc", 2, watched, -1));

    for (int fd = 3; fd <= 6; fd++) { snprintf(path, sizeof path, "%s/100/fd/%d", root, fd); unlink(path); }
    snprintf(path, sizeof path, "%s/100/fd", root); rmdir(path);
    snprintf(path, sizeof path, "%s/100", root); rmdir(path);
    snprintf(path, sizeof path, "%s/self", root); rmdir(path);
    rmdir(root);
}

/* The cameras as devices: one for each capture node, with the camera's own name; an ID that stays while the node does, and goes with it */
static void test_devices(void) {
    char root[] = "/tmp/pacreactd-camera-XXXXXX";
    assert(mkdtemp(root));
    char path[256];
    snprintf(path, sizeof path, "%s/video0", root); assert(mkdir(path, 0700) == 0);
    snprintf(path, sizeof path, "%s/video0/name", root);
    FILE *f = fopen(path, "w"); assert(f); fputs("Integrated Camera: Integrated C\n", f); fclose(f);
    snprintf(path, sizeof path, "%s/video2", root); assert(mkdir(path, 0700) == 0);   /* a camera that does not say its name */
    sys_video = root;

    state_clear();
    snprintf(nodes[0], 16, "video0"); n_nodes = 1;
    sync_cameras();
    assert(!strcmp(cameras[0].id, "c1"));
    assert(!strcmp(state_get("camera.c1"), "Integrated Camera: Integrated C\tnode=video0") && !strcmp(state_get("camera.c1.active"), "off"));
    snprintf(nodes[1], 16, "video2"); n_nodes = 2;
    sync_cameras();
    assert(!strcmp(state_get("camera.c2"), "\tnode=video2") && !strcmp(state_get("camera.c1.active"), "off"));   /* a camera with no name: attributes only */
    int sets = n_sets;
    sync_cameras();                                                /* nothing changed: nothing said */
    assert(!strcmp(cameras[0].id, "c1") && !strcmp(cameras[1].id, "c2"));
    (void)sets;
    snprintf(nodes[0], 16, "video2"); n_nodes = 1;                 /* the first one is unplugged: it goes whole, the other stays as it was */
    sync_cameras();
    assert(!state_get("camera.c1") && !state_get("camera.c1.active") && state_get("camera.c2") && state_get("camera.c2.active"));
    snprintf(nodes[0], 16, "video0"); snprintf(nodes[1], 16, "video2"); n_nodes = 2;   /* plugged again: another camera, with a new ID */
    sync_cameras();
    assert(!strcmp(state_get("camera.c3"), "Integrated Camera: Integrated C\tnode=video0") && !state_get("camera.c1"));
    n_nodes = 0;
    sync_cameras();
    assert(!state_get("camera.c2") && !state_get("camera.c3"));

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
}

int main(void) {
    test_devices();
    test_names();
    test_capture_node();
    test_in_use();
    puts("camera: ok");
    return 0;
}
