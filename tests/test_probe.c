/* Network slowdown decision logic: what is said needs consecutive bad probes, recovery needs
 * consecutive good ones, and neither repeats. `shown`, `last` and `last_level` are what a listener that announces it
 * (pacreact) makes of net.probe and net.latency. */
#include "../src/server.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void probe_result(bool ok, int rtt_ms);

static char last[128], latency[16], probe[16];
static int shown;
static bool warned;

bool bus_set(const char *key, const char *value) {
    if (!strcmp(key, "net.latency")) { snprintf(latency, sizeof latency, "%s", value); return true; }
    if (strcmp(key, "net.probe")) return true;
    snprintf(probe, sizeof probe, "%s", value);
    if (!strcmp(value, "ok")) snprintf(last, sizeof last, "Network back to normal");
    else if (!strcmp(value, "slow")) snprintf(last, sizeof last, "Network slow — %s ms", latency);
    else snprintf(last, sizeof last, "Network not answering — no reply from the host");
    shown++;
    return true;
}
bool bus_set_level(const char *key, const char *value, Level level) {
    warned = level == LEVEL_WARNING;
    return bus_set(key, value);
}
int loop_add(int fd, unsigned ev, void *fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }

int main(void) {
    probe_result(true, 20);   assert(shown == 0);
    probe_result(true, 900);  assert(shown == 0);                 /* one slow probe is noise */
    probe_result(true, 30);   assert(shown == 0);                 /* and it resets the streak */
    probe_result(true, 900);
    probe_result(true, 800);  assert(shown == 1 && warned && !strcmp(probe, "slow"));
    assert(strstr(last, "slow") && strstr(last, "800"));
    probe_result(true, 700);  assert(shown == 1);                 /* no repeats while degraded */
    probe_result(false, 0);   assert(shown == 1);
    probe_result(true, 20);   assert(shown == 1);                 /* one good probe is not recovery */
    probe_result(true, 25);   assert(shown == 2 && !strcmp(last, "Network back to normal") && !strcmp(probe, "ok"));
    probe_result(true, 25);   assert(shown == 2);

    probe_result(false, 0);
    probe_result(false, 0);   assert(shown == 3 && strstr(last, "no reply") && warned && !strcmp(probe, "down"));

    puts("probe: ok");
    return 0;
}
