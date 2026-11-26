# Upstream provenance

| Upstream | Pinned revision | Role |
|---|---|---|
| github.com/wozt/wup-stream (`pad/` client) | `e1b04305ca123e3f6980d60ef302ada9c19ce127` | source of `pad/main.c`, protocol, decoder logic |
| github.com/wozt/wup-homebrew (`libwup/`) | `7b641902f9ea134e2d2a585be3ca3f60442ba174` (`feature/netfs`) | build SDK — **git submodule** `third_party/wup-homebrew` |
| github.com/ravensclawx/wup-homebrew | `main` = `8e717434db04ea713b25ac627e12ab813639bf59` | upstream of the SDK fork |

The submodule points at wozt's `feature/netfs` tip because that is the SDK
the developer's Homebrew Menu/toolchain actually uses; it is fully pushed
to GitHub, so a fresh `git clone --recurse-submodules` reproduces the
environment. An uncommitted local SDK can be substituted via
`LIBWUP_DIR` for development only.

## Adapted files

- `pad/main.c` — adapted from `wup-stream/pad/main.c` @ `e1b0430`
  (~470 → ~610 lines). Unchanged core: UDP/`B` beacon, `H`/`V`/`A`/`R`/`Q`,
  `I`/`N`/`M`, HW H.264 decode, PCM audio, NACK, watchdog, diag thread.
  Local additions: `cfg_load`/`cfg_save` (`/save/thinclient.cfg`),
  `ip_matches`, local server-selection UI (`UI_*`, `draw_ui_*`),
  `autostart_stream`, ThinClient branding.
- `pad/app.cfg` — `name = WUP ThinClient`, own description/icon; build
  defines identical to upstream pad (net buffers, decoder config).
- `pad/icon.png` — new ThinClient icon.
- `LICENSE` — copied from wup-stream (GPL-3.0).

`host/libwupstream` is not used by this app (host-side library); documented
for protocol reference only.

## Compatibility requirements

- Firmware **7677** only.
- H.264 baseline/CABAC slices at 854×480 — produced by WUP Stream servers /
  WUP ThinServer; other encoders are not guaranteed decoder-compatible.
