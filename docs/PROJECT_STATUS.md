# Project status

| Feature | Status |
|---|---|
| GamePad homebrew (adapted pad client) | Implemented |
| Connection state machine + menu (Connect/Edit/Auto-connect/Exit) | Implemented — pending hardware retest |
| Explicit-connect flow: no streaming until Connect; no beacons while disconnected | Implemented — pending hardware retest |
| Server IP editor (4 octets, any-server mode) + persistence | Implemented |
| Auto-connect option (off by default) + auto-reconnect on loss | Implemented — pending hardware retest |
| In-stream HOME menu (Resume/Disconnect/Reconnect/Controls/Exit) | Implemented — pending hardware retest |
| Remote-input release while local menu open (neutral report) | Implemented — pending hardware retest |
| HOME never forwarded to the remote desktop | Implemented |
| Root Makefile build (`make setup/build/test/deploy`) | Tested automatically |
| Pinned libwup SDK via git submodule + `LIBWUP_DIR` override | Tested automatically |
| `.wup` artifact at `build/apps/wup_thinclient.wup` (~126 KB) | Tested automatically |
| NetFS deploy to `~/wup-share` with SHA256 verify | Tested automatically |
| Host-side unit tests (config format, app.cfg) | Tested automatically |
| **Core streaming on physical GamePad (WUP-010)** | **Validated — developer-reported** (desktop visible & usable) |
| Connection menu / HOME menu / auto-show keyboard on hardware | Pending physical retest |
| 60 fps sustained | Not measured |

Last updated: desktop-usability milestone.
