# AGENTS.md

For AI agents and people working in pacreactd. The rules for all the pac tools are in
[pacommon/AGENTS.md](pacommon/AGENTS.md); read it first, then [the principles](pacommon/PRINCIPLES.md).

```
make        # build; there must be no warnings
make test   # unit tests, no display and no running daemon needed
make sanitize   # the tests under AddressSanitizer and UBSan
make SDBUS=0    # without the print, Bluetooth and wifi-signal watchers: no libsystemd
```

- The protocol is [docs/protocol.md](docs/protocol.md): change it first, then the code. A key's meaning and unit never change; a different meaning is a new key.
- Anything a device, a document or a person chose the name of (a USB stick, a print job, a mount point, a Bluetooth device) goes through `state_text_clean()` before it is said, and an event (something that happens) is said with `bus_event()`, never by hand. Attributes are after a TAB, and only there.
- A source (audio, power, ...) sets keys with `bus_set()` and never talks to a client. Moving one in from pacreact means: its watching code here, its thresholds in `pacreactd.conf`, and in pacreact only the code that shows the event.
- The daemon has no UI and no Wayland. Do not add one.
- Shared code is in `pacommon` (a git submodule); change it there, in its own repository, and run this tool's tests afterwards.
- Keep README.md lean and true: update it, `pacreactd.conf` and `docs/` with the change.
- Say what you tested and where. Do not commit or push unless asked.
