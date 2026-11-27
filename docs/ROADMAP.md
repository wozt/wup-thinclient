# Roadmap

1. **Physical validation** — flash `wup_thinclient.wup`, exercise the
   connection profile manager (add/edit/delete/connect, keyboards,
   migration of an existing `thinclient.cfg`), run the checklist in
   docs/TESTING.md, fix crashes.
2. **Direct-connection reliability** — isolated network-namespace NAT
   tests (translated source port, endpoint-dependent filtering, multiple
   clients per public IP, mapping expiry, reconnect); peer endpoint
   hardening; input-mode request idempotency review.
3. **Memory audit** — measure .bss/heap headroom on device; keep buffers
   bounded (see docs/ARCHITECTURE.md). Profile storage is already bounded
   (8 × ~300 B cfg, static buffers only).
4. **Secure WAN feasibility** — evaluate a maintained DTLS/PSK datagram
   transport for libwup/lwIP (size, RAM, handshake, replay); isolated
   proof of concept before touching the streaming path. VPN remains the
   recommended remote-access method until proven.
5. **PC reference client** (`wup-thinclient-pc`) — C11/Meson/SDL3/FFmpeg
   decodes the actual GamePad video bitstream, then profiles, Xbox
   mapping and a scripted test laboratory.
6. **Release** — versioned `.wup` + install ZIP + SHA256 via GitHub
   Actions once hardware-validated.
