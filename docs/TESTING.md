# Testing

## Automated (host, from repo root)

```sh
make test        # = python3 -m unittest discover -s tests -v
```

Current coverage: config-file format parsing/validation (the format the
app persists at `/save/thinclient.cfg`) and `app.cfg` sanity. The
packet/frame logic in `pad/main.c` is hardware-tied and cannot run on the
host; it is shared with the stable upstream implementation.

## Build reproducibility

`make clean && make` from a fresh clone
(`git clone --recurse-submodules`) must produce
`build/apps/wup_thinclient.wup`. The SDK fetches its own pinned
dependencies on first build.

## Hardware validation checklist (GamePad required)

| # | Check | Expected |
|---|---|---|
| 1 | Deploy with `make deploy`, launch from NetFS | local menu appears, no crash |
| 2 | Y → set IP, B save, relaunch | config persists |
| 3 | Connect to ThinServer | video+audio within ~2 s |
| 4 | Touch screen | pointer moves/clicks remotely |
| 5 | Sticks scroll, ZL/ZR click | input reaches X11 |
| 6 | X toggles Onboard | keyboard shows/hides |
| 7 | HOME+B 2 s | app exits, session stays alive server-side |
| 8 | Reconnect | desktop state preserved |
| 9 | 10-min stream | no freeze, memory stable |
| 10 | Wi-Fi loss/recovery | clean resync or return to menu |

Never report hardware results without actually running them.
