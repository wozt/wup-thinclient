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
| In-stream menu | **HOME** while streaming | Resume/Disconnect/Reconnect/Controls/Exit |

`thinclient.cfg` format (one line, whitespace-separated tokens):
`<ip-or-any> auto=<0|1>`, e.g. `192.168.1.10 auto=0` or `any auto=1`.
Files written by older builds (bare IP only) still load — auto-connect
defaults to off.

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
- While the in-stream HOME menu is open the decoder is stopped and a
  neutral input report is sent — every remote key, stick, touch and click
  is released until **Resume**.

## Input transmitted (server maps to X11)

Buttons, sticks (deadzone + rest calibration), touch (10-sample average,
calibrated to 854×480), accelerometer/gyro, mic, volume slider. Details:
docs/ARCHITECTURE.md. The GamePad **HOME** button is consumed locally and
never reaches the desktop.
