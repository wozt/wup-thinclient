# WUP ThinClient

WUP ThinClient turns the **Wii U GamePad** into a thin client for a Linux
desktop: it receives the video stream and audio from a
[WUP ThinServer](https://github.com/wozt/wup-thinserver), plays them through
the GamePad's hardware H.264 decoder and speakers, and sends the touchscreen,
sticks, buttons, motion sensors, microphone and volume slider back.
When requested by a compatible server, it can also uplink the GamePad's
640×480 camera through the V2 hardware H.264 encoder; capture is off by
default.

All Linux applications (Firefox ESR, Thunar, XFCE Terminal, Onboard
on-screen keyboard, ...) run inside the ThinServer session — nothing Linux
runs on the GamePad itself.

## Quick start

### System dependencies (Debian/Ubuntu)

```sh
sudo apt install git python3 make clang lld meson ninja-build \
    llvm-19                        # or any LLVM >= 19 with clang/ld.lld
```

The build also needs network access on first run: the pinned `libwup` SDK
(git submodule) fetches its own third-party sources (littlefs, lwIP, tinf,
heatshrink, picolibc, compiler-rt) — all versioned upstreams.

### Clone, setup, build

```sh
git clone --recurse-submodules https://github.com/wozt/wup-thinclient
cd wup-thinclient
make setup      # fetch/verify SDK + LLVM, pre-fetch sources (once)
make            # -> build/apps/wup_thinclient.wup
make test       # host-side unit tests
make deploy     # copy to ${WUP_SHARE_DIR:-~/wup-share}/wup_thinclient.wup
```

If you already have a compatible libwup checkout, skip the submodule:

```sh
make setup LIBWUP_DIR=/path/to/libwup    # dir containing tools/wupbuild.py
```

`make help` lists all targets (`clean`, `rebuild`, `deploy`, ...).

### Install on the GamePad

The `.wup` is deployed to the NetFS share (`~/wup-share` by default,
override with `WUP_SHARE_DIR`). On the GamePad, open it from the Homebrew
Menu / NetFS browser. See [docs/INSTALLATION.md](docs/INSTALLATION.md).

## Local interface

The app **never streams on its own**: it opens a connection menu and waits
for an explicit **Connect**.

- **Menu**: WUP ThinClient branding, configured server (or "any server"),
  this GamePad's IP, connection status, and items **Connect / Edit server
  address / Use any server / Auto-connect / Exit**. D-pad + A, or tap the
  touchscreen.
- **Edit server address**: large touchscreen numeric keypad with decimal
  point, backspace, clear, validated Save and non-destructive Cancel.
  Saved to `/save/thinclient.cfg` on the GamePad's flash together with the
  auto-connect flag. Saving never connects by itself.
- **Use any server**: explicit menu action; the first matching ThinServer
  to answer is selected.
- **Connect**: beacons for up to 15 s; the first matching ThinServer's
  hello starts the stream. B cancels. With "any server" the first
  ThinServer that answers wins — the IP is a *selection filter*, not
  authentication.
- **Auto-connect** (menu toggle, off by default): connect automatically on
  launch and retry automatically after an unexpected drop.
- **In-stream menu**: hold **HOME for 5 s** → Resume / Disconnect /
  Reconnect / Controls / Exit. While it is open the stream pauses
  (decoder stopped, next keyframe resumes it) and **all remote input is
  released** — nothing you press reaches the desktop until you resume.
- **HOME shortcuts** (local, resolved by a non-blocking state machine):
  - short **HOME** tap → forwarded to ThinServer as a normal button press
    (bindable in Settings; default opens the XFCE app menu);
  - **HOME + X** → live Desktop/Gamepad mode switch on the server
    (`'T'` request + ack; the effective mode is shown in the menu);
  - **HOME + D-pad Up/Down** → physical screen backlight brighter/dimmer;
  - **HOME + B** held 2 s → emergency exit.
  Combo partners are masked from the remote report, so e.g. HOME+X never
  also toggles Onboard.
- **Disconnect / loss**: manual disconnect returns to the menu and stops
  reconnecting; an unexpected drop retries only when auto-connect is on,
  otherwise shows "Connection lost" in the menu. The Linux desktop stays
  alive either way.
- **HOME+B held 2 s** force-quits the app (local emergency exit).

## Status

Builds reproducibly to `build/apps/wup_thinclient.wup` and deploys to the
NetFS share. **Core streaming validated on physical GamePad hardware**
(desktop visible and usable, developer-reported); the connection menu and
in-stream HOME menu and camera uplink are pending physical retest.
See [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md).

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). Derived from `wup-stream/pad`
(GPL-3.0); see [docs/UPSTREAM.md](docs/UPSTREAM.md).
