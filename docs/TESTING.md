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
| 1 | Deploy with `make deploy`, launch from NetFS | **connection menu** appears (no auto-stream), no crash |
| 2 | Menu → Edit server address, set IP, B save & back | config persists; still no stream |
| 3 | Menu → Connect | "Looking for …" then video+audio within ~15 s |
| 4 | Touch screen | pointer moves/clicks remotely |
| 5 | Left stick = pointer, right stick = scroll, ZL/ZR/L3/R3 = mouse buttons | desktop input reaches X11, no WASD/arrows from sticks |
| 6 | X toggles Onboard | keyboard shows/hides, repeats alternate |
| 7 | HOME while streaming → menu | stream pauses, Resume/Disconnect/Reconnect/Controls/Exit work; remote input released while open |
| 8 | HOME menu → Disconnect | back to menu, no auto-reconnect, desktop session alive server-side |
| 9 | HOME+B 2 s | app exits, session stays alive server-side |
| 10 | Auto-connect on + Wi-Fi loss/recovery | auto-reconnect to last server, or clean return to menu |
| 11 | 10-min stream | no freeze, memory stable |

Never report hardware results without actually running them.
