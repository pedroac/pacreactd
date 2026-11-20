#ifndef TUNABLES_H
#define TUNABLES_H

/* What a person may want to change. Each can be set when building:
 *     make CFLAGS="-O2 -DMAX_CLIENTS=32" */

#ifndef MAX_CLIENTS
#define MAX_CLIENTS 16      /* programs connected at once: pacreact, pacbar, a few scripts */
#endif
#ifndef MAX_FILTERS
#define MAX_FILTERS 16      /* prefixes in one `subscribe` */
#endif
#ifndef MAX_OWNED
#define MAX_OWNED 8         /* namespaces one program may own: pacreact owns the sources that have not moved in yet */
#endif
#ifndef MAX_CLIENT_KEYS
#define MAX_CLIENT_KEYS 16  /* keys one program may publish in all its namespaces: no one can use the table up */
#endif
#ifndef STATE_MAX
#define STATE_MAX 128       /* keys the daemon can hold */
#endif
#ifndef KEY_LEN
#define KEY_LEN 32          /* with the NUL */
#endif
#ifndef NS_LEN
#define NS_LEN 16           /* the part of a key before its first dot, with the NUL */
#endif
#ifndef VALUE_LEN
#define VALUE_LEN 64        /* with the NUL */
#endif
#ifndef MSG_MAX
#define MSG_MAX 128         /* one datagram, either way */
#endif

/* The sources. Thresholds that were never settings in pacreact are build-time tunables here too. */
/* The sound server and the jacks. */
#ifndef MAX_FRAME
#define MAX_FRAME 60000             /* the largest packet of the sound server that is taken */
#endif
#ifndef SRC_AUDIO_DEBOUNCE_MS
#define SRC_AUDIO_DEBOUNCE_MS 40    /* coalesces a volume drag into few readings */
#endif
#ifndef JACK_MS
#define JACK_MS 1000                /* a jack event and the server's answer to it are this close */
#endif
#ifndef SRC_AUDIO_SETTLE_MS
#define SRC_AUDIO_SETTLE_MS 500     /* the server moves the port first and applies that port's volume a moment later */
#endif
#ifndef MAX_AUDIO_DEVICES
#define MAX_AUDIO_DEVICES 32        /* sinks and sources followed at once */
#endif

#ifndef MAX_PORTS
#define MAX_PORTS 64
#endif
#ifndef MAX_JACKS
#define MAX_JACKS 16
#endif
#ifndef MAX_PINS
#define MAX_PINS 32
#endif
#ifndef SRC_AUDIO_RETRY_MS
#define SRC_AUDIO_RETRY_MS 200      /* the sound server's socket was just made and is not listening yet: look again this soon, a few times */
#endif
#ifndef SRC_AUDIO_RETRIES
#define SRC_AUDIO_RETRIES 10
#endif

/* USB devices. */
#ifndef MAX_INPUT_DEVICES
#define MAX_INPUT_DEVICES 32        /* keyboards, mice and the like followed at once */
#endif

#ifndef MAX_DEVICES
#define MAX_DEVICES 64              /* devices followed at once */
#endif
#ifndef SRC_USB_SETTLE_MS
#define SRC_USB_SETTLE_MS 800       /* a device is read, and an unplug is told, this long after: what comes straight back is not news */
#endif

#ifndef SRC_USB_BUSY_MS
#define SRC_USB_BUSY_MS 300      /* a scanner opened and closed in bursts: one look, this long after the last */
#endif

/* The latency probe. */
#ifndef SRC_PROBE_DEFAULT_INTERVAL_S
#define SRC_PROBE_DEFAULT_INTERVAL_S 30   /* seconds between probes */
#endif
#ifndef SLOW_MS
#define SLOW_MS 400                 /* an answer later than this is a bad probe */
#endif
#ifndef TIMEOUT_MS
#define TIMEOUT_MS 2000             /* no answer by then is a bad probe */
#endif
#ifndef BAD_TO_ALERT
#define BAD_TO_ALERT 2              /* bad probes in a row before it is said */
#endif
#ifndef GOOD_TO_CLEAR
#define GOOD_TO_CLEAR 2             /* good ones in a row before it is said to be over */
#endif

/* Disk space. */
#ifndef DEFAULT_WARN_PCT
#define DEFAULT_WARN_PCT 10         /* free space, percent, under which a disk is low */
#endif
#ifndef DEFAULT_CRIT_PCT
#define DEFAULT_CRIT_PCT 5          /* ... and almost full */
#endif
#ifndef CRIT_BYTES
#define CRIT_BYTES (1ULL << 30)     /* a small filesystem is not almost full just because it is nearly empty in percent: it needs to be short in bytes too */
#endif
#ifndef REARM_MARGIN_PCT
#define REARM_MARGIN_PCT 3          /* a level is left only this far past its threshold */
#endif
#ifndef MAX_FS
#define MAX_FS 16                   /* filesystems followed */
#endif
#ifndef SRC_DISK_DEFAULT_INTERVAL_S
#define SRC_DISK_DEFAULT_INTERVAL_S 300   /* the one source with a timer to look: the kernel does not say a disk is filling */
#endif

