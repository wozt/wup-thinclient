# Project status

| Feature | Status |
|---|---|
| GamePad homebrew skeleton (adapted pad client) | Implemented |
| Server IP config + persistence (`/save/thinclient.cfg`) | Implemented |
| Local menu (wait screen, IP editor, any-server mode) | Implemented |
| Root Makefile build (`make setup/build/test/deploy`) | Tested automatically |
| Pinned libwup SDK via git submodule + `LIBWUP_DIR` override | Tested automatically |
| `.wup` artifact at `build/apps/wup_thinclient.wup` (~111 KB) | Tested automatically |
| NetFS deploy to `~/wup-share` with SHA256 verify | Tested automatically |
| Host-side unit tests (config format, app.cfg) | Tested automatically |
| Hardware validation on GamePad 7677 | **Not done — pending** |
| Automatic server discovery | Planned |
| Onboard/desktop interaction end-to-end | In progress (server side) |

Last updated: build-workflow correction milestone.
