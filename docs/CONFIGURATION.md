# Configuration

WUP ThinClient has very few local settings — it is a terminal. Server-side
configuration (desktop, quality, input map) lives in WUP ThinServer's
`settings.json`; see that repo's docs/CONFIGURATION.md.

## Client-side settings

| Setting | Where | Default |
|---|---|---|
| Connection profiles | Menu → **Add** / **Edit** / **Delete** (up to 8) | none on fresh install |
| Selected profile | tap a row in the profile list | last used, persisted |
| Connect | Menu → **Connect** on the selected profile | manual only |
| Auto-connect | Menu → **Auto: on/off** (per selected profile) | **off** |
| LAN discovery | Menu → **LAN discovery** (broadcast `B`) | manual only |
| Persisted file | `/save/thinclient.cfg` on GamePad flash | — |
| Emergency quit | **HOME + B** ~2 s (any state) | — |
| Local shortcuts | **HOME** held 5 s | in-stream menu (Resume/Disconnect/Reconnect/Controls/Exit) |
| Local shortcuts | **HOME + X** | live Desktop/Gamepad mode switch (`'T'` packet, acked) |
| Local shortcuts | **HOME + D-pad Up/Down** | physical backlight brighter/dimmer (hardware levels) |
| Remote HOME | **HOME** short tap | forwarded to ThinServer — bindable like any button |

### Connection profiles

Each profile stores a display **name**, a **host** (DNS name or dotted
IPv4 — IPv6 is not supported), a **UDP port** (1–65535, default 50002),
a connection **type** (only UDP today) and an **auto-connect** flag.
Names allow letters, digits, space, `.`, `-`, `_` (max 31 chars); hosts
follow DNS label rules (max 253 chars, labels 1–63, no leading/trailing
hyphen, `[A-Za-z0-9.-]` only — no URL parts, credentials or paths).

The profile editor has four fields — Name and Host open a QWERTY
touchscreen keyboard (shift, backspace, clear, save, cancel; the Host
keyboard also offers an **IP** key that switches to the numeric IPv4
pad), Port opens the numeric pad. Saving validates every field and never
starts a connection; Cancel discards the draft. Deleting asks for
confirmation. Selecting a row never connects — only **Connect** does,
except the selected startup profile flagged auto-connect (or the
migrated LAN-discovery auto flag).

### `thinclient.cfg` format (version 2)

```
v=2
sel=<profile-id|-1>
disc=<0|1>
dport=<1-65535>
P<id>|<name>|<host>|<port>|<type>|<auto>
```

`sel` stores the stable profile **id** (not the row index), so renaming,
reordering or deleting other profiles cannot silently retarget it.
`disc`/`dport` are the LAN-discovery auto-connect flag and broadcast
port. Writes happen only when the configuration actually changes
(selection included); there is no atomic rename in the libwup fs API, so
a mid-write power loss can corrupt the file — the parser then falls back
to defaults rather than truncating hostnames.

**Migration:** legacy single-line files
(`<host-or-any> port=<n> auto=<0|1>`, including bare IPv4) are converted
on first load into one profile that keeps the host, port and auto flag;
`any auto=1` becomes discovery auto-connect with no profiles. Legacy
hosts were capped at 47 chars, so migrated names/hosts never needed
truncation.

Direct endpoints: with a profile selected the client unicasts `B`
beacons to `host:port` (resolved via lwIP DNS at each connect attempt —
numeric IPv4 skips DNS entirely) instead of broadcasting, and only the
resolved address may answer. The server replies from its observed source
endpoint, so no inbound port forwarding is needed on the GamePad's
network (cone-type NAT; symmetric NAT is not supported). DNS resolution
is blocking inside lwIP's own timeouts: the connecting screen stays up
with "Looking for host:port ..." meanwhile, and a failure returns to the
profile list with "Cannot resolve ..." — it never enters the connecting
state.

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
