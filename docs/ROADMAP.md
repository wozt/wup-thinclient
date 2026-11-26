# Roadmap

1. **Physical validation** — flash `wup_thinclient.wup`, connect to
   ThinServer, run the checklist in docs/TESTING.md, fix crashes.
2. **Memory audit** — measure .bss/heap headroom on device; keep buffers
   bounded (see docs/ARCHITECTURE.md).
3. **Discovery UX** — optional mDNS/broadcast server list instead of
   manual IP (protocol already broadcasts).
4. **Input polish** — per-button remapping stored in cfg if needed.
5. **Release** — versioned `.wup` + install ZIP + SHA256 via GitHub
   Actions once hardware-validated.
