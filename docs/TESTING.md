# Testing

## Automated (host)

```sh
python3 -m unittest discover -s tests -v
```

Current coverage: config-file format parsing/validation (the format the
app persists), build-config sanity (`app.cfg` flags). Packet/frame logic
inside `pad/main.c` is hardware-tied and cannot run on the host — it is
covered indirectly by the shared upstream implementation, which was stable
in WUP Stream.

## Hardware validation checklist (GamePad required)

| # | Check | Expected |
|---|---|---|
| 1 | Boot .wup from Homebrew Menu | local menu appears, no crash |
| 2 | Y → set IP, B save, power-cycle app | config persists |
| 3 | Connect to ThinServer | video+audio within ~2 s |
| 4 | Touch screen | pointer moves/clicks remotely |
| 5 | Sticks scroll, ZL/ZR click | input reaches X11 |
| 6 | X toggles Onboard | keyboard shows/hides |
| 7 | HOME+B 2 s | app exits, session stays alive server-side |
| 8 | Reconnect | desktop state preserved |
| 9 | 10-min stream | no freeze, memory stable |
| 10 | Wi-Fi loss/recovery | clean resync or return to menu |

Never report hardware results without actually running them.
