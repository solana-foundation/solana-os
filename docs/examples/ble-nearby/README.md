# Nearby: who is around, over Bluetooth, without connecting

Each badge running this app advertises four bytes under one service (its team and its
score) and scans for the same service. Every badge it hears is a row: name, team, score and
signal, strongest first. Nobody connects, so the badge's one-link limit doesn't apply: any
number of badges can take part.

- `main.lua`, `app.json`: the app (permission `ble`).
- SELECT adds a point, LEFT/RIGHT changes team; each change calls `ble.advertise` again
  with the new bytes. A badge not heard for 10 seconds drops off the list.

The pattern fits anything a badge only needs to *say* to whoever is near: a status, a
team, a "find me" beacon. A gadget can do the same: an ESP32 that advertises the service
with manufacturer data is a row too. Advertised data is small: 24 bytes, 20 with a 16-bit
service, 6 with a 128-bit one (this app uses 4).

**Try it in the emulator.** Open `/emulator` and `/emulator?badge=2` in two tabs of one
browser, run the app in both (Try in emulator from `/apps/submit`, or copy it over each
tab's console), and keep both tabs visible. Each lists the other. On badges, use Run on
badge on each.

**Trust.** Anyone can advertise any team and score under your service UUID. Use this for
presence and fun, and let your server decide anything worth cheating at
([Connectivity: Trust](../../CONNECTIVITY.md#trust)).

Reference: [`ble.advertise` and `ble.scan`](../../LUA_API.md#being-found-bleadvertise).
