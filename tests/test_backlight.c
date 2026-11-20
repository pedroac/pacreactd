/* The brightness: the percentage of the maximum, published when the attribute changes. src_backlight.c is included for its statics. */
#include "recorder.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/src_backlight.c"

static void write_attr(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

int main(void) {
    char path[] = "/tmp/pacreactd-backlight-XXXXXX";
    int tmp = mkstemp(path);
    assert(tmp >= 0);
    close(tmp);
    write_attr(path, "50\n");
    fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    max_brightness = 200;
    last = -1;

    on_change(NULL, 0);
    assert(!strcmp(rec("backlight.level"), "25"));
    int n = rec_changes("backlight.level");
    on_change(NULL, 0);                                         /* the same again: nothing said */
    assert(rec_changes("backlight.level") == n);
    write_attr(path, "200\n");
    on_change(NULL, 0);
    assert(!strcmp(rec("backlight.level"), "100"));
    write_attr(path, "0\n");
    on_change(NULL, 0);
    assert(!strcmp(rec("backlight.level"), "0"));
    write_attr(path, "junk\n");                                 /* unreadable: nothing */
    on_change(NULL, 0);
    assert(!strcmp(rec("backlight.level"), "0"));

    close(fd);
    unlink(path);
    puts("backlight: ok");
    return 0;
}
