# Configuration

WUP ThinClient has very few local settings — it is a terminal. Server-side
configuration (desktop, quality, input map) lives in WUP ThinServer's
`settings.json`; see that repo's docs/CONFIGURATION.md.

## Client-side settings

| Setting | Where | Default |
|---|---|---|
| Server address | Waiting screen, **Y** → IP editor | empty = "any server" (first `H` wins) |
| "Any server" mode | IP editor, **X** | off once an IP is set |
| Persisted file | `/save/thinclient.cfg` on GamePad flash | — |
| Screen brightness | **HOME + Up/Down** | upstream levels |
| Emergency quit | **HOME + B** ~2 s | — |

`thinclient.cfg` format (one line): `ip=192.168.1.10` or `ip=any`.

## Input transmitted (server maps to X11)

Buttons, sticks (deadzone + rest calibration), touch (10-sample average,
calibrated to 854×480), accelerometer/gyro, mic, volume slider. Details:
docs/ARCHITECTURE.md.
