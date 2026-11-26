# Architecture

## What runs where

```
Wii U GamePad                          Linux host / container
┌─────────────────────┐   UDP 50000/1   ┌──────────────────────────┐
│ WUP ThinClient (.wup)│ <============> │ wup-thinserver daemon     │
│  - local menu (Y)    │  H/V/A/R/Q     │  - X11 capture+scale      │
│  - IP config (flash) │  I/N/M         │  - x264 encode            │
│  - hw H.264 decode   │                │  - PulseAudio in/out      │
│  - PCM audio out     │                │  - XTest input injection  │
│  - input report      │                │       │                  │
└─────────────────────┘                │       v                  │
        GamePad screen/speakers        │  Xvfb :99 + XFCE session │
                                       └──────────────────────────┘
```

The GamePad is a pure terminal: decode, present, capture input, manage the
local connection UI. The desktop, browsers and files live on the server.

## App structure (`pad/main.c`, ~600 lines, C)

Derived from `wup-stream/pad/main.c` @ `e1b0430`. Unchanged core:

- UDP socket :50000, broadcast `B` beacons to :50001.
- `H`/`V`/`A`/`R`/`Q` packet handling; `I`/`N`/`M` transmit.
- Hardware decoder ring management (`VDEC`/`ENG` MMIO), frame reassembly
  (`as[NASM]`), NACK (`nack`), watchdog keyframe recovery.
- PCM audio queue (`wup_audio_*`), mic (`wup_mic_*`), rumble, backlight,
  flight-recorder diagnostics (`diag_*`).

Added for ThinClient:

- `cfg_load`/`cfg_save`: server IP + auto-connect flag persisted at
  `/save/thinclient.cfg` (littlefs flash via libwup `fs_*`).
- Connection state machine (`ST_MENU`/`ST_EDIT`/`ST_CONNECTING`/
  `ST_STREAMING`/`ST_INGAME`): beacons and `H` acceptance only happen in
  `ST_CONNECTING`; the menu never broadcasts readiness. The configured IP
  is a selection filter (plus `conn_target` pinning for reconnects), not
  authentication.
- Local UI: connection menu (Connect / Edit / Auto-connect / Exit,
  touch + d-pad), octet IP editor, and an in-stream HOME menu that pauses
  the decoder and sends a neutral input report (all remote inputs
  released) until Resume.
- Branding: "WUP ThinClient", own `app.cfg` (name/author/url/icon).

## Memory budget

GamePad: ARM926EJ-S, **4 MiB RAM total**. The app is a "large" libwup app:
code in WUP_SPARE, `.bss` 258 KB at 0x190000, decoder frames are the display
framebuffer itself (no copies). Build report: `code 151 KB, .bss 258 KB,
322 KB left below the screen`. No dynamic allocation in the hot path; all
packet/frame buffers are static.

## Input mapping (what the pad sends)

The 128-byte `I` report carries buttons, sticks (rest-calibrated), touch
(averaged 10 samples, calibrated to 854×480), accel/gyro, volume slider,
battery, backlight. The **server** maps these into X11 events — see
wup-thinserver `docs/CONFIGURATION.md` for the default GamePad→desktop map
(Onboard toggle on X, etc.). The client only transmits; it does not inject.

## Recovery

- Server silent >1.5 s → `peer_lost()` → auto-reconnect (when enabled)
  or back to the menu; the Linux session keeps running server-side.
- Server `Q` → same path (reconnect when auto-connect is on).
- Decoder stall >60 ms → keyframe request (`need_idr`) via `I` flags.
- Lost fragments → `N` NACKs; >45 ms incomplete → resync.
- HOME+B 2 s → local quit (always available, even with no server).
