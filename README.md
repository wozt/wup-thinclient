# WUP ThinClient

WUP ThinClient turns the **Wii U GamePad** into a thin client for a Linux
desktop: it receives the video stream and audio from a
[WUP ThinServer](https://github.com/wozt/wup-thinserver), plays them through
the GamePad's hardware H.264 decoder and speakers, and sends the touchscreen,
sticks, buttons, motion sensors, microphone and volume slider back.

All Linux applications (Firefox ESR, Thunar, XFCE Terminal, Onboard
on-screen keyboard, ...) run inside the ThinServer session — nothing Linux
runs on the GamePad itself.

## Differences from WUP Stream

- **WUP Stream** (`wup-stream` repo): generic receiver + `libwupstream` for
  programmers; waits for *any* computer.
- **WUP ThinClient**: dedicated product identity, local menu with manual
  server IP entry, saved server configuration, disconnect/reconnect flow —
  purpose-built for WUP ThinServer.

The wire protocol is identical (WUP Stream protocol, UDP), so the two can
interoperate.

## Local interface

- **Waiting screen**: shows the configured server (or "any server"), this
  GamePad's IP, and hints.
- **Y** (while waiting): edit the server address — d-pad/L/R move between
  octets, Up/Down ±1 (±10 with L/R held), A next, B save, X toggle
  "any server" (`*.*.*.*`). Saved to `/save/thinclient.cfg` on the
  GamePad's flash.
- **B** while streaming does nothing destructive; **HOME+B held 2 s**
  force-quits the app (local emergency exit, always available).
- **HOME+Up/Down**: local screen brightness (upstream behaviour kept).

## Building

See [docs/BUILD.md](docs/BUILD.md). Requires the libwup toolchain
(`wup-homebrew`, `libwup/tools/wupbuild.py`) and an ARM cross toolchain.

## Status

Prototype — builds to `.wup`, **not yet validated on physical hardware**.
See [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md).

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). Derived from `wup-stream/pad`
(GPL-3.0); see [docs/UPSTREAM.md](docs/UPSTREAM.md).
