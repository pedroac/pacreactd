#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

/* The settings file (pacreactd.conf, and the shared pac.conf): what is stored goes to pac_setting(). */
bool settings_apply(const char *key, const char *value, void *data);   /* a PacConfigHandler */
void settings_load(void);

#endif
