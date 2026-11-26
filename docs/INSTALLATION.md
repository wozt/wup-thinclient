# Installation (Wii U GamePad)

Prerequisites: firmware 7677 (required for this toolchain), a host PC on the
same LAN, the wup-homebrew Homebrew Menu installed, and the NetFS share
(`~/wup-share` on this machine) reachable from the GamePad.

## Build and deploy

```sh
cd wup-thinclient
make setup && make && make deploy
```

`make deploy` copies `build/apps/wup_thinclient.wup` to
`~/wup-share/wup_thinclient.wup` (override: `WUP_SHARE_DIR=...`). It verifies
the artifact is non-empty, replaces the destination atomically, and checks
the SHA256 checksum.

## Launch

1. On the GamePad, browse the NetFS share and start `wup_thinclient.wup`
   from the Homebrew Menu.
2. Make sure the WUP ThinServer is running on the same network.
3. The **connection menu** appears — nothing streams yet. Choose
   **Edit server address** to type the server IP (or leave "any
   server"), then **Connect**. Config persists on the GamePad flash
   (`/save/thinclient.cfg`, including the auto-connect flag).

## Emergency exit

**HOME + B held ~2 s** quits the app locally, even with no network.
