# The pacreactd protocol

How a program learns what pacreactd knows (a device changed, memory is short, the CPU is hot) and,
for a few namespaces, tells it something. It is small on purpose: a program that speaks it needs a
socket, a `recv` loop, `strchr` and `strcmp`; no library. Where this file and the code differ, one of them is a bug.

Version 1 (the key `daemon.protocol`).

## Who is trusted

**Every process of the same user is trusted; every other user is shut out.** The socket is in the user's runtime directory with
mode 0600, and the daemon checks `SO_PEERCRED` of each connection besides, so that a wrong mode could not open it. What the
protocol guards against is therefore *accidents* (two programs fighting for a key) and *what a device or a document says about
itself*, not a hostile program of the user's own, which could do much worse already.

What follows from that:

- **Names that outsiders chose are data, never instructions.** A USB stick, a printer, a print job, a Bluetooth device or a
  mount point is called whatever its maker or its owner liked. Every such string is cleaned before it is said (control
  characters and bytes that are not UTF-8 become `?`, spaces at the ends go) and cut to fit, so that it is always a valid value
  and cannot carry a newline, an escape sequence or an attribute of its own (see Values).
- **A program says only what it owns.** It cannot publish into the namespaces of the daemon's sources, nor into another program's,
  and it cannot fill the table: it has a few keys (16), not as many as it likes.
- **A program that does not listen is closed**, not waited for: the daemon never blocks on a client, and a client that is too
  slow loses its connection and must reconnect (it is then told the state again).
- **What is said is not secret from the user's own programs**: the names of devices, print jobs and sound outputs, and what
  is plugged in, can be read by any process of the user that connects. They can already be read from the system, and nothing
  is sent anywhere.

## Transport

- A `SOCK_SEQPACKET` Unix socket: `$XDG_RUNTIME_DIR/pacreactd.sock`, or, when that is not set,
  `/tmp/pacreactd-<uid>/pacreactd.sock` in a directory only the user can enter (a program that finds it not private and its own
  must not use it).
- One message is one datagram, at most 128 bytes. There is no framing to parse and no newline. A longer one is answered
  `error message too long` and not acted on.
- The socket is made under another name, listening, and renamed into place: the name never exists without a daemon behind it. A
  program that finds it missing can wait with inotify on the directory (`IN_MOVED_TO`) instead of trying on a timer, and connect the
  moment it shows. Under systemd the socket exists before the daemon does (`pacreactd.socket`), and connecting starts it.
- The text is UTF-8.

## Messages

Every message is a line of text: words separated by a single space, the last of a message that takes a value being all that is left of it.

```
state     = key SP value *( TAB attribute )        a key and what it is now
gone      = key                                    the key is gone
sync      = "sync"                                 the end of a snapshot
event     = "event" SP key SP kind [ SP name ]     something that happened once
reply     = "ok" [ SP text ] / "error" SP reason

key       = ns 1*( "." part )                      a lower-case word of a-z 0-9 - _ at each part, at most 31 bytes in all
ns        = part                                   at most 15 bytes: the namespace
value     = [ 1*63 UTF-8 characters, none a control character (a TAB, a newline, an escape...), no space at either end ]
            (it may be left out when there are attributes: a device that has no name)
attribute = name "=" word                          name: 1 to 16 of a-z; word: 1 to 16 of a-z 0-9 -
```

The whole of a value, attributes included, is at most 63 bytes. A **state** has a dot in its key and a value after the space; a
**reply** is `ok` or `error` and has no dot; so on a connection that sends commands and also hears the changes, the two are never
mistaken for each other. `sync` is a word with no dot and no value, and is likewise nothing else.

### Values and attributes

A value is a number (in the unit the table below gives), `on` or `off`, a short word, or a name. What follows the first TAB is
**attributes**, each also set off by a TAB; a TAB cannot be in a value (it is a control character), so what is text and what is an
attribute is never in doubt, whatever the text says: a device that calls itself `Foo level=critical` is a device by that name and
nothing more.

