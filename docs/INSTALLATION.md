# Installation (Wii U GamePad)

Prerequisites: firmware 7677 (required for this toolchain), a host PC on the
same LAN, the wup-homebrew Homebrew Menu installed.

## Steps

1. Build `wup_thinclient.wup` (see docs/BUILD.md).
2. Deliver it to the GamePad:
   - `python3 libwup/tools/wuplink.py build/apps/wup_thinclient.wup`, then
     press **Y** in the Homebrew Menu to keep + launch; or
   - copy it into the Homebrew Menu `apps/` folder on the GamePad storage.
3. Make sure the WUP ThinServer is running on the same network.
4. Launch **WUP ThinClient** from the Homebrew Menu.

## First connection

- Waiting screen shows status and the configured server.
- Press **Y** → IP editor. D-pad/Left-Right move between octets, Up/Down
  ±1 (±10 while holding L/R), A next octet, B save, X toggles "any server".
- Config is stored on the GamePad's flash (`/save/thinclient.cfg`) and
  survives restarts.

## Emergency exit

**HOME + B held ~2 s** quits the app locally, even with no network.
