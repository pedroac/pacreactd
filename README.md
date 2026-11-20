# pacreactd

A tiny daemon that watches the system and tells the other pac tools, in one normalized message, what changed.

* **One watcher, many listeners:** [pacreact](../pacreact), [pacbar](../pacbar) and any script read the same state from one socket, instead of each watching PulseAudio, udev and D-Bus on its own.
* **State, not events:** it keeps what is true now (`audio.volume 63`, `mem.pressure on level=warning`) and tells a program that connects late first, so nobody has to guess.
* **No UI:** it says what is true, never what to show. How and whether to show it is the listener's.
* **Asleep by default:** one `epoll` loop; sources are event-driven, preferably straight from the kernel.

## What it watches

| Keys | Source | Needs libsystemd |
|---|---|---|
| `power.*` battery and charger | kernel uevents, sysfs | |
| `mem.*`, `cpu.*`, `io.*` pressure | kernel PSI triggers | |
| `temp.*` CPU and SSD temperature | hwmon | |
| `net.*` link, and the network probe | rtnetlink, ICMP (probe is opt-in) | |
| `net.*` wifi signal | NetworkManager or iwd; else the kernel's counter | yes |
| `audio.*` every output and input, `jack.*` | PulseAudio / PipeWire, kernel jack switches | |
| `backlight.level` | sysfs | |
| `camera.active`, `usb.*`, `input.*` | kernel uevents, sysfs | scanners: yes |
| `printer.*` | CUPS | yes |
| `bt.*` | BlueZ | yes |
| `disk.*` free space, `lock.*` Caps and Num Lock | mount table, evdev LED events | |

Everything pacreact used to watch is here now. The monitors stay pacreact's to tell (each is a `monitor.ID` device), since only a Wayland client can know them. `make SDBUS=0` leaves out what needs libsystemd. The keys, units and levels are in [the protocol](docs/protocol.md).

## Try it

```bash
make && make test
./pacreactd &
./pacreactd watch            # the state, then each change
./pacreactd get daemon.version
```

## Install

```bash
sudo make install                              # or: cd contrib && makepkg -si
systemctl --user enable --now pacreactd.socket # starts the daemon when something first connects
```

The socket exists before the daemon does, so pacreact and pacbar can be started in any order. (`pacreactd.service` instead runs it with the session.) Settings are in [`pacreactd.conf`](pacreactd.conf); [`pacreactd(1)`](docs/pacreactd.1) says the rest.

The shared code is the `pacommon` submodule: clone with `--recurse-submodules` (or run `git submodule update --init`), or set `PACOMMON=` to another checkout.

## Talking to it

A `SOCK_SEQPACKET` socket at `$XDG_RUNTIME_DIR/pacreactd.sock`; send `subscribe`, read lines. The whole protocol, with its grammar and what it trusts, is in [docs/protocol.md](docs/protocol.md): only the same user may connect, and what devices, documents and people call themselves can never be taken for anything but a name.

## Platform

Linux only, tested on **Arch Linux with Hyprland**. It will probably need adapting elsewhere.

## Documentation

- `man pacreactd` ([source](docs/pacreactd.1)): the command and every setting in one page
- [Protocol](docs/protocol.md): the socket, the grammar, every key, what it trusts
- [`pacreactd.conf`](pacreactd.conf): the settings, with their defaults
- [AGENTS.md](AGENTS.md): for AI agents and people changing it

## What it stores

Nothing on disk. The state is in memory and is gone when the daemon stops; the only file it makes is the socket, `$XDG_RUNTIME_DIR/pacreactd.sock`, removed when it ends. The one thing it can send over the network is the opt-in `probe_host` ping, and it asks the CUPS server (normally the local one) about print jobs.

## Size

The binary is 120 KiB stripped (`make`, measured 2026-11-20).

## AI Assistance Disclosure

The tools were Claude (code generation), Dropstone (review and improvement), and Gemini and ChatGPT (investigation), as [the principles](pacommon/PRINCIPLES.md#ai-assisted-human-owned) describe. A human, the author, reviews all of the code and tests it; paths that could not be exercised on the development machine are called out in the documentation.

## License

MIT, see [LICENSE](LICENSE).