| Attribute | On | Meaning |
|---|---|---|
| `level=warning` or `level=critical` | any key | How bad it is. A key with no level is normal, so *"anything with a level"* is the list of problems. Said again, without one, when it is normal again. |
| `cause=device` or `cause=jack` | the keys of the sound | The change is not the person's: the default device or its port moved (the level that comes with a device is its own), or a 3.5 mm jack did. A program that shows the person's own changes (a volume bar) leaves these out, and learns the value all the same. Without a cause, a change is the person's. |

A program ignores any attribute it does not know. A key that first appears, or comes back after it was gone, is **new, not a
change**: it is learned, not shown.

### What it carries: what is, and what happened

pacreactd keeps a table of keys, each with one value. A message says what a key is now. **Whatever a program must know when it
starts is a key**: a program that connects an hour after the devices were plugged in, and after the notifications came, is told
all of it in its snapshot, right after `subscribe`. A program that wants an edge ("the charger was just plugged in") compares
with the value it had.

**Devices.** A device is the key `NS.ID`: its value is its name (what it, or a person, called it: cleaned and cut to fit), and its
attributes say what it is: `kind=headset`, `kind=storage`, `scanner=yes`. What it has is under it, `NS.ID.PROP`. It *connects*
by its key's appearing, and *ends* by its going, which takes every key under it (the properties first, the device last). The `ID` is
the first letter of the namespace and a number that only grows (`u7`): it is never given to another device while the daemon runs, so
whoever still holds an old ID can never take a new device for it. When the daemon itself restarts the connection is lost and every
program forgets everything, as it does for any key. An attribute `uid=` gives an identity that lasts across connections when
there is one (a Bluetooth address). Nothing in the protocol takes a device ID as a command: the protocol only tells.

**Events.** What happened once, and was not and will not be (a print job finished, a printer ran out of paper) is told as
`event KEY KIND [NAME]` to the programs listening to that key *now*. It is kept nowhere: it is not in a snapshot, and a program that
was not there did not need it. KIND is a word; NAME is what a document or a device chose (cleaned and cut to fit). A thing that is
(a device, a condition) is never an event.

## What a program sends

A message gets one reply, in order: `ok`, `ok <text>` or `error <reason>`.

| Message | Reply | Meaning |
|---|---|---|
| `subscribe [prefix...]` | `ok`, then the snapshot, then `sync`, then every change | Up to 16 prefixes; none means everything. A prefix `mem` matches `mem` and `mem.*`, never `memory.*`. Sent again, it replaces the filter and sends a new snapshot. |
| `get <key>` | `ok <value>` or `error unknown key` | One key, now. The value is whole, attributes included. |
| `own <namespace>` | `ok` or `error ...` | Claim a namespace to publish into, until the connection closes. |
| `publish <key> <value>` | `ok` or `error ...` | Set a key in a namespace you own. The value is the rest of the message, and is the same as anyone's: a text and attributes. |
| `retract <key>` | `ok` or `error ...` | Remove a key in a namespace you own. |

