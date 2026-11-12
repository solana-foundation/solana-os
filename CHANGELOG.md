# Changelog

Solana OS releases, newest first, from this repository's history. The version is
`CONFIG_APP_PROJECT_VER` in `sdkconfig.defaults`; a badge takes an over-the-air
update only to a newer one. Dates are commit dates. The contracts these builds
implement (`docs/LUA_API.md`, `docs/MESH.md`, `server/API.md`) and the server and
site changes that came with them live in the parent repository.

## 1.1.2 (2026-11-12)

- **Production badges show true colours again.** Their panels need colour inversion,
  which 0.4.3 turned off for good after testing on the October badges, which turned out
  to be prototypes (ESP32-C5 rev 1.2 modules; production badges carry rev 1.0). The
  two panels read the same over SPI (same ILI9341, same registers), so inversion is on
  by default again and **Settings > Invert colours** turns it off on a prototype. It
  applies at once and at every boot (`os_invert_apply()` from `os_splash()`). The
  setting keeps 0.4.1's NVS key (`settings/lcd_invert`, 1 = on), so a badge that saved
  it off then (a prototype) stays right. The console's `info` prints the panel's state.
  `components/bsp/bsp_display.c` (`bsp_display_invert()`, under the LVGL lock and the
  LCD hold of `bsp_bus.h`), `main/core/os_settings.*`, `main/apps/settings/`.

## 1.1.1 (2026-11-12)

- **Fixes to 1.1.0's scans and app channels.** A scan's callback no longer depends on its
  completion event reaching the Lua task: the host delivers finished results
  (`os_mesh_scan_ready`) even if its event queue was full. `mesh.pause()` drops a
  waiting callback, so a paused scan never calls back with nothing. Each frame now
  carries the channel it was heard on (`rx_ctrl.channel`, through reassembly), so a
  HELLO read after a hop isn't counted on the next channel. A scan that ends after
  `mesh.channel(n)` (or the server) moved the mesh off the joined network's channel
  leaves Wi-Fi suspended for the app instead of rejoining it. A bridge taking Wi-Fi
  stops a running scan (no callback), and a script error stops the app's scan and
  gives back its channel at once, not when the error screen is dismissed. The host clears both
  again once the script's state is closed, so an `on_exit` (or a finalizer) that scans
  or calls `mesh.channel` can't leave either behind, whatever ended the app.
  `main/core/os_mesh.*`, `main/core/os_radio.*`, `main/lua/lua_net.c`,
  `main/lua/lua_host.c`; `docs/LUA_API.md` lists every way a scan ends.

## 1.1.0 (2026-11-11)

