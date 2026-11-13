# Cheer (room): the same game through your server only

For rooms where the radios aren't an option: the venue's access points aren't on the mesh
channel, players are spread across the venue, emulators play along, or the organisers want
everything to go through a server. Every badge opens one WebSocket to the app's own host
(`ws`, permission `http`), SELECT sends a cheer, and the server tells everyone in the room
the total and who is there. LEFT/RIGHT changes rooms.

- `main.lua`, `app.json`: the app, hosts `cheer.example.com`.
- `server.ts`: the server, for Bun. It checks the badge's ticket on every socket and the
  signed headers on `POST /score` (from [`../mesh-and-wifi/`](../mesh-and-wifi/)), as
  [LUA_API.md](../../LUA_API.md#ws-permission-http-solana-os-051) describes, and keeps the
  counts in memory. Each ticket and request counts once: the server checks and remembers its
  id in one step, after the signature, so two copies arriving together can't both get in.
  `POST /score` counts each batch id once and answers it every time, because the app sends a
  batch again when an answer gets lost.

```sh
PUBLIC_URL=https://cheer.example.com PORT=8080 bun server.ts
```

`BADGE_SERVER` (default `https://badge-api.solana.com`) is where it fetches the badge
server's signing key: set it to a stand-in of your own to test without badges.

Put it behind HTTPS on port 443 at the host the app lists (badges open only `wss://` and
`https://` on 443), and set `PUBLIC_URL` to that address: signatures cover the URL the
badge asked for, not the one a proxy hands on. Then change the host in `main.lua` and
`app.json`.

What to know about `ws` on a badge (LUA_API.md "ws"):

- **One connection.** While the socket is open the badge isn't connected to the badge
  server: the app's other server calls answer `"socket open"` and `badge.online()` is
  false. Make those calls before `ws.open`, or close the socket first.
- **Memory.** On a badge below Solana OS 2.0.0 a secure connection needs 56 KB of the
  badge's heap free to start, so the app opens its socket first, while it is small, and would
  load big data after. From 2.0.0 the connection takes PSRAM and the order doesn't matter.
- **Rate.** 20 opens a minute per badge: the app reconnects after 2 seconds, doubling up to
  30, and waits a second after the last room change before opening the new room.

**Try it.** The emulator's socket is the browser's: Try in emulator works once the server
is reachable at its `wss://` host, and two tabs make two players. A test run (signed in,
X linked) lets the draft's `ws` reach your host before you publish.
