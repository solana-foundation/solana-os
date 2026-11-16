# Pinger: a background service that answers the mesh

The smallest useful background service (permission `background`, Solana OS 2.3.0).
SELECT pings every badge running Pinger nearby; each one's **service** answers with a
pong and remembers who pinged, whether its app is open or not.

- `main.lua`: the app. It starts the service (the first time the badge asks the owner:
  SELECT allows), sends pings, shows how many badges pinged you (from `storage`, which
  the app and its service share) and refreshes when the service says so (`on_service`).
- `service.lua`: the service, in a state of its own: `on_mesh` answers and counts,
  `service.status` is its line in the badge's Services app, `service.notify` a toast
  (one every 10 s at most).
- `app.json`: `"permissions": ["background", "mesh"]`. The store refuses `background`
  without a `service.lua` next to `main.lua`, and badges below 2.3.0 don't get the app.

What a service can't do matters as much: no `ui`, LEDs, Wi-Fi or Bluetooth, nothing that
spends, no `mesh.channel` or `mesh.scan`. It hears the mesh only while the radio is on
the mesh channel anyway (Wi-Fi off, or joined on that channel): it never moves the radio.
Limits: 1 MB of heap, 100 000 instructions a callback, 2 000 000 a minute, a tick a
second, 3 services on a badge.

**Try it.** In the emulator, `/emulator` and `/emulator?badge=2` in two tabs: run Pinger
in both, close it in one (HOME), ping from the other: the closed one still pongs, and
`svc` on its console lists `app:pinger` with its count. On a badge, the Services app
shows it and stops it.

Reference: [`service`](../../LUA_API.md#service-permission-background-solana-os-230).