Errors, all of them: `unknown command`, `message too long`, `bad prefix`, `too many prefixes`, `unknown key`, `bad namespace`,
`reserved` (a namespace of the daemon's own), `taken` (another program has it), `too many namespaces` (8 at most),
`bad key`, `bad value`, `not owner`, `too many keys` (16 at most, in all a program's namespaces; changing a key it has is
always allowed), `no room` (the table is full), `too many clients` (said, and the connection closed).

### The snapshot and `sync`

After `subscribe` the lines before `sync` are what is true now; the lines after it are changes. A program that must not react
to what was already true (pacreact, which would otherwise show a HUD for every state at start-up) records the snapshot silently
and reacts only after `sync`. When the connection is lost, everything said before is forgotten; the next one starts with a new snapshot.

### Namespaces

The namespaces of the sources the daemon has are reserved, and no program can own them:
`daemon`, `power`, `mem`, `cpu`, `io`, `temp`, `net`, `audio`, `camera`, `usb`, `printer`, `backlight`, `lock`, `disk`, `bt`, `input`, `jack`.

Any other namespace is free to the first program that sends `own`, for as long as it stays connected. When it disconnects, its keys are
removed and subscribers are told. This is how pacreact tells the others how many notifications wait (`notifications.count`) and how
monitors there are (`monitor.ID`: only a Wayland client can know).

## Keys

Every key the daemon says, with the unit of its value and where it comes from. A program ignores the keys it does not know.

| Key | Value | Level | Source |
|---|---|---|---|
| `daemon.version` | the program's version | | |
| `daemon.protocol` | the version of this protocol (`1`) | | |
| `audio.volume` | 0-150, percent of the default output; may say `cause=device` | | PulseAudio/PipeWire |
| `audio.mute` | `on` / `off`; may say `cause=device` | | PulseAudio/PipeWire |
| `audio.mic.volume`, `audio.mic.mute` | as above, for the default input (gone while there is none) | | PulseAudio/PipeWire |
| `audio.output` | the description of the default output (cut to fit); may say `cause=jack` | | PulseAudio/PipeWire |
| `audio.ID` | an output or an input of the sound server: its description (cut to fit), `kind=` `output` or `input`, and `role=` what it is for: `headphone`, `headset`, `speaker`, `lineout`, `earpiece`, `microphone`, `linein`, `radio`, `monitor` (a screen's sound: HDMI, DisplayPort), `beeper` (the PC speaker) or `bluetooth` (when nothing says more). The role follows what is plugged in now, then what the device says it is, then its name; it is left out when nothing says it (an output), and an input that says nothing is a `microphone`. Monitors of outputs are not here | | PulseAudio/PipeWire |
| `audio.ID.volume`, `audio.ID.mute`, `audio.ID.default` | its own level (percent), `on`/`off` mute, and `on` for the output and the input that are the default | | PulseAudio/PipeWire |
| `jack.ID` | something plugged into a connector, once the contacts and the sound server's ports agreed what it is. No name; `kind=` `headset`, `headphones`, `mic`, `lineout` or `device` (a connector that cannot say). One of each kind that is plugged in; none when nothing is | | the kernel's jack switches, the sound server's ports |
| `backlight.level` | 0-100 | | sysfs, signalled by the kernel |
| `power.battery` | 0-100 | `warning` at 15 and below, `critical` at 5, only while discharging; kept until above 20 or charging | sysfs, kernel uevents |
| `power.battery.status` | `discharging`, `charging`, `full`, `not-charging`, `unknown` | | sysfs |
| `power.ac` | `on` if any mains or USB supply is online, else `off` | | sysfs, kernel uevents |
| `mem.pressure` | `on` when tasks stalled on memory for a quarter of a 2 s window (`warning`) or everything stalled for an eighth (`critical`); `off` after 10 s without another | as said | PSI trigger |
| `mem.available` | MB available, as of the last time `mem.pressure` changed (and at start) | | `/proc/meminfo` |
| `cpu.pressure` | `on` when runnable tasks waited for a CPU for three quarters of a 2 s window; `off` as `mem.pressure` | `warning` | PSI trigger |
| `io.pressure` | `on` when everything stalled on I/O for half of a 2 s window; `off` as above | `critical` | PSI trigger |
| `temp.cpu`, `temp.ssd` | degrees Celsius | `warning`, `critical` | hwmon, thermal events |
| `disk.NAME` | percent free; NAME is the mount point as a word (`root`, `home`, `mnt-data`) | `warning` under 10%, `critical` under 5% (or low and under 1 GB); kept until 3 points past | mount table, and a timer every 5 minutes |
| `disk.NAME.mount` | the mount point | | mount table |
| `disk.NAME.free` | bytes available (said before the percentage, so a message can use it) | | `statvfs` |
| `net.online` | `on` if there is a default route, else `off` | | rtnetlink |
| `net.device` | the interface of that route (gone while offline) | | rtnetlink |
| `net.link` | `wired` if a cable interface is up, else `wifi` if a wireless one is, else `none` | | sysfs, read when the link changes |
| `net.signal` | percent (0-100), the signal of the wifi connection (gone unless `net.link` is `wifi`), said when it has moved by 5 points. From NetworkManager's strength of the access point, or else iwd's RSSI (as NetworkManager measures it: -100 dBm or less is 0, -40 or more is 100), both of which tell when it moves; else the kernel's link quality, read when the link changes (and every `net_signal_interval` seconds, if that is set) | | NetworkManager or iwd over the system bus; `/proc/net/wireless` |
| `net.probe` | `ok`, `slow` (answers late) or `down` (no answer), said when it changes: after two bad probes in a row, and ok after two good ones | `warning` unless ok | ICMP probe (opt-in: `probe_host`) |
| `net.latency` | milliseconds of the probe that made it slow, said just before `net.probe` | | ICMP probe |
| `net.probe.host` | the address that is probed | | settings |
| `camera.ID` | a camera: its name, `node=video0` | | `/dev/video*`, sysfs |
| `camera.ID.active` | `on` while a program of the user's has it open, else `off` | | `/proc/PID/fd` |
| `printer.ID` | a printer (a queue of CUPS), called as a person would say it. There from its coming to its going | | CUPS |
| `printer.ID.state` | `idle`, `busy` while a job is going to it, or `problem` (out of paper, jammed, out of ink, open, offline, stopped) | level | CUPS |
| `printer.state` | `idle`, `busy` while a job is going to a printer, `problem` while a printer is out of paper, jammed, out of ink, open, offline or stopped (low ink is not one) | `warning` for `problem` | CUPS (IPP subscription, D-Bus signals) |
| *event* `printer.p1` | what CUPS just said: of a job, `job-started`, `job-done`, `job-done-errors` or `job-failed` and its name; of a printer, `paper`, `jam`, `ink-empty`, `ink-low`, `cover`, `offline`, `stopped` or `ready` and its name as a person would say it. A job you cancelled yourself is not told | | CUPS |
| `monitor.ID` | a monitor: the name of the output (`DP-1`), `size=1920x1080` once known. The ID is the compositor's number for it, which it does not give again | | pacreact (Wayland) |
| `usb.ID` | a USB device that is connected, once it has settled: its name (maybe none), `kind=` `camera`, `printer`, `storage`, `audio`, `scanner` or `other`, and `scanner=yes` on a printer that scans too. Hubs, Bluetooth adapters, built-in devices and keyboards, mice and the like (see `input.ID`) are not here | | kernel uevents, sysfs |
| `usb.ID.busy` | on a scanner (and a printer that scans): `on` while a program has it open, else `off`. A scanner is a device SANE knows (systemd's hardware database); one on the network is not seen | | inotify on its device node, `/proc` |
| `input.ID` | a keyboard, mouse, touchpad, tablet or gamepad, on any bus: its name (maybe none), `kind=` `keyboard`, `mouse`, `touchpad`, `tablet` or `gamepad`, `bus=` `usb`, `bluetooth`, `builtin` (the machine's own) or `other`, and `mouse=yes` on a keyboard that is a pointer too. Ask here whether a keyboard or a mouse is available. Virtual devices (a remapper) are not here | | kernel uevents, sysfs |
| `bt.ID` | a Bluetooth device that is connected: its name, `kind=` `headset`, `headphones`, `speaker`, `keyboard`, `mouse`, `gamepad`, `phone`, `computer` or `other`, and `uid=` its address (`aa-bb-cc-dd-ee-ff`) | | BlueZ, over the system bus |
| `lock.caps`, `lock.num` | `on` / `off`, once a keyboard has been read and whenever it changes (opt-in: `lock_keys = 1`) | | the keyboards' LEDs, through evdev |
| `notifications.count` | notifications on screen or waiting | | pacreact |
| `notifications.dnd` | `on` / `off` (not published yet) | | pacreact |

Thresholds (at what percentage `power.battery` is `warning`, which temperature is `critical`) are
settings of pacreactd, in `pacreactd.conf`, or build-time tunables in `src/tunables.h`. How an event is shown, and whether it is shown at all,
is pacreact's own setting: the daemon says what is true, never what to display.

## Compatibility

New keys, and new attributes, may appear at any time; a program ignores the ones it does not know. A key's meaning and
unit never change: a different meaning is a new key. A change that cannot be made that way (a different transport, a
different message list) is a new protocol version, and the socket's name changes with it (`pacreactd2.sock`), so that an old
program finds no daemon rather than a daemon it misreads.
