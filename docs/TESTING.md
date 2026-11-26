# Testing

## Automated (host, from repo root)

```sh
make test        # = python3 -m unittest discover -s tests -v
```

Current coverage: the C IPv4 editor helpers (validation, bounded append and
backspace), config-file format parsing/validation, `app.cfg` sanity, and the
HOME shortcut state machine (`tests/test_home_keys.c` — host-compiled
`pad/home_keys.c`): short tap vs 5 s hold, HOME+X/Up/Down/B combos, button
and release ordering, remote-input leak prevention, repeated presses.
The packet/frame logic in `pad/main.c` is hardware-tied and cannot run on the
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
| 2 | Menu → Edit server address; enter an incomplete/out-of-range IP | Save remains disabled; large keys show pressed feedback |
| 3 | Correct the IP, Save, reopen editor, then Cancel a change | valid address persists; Cancel preserves it; no stream starts |
| 4 | Menu → Use any server | explicit any-server selection persists; still no stream |
| 5 | Menu → Connect | "Looking for …" then video+audio within ~15 s |
| 6 | Touch screen | pointer moves/clicks remotely |
| 7 | Left stick = pointer, right stick = scroll, ZL/ZR/L3/R3 = mouse buttons | desktop input reaches X11, no WASD/arrows from sticks |
| 8 | X toggles Onboard | keyboard shows/hides, repeats alternate |
| 9 | HOME tap while streaming | remote HOME action fires (default: XFCE app menu) |
| 9b | HOME held 5 s → menu | stream pauses once, Resume/Disconnect/Reconnect/Controls/Exit work; remote input released while open |
| 9c | HOME + X | mode switches live Desktop↔Gamepad; effective mode shown; stream uninterrupted |
| 9d | HOME + D-pad Up/Down | physical backlight steps through hardware levels, never black |
| 9e | HOME + B during combos | no Escape/HOME leaks remotely; 2 s still quits |
| 10 | HOME menu → Disconnect | back to menu, no auto-reconnect, desktop session alive server-side |
| 11 | HOME+B 2 s | app exits, session stays alive server-side |
| 12 | Auto-connect on + Wi-Fi loss/recovery | auto-reconnects to the last server, or returns cleanly to the menu |
| 13 | 10-min stream | no freeze, memory stable |

Never report hardware results without actually running them.