- **Lua apps choose their mesh channel and scan for one** (`docs/LUA_API.md` "Choosing a
  channel", `docs/MESH.md` "App channels and scans"). `mesh.channel(n)` moves this badge's
  mesh to any mesh channel (1 to 13, or the nine 5 GHz ones without radar rules) while
  the app runs, `mesh.channel()` goes back, and closing the app always does: nothing is
  saved, and the server's announcements change only `mesh.info().event` meanwhile.
  Peers heard on the old channel drop out of `mesh.nearby()`, and Wi-Fi joined on the
  old channel is suspended for the app, as at its start. `mesh.scan([opts,] cb)` hops
  through channels (`channels`, or `band` "2.4"/"5"/"all"; `dwell` 500 to 5000 ms,
  2600 by default), only listening, and answers per channel the badges and beacons
  whose HELLOs it heard (24 in all) and which of them sent this app's messages. Wi-Fi
  joined on the mesh channel is lent to the scan and comes back after it; `mesh.pause()`
  or closing the app ends it. `mesh.info()` gains `event` and `scanning`.
  `main/core/os_mesh.*` (`os_mesh_app_channel`, `os_mesh_scan*`: the mesh task hops,
  results in ~1.2 KB allocated only while an app scans; a mesh holder now also takes the
  radio from a network off the mesh channel whenever the channel changes, not only at
  acquire), `main/lua/lua_net.c`, `tests/t-chan`, `mesh status` lines for both.

## 1.0.0 (2026-11-10)

- **Bluetooth LE for Lua apps** (`ble`, permission `ble`; `docs/LUA_API.md` "ble"). An app
  can advertise a few bytes (≤ 24, 20 with a 16-bit service, 6 with a 128-bit one), scan
  for devices, connect to one (`ble.connect`: read, write, subscribe) or serve one service
  of up to 4 characteristics that a phone, a gadget or another badge connects to
  (`ble.serve`). One link at a time, either way round, ≤ 244 bytes a value, no pairing and
  no encryption. It reaches what the mesh can't: a gadget built on any ESP32, a phone, a
  laptop, or another badge while both stay on Wi-Fi. Bluetooth has its own channels, so
  it works on either Wi-Fi band and next to the mesh. `main/core/os_ble.*` (NimBLE,
  device-only: one owner at a time, events waiting for it as for `os_socket`),
  `main/lua/lua_ble.c` (the module), `emulator/platform/os_ble_emu.c` (emulated badges in
  tabs of one browser find, connect to and serve each other over a `BroadcastChannel`;
  nothing else is in range), `tests/t-ble`, and a `ble` console command. `ble` on the
  console turns Bluetooth on, advertises, scans, serves, connects, reads, writes and
  subscribes by hand, prints each event, and is refused while an app has Bluetooth.
- **The build has Bluetooth again** (`CONFIG_BT_ENABLED`; it was left out after the old BLE
  scanner app cost about 41 KB while open): NimBLE trimmed to one link and no pairing, with the
  controller's code in flash (`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY`) so it takes little of the
  IRAM, which shares the SRAM with the heap. Bluetooth is off until an app calls `ble.on()`,
  and goes off when that app ends. Measured at the launcher with Wi-Fi joined: about
  149 KB free without Bluetooth in the build, 145 KB with it in the build and off, 115 KB
  with it on, 110 KB with a link open, 106 KB with a link and a scan. So every badge pays
  about 4 KB (3.6 KB of static RAM), and about 30 KB more while an app has it on. A test
  app with 60 labels reached 49 KB of Lua heap with Bluetooth on before the 32 KB reserve
  stopped it, against 78 KB without (same build). The image grew by about 333 KB. `ble.on()` refuses under 64 KB
  free; a new secure connection still needs 56 KB.
- Bluetooth leaves the heap as it found it: 20 on/off cycles end with the same free heap
  and largest block. ESP-IDF makes an interrupt line's descriptor (16 B, never freed) the
  first time the line is used, and which lines the controller gets depends on what else
  holds one, so `os_ble_init()` turns Bluetooth on and off once at boot while holding six
  spare lines: every line it can get later has its descriptor already, instead of two new
  ones landing in the middle of the heap under an app.
- Measured with Bluetooth on next to Wi-Fi joined on 2.4 and on 5 GHz: the badge advertised
  (a Mac saw it), scanned (16 to 45 devices in a few seconds, at 30 % and 100 % duty) and
  held a connection from a Mac (connected in about a second), while a server request
  answered 200, ESP-NOW frames were acknowledged 30 of 30, and a full TLS handshake
  succeeded with the link open. On the 1.0.0 build, both ways
  round against a Mac: the badge served a service (the Mac read it, wrote to it and got a
  push) and connected out to one the Mac served (read, write with and without an answer,
  21 pushes), with a server request answering 200 and the mesh's HELLOs going out
  meanwhile, and both links outliving two NVS writes (the controller's code runs from
  flash). `tests/t-ble` on the badge: 44 of 44 as the served side, 49 of 50 connecting out
  (the one a Mac can't pass: it can't advertise manufacturer data); two emulated badges:
  all of it. Close reasons seen: `"closed"`, `"peer closed"`, `"unreachable"` (after
  9.8 s). Not seen: `"lost"` (it needs a peer that vanishes without hanging up). The
  button box example builds for the ESP32, C3 and C5 and hasn't run against a badge.
- **5 GHz mesh channels** (`docs/MESH.md` "Which channels"). The mesh channel may be 36,
  40, 44, 48, 149, 153, 157, 161 or 165 besides 1 to 13; a hello naming any other channel
  is ignored and the badge keeps the one it had. The radio also tuned to and sent on 36,
  52, 100 and 165, and the driver doesn't refuse the radar (DFS) channels, which is why
  only the channels without radar rules are allowed. On channel 149 a unicast ESP-NOW frame
  to a 5 GHz access point was acknowledged 20 of 20 times, sent at 6 Mbps (1 Mbps on
  2.4 GHz). Joined to a 5 GHz network on the mesh channel, the mesh and the server work at
  once; joined on another channel, a `mesh` app takes the radio from Wi-Fi while it's open,
  as before. Badges below 1.0.0 ignore a channel above 13, so a 5 GHz mesh channel is for
  events where every badge has updated. Not tested: receiving ESP-NOW on 5 GHz (it needs
  two badges). One check for all of it,
  `os_mesh_channel_ok()`: hello's `mesh`, the channel saved in NVS, `mesh channel <n>` on the
  console (`52 is not a mesh channel (a radar channel)`), and the ESP-NOW app's Mesh tab,
  which still listens everywhere but won't move the mesh onto a radar channel. The Wi-Fi
  and ESP-NOW apps and `mesh status` say the band. On the 1.0.0 build: with the mesh
  moved to 149 and Wi-Fi joined on 6, a `mesh` app took the radio, and after
  `mesh.pause()` Wi-Fi rejoined the same network on 149, so `mesh.info()` read `online =
  true` on 5 GHz. Beacon firmware 0.4.0 follows a 5 GHz channel on the ESP32-C5 and says
  why it can't on the other chips (built, not run on a beacon).
- Review fixes (Greptile on solana-os#11). Bluetooth: a subscription now outlives the
  lookup caches (its handles live in a table of their own, 8 a link; on a badge the
  pushes stopped after the ninth characteristic looked up, and now keep coming through 25
  lookups until the unsubscribe); `ble.notify` asks the radio instead of a flag that a
  dropped `"subscribe"` event left false; callbacks owed by callbacks wait for the next
  round, so a reconnect in a `"busy"` callback can't starve keys and timers (`tests/t-ble-busy`);
  the emulator stops a connectable advertisement when it connects out and drops what
  peers send past 8 waiting events, as the badge does. The example server (`wifi-room`)
  and the server snippets in `docs/LUA_API.md`: a request or ticket is claimed once, in
  one step after its signature is checked; requests that arrive during the first key
  fetch share it; a batch of cheers has an id, is sent at most 1,000 at a time, and counts
  once however often it is resent.
- **`mesh.info()`**: `{channel, band, online, paused}`. `online` is true when Wi-Fi is
  joined on the mesh channel, so the app is on the mesh and the server at once and never
  pauses Wi-Fi; `paused` between `mesh.pause()` and `mesh.resume()`.
- **The Connectivity guide** (`docs/CONNECTIVITY.md`, on the site at
  `/apps/docs/connectivity`): which of the mesh, Bluetooth and Wi-Fi to use for what, how
  the one radio is shared, memory planning with the numbers above, trust, testing, and
  older badges. With complete examples in `docs/examples/`: `ble-button-box` (an ESP-IDF
  NimBLE gadget for any ESP32, built for the ESP32 and C3 on ESP-IDF 5.5.4 and the C5 on
  6.0.1, and its badge app), `ble-nearby`, `ble-link`, `mesh-and-wifi`, and `wifi-room` (a
  `ws` room, with its Bun server).
- The store keeps `ble` apps from badges below 1.0.0, as it keeps `rpc` apps from badges
  below 0.5.2 (`server/API.md`).
- Not measured: the range of Bluetooth or of ESP-NOW on 5 GHz, battery life with Bluetooth
  on, how many badges one room carries, Bluetooth during a software update.

## 0.5.15 (2026-11-10)

- **Meets: Lua apps can prove two badges met** (`docs/LUA_API.md` "Meets",
  `docs/MESH.md` "Meets"), the way X Dex swaps cards with Wi-Fi off. `mesh.meet(id,
  data, cb)` asks a nearby badge running the same app; its `on_meet(from, data)`
  returns its own data to accept. Both badges then sign one statement with their mesh
  keys (app id, both badge ids, a nonce and a clock each, each side's data, radio or
  bridge), each queues it in the outbox with the *other* badge's tag, and the OS uploads
  it when the badge next has Wi-Fi, after the app closed too. The server stores it once
  for the app's author, who reads the meets with the app's key. The host builds every
  signed string with the app's id in it, so a script can't get a tag that counts for X
  Dex, a trade or a quest. The messages ride in the app's own mesh messages (`m` in
  place of `d`), so badges on older builds ignore them. `mesh.meets` lists what the
  server holds. `main/lua/lua_meet.c`; `tests/t-meet`. Frames get lost: the asker repeats
  its request for 12 s, the accepter repeats its answer until the asker's last message
  arrives (it waits for one at a time, so a second request can't push an unfinished meet
  out), and a copy that couldn't be saved is tried again (Greptile on solana-os#10).
- `mesh.pause()` and `mesh.resume()`: an app with `mesh` can hand the radio back to
  Wi-Fi and take it again, to sync without closing (X Dex's "sync first").
- `mesh selftest` checks the meet tag's test vector.
- Measured: two emulated badges meet, with Wi-Fi on (both uploads answer 200, the server
  keeps one meet) and with Wi-Fi off (one upload waiting in each outbox; the asker's, once
  it has Wi-Fi again, settles the meet for both). On a badge (USB, Wi-Fi joined): 149 KB
  free at the launcher as on 0.5.13, 110 KB with `tests/t-meet` running (20 KB of Lua
  heap), the self-test passes, and with the mesh on channel 1 and the network on channel 6
  `mesh.pause()` brought Wi-Fi back for a server call and `mesh.resume()` suspended it
  again. Not measured: a meet between two badges over the air, or over a bridge.

## 0.5.14 (2026-11-09)

- The Lua allocator's 32 KB reserve holds for small allocations too. 0.5.13 reset its
  per-KB check counter before refusing, so Lua's retry of the same allocation after a
  collection, and the small allocations after it, went unchecked until the next KB: an app
  built of small strings or tables could creep under the reserve. Now the counter only
  resets when a check passes, so from a refusal every growth is checked until one does
  (Greptile on solana-os#9). `tests/t-heap` makes 60 labels and then fills the heap with
  200-byte strings: on a badge with Wi-Fi joined it stopped at 80 KB of Lua heap with
  32.4 KB of system heap left, the badge working.

## 0.5.13 (2026-11-09)

- **About 40 KB more free heap, and 96 KB of Lua heap per app** (was 48 KB;
  `LUA_HOST_HEAP_MAX`, `docs/LUA_API.md` "Limits"). The Lua allocator also keeps the
  badge's own share: it refuses growth that would leave the system heap below 32 KB
  (`LUA_HOST_SYS_RESERVE`; Lua collects garbage and retries, then the app stops with
  "The badge is low on memory"), so an app can take what is free without starving the
  OS, mesh and cloud tasks. Measured on a badge at the launcher on USB: 157 KB free and
  a 139 KB largest block with the radio on and no network joined (0.5.11: 116 KB and
  90 KB), 149 KB and 125 KB with Wi-Fi joined. Which limit an app meets first depends on
  its UI and the badge: with Wi-Fi joined and the cap lifted to 128 KB, an app with one
  label reached 103 KB of Lua heap before the reserve stopped it (`tests/t-heap`), so under
  the cap a light app meets the cap and one with many objects the reserve; a TLS
  resumption, a store install and its first launch (the compiled code copied into a flash
  slot) all worked meanwhile. Where it comes from:
  - LVGL's two draw buffers are 320x12 (15 KB together) instead of 320x24 (30 KB): a
    frame is 20 flushes of ~3 ms instead of 10 of ~6 ms, one rendering while the other
    flushes as before (`bsp_display.c`).
  - Code out of IRAM, which shares the SRAM with the heap (`sdkconfig.defaults`): the
    ROM's esp_flash driver instead of ESP-IDF's (`CONFIG_SPI_FLASH_ROM_IMPL`), and the
    SPI master ISR, the ring buffer's ISR functions, heap_caps, the libc locks and the
    I2C ISR in flash. IRAM went from 66.7 KB to 45.0 KB. None of it runs while the cache
    is off, so an LCD flush or a button read that lands on a flash write finishes a few
    ms later.
  - lwIP without IPv6, the DHCP server and loopback, 4 sockets, TIME_WAIT 10 s; the TLS
    handshake's TX buffer 2 KB (was 4 KB); the FreeRTOS timer task's stack 1.5 KB and the
    button task's 2 KB.
  - **The console starts on USB power** (`os_on_usb_power()`, the status bar's rule: over
    4.3 V on the battery sense, checked every second): the REPL task, UART driver and
    command table, about 8 KB, exist only while the badge is plugged in, from boot or from
    the moment it is. On battery that heap goes to apps too.
- `heap stacks` lists the button task.

## 0.5.12 (2026-11-08)

- **Screen brightness** (Settings > Brightness): five levels, LEFT dimmer, RIGHT brighter
  (SELECT goes round), saved in NVS `settings/bright` and applied from the splash on. The
  backlight pin is now PWM'd by LEDC (5 kHz, 10 bits; `bsp_backlight_level()`), with the
  levels spaced to look even (6, 15, 30, 55, 100 % duty). The brightest level is the old
  always-on backlight and stays the default. The emulator dims its screen to match.

## 0.5.11 (2026-11-08)

- **Lua `sys.keys()`**: the keys held right now, as a set (`{ up = true, left = true }`),
  so games can move diagonally and see a key let go: read it in `on_tick`. Raw buttons
  through the calibrated map, never HOME, empty while a dialog has the keys (the app's
  `ui.confirm` or `ui.input`, the OS's confirm or bridge panel) (`lua_sys.c`, `docs/LUA_API.md` "sys").
- **Auto-repeat hands over**: when the repeating direction is released while another is
  still held, that one repeats at once instead of going quiet until pressed again, whether
  the release event or the repeat timer sees it first (`core/os.c`). Every app, built-in ones too.

## 0.5.10 (2026-11-06)

- **Bridge two badges far apart by typing a code** (`docs/MESH.md` "Bridges"). The bridge
  panel (X Dex or Trade: Bridge a badge; Lua `mesh.bridge()`; console `bridge show`) now
  shows a **short code** under its QR code, the badge's id and 6 digits (`7K2Q9M 489433`),
  which changes with the QR code every 30 s. SELECT on the same panel opens the keyboard to
  type the other badge's short code (case, spaces and dashes don't matter); the bridge opens
  as a scan does, and X Dex, Trade and Lua apps then work between the two as over the air.
  Before, two badges could only meet over ESP-NOW, since neither can scan a QR code. The 6
  digits are the first 4 bytes of the code's tag mod 1 000 000, so only the badge can make
  them, and the server caps the short codes tried against one badge. Console: `bridge code`
  prints `bridge short:`, and `bridge <id> <digits>` takes a short code. `core/os_bridge.c`.
- **Images and animations, fixed** (from the 0.5.9 review): an image, canvas or animation
  whose handle couldn't be made (the app at its heap limit, the error caught with `pcall`) no
  longer stays on screen pointing at pixels the collector frees (the view is referenced before
  LVGL gets it, and the object goes if its handle can't be made; `lua_ui.c`, `lua_img.c`). A
  GIF frame must fit the image (a tiny GIF claiming a 65535 x 65535 frame made each step
  decode billions of pixels under the LVGL lock); an animation's last frame stays even when
  it says "restore to background"; disposal 2 in an opaque GIF goes back to the GIF's
  background colour instead of leaving see-through holes; loop counts from 32768 to 65535
  play that many times, not once (`lua_gif.c`). The decoder test fails on any UBSan report.
- **Canvas drawing and UI objects, hardened** (from the 0.5.9 review): `c:image`, `c:rect`,
  `c:pixel` and `c:get` clamp coordinates before narrowing them, so a position near the
  integer limits (`-2^31`) can no longer wrap the clip maths and copy heap memory past a small
  source into the canvas (`lua_img.c`). A UI object's handle is allocated before its pool
  slot is chosen, so a script's `__gc` that runs during that allocation and makes UI objects
  of its own can't have them overwritten (two handles on one object, past the 64-object
  limit; `lua_ui.c`). `tests/t-img` covers both.

## 0.5.9 (2026-11-06)

- **Images for Lua apps** (`docs/LUA_API.md` "Images"): `ui.image(x, y, src, {scale})` shows a
  PNG bundled with the app or pixel art written as rows of characters (a default palette of
  the brand's colours, or the app's own), `ui.load()` loads one to show or draw many times, and
  `ui.canvas(x, y, w, h, {scale})` is a surface to draw on: `pixel`, `rect`, `line`, `image`
  (sprites), `clear`, `get`. Pixels are RGB565 on the app's own 48 KB (2 bytes each, whatever
  the scale); an LVGL image decoder of our own draws them scaled a line at a time, so 8x pixel
  art costs what its few pixels do. PNGs decode on the badge (`lua/lua_png.c`: every colour
  type and bit depth, not interlaced; the file streams through a 256-byte buffer and the
  scratch is freed before `ui.load` returns), checked against Pillow and fuzzed under ASan by
  `lua/tests/png/png_test.py`. A long canvas line is clipped first, so one far off the canvas
  costs only its visible part. `lua/lua_img.c`, `lua/lua_png.c`; test app `lua/tests/t-img`.
- **Animated GIFs**: `ui.anim(x, y, "file.gif", {scale, loop})` plays one with its own timing,
  between the app's callbacks (`play`, `stop`, `playing`, `scale`), and a GIF works anywhere a
  PNG does (its first frame). One frame is kept, whatever the length: each frame is decoded
  from the card onto the GIF's canvas when it's due (transparency and "restore to background"
  composed as browsers do), so the file is reopened per frame and never held. The LZW table
  (13 KB) is reserved by the app's first `ui.anim`. `lua/lua_gif.c`, checked frame by frame
  against Pillow and fuzzed by `png_test.py`; test app `lua/tests/t-anim`.
- **Memory stays in one piece between apps.** After the first app closed, the largest free
  block fell from 72 KB to about 45 KB and stayed there (the total came back): LVGL 9.6 labels
  add a display event callback whenever their text changes and remove it after layout, and
  each removal shrinks the display's callback list to fit, so the next one reallocates it; mid-
  app it landed in the middle of the app's memory and outlived it. The OS now moves that list
  to a fresh allocation on every app switch, once the app's memory is free (`core/os.c`
  `repin_display_events`). Measured on a badge: after Snake the 72 KB block is whole (it was
  50 KB); after the image tests the only thing splitting it is the console's history (below).
  Apps that use the network keep the HTTPS connection's memory for its idle time, as before.
  (A fixed buffer for the list doesn't work: LVGL's cleanup relies on the
  resize to set the list's size, and frees entries twice. When driving a badge over USB, the
  console's command history also leaves small strings mid-heap; that's development only.)

## 0.5.8 (2026-11-05)

- **The built-in server is `https://badge-api.solana.com`**, the server's own address behind a
  load balancer and Cloud Armor, instead of a Cloud Run URL tied to the hosting project
  (`CONFIG_SOLANA_OS_SERVER_URL`). The old URL keeps forwarding for badges on earlier builds.
  A badge on the build default (no `server` set on its console) keeps its ID, secret and owner
  through the update: it's the same server. One set with `server <url>` keeps that address.

## 0.5.7 (2026-11-04)

Security fixes from a review.

- **The confirm dialog takes SELECT only after 800 ms.** An app could open one (a purchase, a
  transfer) just as the player pressed SELECT for something else, and that press answered
  yes. CANCEL and HOME still answer at once. `core/os.c`.
- **Lua `print()` and app errors can't write the console's own lines.** Every line an app
  prints starts with `[<app id>] `, and control characters (CR, ESC, …) show as `?`; error
  messages are logged on one line. An app could print `\rid=…` or a fake `sol> ` prompt, and
  the site's Run on badge and badge-beacon panels read the console. `lua/lua_host.c`.
- **Mesh peers:** one radio MAC holds at most 2 live ids (a badge and the beacon it turns
  into), so a sender making up ids can't fill the 20-entry table and push real badges out;
  a live id moves to a new MAC only after 3 HELLOs from it over at least 6 s, so a quick
  burst between the real peer's HELLOs can't redirect its swaps and trades. `core/os_mesh.c`.
- **App sockets connect to the address they checked.** `os_ws` resolved the host to check that
  it's public, then the TLS connect resolved it again (DNS rebinding); it now connects to the
  checked address, with SNI and the certificate check still on the host name. `core/os_ws.c`.

## 0.5.6 (2026-11-03)

- **Badges as quest beacons** (docs in the parent repository: MESH.md "Badge beacons",
  API.md "Badge beacons"). A QuestMaker pairs a badge from its console (`beacon pair`, run by
  the site's `/admin/beacons/badge` over USB, or the emulator page) and adopts it with a
  character. Nothing is erased: the owner keeps everything, and the launcher gains
  **Beacon** (after Quests, only on paired badges). While Beacon is open the badge plays
  the character: every frame goes out under the beacon id, its HELLO is the character's
  with nonces rotating every 30 s, quest RELAYs go to `POST /api/device/beacon/relay` over
  the badge's own connection (answers cached 20 s for retries), and the screen shows the
  character and the scan code for emulators. The mesh channel follows the joined network
  meanwhile, and the screen warns when that isn't the mesh channel. A restart with the app
  open opens it again; CANCEL or HOME stops it all. `game/beacon.c`, `apps/beacon/`.
- `os_app_t.visible`: apps the launcher lists only while it returns true.
- `os_mesh_beacon()`, `os_mesh_beacon_hello()`, `os_mesh_tag()`; hello `beacon` kept in NVS
  (`cloud/beacon`).

## 0.5.5 (2026-11-02)

- Fixes 0.5.4 going offline after ~45 minutes. ESP-IDF 6.0.1's HTTP client opened a new
  TLS connection for every request on a kept-alive one without closing the old, so each
  reused request (every hello) leaked a socket and ~4 KB until none were left. The fix
  from ESP-IDF's release/v6.0 branch (44987dfd14, after the 6.0.1 tag) is carried in
  `components/esp_http_client/`, which replaces IDF's until 6.0.2.

## 0.5.4 (2026-11-02)

On /stock for a few hours only (see 0.5.5); never offered over the air.

- **Built with ESP-IDF 6.0.1** (was 5.5.4). It fixes a crash (heap corruption,
  `block_locate_free`) whenever the network dropped the badge's kept-alive server
  connection: on some home routers that happened before every 300 s hello, so the badge
  restarted every five minutes. The badge reconnects by resuming its TLS session, and
  5.5's mbedTLS dynamic buffers then read the first answer into a buffer sized for the
  handshake's last message. Resuming after a Lua app's WebSocket closed took the same path.
- About 25 KB more free heap (the launcher has ~110 KB with Wi-Fi joined, was ~85 KB). A
  full TLS handshake peaks a few KB higher (~35-40 KB).
- mbedTLS 4: hashing goes through PSA Crypto. cJSON comes from the component registry
  (`espressif/cjson`), still capped at 16 levels (`CONFIG_CJSON_NESTING_LIMIT`). The
  certificate bundle verifies Google's cross-signed GTS Root R1 through the bundle's own
  copy, since 6.0.1 dropped the GlobalSign root it is cross-signed by.
- Badges updated over the air keep their 5.5.4 bootloader; `/stock` writes 6.0.1's.
- The `Authorization` header no longer stays on the stack, so a crash's stack dump
  doesn't print the badge secret.
- Console: `cloud hangup` closes the server connection; the next request resumes it.

## 0.5.3 (2026-10-30)

- **Lua QR codes**: `ui.qr(x, y, size, text)` and `q:text()`, 25 to 218 px, text up to
  256 bytes, one at a time (its canvas sits on the system heap). Always black on white
  with a quiet zone, whatever the theme, since not every phone scanner reads
  light-on-dark codes. Text that doesn't fit at that size is a Lua error.
- The Lua API docs show a player's phone wallet signing a transaction the badge builds:
  the app posts the message to its own server, shows a Solana Pay transaction request
  as a QR code, and finds the sent transaction on chain by its reference.
- A UI method called on another kind of object (`bar.set(label, 50)`,
  `label.text(bar, "x")`) is now a Lua error. Before, it wrote one widget's fields into
  another's memory.

## 0.5.2 (2026-10-29)

- **Lua Solana RPC** (`rpc`, permission `rpc`): `rpc.call(method, [params], [opts], cb)`
  on mainnet, or devnet with `{cluster = "devnet"}`. RPC is sponsored by Triton One. The
  badge server holds the node URLs and makes the call (`POST /api/device/apps/:id/rpc`,
  as for `http`), allowing reads that fit a badge's 4 KB answer and `sendTransaction`
  of a transaction signed elsewhere; the callback gets the call's `result`. Whole
  numbers beyond 32 bits (lamports) arrive as exact decimal strings, because the
  badge's Lua integers are 32-bit. The docs warn never to create or store a keypair on
  the badge.
- The permission shows as "Read and send to Solana" in the App Store's details and the
  Apps library's info. The store refuses `rpc` apps to 0.5.0 and 0.5.1 badges, which
  have no `rpc` module.
- A server error shown to a script can be up to 127 characters (was 95), so an RPC
  node's error reads in full. A 429 that comes without an error text reads
  "rate limited" (was "server error 429").

## 0.5.1 (2026-10-29)

- **Lua WebSockets** (`ws`, permission `http`, to the app's `hosts`):
  `ws.open(url, [opts], cb)`, `sock:send`, `sock:close`, `sock:state`, with the events
  `open`, `message` and `close(reason, code)`. The socket goes straight from the badge
  to the host and takes the place of the badge's one TLS connection: the badge server
  checks the URL and signs a ticket (`badge_ticket` in the query, so the host learns the
  app, badge and player), then the cloud task closes its server connection, keeping the
  session ticket (`os_http_hangup`), and runs the socket (`core/os_socket.c`) until it
  closes. Meanwhile the app's other server calls fail with "socket open" and the badge's
  own syncs wait. One socket at a time, ≤ 4 KB per message, 8 waiting each way (a fast
  host is slowed down, not dropped); the handshake needs the same 56 KB free as the
  server connection's.
- `core/os_ws.c`: an RFC 6455 client over ESP-IDF's TLS transport, written here because
  the IDF WebSocket transport can stall on bytes it read along with the upgrade answer.
  It checks the host's addresses again before connecting (no private networks). The
  emulator uses the browser's WebSocket (`emulator/platform/os_ws_emu.c`).
- Fix: after a console upload (`lua put`, `lua recv`, so Run on badge too) to an app
  installed from the store, the badge kept running the store's compiled code from its
  flash slot. A slot now runs only while `code.bin` exists.

## 0.5.0 (2026-10-28)

- Lua 5.5.1 replaces 5.4.9, with the sandbox patches carried over. Apps need about 40 %
  less heap: snake peaks at 17.5 KB (30.9 KB on 0.4.5), magic-8 at 13.6 KB (22.8 KB).
- Store apps run compiled, in place from flash: the App Store compiles each version with
  this firmware's own Lua (`tools/luac/`), the badge installs the result as `code.bin`
  (checked by SHA-256) and `lua_xip.c` maps it from a 1 MB slot of the `storage`
  partition, so instructions, line info and long strings never enter the heap. Bytecode
  is loaded from nowhere else: console uploads and quest downloads delete `code.bin`,
  scripts can't load binary chunks, sideloaded apps compile from source.
- Lazy libraries (a function takes RAM once a script uses it), no `string.dump`, the
  string table at two strings per slot, strip level 2, source compiled before the
  libraries open, young collections at 10 % growth.
- For scripts: a `for` loop's variable is read-only, and `global` declares a global.
  Store apps need 0.5.0 from here on; older badges' App Store says how to update.

## 0.4.6 (2026-10-28)

- Lua `http` (`http.get`, `http.post`, `http.request`, permission `http`): the badge
  server makes the request (the badge has no heap for a second TLS connection while an
  app runs), only to the hosts the app lists, and signs who is asking. The App Store
  details and the Apps info screen show the hosts; `http` apps open the server
  connection before they start.

## 0.4.5 (2026-10-28)

- Bridges: badges that share no radio (an emulator and a badge, or two emulators) meet
  through the server: the code panel, scanning, a 1-3 s sync and a send queue
  (`core/os_bridge.c`). Bridged peers sit next to radio ones in the mesh, so X Dex,
  Trade and Lua `mesh` work over them; X Dex and Trade offer "Bridge a badge".
- The firmware became its own repository (this one), under the MIT license.

## 0.4.4 (2026-10-28)

- Emulated badges' ids start with a lowercase `e` (a badge's never starts with `E`), and
  the mesh accepts them.

## 0.4.3 (2026-10-26)

- Production panels show true colours: inversion is off for good, and the 0.4.1
  setting and its HW Debug toggle are gone.
- Wi-Fi joins the saved network at boot when it's in range, else turns off after 30 s
  (`core/os_wifi_boot.c`).
- My Badge's "Link X at <site>/profile" uses the address hello's `site` gives (the site
  moved to solana.com/breakpoint/badge).

## 0.4.2 (2026-10-25)

- Fix: every over-the-air download crashed (an SD command started while a display
  flush's DMA was still running on the shared SPI bus). A flush now holds the bus from
  its start to its done callback, and every SD command and probe waits for it
  (`components/bsp/bsp_bus.h`). Badges on 0.4.0 or 0.4.1 can't update over the air
  because of this: update them over USB (`/stock` on the site).
- The `ota` console command (the console now starts after every command is registered).

## 0.4.1 (2026-10-25)

- Colour inversion became a setting (some panels showed RED on cyan with it on), applied
  at boot and flipped in HW Debug > Display.

## 0.4.0 (2026-10-25)

- Over-the-air updates: Settings > Software update installs the build an admin offers
  (hello `update`). It needs Wi-Fi, a card and 70 % battery or USB; it downloads to the
  card, verifies, writes the other OTA slot and restarts, and a build that doesn't
  reach the launcher rolls back. 0.4.0 and 0.4.1 come from the same commit.

## 0.3.1 (2026-10-06)

- `lamps.deposit`: Lua apps pay into their author's escrows (`ESC-…` addresses), always
  through the OS confirm and under the badge's daily cap.

## 0.3.0 (2026-10-02)

- Apps are folders: `main.lua`, `app.json` and up to 64 bundled files (≤ 256 KB each,
  1 MB together) that stay on the card until the app reads them. Lua `require` for the
  app's modules and a read-only `fs` (`exists`, `size`, `list`, `read`, `lines`,
  `json`, `load`) on the app's heap and instruction budget; a vendor patch
  (`lua_pushfilled`) reads straight into the string.
- Installs (`lua/lua_install.c`, for the App Store and `lua install`): only changed
  files download, into `apps/<id>.new/`; every SHA-256 is checked, `data.json` is kept,
  the folders swap, and a power cut mid-swap is recovered. The store shows sizes and
  file progress.
- Console: `lua recv` (a base64 stream acknowledged block by block), `lua sums`,
  `lua rm <id> <file>` and `lua put` for bundled paths, which the site's Run on badge
  uses. The REPL task has 5 KB of stack.

## 0.2.2 (2026-10-02)

- Selection highlights are solid Solana Green.
- ESP-NOW app: a Mesh tab sweeps every channel for beacons and badges and moves the
  badge's mesh channel there (console `mesh channel <n|auto>`); the Frames tab keeps the
  sniffer.

## 0.2.1 (2026-10-01)

- The production server is the build default: a new badge needs no `server` command.
- My Badge shows the claim code (`ABCD-2345`) next to the badge ID and QR code, to claim
  without scanning.
- Pack reveals burst into confetti (`ui_confetti`), more for rarer pages.
- Skate Monkey is no longer a built-in app.
- Fixes: stale trade confirms close themselves, X Dex learns a queued upload's result
  once the outbox sends it, the console's quest log prints, character colours are
  readable in Light.

## 0.2.0 (2026-10-01): the Hitchhiker's Guide release

The game layer, *The Hitchhiker's Guide to Solana*:

- Apps: My Badge (owner, !LAMPs, Level, pages and packs, offline from the card), Guide
  (collection, page reader, pack reveal, shop), X Dex (badge swaps over the air, the
  yearbook), Trade (!LAMPs, items and pages; both sides lock, the inviter signs on a
  deliberate confirm), Quests (beacons nearby, the quest log), App Store and Apps.
- The mesh (`core/os_mesh.c`, docs/MESH.md) over ESP-NOW: fragments, HELLO presence
  and a peer table, HMAC tags, MAC binding. Mesh input is hostile: nested JSON is
  dropped before parsing, each message type is pre-filtered, only HELLOs add peers.
- The server link (`core/os_cloud.c`, `core/os_api.c`): hello, the device API and an
  outbox for swaps and trades, all on the cloud task, over one kept-alive TLS
  connection with session tickets (a new handshake only when the heap can hold it). A
  new owner wipes the card's game data; factory reset (`reset yes`).
- The Lua host: Lua 5.4.9 in a sandbox (48 KB heap, an instruction budget per callback,
  a C stack guard) with `ui`, `led`, `timer`, `sys`, `badge`, `json`, `storage`,
  `wifi`, `mesh`, `lamps`, `items`, `profile` and `quest`. Spending and transfers go
  through an OS confirm a script can't press; server calls queue per app.
- Wi-Fi is off at boot with the radio parked on the mesh channel; joining prefers the
  SSID's access point on the mesh channel; mesh apps keep the radio awake.
- Shared UI: the offline panel, amounts, category and rarity colours, `os_confirm`,
  toasts from any task; console commands from portable code (`os_cmd`).
- Skate Monkey, a Flappy Bird game, joined the built-in apps.

## 0.1.0 (2026-09-30)

- First release: the launcher and the Wi-Fi, My Badge (claim QR code), ESP-NOW
  sniffer, HW Debug, Settings and About apps; the board drivers from the reference
  investigation (display, microSD on the shared SPI bus, buttons with calibration,
  LEDs, battery); the UART console with screenshots (`shot`, `tools/badge.py`); and the
  browser emulator built from the same sources.
