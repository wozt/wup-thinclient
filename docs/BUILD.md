# Build

## Requirements

- Debian/Ubuntu host with Python 3, `make`, `git`.
- LLVM 19 toolchain (`llvm-ar`, `llvm-nm`, `llvm-objcopy`, `ld.lld`) —
  on this machine: `/usr/lib/llvm-19/bin`; unsuffixed names are provided by
  symlinks in `/tmp/wupbin`.
- A libwup checkout: `wup-homebrew` (ravensclawx) with `libwup/` — locally
  `/home/wozt/dev/wup-homebrew-with-lan-storage/libwup`.
- First build fetches littlefs/lwIP/picolibc once via `tools/deps.py`
  (already done in the local libwup).

## Build the `.wup`

```sh
export LLVM_BIN=/usr/lib/llvm-19/bin
mkdir -p /tmp/wupbin
for t in ar nm objcopy; do ln -sf /usr/bin/llvm-$t-19 /tmp/wupbin/llvm-$t; done
export PATH=/tmp/wupbin:$PATH

cd /home/wozt/dev/wup-homebrew-with-lan-storage/libwup
python3 tools/deps.py                       # once, if third_party/ is empty
python3 tools/wupbuild.py /home/wozt/dev/wup-thinclient/pad
# -> build/apps/wup_thinclient.wup
```

`app.cfg` is read **only when the app dir is the sole argument** — extra
flags (`-o`, `--net`, ...) bypass it; pass them explicitly then.

## Install on the GamePad

The Homebrew Menu must support `.wup` apps (wup-homebrew). From a PC on the
same network as the GamePad:

```sh
python3 libwup/tools/wuplink.py build/apps/wup_thinclient.wup
# press Y in the Homebrew Menu to keep+start it (see wup-homebrew docs)
```

Or copy `wup_thinclient.wup` into the Homebrew Menu's apps directory on the
GamePad's storage.

## Run

1. GamePad on the same Wi-Fi/LAN as the WUP ThinServer.
2. Start WUP ThinClient from the Homebrew Menu.
3. Waiting screen → press **Y** to set the server IP (or leave "any
   server" for first-come discovery), **B** to save.
4. The server streams automatically (`autostart_stream`).

## Host-side checks

```sh
python3 -m unittest discover -s tests -v
```

(see docs/TESTING.md — hardware behaviour itself needs the GamePad.)
