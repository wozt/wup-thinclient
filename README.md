# WUP ThinClient

WUP ThinClient turns the **Wii U GamePad** into a thin client for a Linux
desktop: it receives the video stream and audio from a
[WUP ThinServer](https://github.com/wozt/wup-thinserver), plays them through
the GamePad's hardware H.264 decoder and speakers, and sends the touchscreen,
sticks, buttons, motion sensors, microphone and volume slider back.

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

- **Waiting screen**: shows the configured server (or "any server"), this
  GamePad's IP, and hints.
- **Y** (while waiting): edit the server address — d-pad/L/R move between
  octets, Up/Down ±1 (±10 with L/R held), A next, B save, X toggle
  "any server" (`*.*.*.*`). Saved to `/save/thinclient.cfg` on the
  GamePad's flash.
- **HOME+B held 2 s** force-quits the app (local emergency exit).

## Status

Prototype — builds reproducibly to `build/apps/wup_thinclient.wup`,
**not yet validated on physical hardware**.
See [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md).

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). Derived from `wup-stream/pad`
(GPL-3.0); see [docs/UPSTREAM.md](docs/UPSTREAM.md).
