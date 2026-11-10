# Connectivity: Wi-Fi, the mesh and Bluetooth

How a badge app talks to other badges, to gadgets, to phones and to servers, which way to
pick for what, and working examples to start from. For Solana OS 1.0.0. The calls
themselves are in the [Lua API](LUA_API.md); this guide is about choosing between them.
The web shows it at `/apps/docs/connectivity` (raw Markdown: `/docs/connectivity.md`).

## The short version

A badge reaches the world three ways, all through one radio:

| | Reaches | Needs | Good for |
|---|---|---|---|
| **The mesh** (`mesh`) | badges and quest beacons near it, running your app | nothing: no Wi-Fi, no pairing, no setup | presence, small game messages, proving two badges met |
| **Bluetooth** (`ble`, 1.0.0) | anything else that speaks Bluetooth LE: a gadget you built, a phone, a laptop, another badge | your app turns it on; about 30 KB of the badge's memory while it is on | gadgets, phones, two badges that both stay on Wi-Fi |
| **Wi-Fi** (`http`, `ws`, `rpc`) | the badge server, your own server, public APIs, Solana | Wi-Fi joined | anything far away, anything that must be counted or trusted |

If two badges in one room should play together, start with the mesh. If the other end
isn't a badge, use Bluetooth. If it's far away, or something is worth cheating at, go
through a server.

## The three ways

### The mesh (ESP-NOW, permission `mesh`)

Badges and quest beacons at an event share one **mesh channel**, which the server
announces. On it they hear each other directly over ESP-NOW, a short-range radio protocol,
with Wi-Fi on or off. Your app sees the badges around it in `mesh.nearby()` (with their
signal, `rssi`) and sends messages with `mesh.send(id or "*", table)`; `on_mesh` receives
them on badges running the same app.

- **Small and lossy.** A message is ≤ 200 bytes as JSON, 4 a second. Nothing confirms it
  arrived: send state ("my score is 12"), not events ("+1"), and repeat what matters until
  the other side answers.
- **No setup.** No pairing, no connection, no network. Any number of badges.
- **Proof.** `mesh.send` is unsigned. When your server must believe that two badges met,
  use a **meet** (`mesh.meet`): both badges sign one statement with keys only they and the
  badge server know, and the meet reaches your server through the badges' outboxes,
  later if they're offline (LUA_API.md "Meets").
- **Bridges.** Badges out of range of each other, and emulators (they have no radio), meet
  over a **bridge** through the server: one shows its code (`mesh.bridge()`), the other
  types or scans it, and after that `mesh.send` and `on_mesh` work as over the air, about
  1 to 3 seconds slower. Both need Wi-Fi.

### Bluetooth LE (permission `ble`, Solana OS 1.0.0)

For whatever isn't a badge on the mesh: a gadget you built on any ESP32 (or anything else
that speaks Bluetooth LE), a phone, a laptop, or another badge while both stay on Wi-Fi.
Bluetooth has its own channels, so it works whatever Wi-Fi channel or band the badge is on.

