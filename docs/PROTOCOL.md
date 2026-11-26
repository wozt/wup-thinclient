# Protocol

WUP ThinClient speaks the **WUP Stream protocol** — see the authoritative
description in the wup-thinserver repo (`docs/PROTOCOL.md`) and upstream
`wup-stream/host/wupstream.h` / `pad/main.c` @ `e1b0430`.

Summary (UDP):

- Client listens on **50000/udp**, broadcasts `B` beacons to **50001/udp**
  (~4/s) announcing its presence; servers reply with `H` hello.
- `H` → video `V` fragments (H.264, CABAC, slices to the GamePad decoder),
  audio `A` (48 kHz stereo PCM), `R` rumble, `Q` quit. ThinServer extends
  `H` to 5 bytes: byte 4 = effective input mode (`0` gamepad, `1` desktop,
  `0xff` unknown); older 4-byte hellos are still accepted.
- Client sends `I` input reports (128 B), `N` NACKs, `M` mic, and — as a
  WUP_THIN_PROTOCOL extension — `T` mode requests `'T' reqid mode 0`
  (`mode`: 0 gamepad, 1 desktop, 0xff toggle). The server acknowledges
  with `'T' reqid status mode`; a missing ack within 2.5 s means the
  server does not support live switching.

ThinClient-specific behaviour: the `H` handshake is filtered by the saved
server IP (`ip_matches`); `*.*.*.*`/empty accepts any server.
