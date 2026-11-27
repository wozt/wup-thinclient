# Upstream provenance

| Upstream | Pinned revision | Role |
|---|---|---|
| github.com/wozt/wup-stream (`pad/` client) | `6ac48c732c9d9adc170719abef2696f9ea108d08` (camera baseline `a082db95ba62`) | current protocol/V2/camera reference; ThinClient remains an adapted independent client |
| github.com/wozt/wup-homebrew (`libwup/`) | `3da1bfa5b3b2d3a46d31a0fceeef3b2c1256ef15` (`feature/netfs-v2`) | build SDK — **git submodule** `third_party/wup-homebrew` |
| github.com/ravensclawx/wup-homebrew | V2.1.0 `b4f7ff70734c1503fae333f3e0c01af4147f6dfb` | upstream V2 base merged into the SDK fork |

The submodule points at wozt's `feature/netfs-v2` tip because that is the SDK
the developer's Homebrew Menu/toolchain actually uses; it is fully pushed
to GitHub, so a fresh `git clone --recurse-submodules` reproduces the
environment. An uncommitted local SDK can be substituted via
`LIBWUP_DIR` for development only.

## Adapted files

- `pad/main.c` — originally adapted from `wup-stream/pad/main.c`; subsequently
  migrated to the V2 kernel video/input/timing APIs while retaining the WUP
  Stream wire format. Unchanged core: UDP/`B` beacon,
  `H`/`V`/`A`/`R`/`Q`, `I`/`N`/`M`, hardware H.264 decode, PCM audio, NACK,
  watchdog, and diagnostic thread.
  Local additions: `pad/profiles.c` + `cfg_load`/`cfg_save`
  (`/save/thinclient.cfg`, versioned profiles with legacy migration),
  `ip_matches`, local connection UI (profile list, profile editor,
  alphanumeric keyboard, numeric pad, in-stream menu), `begin_connect`,
  ThinClient branding.
- `pad/app.cfg` — `name = WUP ThinClient`, own description/icon; build
  defines identical to upstream pad (net buffers, decoder config).
- `pad/icon.png` — new ThinClient icon.
- `LICENSE` — copied from wup-stream (GPL-3.0).

`host/libwupstream` is not used by this app (host-side library); documented
for protocol reference only.

## Compatibility requirements

- Firmware **7677** only.
- WUP Homebrew **V2** kernel/menu. V1 and V2 `.wup` binaries are ABI
  incompatible; a current ThinClient must be rebuilt from this repository.
- H.264 baseline/CABAC slices at 854×480 — produced by WUP Stream servers /
  WUP ThinServer; other encoders are not guaranteed decoder-compatible.
