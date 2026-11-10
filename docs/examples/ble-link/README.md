# Link: two badges on one Bluetooth connection

One badge **hosts**: it serves the Nordic UART Service (NUS) and advertises it. The other
**joins**: it scans for the service, connects, subscribes to the host's TX characteristic
and writes to its RX characteristic. A line typed (SELECT opens the keyboard) or an arrow
pressed on either badge shows on both.

| | UUID | |
|---|---|---|
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | advertised by the host |
| RX | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | write: the joiner sends here, the host gets `cb("write", RX, data)` |
| TX | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | read, notify: the host sends with `ble.notify(TX, data)`, the joiner gets `cb("notify", TX, data)` |

NUS is a layout many phone apps and libraries know, so a phone can join a hosting badge
too: in nRF Connect, or any "Bluetooth serial" app that speaks NUS, connect to *Link
&lt;id&gt;*, turn on notifications for TX and write text to RX.

Use this pattern when two badges (or a badge and a phone) need a conversation, not just a
broadcast: a two-player game while both stay on Wi-Fi, a controller, a chat. The badge holds
one link at a time, either way round; for more players, see the mesh or a `ws` room in the
[Connectivity guide](../../CONNECTIVITY.md).

**Try it in the emulator.** `/emulator` and `/emulator?badge=2` in two visible tabs of one
browser: run the app in both, press UP (host) in one and DOWN (join) in the other, pick
the host and press SELECT.

Every byte from the other side is checked: the app shows printable ASCII only and cuts
lines at 60 characters. Nothing on the link is encrypted, and anyone in range can join a
host that advertises.

Reference: [`ble.serve`](../../LUA_API.md#being-connected-to-bleserve) and
[`ble.connect`](../../LUA_API.md#connecting-to-a-device-bleconnect).
