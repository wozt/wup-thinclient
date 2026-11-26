# Configuration

WUP ThinClient has very few local settings — it is a terminal. Server-side
configuration (desktop, quality, input map) lives in WUP ThinServer's
`settings.json`; see that repo's docs/CONFIGURATION.md.

## Client-side settings

| Setting | Where | Default |
|---|---|---|
| Server address | Menu → **Edit server address** (touchscreen numeric keypad) | empty = "any server" (first `H` wins) |
| Any-server mode | Menu → **Use any server** | Explicitly clears the fixed address |
| Auto-connect | Menu → **Auto-connect** toggle | **off** |
| Persisted file | `/save/thinclient.cfg` on GamePad flash | — |
| Emergency quit | **HOME + B** ~2 s (any state) | — |
| Local shortcuts | **HOME** held 5 s | in-stream menu (Resume/Disconnect/Reconnect/Controls/Exit) |
| Local shortcuts | **HOME + X** | live Desktop/Gamepad mode switch (`'T'` packet, acked) |
| Local shortcuts | **HOME + D-pad Up/Down** | physical backlight brighter/dimmer (hardware levels) |
| Remote HOME | **HOME** short tap | forwarded to ThinServer — bindable like any button |

`thinclient.cfg` format (one line, whitespace-separated tokens):
`<host-or-any> port=<1-65535> auto=<0|1>`, e.g.
`192.168.1.10 port=50001 auto=0`, `thinserver.home.lan port=5080 auto=1`
or `any auto=1`. `<host>` is a DNS name or dotted IPv4; `any` selects LAN
broadcast discovery. `port` defaults to 50001. Files written by older
builds (bare IP + `auto=`) still load unchanged.

Direct endpoints: when a host is configured the client unicasts `B`
beacons to `host:port` (resolved once per attempt via lwIP DNS) instead
of broadcasting, and only that server address may answer. The server
replies from its observed source endpoint, so no inbound port forwarding
is needed on the GamePad's network (cone-type NAT; symmetric NAT is not
supported). DNS failures and timeouts show as menu status messages.

## Connection behavior

- The app starts in the **menu** and sends no discovery beacons; nothing
  streams until **Connect** is chosen (or auto-connect is on).
- The address editor only enables **Save** for four complete decimal octets
  in the 0–255 range. **Cancel** leaves the saved address unchanged. Saving
  never starts a connection.
- **Connect** beacons for up to 15 s; the first `H` hello from a matching
  server starts the stream. With "any server" the first ThinServer on the
  LAN wins — the configured IP is a *selection filter*, not
  authentication.
- An unexpected drop returns to the menu with "Connection lost", or
  auto-reconnects to the last server when auto-connect is on. A server
  `Q` (shutdown) does the same. Manual **Disconnect** never reconnects.
- While the in-stream menu is open (5 s HOME hold) the decoder is stopped
  and a neutral input report is sent — every remote key, stick, touch and
  click is released until **Resume**.
- **HOME shortcuts** are resolved by a non-blocking state machine
  (`pad/home_keys.c`): a short tap is sent as a remote HOME press+release
  pulse on release; a 5 s hold opens the local menu once; HOME+X sends a
  `'T'` mode request; HOME+Up/Down step the physical backlight; combo
  partners are masked from the remote report while HOME is held so no
  constituent leaks. HOME+B keeps its 2 s emergency exit.

## Input transmitted (server maps to X11)

Buttons, sticks (deadzone + rest calibration), touch (10-sample average,
calibrated to 854×480), accelerometer/gyro, mic, volume slider. Details:
docs/ARCHITECTURE.md. A **short HOME tap is forwarded** to the server
(bindable in ThinServer Settings); only the local shortcut combinations
stay on the GamePad.
