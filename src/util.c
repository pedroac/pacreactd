#include "util.h"
#include "setting.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

long int_value(const char *name, const char *value, long def, long lo, long hi, const char *unit) {
    if (!value || !*value) return def;
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (end == value || *end || errno == ERANGE) {
        fprintf(stderr, "pacreactd: %s must be a whole number of %s, not \"%s\"; ignored\n", name, unit, value);
        return def;
    }
    if (n < lo || n > hi) {
        long to = n < lo ? lo : hi;
        fprintf(stderr, "pacreactd: %s=%ld is outside %ld to %ld; using %ld\n", name, n, lo, hi, to);
        return to;
    }
    return n;
}

long setting_int(const char *name, long def, long lo, long hi, const char *unit) {
    return int_value(name, pac_setting(name), def, lo, hi, unit);
}

/* Does a process (other than `skip`) of the tree under proc_root hold one of the nodes open? */
unsigned dev_in_use(const char *proc_root, int n, char names[][16], int skip) {
    DIR *procs = opendir(proc_root);
    if (!procs) return 0;
    unsigned all = n >= 32 ? ~0u : (1u << n) - 1, found = 0;
    struct dirent *p;
    while (found != all && (p = readdir(procs))) {
        if (p->d_name[0] < '0' || p->d_name[0] > '9' || atoi(p->d_name) == skip) continue;
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s/%s/fd", proc_root, p->d_name);
        DIR *fds = opendir(dir);
        if (!fds) continue;
        struct dirent *f;
        while (found != all && (f = readdir(fds))) {
            char link[PATH_MAX + 260], target[64];
            snprintf(link, sizeof link, "%s/%s", dir, f->d_name);
            ssize_t len = readlink(link, target, sizeof target - 1);
            if (len < 6) continue;
            target[len] = '\0';
            if (strncmp(target, "/dev/", 5)) continue;
            for (int i = 0; i < n && i < 32; i++)
                if (!strcmp(target + 5, names[i])) found |= 1u << i;
        }
        closedir(fds);
    }
    closedir(procs);
    return found;
}


bool util_read_text(const char *dir, const char *attr, char *out, size_t n) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", dir, attr) >= (int)sizeof path) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (!ok) return false;
    size_t len = strcspn(out, "\n");
    while (len && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '\r')) len--;
    out[len] = '\0';
    return true;
}
