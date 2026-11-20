#include "state.h"
#include "tunables.h"
#include <stdio.h>
#include <string.h>

static struct { char key[KEY_LEN], value[VALUE_LEN]; } table[STATE_MAX];
static int count;

bool state_key_valid(const char *key) {
    size_t n = strlen(key);
    if (n == 0 || n >= KEY_LEN) return false;
    bool dot = false;
    for (size_t i = 0; i < n; i++) {
        char c = key[i];
        if (c == '.') {
            if (i == 0 || key[i - 1] == '.' || i == n - 1) return false;
            dot = true;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    char ns[NS_LEN];
    return dot && state_namespace(key, ns);
}

/* The length of the UTF-8 sequence at s (n bytes left), or 0 if it is not a well-formed one: no overlong forms, no surrogates,
 * nothing above U+10FFFF. */
static size_t utf8_len(const unsigned char *s, size_t n) {
    if (s[0] < 0x80) return 1;
    size_t need = s[0] >= 0xF0 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC2 ? 2 : 0;
    if (!need || n < need || s[0] > 0xF4) return 0;
    for (size_t i = 1; i < need; i++)
        if ((s[i] & 0xC0) != 0x80) return 0;
    if (s[0] == 0xE0 && s[1] < 0xA0) return 0;                 /* overlong */
    if (s[0] == 0xED && s[1] >= 0xA0) return 0;                /* a surrogate */
    if (s[0] == 0xF0 && s[1] < 0x90) return 0;                 /* overlong */
    if (s[0] == 0xF4 && s[1] >= 0x90) return 0;                /* above U+10FFFF */
    return need;
}

static bool text_valid(const char *text, size_t n) {
    if (n == 0 || n >= VALUE_LEN || text[0] == ' ' || text[n - 1] == ' ') return false;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7f) return false;
        size_t l = utf8_len((const unsigned char *)text + i, n - i);
        if (!l) return false;
        i += l;
    }
    return true;
}

bool state_text_valid(const char *text) {
    return text_valid(text, strlen(text));
}

/* One attribute: a lower-case name, "=", a word. */
static bool attr_valid(const char *a, size_t n) {
    size_t eq = 0;
    while (eq < n && a[eq] >= 'a' && a[eq] <= 'z') eq++;
    if (eq == 0 || eq > 16 || eq == n || a[eq] != '=') return false;
    size_t w = n - eq - 1;
    if (w == 0 || w > 16) return false;
    for (size_t i = eq + 1; i < n; i++)
        if (!((a[i] >= 'a' && a[i] <= 'z') || (a[i] >= '0' && a[i] <= '9') || a[i] == '-')) return false;
    return true;
}

bool state_value_valid(const char *value) {
    size_t n = strlen(value);
    if (n >= VALUE_LEN) return false;
    size_t text = strcspn(value, "\t");
    if (text == 0 ? value[0] != '\t' : !text_valid(value, text)) return false;   /* a value may be attributes only: a device with no name */
    for (const char *p = value + text; *p;) {
        p++;                                                   /* the TAB */
        size_t l = strcspn(p, "\t");
        if (!attr_valid(p, l)) return false;
        p += l;
    }
    return true;
}

void state_text_clean(char *text) {
    size_t n = strlen(text), o = 0;
    bool space = true;                                         /* the start: no space there */
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)text[i];
        size_t l = c < 0x20 || c == 0x7f ? 0 : utf8_len((const unsigned char *)text + i, n - i);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!space) text[o++] = ' ';
            space = true;
            i++;
            continue;
        }
        space = false;
        if (!l) { text[o++] = '?'; i++; continue; }
        for (size_t k = 0; k < l; k++) text[o++] = text[i + k];
        i += l;
    }
    while (o && text[o - 1] == ' ') o--;
    text[o] = '\0';
}

bool state_namespace(const char *key, char *out) {
    size_t n = strcspn(key, ".");
    if (n == 0 || n >= NS_LEN) return false;
    memcpy(out, key, n);
    out[n] = '\0';
    return true;
}

static int find(const char *key) {
    for (int i = 0; i < count; i++)
        if (!strcmp(table[i].key, key)) return i;
    return -1;
}

int state_set(const char *key, const char *value) {
    if (!state_key_valid(key) || !state_value_valid(value)) return -1;
    int i = find(key);
    if (i < 0) {
        if (count == STATE_MAX) return -1;
        i = count++;
        snprintf(table[i].key, sizeof table[i].key, "%s", key);
    } else if (!strcmp(table[i].value, value)) {
        return 0;
    }
    snprintf(table[i].value, sizeof table[i].value, "%s", value);
    return 1;
}

static void remove_at(int i) {
    table[i] = table[--count];
}

bool state_unset(const char *key) {
    int i = find(key);
    if (i < 0) return false;
    remove_at(i);
    return true;
}

const char *state_get(const char *key) {
    int i = find(key);
    return i < 0 ? NULL : table[i].value;
}

void state_each(void (*cb)(const char *, const char *, void *), void *ctx) {
    for (int i = 0; i < count; i++) cb(table[i].key, table[i].value, ctx);
}

void state_text_cut(char *text, size_t max) {
    if (strlen(text) <= max) return;
    if (max < 3) {   /* no room for the ellipsis: just cut, not inside a UTF-8 sequence */
        while (max > 0 && ((unsigned char)text[max] & 0xC0) == 0x80) max--;
        text[max] = '\0';
        return;
    }
    size_t cut = max - 3;
    while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) cut--;   /* not inside a UTF-8 sequence */
    while (cut > 0 && text[cut - 1] == ' ') cut--;
    snprintf(text + cut, 4, "…");
}

int state_count_namespace(const char *ns) {
    size_t n = strlen(ns);
    int found = 0;
    for (int i = 0; i < count; i++)
        if (!strncmp(table[i].key, ns, n) && table[i].key[n] == '.') found++;
    return found;
}

bool state_take_namespace(const char *ns, char *key) {
    size_t n = strlen(ns);
    for (int i = 0; i < count; i++) {
        if (strncmp(table[i].key, ns, n) || table[i].key[n] != '.') continue;
        memcpy(key, table[i].key, KEY_LEN);
        remove_at(i);
        return true;
    }
    return false;
}

void state_clear(void) {
    count = 0;
}