/* The keyboards' lock LEDs. */
#ifndef MAX_KEYBOARDS
#define MAX_KEYBOARDS 8
#endif

#ifndef MAX_BT_DEVICES
#define MAX_BT_DEVICES 16           /* Bluetooth devices connected at once that are followed */
#endif

/* Printing. */
#ifndef MAX_PRINT_ACTIVE
#define MAX_PRINT_ACTIVE 16         /* jobs counted as being printed at once; more are not counted */
#endif
#ifndef LEASE_S
#define LEASE_S 86400               /* how long CUPS keeps our subscription: renewed at half of it */
#endif
#ifndef RETRY_MIN_S
#define RETRY_MIN_S 60              /* CUPS did not answer: a few more tries, this long and doubling; then its socket appearing is waited for */
#endif
#ifndef HTTP_TIMEOUT_S
#define HTTP_TIMEOUT_S 2            /* CUPS is on our own machine: it answers at once or not at all */
#endif
#ifndef MAX_PRINTERS
#define MAX_PRINTERS 8              /* printers whose problems are followed */
#endif
#ifndef NAME_MAX_SHOWN
#define NAME_MAX_SHOWN 80           /* a job's name in `pacreactd print`, cut to this */
#endif

/* The webcam. */
#ifndef MAX_CAMERAS
#define MAX_CAMERAS 8               /* capture nodes watched */
#endif
#ifndef CAMERA_DEBOUNCE_MS
#define CAMERA_DEBOUNCE_MS 300      /* an opening and a closing come in bursts (a program probing the device): one look, a moment after */
#endif

#ifndef LOW_PCT
#define LOW_PCT 10                  /* without PSI: memory is critical under this much available, percent */
#endif
#ifndef SRC_MEM_REARM_PCT
#define SRC_MEM_REARM_PCT 15        /* ... and is fine again above this */
#endif
#ifndef SRC_MEM_POLL_SECS
#define SRC_MEM_POLL_SECS 10        /* without PSI: how often MemAvailable is read */
#endif
#ifndef SRC_PSI_CLEAR_SECS
#define SRC_PSI_CLEAR_SECS 10       /* a pressure key goes back to "off" after this long without a trigger */
#endif
#ifndef SIGNAL_STEP_PCT
#define SIGNAL_STEP_PCT 5           /* a followed wifi signal is said when it has moved this much (percentage points) */
#endif
#ifndef SRC_NET_DEBOUNCE_MS
#define SRC_NET_DEBOUNCE_MS 1000    /* link flaps and resume bursts settle before the network is looked at */
#endif
#ifndef SRC_PSI_MIN_AVG10
#define SRC_PSI_MIN_AVG10 1.0       /* a trigger counts only if the kernel's own average of the last 10 s (percent) agrees: below the least a real one gives */
#endif
#ifndef SRC_PSI_CONFIRM_MS
#define SRC_PSI_CONFIRM_MS 2500     /* ... and a trigger that fires before the average has caught up is looked at again after this long (the average moves every 2 s) */
#endif
#ifndef WARN_PCT
#define WARN_PCT 15                 /* battery: warning at or below this percent while discharging */
#endif
#ifndef CRIT_PCT
#define CRIT_PCT 5                  /* ... critical at or below this */
#endif
#ifndef SRC_POWER_REARM_PCT
#define SRC_POWER_REARM_PCT 20      /* ... a level is kept until the battery is above this, or charging */
#endif
#ifndef SRC_POWER_POLL_SECS
#define SRC_POWER_POLL_SECS 60      /* the battery is read this often while discharging, in case an event is missed */
#endif
#ifndef SRC_TEMP_DEFAULT_INTERVAL_S
#define SRC_TEMP_DEFAULT_INTERVAL_S 120   /* the slowest look at the temperature sensors */
#endif
#ifndef MIN_INTERVAL_S
#define MIN_INTERVAL_S 10           /* the fastest */
#endif
#ifndef CPU_DEFAULT_LIMIT
#define CPU_DEFAULT_LIMIT 100       /* a CPU that does not say its own limit, degrees Celsius */
#endif
#ifndef CPU_DEFAULT_WARN
#define CPU_DEFAULT_WARN 85
#endif
#ifndef CPU_DEFAULT_HOT
#define CPU_DEFAULT_HOT 95
#endif
#ifndef SSD_DEFAULT_LIMIT
#define SSD_DEFAULT_LIMIT 80
#endif
#ifndef HYSTERESIS_C
#define HYSTERESIS_C 5              /* a temperature level is left only this far below its threshold */
#endif

_Static_assert(KEY_LEN > NS_LEN, "a key must have room for more than its namespace");
_Static_assert(KEY_LEN + VALUE_LEN <= MSG_MAX, "a state line (a level is part of the value) must fit in a message");
_Static_assert(STATE_MAX > 0 && MAX_CLIENTS > 0 && MAX_FILTERS > 0 && MAX_OWNED > 0, "the limits must be positive");

#endif