- **Off until you ask.** `ble.on()` turns it on for your app, `ble.off()` or the app's exit
  turns it off. While it's on it takes about 30 KB of the badge's own memory (see
  [Memory](#memory)).
- **Three ways to use it.** Advertise a few bytes for anyone listening (`ble.advertise`:
  ≤ 24 bytes, 20 with a 16-bit service, 6 with a 128-bit one), scan for what others
  advertise (`ble.scan`), or hold **one** connection: the badge connects to a device
  (`ble.connect`) or serves one service a device connects to (`ble.serve`). A value is
  ≤ 244 bytes.
- **No pairing, no encryption.** Anyone in range sees what you advertise and can connect to
  what you serve. See [Trust](#trust).
- **In the emulator**, emulated badges in tabs of one browser find, connect to and serve
  each other, and nothing else is in range.

### Wi-Fi: the server and your own hosts (`http`, `ws`, `rpc`)

With Wi-Fi joined, an app reaches anything on the internet, always through rules the badge
server keeps:

- **`http`**: requests to the hosts your app lists, made by the badge server for the
  badge. 60 a minute per badge, ≤ 2 KB sent, ≤ about 3.5 KB of answer, 6 s. Each request
  carries headers the badge server signs, so your server knows which badge and player is
  asking.
- **`ws`**: one live WebSocket to one of your hosts, straight from the badge, for messages
  both ways as they happen (≤ 4 KB each). While it is open the badge isn't connected to
  the badge server, so your other server calls answer `"socket open"`.
- **`rpc`**: Solana's JSON-RPC (reads, and sending transactions signed elsewhere).
- The badge server's own calls (`lamps`, `items`, `profile`, `quest`, `mesh.meets`).

All of it works the same in the emulator, which makes Wi-Fi the easiest path to test.

## Choosing

Find what your app needs below; each says what to use, what it costs in memory and what
to know.

### Badge to badge in the same room

- **Use:** the mesh, `mesh.send` / `on_mesh`; `mesh.meet` when your server must trust it.
- **Memory:** nothing beyond your app.
- **Know:** messages get lost: repeat them. All badges must be on the mesh channel. A
  `mesh` app takes the radio from Wi-Fi joined on another channel while it's open
  ([One radio](#one-radio-shared)).

### Badge to badge across the venue, or with an emulator

- **Use:** a bridge (same `mesh` code, `mesh.bridge()`), or a `ws` room on your server.
- **Memory:** bridge: nothing beyond your app. `ws`: 56 KB free to start, about 5 KB while
  open.
- **Know:** both need Wi-Fi. A bridged message takes about 1 to 3 seconds: turn-based games
  work, fast action doesn't. With `ws`, other server calls wait while the socket is open,
  and your server must be up.

### Badge to your own gadget

- **Use:** Bluetooth, `ble.scan` + `ble.connect` to talk to one; `ble.scan` alone to hear
  many that advertise.
- **Memory:** about 30 KB while Bluetooth is on.
- **Know:** one connection at a time; for several gadgets, have them advertise their state
  and just scan. The emulator can't reach a gadget: test with Run on badge. Anyone can
  build a gadget that looks like yours.

### Badge to a phone

- **Use:** Bluetooth, `ble.serve` (the phone connects; a Nordic UART layout works with
  common phone apps). Or a QR code (`ui.qr`) to a page on your server, which both sides
  reach.
- **Memory:** about 30 KB while Bluetooth is on; a QR code is one UI object.
- **Know:** the phone needs an app that speaks Bluetooth LE. Phones change their Bluetooth
  address every few minutes: find them by service, not by address. To sign a transaction,
  use a phone wallet ([Build on the badge, sign on a phone](LUA_API.md#build-on-the-badge-sign-on-a-phone)).

### A room where only Wi-Fi works

Its access points aren't on the mesh channel, or the organisers want everything through
the server.

- **Use:** a `ws` room on your host, or `http`.
- **Memory:** 56 KB free to start a secure connection.
- **Know:** everything depends on the venue's Wi-Fi and your server. While a socket is
  open, `lamps`, `items`, `http` and the rest answer `"socket open"`: call them before
  `ws.open`. 20 socket opens a minute per badge.

### Two badges that both stay on Wi-Fi

Joined on a channel that isn't the mesh channel.

- **Use:** Bluetooth, as [`ble-link`](examples/ble-link/) does: the two badges still talk
  directly, and it doesn't take the radio from Wi-Fi the way a `mesh` app does.
- **Memory:** about 30 KB while Bluetooth is on.

## One radio, shared

The badge has one radio. Wi-Fi (2.4 or 5 GHz) and ESP-NOW on the mesh channel share it, and
Bluetooth takes turns with both.

**The mesh channel and Wi-Fi.** `mesh.info()` (1.0.0) tells your app where it stands:

```lua
local i = mesh.info()   -- { channel = 149, band = "5", online = true, paused = false }
```

- `online = true`: Wi-Fi is joined **on the mesh channel**. The badge is on the mesh and
  online at once, and your `mesh` app never pauses Wi-Fi. Measured on a badge joined to a
  5 GHz network on the mesh channel: the mesh and the server worked at the same time.
- `online = false`: Wi-Fi is off, or joined on another channel. While joined, the radio
  can't change channel, so opening your `mesh` app took the radio from Wi-Fi: server calls
  answer `"offline"` and meets wait in the outbox until the app closes.
- To reach the server without leaving: `mesh.pause()` hands the radio back (Wi-Fi rejoins
  in a few seconds; wait for `badge.online()`), make your calls, then `mesh.resume()`.
  While paused, messages and meets don't arrive, and nearby badges drop out of
  `mesh.nearby()` after 30 seconds. [`mesh-and-wifi`](examples/mesh-and-wifi/) does this.
- **Sync first, then mesh.** Fetch what the app needs (a profile, scores) while it's cheap:
  at launch when `online` is true, or in one pause; then play on the mesh; sync again when
  the game ends. Don't pause for every message.
- `online` can change while your app runs (Wi-Fi drops, or the server announces another
  channel): read `mesh.info()` when you need it, not once at launch.

**A 5 GHz mesh channel** changes nothing in your code: `mesh.info().band` is `"5"` and
everything else works as on 2.4 GHz. It changes who can be on the mesh. The mesh channel is
1 to 13 on 2.4 GHz, or one of 36, 40, 44, 48, 149, 153, 157, 161 and 165 on 5 GHz (the
channels without radar rules; docs/MESH.md "Which channels"). Only badges on Solana OS
1.0.0 or later, ESP32-C5 quest beacons (beacon firmware 0.4.0 or later) and badge beacons
can follow it to 5 GHz; older badges and beacons on other chips stay where they were, off
the mesh. So the organisers pick a 5 GHz mesh channel only when every badge has updated
and the quest beacons are C5s or badges, and your app needs no check for it. What was
measured: on channel 149 a badge's ESP-NOW frames to a 5 GHz access point were
acknowledged 20 of 20 times, sent at 6 Mbps (2.4 GHz frames go at 1 Mbps; expect less range
through walls and people). On the 1.0.0 build, a badge
joined on channel 6 with the mesh moved to 149 took the radio for a `mesh` app, and
after `mesh.pause()` Wi-Fi rejoined the same network's access point on 149: `mesh.info()`
then read `online = true` on 5 GHz. Two badges exchanging frames on a 5 GHz channel is
still untested (it needs two badges in one place).

**Bluetooth next to both.** Bluetooth has its own channels, so it doesn't care which Wi-Fi
channel or band the badge is on. Measured on a badge with Wi-Fi joined on 2.4 and on 5 GHz,
with Bluetooth on: it advertised (a Mac saw it), scanned (16 to 45 devices found in a few
seconds, at 30 % and at 100 % duty), and held a connection from a Mac (connected in about
a second), and meanwhile a server request answered 200, ESP-NOW frames were acknowledged
30 of 30, and a full TLS handshake succeeded with a link open. The radio takes turns, so a
busy scan slows Wi-Fi and the mesh a little: scan at the default duty (30 %) unless you
must find something fast, give a scan `seconds`, and stop it (`ble.scan(false)`) once you
have what you need. The other way round measured the same: the badge
connected out to a Mac playing a gadget, read, wrote and took its pushes, and with that
link open and a scan running a server request still answered 200 and the mesh kept
sending its HELLOs with no errors. A device that isn't there is given up on after 10
seconds (`"unreachable"`).

**For organisers.** A quest beacon's radio follows its access point, so its access point
must be on the mesh channel; the best setup is a dedicated network pinned to it.
Badges whose network is on the mesh channel get `online = true` everywhere on the mesh.

## Memory

The badge has no extra RAM chip: your app, Wi-Fi, secure connections and Bluetooth share one
heap. Measured at the launcher with Wi-Fi joined:

| | Free heap |
|---|---|
| Bluetooth not in the build (Solana OS before 1.0.0) | about 149 KB |
| Bluetooth in the build, off | about 145 KB |
| Bluetooth on | about 115 KB |
| Bluetooth on, a link open (either way round) | about 110 KB |
| Bluetooth on, a link open and a scan running | about 106 KB |

So Bluetooth costs every badge about 4 KB, and about 30 KB more while an app has it on (up to about 39 KB with a link open and a scan
running);
`ble.off()` gives that back. Your Lua heap has its own cap of 96 KB, and the badge stops an
app that would leave the badge's own heap under 32 KB ("The badge is low on memory"). With Wi-Fi
joined, a test app with 60 labels reached about 80 KB of Lua heap before that reserve
stopped it, and about 49 KB with Bluetooth on. Lighter apps go further; apps with many UI
objects stop sooner.

What needs room, and when:

- `ble.on()` refuses with `"not enough memory"` while the badge has under 64 KB free.
- A new secure connection (`ws.open`, the App Store, the badge's own server connection)
  needs 56 KB free to start, takes up to about 40 KB for a moment, and an open socket keeps
  about 5 KB.

So do things in this order:

1. **Turn Bluetooth on first**, before you build big tables, load images or read files:
   `if ble.on() then ... end` at the top of `main.lua`.
2. **Open your socket, or make your server calls, before loading big data.** `http` and the
   other server calls go through the badge's existing connection, but when that connection
   has to be made again it needs room too (up to the same 56 KB).
3. **Turn Bluetooth off when you're done with it**: after reading a gadget once, or when the
   player leaves the screen that needs it. Turn it on again later the same way.
4. **Measure on a badge.** `lua mem` on the console shows the last run's peak Lua heap,
   `heap` the badge's free heap. A socket and Bluetooth together haven't been measured: if
   your app needs both, open the socket first, then turn Bluetooth on, and check `heap`.

## Trust

The mesh and Bluetooth are open air, and every badge's owner can change the Lua on it:

- **Everything that arrives is untrusted.** Check types, lengths and ranges before you use
  a message, an advertisement or a written value, and keep text printable ASCII before it
  goes on screen. Names in scans arrive printable already; data doesn't.
- **Anyone can pretend.** A gadget, a phone or a badge can advertise your service UUID and
  any data, and connect to what you serve. A Bluetooth address says nothing about who is
  there, and phones change theirs.
- **Make up your own service UUID** (`uuidgen`), and scan for it. A well-known one matches
  other people's devices: testing this release, a scan for the Nordic UART service found a
  stranger's gadget in the next room and the test connected to it. If you use a well-known
  layout on purpose (so phone apps can talk to your badge), filter by name as well, as
  [`ble-link`](examples/ble-link/) does, and connect only to what the player picked.
- **What a meet proves.** Both badges' keys signed one statement: both badges took part, in
  your app, with that data. Not that two people stood together: two owners who cooperate can
  pass messages along, and a bridge does exactly that (`bridged`). Count a pair of people
  once.
- **Your server decides.** `http`'s signed headers and `ws` tickets prove which badge and
  account is calling through your app's id, not that your code is what's calling. Keep the
  score, the rules and anything of value on your server, and count people (the owner's
  user id), not badges.
- **Never a secret on a badge.** Not an API key, not a keypair, not a password: `main.lua`
  is readable by anyone who installs the app, the card comes out, and badges change hands.
  Nothing on Bluetooth or the mesh is encrypted, so nothing sent there is secret either.

## Testing each path

| Path | How |
|---|---|
| Mesh between two badges | two emulator tabs of one browser: `/emulator` and `/emulator?badge=2`. Turn Wi-Fi on once in each to get a badge id, keep both tabs visible (background tabs are throttled), run your app in both |
| Bluetooth between two badges | the same two tabs: emulated badges advertise to, find, connect to and serve each other |
| An emulator with a real badge | a bridge: one shows its code (`mesh.bridge()`), the emulator page's **Bridge a badge** panel scans or pastes it. Bridges carry the mesh, not Bluetooth |
| A gadget, or a phone | **Run on badge** on `/apps/submit` (desktop Chrome or Edge, USB): the emulator has no radio. `print()` shows in the console below |
| `http`, `ws`, `rpc` | Try in emulator or Run on badge, signed in: a test run lets the draft's calls reach its hosts for 12 hours (your account needs X linked) |
| Memory | Run on badge, then `lua mem` and `heap` on the console, with Wi-Fi joined and Bluetooth on if your app uses it |

On a badge's console (USB, 115200 baud), `ble` drives Bluetooth by hand without an app:
`ble on`, `ble adv <name> <service> <hex> conn`, `ble scan 5 active`, `ble serve <service>
<chr>:rwn`, `ble connect <address>`, `ble read|write|sub <service> <chr>`, `ble status` (who
has it, the badge's address, the heap). It is refused while an app has Bluetooth on.

## Older badges

- `ble` is `nil` below Solana OS 1.0.0, `mesh.info` too; `mesh.meet` and `mesh.pause` are
  0.5.15. Check before you call: `if ble then`, `if mesh.info then`.
- The App Store doesn't offer a `ble` app to a badge below 1.0.0: its details say *This app
  needs Solana OS 1.0.0 or later* and how to update, and buying and downloads are refused
  with "Update Solana OS to 1.0.0 or later to install this app". A check like `if not ble`
  still matters for apps copied over the console or run from the web editor.
- A `mesh` app without `ble` runs on every badge from 0.5.0. A badge below 1.0.0 ignores a
  5 GHz mesh channel, so it's simply not on the mesh at such an event.

## Examples

Each folder is a complete app you can drop on `/apps/submit`, with a README:

| Example | Shows |
|---|---|
| [`ble-button-box/`](examples/ble-button-box/) | a Bluetooth gadget for any ESP32 (an ESP-IDF project: one service, one characteristic that notifies when a button changes) and the badge app that finds it, subscribes and reconnects when it goes away |
| [`ble-nearby/`](examples/ble-nearby/) | no connections: every badge advertises a team and a score and lists who is around, with their signal |
| [`ble-link/`](examples/ble-link/) | two badges on one connection (the Nordic UART layout): a line typed or a key pressed on one shows on the other |
| [`mesh-and-wifi/`](examples/mesh-and-wifi/) | a mesh game that reads `mesh.info()`, tells the player when it is online and on the mesh at once, and syncs a score with `mesh.pause()` / `mesh.resume()`, in batches that are safe to send again when an answer gets lost |
| [`wifi-room/`](examples/wifi-room/) | the same game through the server only: a `ws` room on the app's own host, with the few lines of server it needs |
