#ifndef SOURCES_H
#define SOURCES_H

#include <stdbool.h>
#include <stddef.h>

/* Each source registers its fds with the loop and publishes with bus_set(). A source whose kernel
 * interface is missing (no battery, no PSI, ...) quietly does nothing. They are started before the
 * uevent socket is opened (uevent_init), so that what they find at start-up is in the first snapshot. */
void power_init(void);   /* power.battery, power.battery.status, power.ac: sysfs, and kernel uevents */
void mem_init(void);     /* mem.pressure and mem.available; cpu.pressure and io.pressure: PSI triggers */
void net_signal_external(int percent);   /* the wifi signal as NetworkManager or iwd say it (src_wifi.c), or -1 */
void net_init(void);     /* net.online, net.device, net.link, net.signal: rtnetlink */
void jack_init(void);    /* audio.jack.*: the kernel's jack switches, and the sound server's ports to say what was plugged */
void jack_describe(char *out, size_t n);   /* what is plugged in, as words each led by a space: " headset", " none"... */
void audio_init(void);   /* audio.volume, audio.mute, audio.mic.*, audio.output: the sound server's native protocol */
/* Which jack-type card ports (headphones, headset or plain microphone, line out) went from absent to
 * present (+1) or back (-1) since the last answer; 0 for no change. */
typedef struct { int headphones, headset, mic, lineout; } PortChanges;
void audio_port_changes(void (*cb)(const PortChanges *));   /* asks; answers with NULL when the server cannot say */
/* A 3.5 mm jack was plugged or unplugged: the output that moves with it is its doing, not a change of its own. */
void audio_jack_plugged(void);
void input_init(void);   /* input.<id>: keyboards, mice, touchpads, tablets, gamepads (kernel uevents and sysfs) */
void usb_init(void);     /* usb.<role>, usb.event: USB devices connecting (kernel uevents and sysfs) */
void usb_uevent(const char *msg, size_t len);   /* one kernel uevent (the usb source subscribes to them itself; tests call this) */
#ifndef NO_SDBUS
void print_init(void);   /* printer.state, printer.event: print jobs and printer problems, from CUPS (needs sd-bus) */
void print_shutdown(void);   /* gives CUPS its subscription back */
int print_queue(void);   /* `pacreactd print`: the jobs that have not finished, asked of CUPS now; 0, or 1 if it cannot be reached */
#endif
void disk_init(void);    /* disk.NAME: free space of the filesystems on block devices; a slow timer, and mount table changes */
void backlight_init(void);   /* backlight.level: sysfs notifications */
void locks_init(void);   /* lock.caps, lock.num: the keyboards' LEDs (opt-in: lock_keys = 1) */
#ifndef NO_SDBUS
void bt_init(void);      /* bt.event: Bluetooth devices connecting (BlueZ, over sd-bus) */
void wifi_init(void);    /* the wifi signal, as NetworkManager or iwd say it, for net.signal (over sd-bus) */
void wifi_shutdown(void);   /* iwd is told it need not call us any more */
#endif
void probe_init(void);   /* net.probe, net.latency: opt-in ICMP probe (probe_host) */
void camera_init(void);  /* camera.active: is a webcam in use (watches /dev/video* for opens and closes) */
void temp_init(void);    /* temp.cpu and temp.ssd: hwmon sensors, thermal events, an adaptive timer */

#endif
