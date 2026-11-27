# Protocol

WUP ThinClient speaks the **WUP Stream protocol** — see the authoritative
description in the wup-thinserver repo (`docs/PROTOCOL.md`) and upstream
`wup-stream/host/wupstream.h` / `pad/main.c` camera baseline `a082db95ba62`.

Summary (UDP):

- Client listens on **50000/udp**, broadcasts `B` beacons to **50002/udp** (WUP ThinServer's own discovery port; WUP Stream stays on 50001)
  (~4/s) announcing its presence; servers reply with `H` hello.
- `H` → video `V` fragments (H.264, CABAC, slices to the GamePad decoder),
  audio `A` (48 kHz stereo PCM), `R` rumble, `Q` quit. ThinServer extends
  `H` to 5 bytes: byte 4 = effective input mode (`0` gamepad, `1` desktop,
  `0xff` unknown); older 4-byte hellos are still accepted. Hello byte 2 bit
  0 requests the microphone and bit 1 requests the camera.
- Client sends `I` input reports (128 B), `N` NACKs, `M` mic, camera `C`
  fragments, and — as a
  WUP_THIN_PROTOCOL extension — `T` mode requests `'T' reqid mode 0`
  (`mode`: 0 gamepad, 1 desktop, 0xff toggle). The server acknowledges
  with `'T' reqid status mode`; a missing ack within 2.5 s means the
  server does not support live switching.

Camera uses the existing WUP Stream V2 extension. Each `C` datagram contains
an encoder-run byte, fragment index/count, little-endian frame number, and at
most 1024 bytes of 640×480 hardware-H.264 bare slice data. A server detecting
a missing dependent frame sends `K`; ThinClient restarts the encoder and the
new run begins with a keyframe. Input report flag bit 7 announces camera
capability and is ignored by older servers. Camera capture remains off until
hello bit 1 requests it.

ThinClient-specific behaviour: the `H` handshake is filtered by the saved
server IP (`ip_matches`); `*.*.*.*`/empty accepts any server.
