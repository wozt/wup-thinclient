# Upstream provenance

| Upstream | Local path | Commit | License |
|---|---|---|---|
| github.com/wozt/wup-stream (client `pad/` + `host/libwupstream`) | `/home/wozt/dev/wup-stream` | `e1b04305ca123e3f6980d60ef302ada9c19ce127` | GPL-3.0-or-later |
| github.com/ravensclawx/wup-homebrew (`libwup` toolchain) | `/home/wozt/dev/wup-homebrew-with-lan-storage/libwup` | fork `feature/netfs` | see repo |

## Imported / adapted files

- `pad/main.c` — adapted into `pad/main.c` of this repo (~470 → ~610
  lines). Unchanged core: network/decoder/audio/input/NACK/watchdog/diag.
  Local modifications: server-IP config (`cfg_load`/`cfg_save`,
  `ip_matches`), local server-selection UI (`UI_*`, `draw_ui_*`),
  `autostart_stream`, ThinClient branding.
- `pad/app.cfg` — adapted: `name = WUP ThinClient`, own description/icon;
  build defines kept identical (net buffers, decoder config).
- `pad/icon.png` — new ThinClient icon.
- `LICENSE` — copied from wup-stream (GPL-3.0).

`host/libwupstream` is **not** used by this app (it is a host-side library);
it is documented here for protocol reference.

## Compatibility requirements

- Firmware **7677** only (per libwup toolchain constraint).
- H.264 baseline, CABAC slices, 854×480@~60 — produced by WUP Stream
  servers / WUP ThinServer only. Other encoders are not guaranteed to
  produce decoder-compatible streams.
