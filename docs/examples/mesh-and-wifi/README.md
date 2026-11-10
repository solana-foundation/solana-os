# Cheer (mesh): the mesh and Wi-Fi in one app

SELECT sends a cheer over the mesh (ESP-NOW) to every badge nearby running the app; each
shows who cheered. Your own count syncs to your server with `http`. The app tells the
player what the radio is doing, from `mesh.info()` (Solana OS 1.0.0):

| `mesh.info()` | What the app says | Syncing |
|---|---|---|
| `online = true` | "On the mesh and online" | posts as it goes: Wi-Fi is joined on the mesh channel, so both work at once |
| `online = false` | "On the mesh, offline" | RIGHT: `mesh.pause()`, wait for `badge.online()`, post, `mesh.resume()` |
| `paused = true` | "Syncing: Wi-Fi has the radio" | cheers from others don't arrive until `mesh.resume()` |

The automatic sync (every 20 cheers) runs only while `online` is true, when it costs
nothing; pausing is left to the player, because it takes the badge off the mesh for a few
seconds.

**Syncing is safe to repeat.** Answers get lost: Wi-Fi drops, or the request times out after
the server has already counted the cheers, and the app can't tell which. So the app saves its
cheers in `storage` as a batch with a random id before sending, sends that same batch until the
server answers with its id, and only then forgets it; the server counts each id once. A batch
holds at most 1,000 cheers, the most the server takes, so a backlog goes up a batch at a time.

- `main.lua`, `app.json`: permissions `mesh`, `http` and `led`, hosts `cheer.example.com`.
- The server side is [`../wifi-room/server.ts`](../wifi-room/server.ts) (`POST /score`):
  put it on your own host and change `HOST` in `main.lua` and `hosts` in `app.json`.
- On older badges: without `mesh.info` (before 1.0.0) the app reads `badge.online()`;
  without `mesh.pause` (before 0.5.15) it syncs only when the badge is online anyway.

**Try it.** Two emulator tabs (`/emulator` and `/emulator?badge=2`) hear each other's
cheers over the emulator's radio. The emulator's demo network is on the mesh channel, so
there `online` is true. To see the pause path, use a badge whose network is on another
channel than the mesh: [Connectivity: One radio](../../CONNECTIVITY.md#one-radio-shared).
