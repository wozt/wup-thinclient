# Project status

| Feature | Status |
|---|---|
| GamePad homebrew (adapted pad client) | Implemented |
| WUP Homebrew V2.1 + WOZT NetFS SDK migration (`3da1bfa5b3b2`) | Implemented + build-tested |
| V2 kernel video/input/timing APIs | Implemented + build-tested; physical V2 regression pending |
| GamePad camera H.264 uplink | Audited; implementation in progress |
| Connection profile manager (≤8 named profiles, stable ids, v2 cfg, legacy migration) | Implemented + host unit tests — pending hardware validation |
| Touchscreen alphanumeric QWERTY keyboard (name/host) + numeric pad (IPv4/port) | Implemented — pending hardware validation |
| Profile editor validation, endpoint preview, delete confirmation | Implemented — pending hardware validation |
| Direct UDP endpoint (unicast `B` to `host:port`, DNS via lwIP, custom port) | Implemented — LAN-verified, pending hardware retest |
| LAN discovery (explicit broadcast action, `disc`/`dport` persisted) | Implemented — pending hardware retest |
| Auto-connect per profile + auto-reconnect on loss (DNS re-resolved each attempt) | Implemented — pending hardware retest |
| In-stream menu via 5 s HOME hold (Resume/Disconnect/Reconnect/Controls/Exit) | Implemented — pending hardware retest |
| Remote-input release while local menu open (neutral report) | Implemented — pending hardware retest |
| HOME shortcut state machine (short tap forwarded, 5 s menu, HOME+X mode switch, HOME+Up/Down backlight, HOME+B exit, no leakage) | Implemented + host unit tests — pending hardware retest |
| Remote HOME forwarding + server-side bindable HOME | Implemented — pending hardware retest |
| Live Desktop/Gamepad mode switching (`'T'` request/ack + `'H'` mode byte) | Implemented + host unit tests — pending hardware retest |
| Local hardware backlight shortcuts (HOME+Up/Down) | Implemented — pending hardware retest |
| Root Makefile build (`make setup/build/test/deploy`) | Tested automatically |
| Pinned libwup SDK via git submodule + `LIBWUP_DIR` override | Tested automatically |
| `.wup` artifact at `build/apps/wup_thinclient.wup` | Tested automatically |
| NetFS deploy to `~/wup-share` with SHA256 verify | Tested automatically |
| Host-side unit tests (config format, app.cfg, IPv4 editor, HOME shortcut state machine, profiles) | Tested automatically |
| **Core streaming on physical GamePad (WUP-010)** | **Validated — developer-reported** (all 13 baseline checks passed) |
| HOME shortcuts / mode switching / remote HOME on hardware | Pending physical retest |
| Profile manager / keyboards / direct connect on hardware | Pending physical retest |
| 60 fps sustained | Not measured |

Last updated: 2026-11-27 V2 compatibility audit. Pending: camera uplink,
physical V2/profile/HOME regression retest, secure WAN transport feasibility,
and the PC reference client.
