# Solana OS Lua apps

Badge apps from the App Store are Lua 5.5 programs that run in a sandbox on Solana
OS: `main.lua`, plus any modules and data files the app brings along. This file is
the API reference. The web shows it at `/apps/docs`, Solana OS implements it
(`main/lua/`) and the server enforces the permissions. Which way an app should talk to
other badges, gadgets, phones and servers (the mesh, Bluetooth or Wi-Fi) is in the
[Connectivity guide](CONNECTIVITY.md), with [examples](examples/) to start from.

**C apps.** From Solana OS 2.3.0 an app can also be written in C: the store compiles it to
WebAssembly and the badge's native code, and it calls this same API through `badge.h`
(`ui_label()`, `mesh_send()`, `http_get()`, ...), with the same permissions and limits
(the CPU limit aside). See the [C API](C_API.md); what each function does stays
documented here.

## The package

An app is a folder. `main.lua` (UTF-8, ≤ 64 KB) runs first, and `app.json` is the
manifest, which you fill in when you submit the app on the web (a folder or .zip you
upload can bring one along to fill in the form). Every other file in the folder ships
with the app: Lua modules for `require`, and data of any kind (text, JSON, binary) for
`fs`. They stay on the microSD card until the app reads them, so a big app only spends
memory on what it has loaded.

```
trivia/
  main.lua              runs first
  app.json              the manifest
  lib/quiz.lua          require("lib.quiz")
  questions/space.txt   fs.lines("questions/space.txt")
```

Bundled files: at most 64 besides `main.lua` and `app.json`, each ≤ 256 KB, ≤ 1 MB
together. Paths use lowercase letters, digits, `_`, `-` and `.`, with `/` between
folders: ≤ 64 characters, ≤ 4 levels, and no name starts or ends with `.`. `app.wasm`
and `app.aot` at the top of the folder are taken (the badge keeps a C app's code under
those names), so the store refuses a bundled file called that; `bin/app.wasm` is fine. The App
Store installs an app into `/sdcard/hh/apps/<id>/`: `main.lua`, `app.json`, the app's
`storage` in `data.json`, and the bundled files under `files/`. An update downloads
only the files that changed, then swaps the whole folder in once everything is there,
keeping `data.json`.

Apps that quests hand out through a beacon are a `main.lua` only: beacons can't carry
bundled files.

### Compiled by the store

When you publish, the store compiles `main.lua` and every bundled file ending in `.lua`
with the badge's own Lua (the same sources, built for the server), and refuses a
version that doesn't compile, naming the file and line (`main.lua:12: 'end' expected
near <eof>`). Badges download that compiled code along with the source and run it
**in place from the badge's flash**: the instructions, line
numbers and long strings never enter the app's heap, there's no compiling at launch,
and `require` and `fs.load` of a compiled module read nothing from the card. On the
sample apps the heap peaks 15-25 % lower than when the badge compiles the same source
(and 30-45 % lower than on Solana OS 0.4: snake 31 KB then, 18 KB now).

The compiled code keeps file and line numbers in error messages but not the names of
local variables: an error reads `main.lua:12: attempt to index a nil value` rather than
`... (local 'player')`. Apps you run from the web editor (Try in emulator, Run on badge)
or copy over the console are compiled on the badge from their source, with full
messages.

**Apps need Solana OS 0.5.0 or later.** An older badge installs nothing: its App Store
says "Update to Solana OS 0.5.0 to install apps" for every app and tells how in the
app's details, buying and downloads are refused, a quest's app download stops with
"update Solana OS to 0.5.0 for apps", and Run on badge stops before sending anything.
Update it from the site's firmware flasher (`/stock`, a USB-C cable) or, from 0.4.2, in
Settings > Software update when it's offered. An app with the `rpc` permission needs
0.5.2: on 0.5.0 and 0.5.1 its details say so, and buying and downloads are refused with
"Update Solana OS to 0.5.2 or later to install this app". An app with the `ble` permission
needs 1.0.0 in the same way ("Update Solana OS to 1.0.0 or later to install this app"). An app with `mail` (or `background`, `files` or `inventory`) needs 2.3.0, and one with `gossip` needs 2.4.0.

The manifest, as the badge stores it:

```json
{
  "id": "snake",                    // 3-24 chars: a-z 0-9 -, unique in the store
  "name": "Snake",                  // ≤ 20 chars, shown in the launcher list
  "version": "1.0.0",
  "author": "@toly",
  "summary": "Eat, grow, don't bite yourself.",
  "icon": "play",                   // one of the icon names below
  "accent": "14F195",               // hex colour, no '#'
  "category": "games",              // games | tools | social | quests | demos
  "permissions": ["lamps", "items", "http"],  // led wifi mesh ble lamps items profile quest http rpc mail background files inventory gossip
  "hosts": ["api.example.com"],     // what `http` and `ws` may call (only with the `http` permission)
  "files": 2,                       // bundled files (the store fills it in)
  "filesSize": 5120,                // their bytes together
  "temp": false,                    // true for quest-granted apps
  "expires": null,                  // unix ms when a temporary app stops working
  "code": {                         // the store's compiled code, for Solana OS 0.5.0+
    "luac": "5.5.1-sol.1",          // the compiler; the badge runs it only if it's its own
    "sha256": "…", "size": 5692     // code.bin, checked when it's installed
  }
}
```

Icons: `play`, `star`, `heart`, `bell`, `gift`, `home`, `image`, `list`, `wifi`,
`shuffle`, `eye`, `edit`, `charge`, `gps`, `settings`, `audio`, `bulb` (the badge maps
them to LVGL symbols; the web to matching glyphs).

## Lifecycle

The host loads `main.lua`, runs the top-level chunk once (build your UI there), then
calls these globals if you define them:

```lua
function on_key(key, rep)    -- "up" "down" "left" "right" "select" "back"; rep = auto-repeat
  return true                -- handled. Returning false/nil for "back" exits the app.
end
function on_tick(ms)         -- every 30 ms (50 ms before 2.1.0): move things by ms,
end                          -- the time since the last tick, never by counting ticks
function on_mesh(from, msg)  -- a message from the same app on another badge (needs "mesh")
end
function on_meet(from, data) -- another badge asks for a signed meet (needs "mesh", 0.5.15):
end                          -- return a table to sign it ("Meets")
function on_exit()           -- the app is closing (HOME, CANCEL, sys.exit, error)
end
function on_service(msg)     -- the app's own background service sent msg (needs "background", 2.3.0)
end
function on_mail(m)          -- a notification for this app arrived (needs "mail", 2.3.0, "mail")
end
```

An app with the `background` permission can also leave a **service** running after it
closes: `service.lua`, a separate script with its own callbacks (see
["`service`"](#service-permission-background-solana-os-230)).

Up, down, left, right and CANCEL auto-repeat while held (`rep` true: after 400 ms, then
every 90 ms); SELECT doesn't. One key repeats at a time, the last one pressed; when it's
released, another direction still held takes over (0.5.11). `on_key` has no release event:
for games that need several keys at once (diagonals, hold to charge), read `sys.keys()` in
`on_tick`.

HOME always returns to the launcher. The screen below the status bar is
**320 × 218** pixels (`ui.W`, `ui.H`), with (0, 0) at the top-left.

### Limits

| | |
|---|---|
| Memory | 6 MB of Lua heap, in the badge's 8 MB of PSRAM (Solana OS 2.0.0). Past it the app stops with "Out of memory: The app needs more than 6 MB." The badge keeps 512 KB of PSRAM for itself while an app runs, and the allocator's own overhead (up to about a quarter for very many small tables or strings) comes out of the rest, so an app made of millions of tiny objects can be stopped a little earlier with "The badge is low on memory". The app's UI objects, label text and 12 KB task stack are in the badge's other memory, capped by the rows below, not in the 6 MB, and Bluetooth no longer shrinks it. `sys.memory()` tells an app where it stands. Badges below 2.0.0 (until they update) give 96 KB (48 KB below 0.5.13), shared with Wi-Fi, TLS and Bluetooth: there an app with 60 labels gets about 80 KB with Wi-Fi joined, about 49 KB with Bluetooth on as well. A store app's compiled code runs from flash and mostly isn't in it; a library function takes a few bytes once the app first uses it. A big heap is a slower one: PSRAM is slower than the chip's own memory, and the collector walks everything an app keeps, so a few MB of live tables can pause a callback noticeably |
| CPU | 200 000 VM instructions per callback, coroutines and pattern matching included (else "Script took too long") |
| UI objects | 128 at a time (64 below 2.0.0), one of them a QR code. They live in the badge's SRAM, not the Lua heap: a call that makes one (`ui.label`, `ui.list`, `ui.image`, …) raises "ui.list: the badge is low on memory" (with its own name) when it would leave the badge less than 32 KB, a catchable error (2.0.1). So does a call that makes what they hold grow (2.1.0): `label:text` with longer text, `ui.hint`, `ui.toast`, `qr:text`, and `ui.confirm` and `ui.input` for their text. The 32 KB are SRAM that the badge can still allocate, not counting the 32 KB set aside for DMA (2.0.1 counted those, so its check left the badge almost nothing), and each list, the keyboard and the first image keep a little more for what they redraw on their own (a list's rows, about 1.3 KB). A `ui.list` costs about 12 KB of it, a label about 250 bytes. Measured on 2.1.0 with Wi-Fi off: 60 labels and six lists, or 128 labels; Wi-Fi joined and Bluetooth on leave less |
| Images | 2 bytes per pixel of the app's heap (a 48 × 48 PNG: 4.5 KB); up to 1024 pixels a side, shown at up to 16×. The first `ui.anim` reserves 13 KB more for decoding |
| Text | 200 bytes per label (longer text ends in "..."), 12 KB of label text on screen (6 KB below 2.0.0) |
| Timers | 8 at a time, ≥ 20 ms |
| Storage | 16 KB per app (`storage.*`) |
| Files | 64 bundled files, ≤ 256 KB each, ≤ 1 MB together, read-only (`fs`, `require`) |
| Reads | ≤ 256 KB per `fs.read` and `fs.json`, so any bundled file in one read (16 KB below 2.0.0), lines ≤ 512 bytes. Card reads count toward the instruction budget: 500 per file opened or looked up (`fs.exists`, `fs.size`, `fs.list`), 1 per 4 bytes read or compiled |
| Mesh | 4 messages per second, ≤ 200 bytes each. Meets: one asked and one accepted a second, ≤ 100 bytes of data a side, 300 per badge a day, 40 waiting to upload |
| Bluetooth | one connection at a time, ≤ 244 bytes per read, write or notification, 4 waiting for an answer; a served service has ≤ 4 characteristics; an advertisement carries ≤ 24 bytes of app data; about 30 KB of the badge's own memory while it is on (not of the app's heap from 2.0.0) |
| HTTP | 60 requests a minute per badge, ≤ 2 KB sent, ≤ about 3.5 KB of answer, 6 s to answer |
| RPC | 30 calls a minute per badge (6 of them `sendTransaction`), ≤ 2 KB of params, ≤ about 3.5 KB of answer, 6 s to answer |
| WebSocket | one at a time, 20 opens a minute per badge, ≤ 4 KB per message either way, 8 waiting to go |
| Crypto | of a callback's 200 000 instructions: a signature check takes 40 000, an on-curve test 5 000, a program-derived address 10 000 (see `crypto` and `solana`) |
| Background | a service (`background`, 2.3.0): 1 MB of heap, 100 000 instructions per callback and 2 000 000 a minute, a tick a second, 3 services on a badge at once (see "`service`") |
| Inventory | `profile.items` / `profile.pages` (`inventory`, 2.3.0): up to 10 entries a call (fewer when long: an answer fits 4 KB), 30 calls a minute per badge for the two together |
| Mail | `mail` (2.3.0): title ≤ 32 and text ≤ 200 printable ASCII, data ≤ 128 bytes of JSON, a send a second, none while 40 uploads wait in the outbox; the server's caps: 30 a day per owner, 5 a day to one person per app (see "`mail`") |
| Numbers | 32-bit integers (they wrap at ±2^31) and 32-bit floats (about 7 digits) |

How fast Lua is and what these limits allow, measured on a badge against the same work in
a C app: [C_API.md "Speed and limits: C against Lua"](C_API.md#speed-and-limits-c-against-lua).

A runtime error stops the app and shows the message with the line number. CANCEL
returns to the launcher. `print(...)` goes to the badge's serial console, each line
tagged `[<app id>] ` and with control characters (CR, ESC, …) shown as `?` (0.5.7: an app can't
write a line of the console's own, which tools such as Run on badge read).

### Callbacks and network calls

Anything that talks to the server (including `http` and `rpc`, which go through it) is
asynchronous and takes a callback `cb(ok, result)`: `ok` is a boolean, and `result` is
the value or an error string (`"offline"` when Wi-Fi is off, `"not linked"` when the
badge has no owner, `"denied"` when the app lacks the permission or the user said no,
`"busy"` when 16 calls are already waiting, `"socket open"` while the app's `ws` socket
is open). Callbacks run like any other callback: between `on_tick`s, never
concurrently.

**The server's address.** The badge knows its server, so an app never names it. Your
own servers do, when they pay out of an escrow or check a request's signature (`http`,
`ws`): the API is at **`https://badge-api.solana.com/api/...`** (`/api/escrow`,
`/api/escrow/ledger`, `/api/escrow/pay`, `/api/http/key`). Not the store's address
(`https://solana.com/breakpoint/badge`): its firewall answers scripts with a browser
challenge (429 and an HTML page), whatever their headers.

## Standard library

Lua 5.5 (Solana OS 0.5.0; earlier badges ran 5.4). Available: `assert error ipairs next
pairs pcall select tonumber tostring type xpcall print getmetatable setmetatable
rawequal rawget rawlen rawset _G _VERSION`, `string.*` (no `string.dump`), `table.*`
(with 5.5's `table.create`), `math.*` (with `math.random` seeded per launch), `utf8.*`,
`coroutine.*`, and `require` for the app's own modules (see `fs`). Removed: `io`, `os`,
`debug`, `package`, `load`/`loadfile`/`dofile` (`fs.load` compiles the app's own files),
`warn`, `collectgarbage` (except `collectgarbage("count")`).

What 5.5 changes for a 5.4 script: a `for` loop's variable is read-only (`for i = 1, 3
do i = i + 1 end` doesn't compile; copy it into a local), and `global x` declares a
global (`global` is still fine as a name elsewhere). The store refuses code that
doesn't compile, so you see these when you publish.

Library tables fill in as you use them: `string`, `table`, `math`, `utf8`, `coroutine`
and the badge's own modules (`ui`, `led`, `fs`, ...) keep their functions in flash, and
a function takes RAM only once the app uses it. Calling and indexing work as usual, and
`pairs(string)` lists everything; but `next` and `rawget` see only the functions used so
far, and `string.format = nil` doesn't remove a function (it comes back on the next
lookup). Adding your own functions to a library table (`function string.trim(s)`) works.

## Modules

### `ui`

```lua
ui.W, ui.H                          -- 320, 218
ui.bg(color)                        -- background of the app area
local l = ui.label(x, y, text, {size=14, color="text", mono=false, w=nil, align="left"})
--   size: 12 | 14 | 20 | 32 (12 is always mono), align: "left" | "center" | "right" (needs w)
l:text("hi")  l:pos(x, y)  l:color("green")  l:show(false)  l:remove()
local r = ui.rect(x, y, w, h, {color="panel", radius=0, border=nil})
r:pos(x, y)  r:size(w, h)  r:color(c)  r:show(b)  r:remove()
local b = ui.bar(x, y, w, h, value)     -- progress 0..100, Solana gradient fill
b:set(value)  b:remove()
local g = ui.gradient(x, y, w, h)       -- the Solana gradient
local list = ui.list(x, y, w, h, items, {on_select=function(i, item) end})
--   items: { "text", ... } or { {title=, sub=, right=}, ... }; the list takes up/down
--   rows are 36 px; long text ends in "..."
list:items(newItems)  list:selected() --> index  list:select(i)  list:remove()
local q = ui.qr(x, y, size, text)       -- a QR code, size x size px (25..218), text ≤ 256 bytes
q:text("solana:https://...")  q:pos(x, y)  q:show(b)  q:remove()
ui.hint("SELECT jump   CANCEL exit")    -- key hint pinned to the bottom (16 px)
ui.toast("Saved")
ui.confirm("Reset the score?", function(yes) end)      -- modal, SELECT = yes, CANCEL = no
ui.input("Your name", 16, function(text) end)          -- d-pad keyboard; text = nil if cancelled
ui.clear()                              -- remove every object
```

Colours are theme names: `"text" "dim" "bg" "panel" "border" "purple" "green" "pink"
"lpurple" "blue" "aqua" "lime" "yellow" "orange"`. They follow the Dark/Light theme.
A number `0xRRGGBB` is a fixed colour. While a list, `ui.confirm` or `ui.input` has
focus it consumes the keys it uses before `on_key` sees them.

`ui.qr` (Solana OS 0.5.3; `nil` on older badges, so check `if ui.qr then`) draws a QR
code a phone can scan: a link, an address, a [Solana Pay](#build-on-the-badge-sign-on-a-phone)
request. It's always black on white with a white margin, whatever the theme, because
not every scanner reads light-on-dark codes. One at a time: change what it shows with
`q:text()`. Text that doesn't fit at `size` is an error. Shorter text means bigger
squares that scan from farther away: up to about 130 bytes at 180 px gives 3 px per
square.

#### Images

Solana OS 0.5.9 (`nil` on older badges: check `if ui.image then`). Show PNGs and GIFs
bundled with the app, play animated GIFs, write pixel art in Lua, or draw on a canvas.

```lua
local logo = ui.image(10, 10, "art/logo.png")         -- a bundled PNG, shown as it is
local hero = ui.image(100, 60, {                      -- pixel art: one character per pixel
  "..pp..",
  ".pggp.",
  "pggggp",
  ".p..p.",
}, {scale = 8})                                       -- each pixel 8 x 8 on screen
hero:pos(x, y)  hero:show(b)  hero:remove()
hero:set(other)                                       -- show another image (same sources as ui.image)
hero:scale(4)                                         -- 1..16

local ship = ui.load("ship.png")                      -- load once, show or draw it many times
local w, h = ship:size()
ui.image(0, 0, ship, {scale = 2})                     -- views share the pixels: no copy

local c = ui.canvas(0, 0, 80, 54, {scale = 4, color = "bg"})   -- 80 x 54 pixels on 320 x 216 px
c:clear()                     -- everything see-through (or c:clear("bg"))
c:pixel(x, y, "green")
c:rect(x, y, w, h, 0xFF754A)  -- filled
c:line(x1, y1, x2, y2, "aqua")
c:image(ship, x, y)           -- stamp an image (or a path, or rows); its see-through pixels leave the canvas as it was
c:get(x, y)                   -- 0xRRGGBB, or nil where it's see-through or outside
c:pos(x, y)  c:show(b)  c:remove()

local dance = ui.anim(100, 40, "dance.gif", {scale = 2})   -- an animated GIF, playing
local once = ui.anim(0, 0, "intro.gif", {loop = false})     -- loop: true, false, or a number of times
dance:stop()  dance:play()  dance:playing()   -- play() after the end starts over
dance:scale(3)  dance:pos(x, y)  dance:show(b)  dance:remove()
```

- **Sources.** Anywhere an image goes (`ui.image`, `image:set`, `ui.load`, `c:image`):
  a `ui.load` result, the path of a PNG or GIF bundled with the app (as for `fs`; a GIF
  gives its first frame), or a table of rows. A path is read and decoded each time it's given, so load what you draw
  often with `ui.load` once. The `{palette = ...}` option, after the position, goes
  with rows.
- **Pixel art.** Each character is a pixel: `.` and space are see-through, `#` is the
  theme's text colour, and `k` black, `w` white, `p` purple, `g` green, `m` pink,
  `v` light purple, `b` blue, `a` aqua, `l` lime, `y` yellow, `o` orange are the
  brand's. `{palette = {r = 0xFF0000, s = "dim", x = false}}` adds or changes
  characters (`false`: see-through). Rows can differ in length; short ones end
  see-through. A character not in the palette is an error.
- **PNGs.** Any colour type and bit depth; not interlaced. A pixel less than half
  opaque is see-through and the rest are solid (no blending). Loading needs the
  picture's pixels plus its unpacked rows for a moment (about 4 bytes a pixel for
  RGBA, 1 for a 256-colour palette, less for fewer colours), so save big pictures
  with a palette. Reading costs instructions like `fs.read`.
- **Animations.** `ui.anim` plays a GIF on its own, with the GIF's own timing (at
  most 50 frames a second; a delay of 0 plays at 10), between your callbacks. It keeps
  one frame in memory, whatever the length: the GIF's size × 2 bytes, plus 13 KB for
  decoding that the app's first `ui.anim` reserves and keeps (`ui.load` of a GIF
  borrows it then, or needs it for a moment). Frames are read from the card as they
  play, so a long animation costs no more than a short one. Without `loop`, it does
  what the file says (most loop forever); when it ends, the last frame stays. A
  106 × 72 GIF shown at 3× (the whole screen) takes about 20 ms a frame to decode;
  smaller ones less. Transparent pixels and the "restore to background" disposal
  work (see-through for a frame with transparency, else the GIF's background colour);
  "restore to previous" is treated as "leave it". Every frame must fit inside the
  GIF's size, or the animation stops with an error.
- **Scale.** Integer only: each pixel becomes a `scale` × `scale` square, sharp
  edges, no smoothing. Memory is the image's own pixels, whatever the scale: a
  40 × 27 canvas shown at 8× fills the screen for 2 KB.
- **Colours** are the screen's 65,536 (RGB565): `c:get` gives back `0x9C45FF` for
  `"purple"` (`0x9945FF`). Theme names are taken when the pixel is drawn or the art
  loaded; they don't follow a later theme change.
- **Drawing** off the canvas is clipped, not an error. Every pixel written costs
  instructions (1 per 16), so redrawing a whole 80 × 54 canvas each `on_tick` is
  fine; the canvas redraws on screen after the callback. Images and canvases are UI
  objects (the 128), drawn in the order made, so a label made after a canvas sits on
  top of it. So are animations.

### `led` (permission `led`)

```lua
led.count                   -- 8 (1..6 across the top, 7..8 on the right edge)
led.set(i, r, g, b)         -- 0..255 each; the OS scales to its brightness cap
led.hex(i, 0x9945FF)
led.fill(r, g, b)
led.off()
```

Changes show at the end of the current callback. LEDs turn off when the app exits.

### `timer`

```lua
local id = timer.after(ms, fn)
local id = timer.every(ms, fn)
timer.cancel(id)
```

### `sys`, `badge`, `json`

```lua
sys.millis()                -- ms since the app started
sys.time()                  -- unix seconds from the server clock, or nil if never synced
sys.battery()               -- percent, or nil on USB / unknown; the status bar's number (2.4.0: filtered, only falls until USB)
sys.keys()                  -- the keys held now, e.g. { up = true, left = true } (0.5.11)
sys.memory()                -- { used = 18773, max = 6291456 }: the app's Lua heap in bytes (2.0.0)
sys.tick(16)                -- on_tick every 16 ms from now on (16..1000; 30 by default); returns the old period (2.3.0)
sys.exit()                  -- close the app
badge.id()                  -- "7K2Q9M", or nil before the first hello
badge.owner()               -- "@toly", or nil while unclaimed
badge.online()              -- Wi-Fi joined and registered with the server (false while a ws socket is open)
json.encode(value)  json.decode(text)   -- nil plus an error message on bad input
```

`sys.memory()` counts what the app's Lua heap holds now (live objects and garbage not yet
collected) and its cap on this badge: 6 MB with PSRAM, 96 KB on a badge that has none.
Before 2.0.0 there is no `sys.memory` and the cap is 96 KB, so an app that wants room for
big caches can ask: `local max = sys.memory and sys.memory().max or 96 * 1024`.

`sys.tick(ms)` sets how often `on_tick` runs, 16 to 1000 ms (Solana OS 2.3.0; 30 ms by
default): a game that wants every frame of a 60 Hz screen asks 16 or 17 and moves things by
`ms` as always. An app that set its period is woken at the right millisecond (the badge's
scheduler otherwise sleeps in 10 ms steps, so 16 would come out as 20). A tick still runs
only after the last callback ended: when the app's callbacks take longer than the period,
ticks come as fast as they can, and `ms` says how long it really was. The budgets per
callback don't change ("Limits").

`sys.keys()` names `up`, `down`, `left`, `right`, `select` and `back` (CANCEL), never HOME,
and is empty while a dialog has the keys (your `ui.confirm` or `ui.input`, or the badge's own
confirm or bridge panel). It reads the buttons themselves, so a focused `ui.list` doesn't hide
its keys. Solana OS before 0.5.11 has no `sys.keys` (`if sys.keys then`):

```lua
local x, y = 160, 109
local dot = ui.rect(x, y, 8, 8, {color="green"})
function on_tick(ms)
  local k = sys.keys and sys.keys() or {}
  local d = ms // 10                 -- 100 px a second
  if k.left then x = x - d elseif k.right then x = x + d end
  if k.up then y = y - d elseif k.down then y = y + d end
  dot:pos(x, y)
end
```

### `storage` (always allowed)

Private key/value storage on the SD card, kept across launches and removed with the app.

```lua
storage.get("best")         -- value or nil
storage.set("best", 42)     -- numbers, strings, booleans, tables (JSON-encodable)
storage.remove("best")
```

### `fs` and `require` (always allowed)

The app's bundled files (see "The package"), read-only. Paths are the ones in your
folder, like `"questions/space.txt"`: no leading `/` and no `..`. An app sees only its
own files; `main.lua`, `app.json` and `data.json` aren't among them.

```lua
local quiz = require("lib.quiz")      -- runs lib/quiz.lua once; later calls return the same value
fs.exists("questions")                -- true for a file or a folder
fs.size("questions/space.txt")        -- bytes, or nil if there's no such file
fs.list("questions")                  -- { "art.txt", "extra/", "space.txt" }: sorted, folders end in "/"
fs.list()                             -- the top of the app's files; {} for a folder that isn't there
fs.read("intro.txt")                  -- the whole file (≤ 256 KB) as a string, or nil, "not found"
fs.read("book.txt", 8192, 1024)       -- 1024 bytes from byte 8192 (counting from 0); "" past the end
for line in fs.lines("words.txt") do  -- each line without its "\n" or "\r\n"; ≤ 512 bytes,
end                                   -- longer lines come in pieces
fs.json("levels/1.json")              -- the decoded value (file ≤ 256 KB), or nil and an error
local scene = fs.load("scenes/intro.lua")  -- compiled, not run or kept: scene() runs it;
                                           -- nil and an error for a syntax error
```

`require("lib.quiz")` loads `lib/quiz.lua`: dots separate folders. The module runs
with the same globals as `main.lua`, and what it returns (usually a table, `true` if
nothing) is what every `require` of that name gets. A missing module or an error in
one stops the app like any other error; wrap it in `pcall` to handle it. A missing
file is an error for `fs.lines` too, while `fs.read`, `fs.json` and `fs.load` return
`nil` and a message. A bad path, or asking for more than 256 KB at once (16 KB below
2.0.0), is an error.

Everything you read lands in the app's heap (6 MB; 96 KB below 2.0.0), so keep what you
use: on an older badge read big files in pieces (offsets or `fs.lines`), and load data
when the app needs it.
`require` keeps a module until the app exits. For code you need only for a while (a
level, a scene), use `fs.load` and drop the function and what it returned once you're
done: the collector frees them. In a store app both load the store's compiled code of
the file from flash (see "Compiled by the store"), which costs far less than compiling
it; other files (an app copied over the console) compile from the card. Reading also counts toward the instruction budget
(see Limits): 100 KB costs 25 000 of the 200 000 a callback gets. The file read last
stays open, so reading on in it (pages, lines) doesn't pay to open it again.

### `files` (permission `files`, Solana OS 2.3.0)

The app's own folder on the badge's microSD card, `hh/files/<app id>/`, for files the
badge's owner copies there: a game for an emulator, a level pack, a font, a book. Bundled
files (`fs`) come from the store, are small and are the same on every badge; these are the
owner's own, can be any size (a few MB), and the app reads them a range at a time. The app
reads every file in the folder and writes only in its `save/` subfolder:

```lua
files.dir()                         -- "hh/files/gameboy/": the folder, as the owner sees it on the card
files.list()                        -- { {name = "Tetris DX.gbc", size = 1048576}, {name = "save/Tetris DX.sav", size = 32768} }
files.size("Tetris DX.gbc")         -- bytes, or nil if there's no such file
files.read("Tetris DX.gbc", 16384, 16384)   -- 16 KB from byte 16384 ("" past the end), or nil, "not found"
files.write("save/Tetris DX.sav", 0, data)  -- true, or nil and why
files.remove("save/Tetris DX.sav")  -- true if it was there
```

- **Names**, not paths: one file in the folder, or `save/<name>`. Printable ASCII, at most
  64 characters, none of `/ \ : * ? " < > |`, not starting with `.` or a space (a Mac's
  `._` files don't count) nor ending with `.` or a space. Spaces inside are fine. Anything
  else is an error, so nothing outside the folder can be named. Folders the owner makes
  inside it are ignored.
- **In a background service** (`service.lua`, permission `background`) the reads work and
  `files.write` and `files.remove` are `nil`: only the app on the screen writes.
- **Reading**: `files.read(name, offset, length)`, at most 256 KB a call, fewer bytes at the
  end of the file. `files.list()` gives at most 128 files, the owner's first, then the saves.
- **Writing**: only `save/<name>`: at most 1 MB a file, 256 KB a call, 32 files in `save/`,
  and `offset` at most the file's size (no holes; a new file starts at 0). The owner's
  files are read-only to the app.
- **Cost**: as `fs`: 500 instructions for each file opened and one per 4 bytes read or
  written, so one callback reads about 750 KB. The file read last stays open, so paging
  through it doesn't pay to open it again.
- **Where it lives**: under `/sdcard/hh`, so a factory reset or a new owner wipes it with
  the rest of the old owner's data. Removing the app leaves the folder (its saves outlive
  a reinstall; delete the folder on the card to remove them). The badge makes the folder,
  with `save/`, the first time an app with the permission starts, so the owner finds it.
- **Getting files there**: the owner copies them with a card reader (the card is on the
  badge's back), or over USB with the console: `lua recv <id> files/<name> <bytes> <sha256>`
  (up to 32 MB; a name without spaces), which Run on badge's tools use.

C apps read with `files_read()` straight into their own memory, with no Lua string on the
way ([C_API.md "files"](C_API.md#files-permission-files-solana-os-230-lua)).

### `wifi` (permission `wifi`)

```lua
wifi.status()               -- "off" | "connecting" | "on" | "failed"
wifi.ssid()                 -- saved network name or nil
wifi.on()                   -- join the saved network
wifi.off()
```

### `mesh` (permission `mesh`)

Opening an app with `mesh` puts the radio on the mesh channel while it runs.

```lua
mesh.nearby()               -- { {id="7K2Q9M", name="@toly", level=12, rssi=-48, kind="badge", bridged=false}, ... }
mesh.send(to, msg)          -- to = badge id or "*"; msg = table (≤ 200 bytes as JSON)
-- received by on_mesh(from_id, msg) on badges running the same app
mesh.bridge()               -- show this badge's bridge code over the app (CANCEL closes it)
mesh.meet(id, data, cb)     -- a meet both badges sign, for your server to trust ("Meets", 0.5.15)
mesh.meets([after,] cb)     -- the meets the server holds for this badge's owner
mesh.pause()                -- give the radio back to Wi-Fi, mesh.resume() takes it again (0.5.15)
mesh.info()                 -- { channel = 149, band = "5", online = true, paused = false, event = 149, scanning = false }   (1.0.0)
mesh.channel(n)             -- move this badge's mesh to channel n while the app runs; mesh.channel() goes back (1.1.0)
mesh.scan([opts,] cb)       -- listen on each channel a moment: who is on which ("Choosing a channel", 1.1.0)
```

`kind` is `"badge"`, `"beacon"` or, from 2.3.0, `"station"`: a badge the event's server
is speaking through (Collaborator, the parent repo's docs/MESH.md "Collaborator stations").
A station sends APP messages like an app on another badge (`on_mesh(station_id, msg)`),
so an app can take part in something the event runs (a light show, a game); it never
swaps, trades or meets. In a scan's `peers` a station counts as a badge.

**The mesh channel** (`mesh.info()`, Solana OS 1.0.0). Every badge and beacon at an event
shares one channel, which the server announces. It is a 2.4 GHz channel (1 to 13,
`band = "2.4"`) or, when every badge at the event runs 1.0.0, a 5 GHz one (36, 40, 44,
48, 149, 153, 157, 161 or 165, `band = "5"`). `online` is true when Wi-Fi is joined **on that channel** (false with Wi-Fi off): the badge
is then on the mesh and the server at once, and your `mesh` app never pauses Wi-Fi. When
it is false, opening the app took the radio from Wi-Fi (server calls answer `"offline"`
until `mesh.pause()` or the app closes). `paused` is true between `mesh.pause()` and
`mesh.resume()`. `event` (1.1.0) is the channel the badge uses outside your app: the
server's, or one its owner picked in the ESP-NOW app; `channel` differs from it only
after `mesh.channel(n)`. `scanning` is true while `mesh.scan` runs. Check
`if mesh.info then` on badges that may run an older build. The guide
([Connectivity](CONNECTIVITY.md)) says which radio to pick for what.

**Choosing a channel** (Solana OS 1.1.0; check `if mesh.channel then`). By default your
app talks on the event's channel, with every badge and beacon there. An app can move
its own badge to another mesh channel while it runs, for a game of its own away from a
crowded one, and scan the channels first to see who is where.

`mesh.channel(n)` puts this badge's mesh on channel `n` within a second: its HELLOs,
`mesh.send` and `on_mesh` now use `n`, and badges heard on the old channel drop out of
`mesh.nearby()`. `n` must be a mesh channel (1 to 13, or 36, 40, 44, 48, 149, 153, 157,
161, 165), else it answers `nil, "not a mesh channel"`; otherwise `true`.
`mesh.channel()` (or `0`) goes back to the event's channel, and so does closing the app:
nothing is saved. Meanwhile:

- **The others have to move too.** Badges hear each other only on the same channel, so
  every badge in your game runs the app and picks the same `n`: from a scan, from the
  host's broadcast on the event channel before everyone moves, or from something the
  players share (a room name hashed to one of the channels).
- **The rest of the event can't hear this badge.** Beacons (quests), and X Dex and
  Trade requests from badges on the event channel, wait until the app goes back or
  closes.
- **Wi-Fi.** A network joined on the event channel (`online`) pauses once the app
  moves to another channel, and server calls answer `"offline"` until `mesh.pause()` or
  the app closes (even if the app goes back meanwhile); a network on channel `n` itself
  keeps working. Bridged badges are
  unaffected: a bridge goes through the server.
- The server announcing a new event channel changes `event`, not your app's channel.

`mesh.scan([opts,] cb)` hops through channels, listening on each for a moment, and
calls `cb(channels)` once at the end. It sends nothing on the channels it visits, the
badge's own included (the others don't see a scan), so it finds badges and beacons by
their HELLOs, which they send every 1.5 to 2.5 s, and badges running **your** app by the
`mesh.send` messages they broadcast meanwhile. `opts`:

| Field | |
|---|---|
| `channels` | the channels to visit, in order: a list of up to 22 mesh channels |
| `band` | instead of `channels`: `"2.4"` (1 to 13), `"5"` (the nine above) or `"all"`. Default: the band of the badge's channel |
| `dwell` | milliseconds per channel, 500 to 5000. Default 2600, long enough to catch every HELLO once; a shorter one may miss badges |

The whole 2.4 GHz band takes about 34 s at the default dwell, both bands about 57 s.
`channels` holds one entry per channel visited, in the order visited, heard or not
(so an empty channel is easy to find):

```lua
{ channel = 6, band = "2.4", badges = 3, beacons = 1, app = 2,
  peers = { { id = "7K2Q9M", kind = "badge", name = "@toly", level = 12, rssi = -48, app = true }, ... } }
```

`app` counts the badges heard sending your app's messages there (`peers[i].app`). A
badge that runs your app but sends nothing isn't marked, so broadcast now and then
(once a second or two) if your players should find each other by scanning. `peers` holds
at most 24 badges and beacons in all, the first heard. Nothing in a scan is proof:
anyone can send a HELLO or claim an id. While a scan runs the badge is off the mesh,
even while it visits its own channel: it sends no HELLOs, `mesh.send` answers
`nil, "scanning"`, `on_mesh` hears nothing (what it hears goes only into the scan's
results), `mesh.nearby()` keeps what it heard before
(30 s), and Wi-Fi joined on the mesh channel pauses until the scan ends. One scan at a
time: another answers `nil, "scanning"`; between `mesh.pause()` and `mesh.resume()`,
`nil, "paused"`; with a bridge open, `nil, "bridge open"` (a bridge keeps Wi-Fi joined,
and a joined radio can't change channel). `mesh.pause()`, a bridge opening, a script
error and closing the app end a scan without calling `cb`; a script error also gives
back the channel `mesh.channel(n)` chose. Neither outlives the app: a scan or a channel
an `on_exit` asks for ends with it.

```lua
-- Find this game's players, else the quietest channel, and go there.
mesh.scan({band = "2.4"}, function(chs)
  local best
  for _, c in ipairs(chs) do
    if c.app > 0 then best = c break end
    if not best or c.badges + c.beacons < best.badges + best.beacons then best = c end
  end
  mesh.channel(best.channel)
end)
```

**Bridged badges.** Badges out of radio range, and emulators (no radio), meet over a
**bridge** through the server (docs/MESH.md "Bridges"). One badge shows its bridge
code; another badge types its short code (SELECT on the code screen), or the emulator
page scans or pastes it. After that each is in the other's
`mesh.nearby()` with `bridged=true` and `rssi=0`, and `mesh.send` and `on_mesh` work
exactly as over the air: a message to its id goes to it, and `"*"` reaches every badge
in range *and* every bridged badge. The same app has to run on both. Your app doesn't
need to know which is which, with two differences:

- **Latency.** A bridged message takes about 1 to 3 seconds (radio: a few ms), and
  messages to one badge arrive in order. Send state, not every key press: a turn-based
  game or a shared score works well, a fast action game doesn't.
- **Loss.** Like the radio, a bridge may drop a message (one waiting more than 60 s,
  or a backlog of more than 64). Repeat what matters until the other side answers.

`mesh.bridge()` opens the code on top of your app until the user presses CANCEL (keys
don't reach your app meanwhile); bridges that open show up in `mesh.nearby()` within a
few seconds. It needs Wi-Fi: without it the panel says so. In the emulator, the page's
**Bridge a badge** panel scans or pastes a code, so you can test a multiplayer app
with one badge and one emulator, or two emulator tabs.

```lua
-- Who's here, over the radio or a bridge?
local list = ui.list(0, 0, ui.W, ui.H - 16, {}, {})
ui.hint("SELECT ping   RIGHT bridge   CANCEL exit")

local function refresh()
  local names = {}
  for _, p in ipairs(mesh.nearby()) do
    if p.kind == "badge" then
      names[#names + 1] = (p.name ~= "" and p.name or p.id) .. (p.bridged and "  (bridged)" or "")
    end
  end
  list:items(names)
end
timer.every(1000, refresh)

function on_key(k)
  if k == "right" then mesh.bridge() return true end
  if k == "select" then mesh.send("*", {ping = true}) return true end
  return false
end

function on_mesh(from, msg)
  if msg.ping then ui.toast(from .. " says hi") end
end
```

#### Meets: prove that two badges met (Solana OS 0.5.15)

`mesh.send` messages are unsigned. What your app says it heard from another badge is
only that badge's word, and a badge's owner can change the Lua on it, so a server
can't believe "I met 7K2Q9M" in an `http` body. A **meet** is the way around that, and
it is how X Dex swaps cards with Wi-Fi off:

1. Two badges running your app exchange three short messages over the mesh. No Wi-Fi
   is involved.
2. Both sign one statement with their **mesh key**, which only that badge and the badge
   server know: your app's id, both badge ids, a fresh random number from each, each
   badge's clock and each side's data. The OS builds the statement, so a script can't
   sign anything else.
3. Each badge keeps the *other* badge's signature and puts the meet in the badge's
   outbox, on the card. The OS uploads it the next time the badge has Wi-Fi, also after
   your app has closed or the badge restarted.
4. The badge server checks the signature and stores the meet once, whichever badge
   uploads it first. Your own server reads the meets with your app's key.

A badge can only upload a meet with a signature it could not have made itself, so
changing the code on one badge isn't enough to invent one.

```lua
ok, err = mesh.meet(id, data, cb)
-- Ask badge `id` (from mesh.nearby) for a meet. data: a table of named fields,
-- at most 100 bytes as JSON, plain ASCII.
-- Returns true when the request is on its way (cb follows), else nil and why:
--   "busy" (one ask at a time), "too fast: one meet a second", "not nearby",
--   "the mesh is off", "outbox full" (40 uploads waiting), "data over 100 bytes"
-- cb(true, {peer = id, data = <their table>})   both signed; the upload is queued
-- cb(false, "refused" | "no answer" | "outbox full")

function on_meet(from, data)    -- another badge asks. Return a table (your side's
  return { ok = true }          -- data) to sign, anything else to refuse
end

mesh.meets([after,] cb)
-- What the server holds for this badge's owner, oldest first:
-- cb(true, {items = { {id=, peer=, asked=, mine={...}, theirs={...}, bridged=, at=}, ... }, next = id or nil})
-- at: unix seconds. Pass `next` as `after` for more.

mesh.pause()                    -- give the radio back to Wi-Fi
mesh.resume()                   -- take it for the mesh again
```

**One radio.** Wi-Fi and the mesh share the badge's radio. While an app with `mesh`
runs, a Wi-Fi network on another channel is paused: server calls answer `"offline"`
and meets wait in the outbox. They upload when your app closes. To sync without
leaving, do what X Dex does and switch: `mesh.pause()` lets Wi-Fi rejoin (a few
seconds; `badge.online()` turns true), the outbox empties and `mesh.meets`,
`http` and the rest work; `mesh.resume()` goes back to the mesh. While paused,
messages and meets don't arrive and nearby badges drop out of `mesh.nearby()` after
30 seconds. With Wi-Fi off, meets wait until the owner turns it on. Over a bridge, or
on a network that is on the mesh channel, both work at once and there is nothing to
switch: `mesh.info().online` (1.0.0) tells which. The example
[`mesh-and-wifi`](examples/mesh-and-wifi/) shows the player which it is and syncs with
`mesh.pause()` only when it has to.

**What a meet proves.** Both badges' keys signed that statement: both owners' badges
took part, in your app, with that data. It does **not** prove two people stood
together: two owners who cooperate can pass messages along, and a bridge does exactly
that (`bridged` says so, when the badges are honest). A badge's clock is its own word
too. So let a meet earn what cooperation between two people is worth, count a pair of
people once, and keep anything valuable behind your own server's rules.

**Your side decides.** `on_meet` runs for any badge nearby that runs your app and
asks, as often as once a second. Refuse what makes no sense for your game (a tag from
someone who isn't "it", a second meet with the same badge), and check on your server
again: the other badge's script may have been changed.

```lua
-- Tag: whoever is "it" tags a badge nearby; the server keeps score from the meets.
local it = storage.get("it") or false
local label = ui.label(8, 8, it and "You're it. SELECT tags the nearest badge" or "Run!")

function on_key(k)
  if k ~= "select" or not it then return false end
  local near = mesh.nearby()[1]                 -- strongest signal first
  if not near or near.kind ~= "badge" then ui.toast("Nobody in range") return true end
  local ok, err = mesh.meet(near.id, { tag = 1 }, function(ok, res)
    if not ok then ui.toast("Missed: " .. res) return end
    it = false
    storage.set("it", false)
    label:text("Tagged " .. res.peer .. ". Run!")
  end)
  if not ok then ui.toast(err) end
  return true
end

function on_meet(from, data)
  if it or not data.tag then return nil end     -- only someone who isn't "it" can be tagged
  it = true
  storage.set("it", true)
  label:text("Tagged by " .. from .. ". You're it")
  return { caught = 1 }
end
```

**Your server** reads the meets with the app's key: make one in **My apps**
(`/apps/mine`, *Server key*; it is shown once, and making a new one retires the old).
Poll with the last `id` you saw, at the API's address (not the store's):

```sh
curl -H "Authorization: Bearer $APP_KEY" "https://badge-api.solana.com/api/app/meets?after=0"
```

```json
{ "app": "tag",
  "items": [
    { "id": 812, "at": 1790000123000, "by": "9XQ3TT", "bridged": false,
      "req": { "badge": "9XQ3TT", "user": "2f1c...", "t": 1790000061, "data": { "tag": 1 } },
      "acc": { "badge": "7K2Q9M", "user": "8e40...", "t": 1790000063, "data": { "caught": 1 } } } ] }
```

`req` is the badge that asked, `acc` the one whose app accepted. `user` is the owner's
id, the same one escrow ledgers and the signed `http` headers carry (null for a badge
nobody claimed yet): count people, not badges. `at` is when the server got the meet
(unix ms) and `by` the badge that uploaded it; `t` is each badge's clock at the meet
(unix seconds), null when it didn't know the time. A meet reaches you once, however
many times the badges upload it. From there it's yours: keep a score, or pay players
from an escrow.

A badge takes part in at most 300 meets per app a day. Test runs (Try in emulator,
Run on badge) make real meets too; those of an app you have never submitted stay on
the badges' side (`mesh.meets`) and never reach a server. Solana OS before 0.5.15 has
no meets: check `if mesh.meet then`, and a badge on an older build never answers
(`"no answer"`).

### `ble` (permission `ble`, Solana OS 1.0.0)

Bluetooth Low Energy, for what the mesh can't reach: a gadget you built on any ESP32 (or
anything else that speaks BLE), a phone, or another badge while both stay on Wi-Fi.
Bluetooth has its own channels, so it works whatever Wi-Fi channel or band the badge is
on, and next to `mesh`, `http` and `ws`. The badge has one radio that takes turns, so a
busy scan slows Wi-Fi a little. When to choose Bluetooth over the mesh or Wi-Fi, and how
to plan its memory: the [Connectivity guide](CONNECTIVITY.md). Complete apps:
[`ble-button-box`](examples/ble-button-box/) (a gadget on any ESP32, firmware included),
[`ble-nearby`](examples/ble-nearby/) (advertise and scan, no connections) and
[`ble-link`](examples/ble-link/) (two badges on one connection).

```lua
ble.on()                            -- turn Bluetooth on: true, or nil and why not
ble.off()                           -- stop everything; the memory comes back
ble.state()                         -- "off" | "on"
ble.address()                       -- this badge's address, "7c:df:a1:00:56:78" (nil while off)

ble.advertise{name = "Dice", service = "ffe0", data = "\1\2", connectable = true}
ble.advertise(false)                -- stop

ble.scan({service = "ffe0"}, function(dev) end)   -- dev: see "Finding devices"
ble.scan(false)                     -- stop

local link = ble.connect(dev, cb)   -- cb("open") | cb("notify", chr, data) | cb("close", reason)
link:read(service, chr, function(ok, data) end)
link:write(service, chr, data)              -- no answer wanted: true, or nil and why not
link:write(service, chr, data, function(ok, err) end)
link:subscribe(service, chr)                -- notifications arrive as cb("notify", chr, data)
link:subscribe(service, chr, false)
link:close()                        -- cb("close", "closed") follows
link:state()                        -- "connecting" | "open" | "closed"

ble.serve({service = "ffe0", chars = {{uuid = "ffe1", read = true, write = true, notify = true}}}, cb)
-- cb("open", addr) | cb("write", chr, data) | cb("subscribe", chr, on) | cb("close", reason)
ble.set(chr, data)                  -- the value a connected device reads
ble.notify(chr, data)               -- set it and tell the connected device: true, or nil and why not
ble.drop()                          -- disconnect the device that connected to us
```

**On and off.** Bluetooth is off until the app calls `ble.on()`, and goes off when the
app exits. While it is on it takes about 30 KB of the badge's own memory, not of the
app's heap (below 2.0.0 the two shared one heap, so the badge could stop a big app earlier:
see "Limits"). `ble.on()` returns
`true`, or `nil` and `"not enough memory"` (the badge has under 64 KB of SRAM free; below
2.0.0 the app's heap counts, so free some of it first), or `"unavailable"`. Every other function is a Lua error while Bluetooth
is off, except `ble.off`, `ble.state` and `ble.address`; a link from before answers
`"closed"` to `link:state()`, and its other methods return `nil` and `"not open"`. Turn it on when the app needs it
and off when it doesn't (below 2.0.0 it also shares the heap that a secure connection
(`http`, `ws`, the store) needs 56 KB of to start).

**UUIDs and data.** A service or characteristic is a string: 16-bit as 4 hex digits
(`"180f"`), or 128-bit as `"6e400001-b5a3-f393-e0a9-e50e24dcca9e"`; case doesn't matter,
anything else is a Lua error. Values are Lua strings of bytes (`string.pack`,
`string.byte`), ≤ 244 bytes; longer is a Lua error. How much fits in one push, one read's
answer or one write without an answer is up to the link: 20 bytes until the two sides
agree on more (they do in the first moments; a Mac agreed to the full 244), and anything longer
is cut there. A write with a callback goes through whole. Keep messages under 20 bytes
if they must arrive whatever the other device is.

**No pairing, no encryption.** Anyone in range can see an advertisement, connect to a
served service, and read and write it; a device can lie about its name. Treat everything
that arrives as untrusted (check lengths and ranges), never send a secret, and when
something is worth cheating at, let your server decide (`http`), or use a meet
(`mesh.meet`), which both badges sign.

#### Being found: `ble.advertise`

`ble.advertise(opts)` starts (or replaces) what the badge broadcasts about ten times a
second; `ble.advertise(false)` stops it. All fields are optional:

| | |
|---|---|
| `name` | what scanners show, ≤ 29 bytes of printable ASCII. Default `"Badge <id>"` |
| `service` | one service UUID, so that others can scan for your app only |
| `data` | bytes for whoever is listening, no connection needed: ≤ 24 bytes, ≤ 20 with a 16-bit `service`, ≤ 6 with a 128-bit one |
| `connectable` | `true`: devices may connect (see `ble.serve`). Default `false` |
| `interval` | milliseconds between broadcasts, 20 to 10000. Default 100 |

It returns `true`, or `nil` and `"too big"`, or `"busy"` for a `connectable` one while a
link exists (one link at a time; a plain advertisement keeps going through a link). The name travels in the scan response, so
only an active scan (below) sees it; `service` and `data` are in the advertisement
itself. Call it again to change `data`: a counter, a score, a state.

#### Finding devices: `ble.scan`

`ble.scan(opts, cb)` listens for advertisements and calls `cb(dev)` for each device, at
most once a second per device; `ble.scan(false)` stops. It returns `true`, or `nil` and `"busy"` while the badge is connecting. One scan at a
time (a new one replaces it). A scan runs alongside advertising, serving and an open
link; it stops by itself when the app calls `ble.connect`, and doesn't start again
(call `ble.scan` again once the link is open or closed). A `duty` or `seconds` out of
range is a Lua error, as is an `interval` in `ble.advertise`. `opts`, all optional:

| | |
|---|---|
| `service` | only devices advertising this service |
| `name` | only devices whose name starts with this. Names travel in the scan response, so this needs `active = true` (a passive scan with `name` hears nothing) |
| `active` | `true`: also ask each device for its name (it then sees a scan request). Default `false`: names are often missing |
| `seconds` | stop by itself after this long. Default: until `ble.scan(false)` |
| `duty` | percent of the time the radio listens, 10 to 100. Default 30. More finds devices faster and slows Wi-Fi and the mesh more |

```lua
dev = {
  addr = "7c:df:a1:00:12:34",   -- the device's address (phones change theirs every few minutes)
  rssi = -61,                   -- signal: about -40 next to it, -90 at the edge of range
  name = "Dice",                -- "" when it sent none (or the scan isn't active)
  services = {"ffe0"},          -- the services it advertises (up to 4), written as above: lower case, 4 hex digits or the full 128-bit form
  company = 0xffff,             -- who defined `data` (0xffff: a badge app, or a gadget using the test id), nil if none
  data = "\1\2",                -- its manufacturer data after the company id, "" if none
  connectable = true,
  random = false,               -- the address kind; ble.connect needs it, keep the table
}
```

Names arrive as printable ASCII (other bytes become `?`). When 8 events wait for the
app, newer reports are dropped: keep `cb` quick, and filter by `service`. Use a service
UUID you made up (`uuidgen`): a well-known one, such as the Nordic UART service, also
matches other people's gadgets in the room.

#### Connecting to a device: `ble.connect`

`ble.connect(dev, cb)` connects to a device from a scan (or to `{addr = "...", random =
false}`) and returns a link at once; `cb("open")` follows within about a second, or
`cb("close", reason)`. One link at a time, counting a device that connected to this
badge: a second `ble.connect` returns a link that closes at once with `"busy"`.
Connecting stops the scan and a connectable advertisement (advertise again after the
link closes).

- `link:read(service, chr, cb)`: `cb(true, data)` or `cb(false, reason)`.
- `link:write(service, chr, data)`: sends without waiting for an answer (fast, may be
  lost). With a callback it waits for the device to confirm: `cb(true)` or `cb(false,
  reason)`.
- `link:subscribe(service, chr)`: asks the device to push that characteristic; each push
  arrives as `cb("notify", chr, data)` on the link's callback (`chr` as you wrote it, in
  lower case). A device that refuses just never pushes: there is no answer to wait for.

Each returns `true` when it was queued, or `nil` and `"not open"` or `"busy"` (4 already
wait for their answer, or a subscribe when 8 characteristics are subscribed on the link).
A subscription lasts until you unsubscribe or the link closes, whatever else the app
reads or writes meanwhile. The answers' reasons: `"no such characteristic"`, `"not allowed"`
(the device refused), `"timeout"`, `"lost"` (the link ended first). The first use of a
characteristic looks it up on the device, which takes a few tenths of a second.

Every link ends with one `cb("close", reason)`, and when the app exits (no callback
then):

| `reason` | |
|---|---|
| `"closed"` | the app called `link:close()` |
| `"peer closed"` | the device hung up |
| `"lost"` | it went out of range or was switched off |
| `"unreachable"` | it didn't answer the connection within 10 s |
| `"busy"` | another link is open or being made |
| `"off"` | `ble.off()` |

To reconnect, scan again and connect from the `"close"` callback, after a second or two.

#### Being connected to: `ble.serve`

`ble.serve(def, cb)` gives the badge one service that a phone, a computer, a gadget or
another badge can connect to. Call it once after `ble.on()`, before any link, then
advertise with `connectable = true` (and the same `service`, so they can find it).

```lua
def = {
  service = "6e400001-b5a3-f393-e0a9-e50e24dcca9e",
  chars = {                                   -- 1 to 4
    {uuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e", write = true},
    {uuid = "6e400003-b5a3-f393-e0a9-e50e24dcca9e", read = true, notify = true, value = "hello"},
  },
}
```

- `cb("open", addr)`: a device connected (one at a time; advertising stops meanwhile).
- `cb("write", chr, data)`: it wrote to a `write` characteristic.
- `cb("subscribe", chr, on)`: it asked for (or stopped) that characteristic's pushes.
- `cb("close", reason)`: `"peer closed"`, `"lost"`, `"closed"` (`ble.drop()`) or `"off"`.
  Advertise again to be found again.

The same queue of 8 holds what a connected device sends: a burst of writes or subscribes
beyond it is dropped (a write still becomes the value), so don't rely on seeing every
one. A read of a `read` characteristic is answered with its current value, without calling
the app: `value` in `def`, then whatever `ble.set(chr, data)` last set (a write from the
device sets it too). `ble.notify(chr, data)` sets the value and pushes it: `true`, or
`nil` and `"not open"` (nobody connected), `"not subscribed"` or `"busy"` (the radio has
no room for another push right now: try again on the next tick). It asks the radio, so it works even when the
`"subscribe"` callback was one of those dropped. `ble.set` returns `true`. `ble.serve`
returns `true`, or `nil` and `"busy"` (a link exists), `"already serving"` or `"not enough
memory"`. `ble.drop()` with nobody connected does nothing. Lua errors: a characteristic
that isn't served (`ble.set`, `ble.notify`), no characteristic or more than 4, or the
same UUID twice (`ble.serve`).

#### In the emulator

The emulator has no radio, so its Bluetooth is the browser: emulated badges in tabs of
one browser (`/emulator` and `/emulator?badge=2`) advertise to, find, connect to and
serve each other with the same calls, and nothing else is in range. Use it to build both
halves of a two-badge app; a gadget needs a real badge (Run on badge).

```lua
-- Read a button box you built: an ESP32 serving ffe0, whose ffe1 notifies 2 bytes,
-- 1 while the button is held (else 0) and a press count (examples/ble-button-box/).
-- app.json: "permissions": ["ble"]
local status = ui.label(10, 40, "Looking for the box...")
local link

local function find()
  ble.scan({service = "ffe0", seconds = 20}, function(dev)
    ble.scan(false)
    link = ble.connect(dev, function(what, a, b)
      if what == "open" then
        status:text("Connected")
        link:subscribe("ffe0", "ffe1")
      elseif what == "notify" then
        status:text(string.byte(b, 1) == 1 and "Pressed" or "Released")
      elseif what == "close" then
        status:text("Lost it (" .. a .. "), looking again")
        timer.after(2000, find)
      end
    end)
  end)
end

local ok, why = ble.on()                       -- nil and "not enough memory" or "unavailable"
if ok then find() else status:text("No Bluetooth: " .. why) end
```

### `lamps` (permission `lamps`)

```lua
lamps.balance(cb)                       -- cb(true, 1250)
lamps.spend(amount, sku, cb)            -- in-app purchase paid to the app's author.
                                        -- The OS asks "Pay 50 !LAMPs to <App> for <sku>?"
                                        -- cb(true, {balance=1200}) | cb(false, "denied")
lamps.transfer(badgeId, amount, memo, cb) -- pay another badge's owner (OS confirm dialog too)
lamps.deposit(address, amount, memo, cb) -- pay into one of the author's escrows (OS confirm too):
                                        -- "Pay 50 !LAMPs into escrow ESC-7K2Q9M3X of <App>?"
                                        -- cb(true, {balance=1150, id="4821"})  (Solana OS 0.3.1)
                                        -- spend + transfer + deposit: at most 500 !LAMPs a day per user
lamps.award(amount, reason, cb)         -- earn: only within the app's daily award cap,
                                        -- cb(true, {awarded=10, balance=1260})
                                        -- only owners who linked X earn (the cap counts
                                        -- per X account); otherwise
                                        -- cb(true, {awarded=0, balance=1250, needsX=true})
```

#### Escrows

`lamps.spend` pays the author's own balance. To hold !LAMPs for players instead (a
wager's pot, buy-ins, a jackpot, a bank the app pays winners from), create an escrow
at `/apps/escrows`: a payment address like `ESC-7K2Q9M3X` with a balance of its own,
apart from yours. Put the address in your app and pay into it with `lamps.deposit`;
it only accepts escrows that belong to the app's author. `memo` (≤ 64 characters) is
how your app says what a deposit is for (`"round 42 red"`).

Paying out is up to your own server, with the escrow's key (shown once when you make
the escrow; you can rotate it): it polls the escrow's ledger for new deposits (who
paid, from which badge and app, the memo), decides, and pays players back, winners,
or you, by badge id or user id. The badge never holds the key, so nobody can pay
themselves out of your escrow by tampering with a badge. The endpoints are in the
server API (`/api/escrow`, `/api/escrow/ledger`, `/api/escrow/pay`):

```sh
curl -H "Authorization: Bearer $ESCROW_KEY" "https://badge-api.solana.com/api/escrow/ledger?after=0"
curl -H "Authorization: Bearer $ESCROW_KEY" -H "content-type: application/json" \
  -d '{"to":"7K2Q9M","amount":100,"memo":"won round 42","key":"round-42"}' https://badge-api.solana.com/api/escrow/pay
```

Payouts show in the player's wallet as "Escrow payout" with your memo. `key` makes a
payout safe to retry. Your app can also talk to your server with `http` (below), and
the signed headers tell the server which badge and player is asking: join a round,
then pay into it with the round in the memo.

### `items` (permission `items`)

Items are JSON objects in the owner's inventory. An app can create and change only
its own items, and it can read everything. An app granted by a quest may also update
the items that quest gave while the grant is live (e.g. Virus Lab mutating the quest's
virus), and `items.list` includes them. Lists hold the newest 50.

```lua
items.list(cb)              -- the items this app may update: { {id=, kind=, name=, data={...}, tradable=true}, ... }
items.all(cb)               -- every item the owner has (read-only for other apps' items)
items.grant(kind, name, data, cb)        -- new item; data ≤ 1 KB as JSON; cb(true, item)
items.update(id, data, cb)               -- replace data of one of this app's items
items.remove(id, cb)
```

The server rate-limits grants (20 per user per app per hour) and caps an app at
200 live items per user.

### `profile` (permission `profile`)

```lua
profile.me(cb)              -- {name=, x=, xName=, solana=, level=, lamps=, pages=, badge=}
profile.get(badgeId, cb)    -- public profile of that badge's owner: {name=, x=, xName=, solana=, level=}
```

- `name`: the name people see: the X name when X is linked, else the name the person set
  at `/profile` (nil if neither).
- `x`: the X handle without the `@` (`"toly"`), `xName`: the X display name; both nil
  without a linked X account.
- `solana`: the Solana address (base58) the person linked at `/profile` by signing with
  their wallet, nil without one. `profile.me` always has it (the owner installed your
  app); `profile.get` only when that person chose to make it public at `/profile`, else
  nil, since anyone nearby can read a badge's id. Use it to look up their balances with
  `rpc` or to pay them from your own server. The badge never holds
  their key: anything to sign still goes to their phone ("Build on the badge, sign on a
  phone").
- Never an email.

#### The player's items and Guide pages (permission `inventory`, Solana OS 2.3.0)

Two more reads, about the badge's own owner only: what they hold in the game. They need
the permission `inventory` (not `profile`: the store and the badge tell the player "See
the items and Guide pages you hold" before they install your app). There is no form for
another badge: `profile.get` stays a public profile.

```lua
profile.items([cursor,] cb)  -- cb(true, {items = {...}, next = "..." or nil, total = 37})
profile.pages([cursor,] cb)  -- cb(true, {pages = {...}, next = "..." or nil, owned = 12, total = 64})
```

- **Items**: every item in the owner's inventory, whatever made it (a quest, the shop, your
  app, another app), oldest first: `{id=, kind=, name=, app=, quest=, tradable=}`. `app` is
  the store app that made it (nil once that app is gone), `quest` the quest that gave it;
  both nil for the shop's and an admin's. Items don't stack: three towels are three entries
  (count them by `kind` or `name`). Never `data`: what an app keeps in its items is that
  app's (an app with `items` reads its own with `items.list`). `total`: how many items the
  owner holds.
- **Pages**: the Guide pages the owner holds, by page number:
  `{id=, number=, title=, category=, rarity=, count=}` (`count`: copies held, 1 or more).
  `owned`: how many different pages they hold, `total`: how many the Guide has. Pages they
  don't hold aren't listed (not even as "???").
- **Paging**: an answer holds up to 10 entries (fewer when they're long: it always fits the
  badge's 4 KB). `next` is a cursor for the rest: pass it back as the first argument, until
  `next` is nil. A walk to the end never repeats an entry and never skips one the owner held
  all along, even while the inventory changes: an item that arrives meanwhile shows up at
  the end of the walk (or on the next one, if it came in a trade), one given away meanwhile
  is just missing from the pages not yet read. A cursor is opaque: keep it as it is.
- **Errors**: as for every server call (`"offline"`, `"not linked"` while the badge has no
  owner, `"denied"` without the permission, `"busy"`), plus `"Too many inventory reads. Try
  again in a minute"` past 30 calls a minute per badge (the two together). A cursor that isn't
  one (`"not a cursor (pass the answer's next)"`) is an error raised at the call.
- Names and titles are printable ASCII. Before 2.3.0 the functions don't exist; the store
  doesn't install an app with `inventory` on older badges, but a script copied over the
  console can check `if profile.items then`.

An app that shows how many of the Guide's pages its player has:

```lua
-- app.json: "permissions": ["inventory"]
local line = ui.label(8, 8, "Counting your pages...", {size = 20, w = 304})
local rarest = {}

local function walk(cursor)
  profile.pages(cursor, function(ok, r)
    if not ok then return line:text("Can't see your pages: " .. r) end
    for _, p in ipairs(r.pages) do
      if p.rarity == "legendary" then rarest[#rarest + 1] = p.title end
    end
    if r.next then return walk(r.next) end
    line:text(("You hold %d of the Guide's %d pages"):format(r.owned, r.total))
    if #rarest > 0 then ui.label(8, 40, "Legendary: " .. table.concat(rarest, ", "), {w = 304}) end
  end)
end
walk()
```

### `quest` (permission `quest`)

For apps that quests hand out.

```lua
quest.report(event, data, cb)   -- tell the quest engine something happened, e.g.
                                -- quest.report("mutated", {strain="omega"})
quest.active(cb)                -- the owner's active quests: { {id=, title=, step=}, ... }
```

### `http` (permission `http`, Solana OS 0.4.6)

Requests to web servers: your own (a game server, the server behind your escrow) or a
public API (when `http` is the right choice, and when the mesh or Bluetooth is:
[Connectivity](CONNECTIVITY.md)). The badge can't open a second secure connection while an app runs, so the
badge server makes the request for it and hands back the answer. A request needs
Wi-Fi like every server call, and works the same in the emulator.

```lua
http.get(url, cb)                       -- cb(true, res) or cb(false, "offline")
http.get(url, headers, cb)              -- headers: {["Accept"] = "text/plain"}
http.post(url, body, cb)                -- body: a string, or a table (sent as JSON)
http.post(url, body, headers, cb)
http.request({method = "PUT", url = url, headers = {...}, body = body}, cb)
-- res = {status = 200, body = "...", type = "application/json", truncated = false}
```

**Hosts.** An app calls only the hosts it lists in `hosts` when you publish it: up to
8 host names, each a name like `api.example.com` or `*.example.com` (every subdomain of
example.com, not example.com itself), with no IP addresses, ports or paths. The store
shows them next to the permission, so people see where the app sends data, and the
badge server refuses any other host with `"host not allowed"`. Change them in a new
version.

**Requests.**

- `url`: `https://` only (port 443), ≤ 512 characters, no `user:password@`.
- `method`: `GET`, `POST`, `PUT`, `PATCH`, `DELETE` or `HEAD`; `http.get` and
  `http.post` set it, `http.request` defaults to `GET`, or `POST` when there's a body.
- `headers`: a table of up to 8 name = value pairs. Names are letters, digits and `-`;
  values printable ASCII, ≤ 256 characters. The badge server sets `Host`,
  `User-Agent`, `Content-Length`, the connection headers and the `X-Badge-*` headers
  itself, so those are refused.
- `body`: ≤ 2 KB. A table is sent as JSON (with `Content-Type: application/json`
  unless you set one), a string as it is (`text/plain; charset=utf-8` unless you set
  one).

**Answers.** Any answer from the server counts: `ok` is `true` and `res.status` says
what it was, so check it (a 404 is `ok` too). `res.body` is its text, up to about
3.5 KB; a longer answer is cut there with `res.truncated = true`. Ask your server for
just what the app needs (a 20 KB weather report won't fit, a 300-byte summary will).
Bodies are text: bytes that aren't UTF-8 come out as `"�"`. `res.type` is the
`Content-Type`. Redirects aren't followed: a 3xx comes back as it is, with
`res.location`. `json.decode(res.body)` turns a JSON answer into a table.

`ok` is `false` when there was no answer: `"offline"`, `"not linked"`, `"denied"` (the
app lacks the permission, or isn't in the store: see below), `"busy"`, `"host not
allowed"`, `"too fast"` (over 60 a minute), `"timeout"` (no answer within 6 s),
`"unreachable"` (no such host, no connection, or a bad certificate), `"bad url"`,
`"bad header: <name>"` (one the badge server sets itself) or `"request too big"`. A
URL that doesn't start with `https://` or has spaces in it, and a malformed header,
are Lua errors, like a bad argument anywhere else.

**Test before you publish.** The badge server checks each request against the app as
it is in the store: the permission, the hosts, and that the badge's owner may run it.
When you're signed in, Try in emulator and Run on badge start a *test run* of the app
you're editing on that badge first (see "Develop and publish"): for 12 hours its
requests are checked against your draft's permissions and hosts instead, and act for
you, before the app is in the store or while a new version waits. A test run's `http`,
`ws` and `rpc` need your account to have X linked (`"Link X at /profile to test http, ws
and rpc before publishing"` otherwise). Anywhere else, an app that isn't in the store
gets `"denied"`, and the hosts in effect are the published version's.

**What your server receives.** Each request carries headers that say who is calling,
signed by the badge server so nobody can fake them:

```
User-Agent: SolanaOS-Badge/1
X-Badge-App: snake                     the app's id
X-Badge-Id: 7K2Q9M                     the badge (an emulator's id starts with "e")
X-Badge-User: 2f1c...-...              the owner's user id, as in escrow ledgers
X-Badge-Time: 1790000000000            unix ms
X-Badge-Request: 4c1f0e9a7b2d6e83      random, different for every request
X-Badge-Key: 9d2b7e41c0a8f356          which signing key (see below)
X-Badge-Signature: 3q2+7w...==         Ed25519 signature, base64
```

They are ids, never a name or email (an app that needs the name has `profile`). The
signature covers these nine lines joined with `"\n"` (no newline at the end):

```
badge-http-v1
POST                                   the method
https://api.example.com/round?id=42    the URL as sent (without a #fragment)
9f86d081...                            SHA-256 of the body as sent, hex (of "" for none)
snake                                  X-Badge-App
7K2Q9M                                 X-Badge-Id
2f1c...-...                            X-Badge-User
1790000000000                          X-Badge-Time
4c1f0e9a7b2d6e83                       X-Badge-Request
```

The public key is at `GET https://badge-api.solana.com/api/http/key` (the server's
address, not the store's: see "The server's address" above):
`{"id": "9d2b7e41c0a8f356", "alg": "Ed25519", "key": "<32 bytes, base64>", "pem":
"-----BEGIN PUBLIC KEY-----..."}`. Keep it by `id`, and fetch it again when
`X-Badge-Key` names one you don't have (the key may be replaced). To trust a request,
check the signature, that `X-Badge-App` is your app, that `X-Badge-Time` is within a
minute of your clock, and that you haven't seen its `X-Badge-Request` in that minute
(a replay). Check and remember the request in one step, after the signature and with no
`await` in between, or two copies arriving together both pass. In TypeScript (Bun,
Node 20+, Deno, Cloudflare Workers):

```ts
const SERVER = 'https://badge-api.solana.com';
const keys = new Map<string, CryptoKey>();
const seen = new Map<string, number>(); // X-Badge-Request -> time
let fetching: Promise<void> | null = null; // the key fetch in flight: every request waits for the same one
let fetchedAt = 0;

async function badgeKey(id: string): Promise<CryptoKey | null> {
  if (!keys.has(id) && !fetching && Date.now() - fetchedAt > 60_000) { // at most once a minute
    fetchedAt = Date.now();
    fetching = (async () => {
      const k = await (await fetch(`${SERVER}/api/http/key`)).json();
      const raw = Uint8Array.from(atob(k.key), (c) => c.charCodeAt(0));
      keys.set(k.id, await crypto.subtle.importKey('raw', raw, 'Ed25519', false, ['verify']));
    })()
      .catch(() => { fetchedAt = 0; }) // failed: the next request tries again
      .finally(() => { fetching = null; });
  }
  if (!keys.has(id) && fetching) await fetching;
  return keys.get(id) ?? null;
}

/** True the first time `id` comes, false again until a minute after `time`. The check and
 *  the claim are one step, so call it after your last `await`. Old ids go as it runs. */
function once(id: string, time: number): boolean {
  for (const [k, t] of seen) if (Date.now() - t > 60_000) seen.delete(k);
  if (seen.has(id)) return false;
  seen.set(id, time);
  return true;
}

/** The badge and player behind a request from your app, or null. `body`: the raw body. */
export async function fromBadge(req: Request, body: string, app: string) {
  const h = (n: string) => req.headers.get(n) ?? '';
  const time = Number(h('x-badge-time'));
  const id = h('x-badge-request');
  if (h('x-badge-app') !== app || !(Math.abs(Date.now() - time) < 60_000) || !id) return null;
  const key = await badgeKey(h('x-badge-key'));
  if (!key) return null;
  const hash = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(body)));
  const signed = ['badge-http-v1', req.method, req.url, [...hash].map((b) => b.toString(16).padStart(2, '0')).join(''),
    app, h('x-badge-id'), h('x-badge-user'), h('x-badge-time'), id].join('\n');
  const sig = Uint8Array.from(atob(h('x-badge-signature')), (c) => c.charCodeAt(0));
  if (!(await crypto.subtle.verify('Ed25519', key, sig, new TextEncoder().encode(signed)))) return null;
  if (!once(id, time)) return null; // a replay
  return { badge: h('x-badge-id'), user: h('x-badge-user') };
}
```

`req.url` has to be the URL the badge asked for. Behind a proxy or load balancer that
changes it (to `http://`, or another host), put your public `https://` address and the
path with its query together yourself.

The signature proves which badge and account sent a request through your app's id,
not that your exact code sent it: anyone can run their own script under your app's id
on their own badge. Decide on your server what a player may do, as you would for any
client. And since anyone who has your app can read `main.lua`, an API key in it is no
secret: keep keys on your server and let the app call that.

```lua
-- A shared high-score table on your own server (hosts = {"scores.example.com"}).
local list = ui.list(0, 0, ui.W, ui.H - 16, {"Loading..."}, {})
ui.hint("SELECT submit 100   CANCEL exit")

local function show(ok, res)
  if not ok then list:items({"No scores: " .. res}) return end
  if res.status ~= 200 then list:items({"Server said " .. res.status}) return end
  local rows = {}
  for i, s in ipairs(json.decode(res.body) or {}) do
    rows[#rows + 1] = {title = s.name, right = tostring(s.score)}
  end
  list:items(rows)
end

http.get("https://scores.example.com/top?n=10", show)

function on_key(k)
  if k ~= "select" then return false end
  -- The server learns the player from the signed headers, so the body only says what happened.
  http.post("https://scores.example.com/score", {score = 100}, function(ok, res)
    ui.toast(ok and res.status == 200 and "Sent" or "Not sent")
    http.get("https://scores.example.com/top?n=10", show)
  end)
  return true
end
```

### `ws` (permission `http`, Solana OS 0.5.1)

A live connection (a WebSocket) to one of the app's hosts, for messages both ways as
they happen: a multiplayer game, a chat, live scores. Unlike `http` it goes straight
from the badge to the host, so messages arrive as the host sends them. The badge holds
one secure connection at a time, so while the socket is open it takes the place of the
badge's own connection to the badge server (see "While a socket is open"). A complete
room game with its server: [`wifi-room`](examples/wifi-room/).

```lua
local sock = ws.open(url, cb)                         -- url: wss://, to one of the app's hosts
local sock = ws.open(url, {protocols = {"chat.v1"}}, cb)
-- cb("open")                       connected
-- cb("message", text)              a message from the host
-- cb("close", reason, code)        the end, always the last event (reasons below)
sock:send(text)                     -- or a table, sent as JSON: true, or nil and why not
sock:close()                        -- cb("close", "closed") follows
sock:state()                        -- "connecting", "open" or "closed"
```

**Opening.** `url` is `wss://` on port 443 (no other port, no `user:password@`), ≤ 512
characters, to a host in the app's `hosts` (the same list as `http`), with no
`badge_ticket` in its query. The badge first asks the badge server for a ticket (see
"What the host receives"), which checks the permission, the hosts and that the owner
may run the app, as for `http` (a test run counts). Then it closes its connection
to the badge server and connects. `protocols` (optional): up to 4 subprotocols to
offer, each letters, digits, `.`, `-` and `_`, ≤ 32 characters; the host picks one or
none. One socket at a time: while one is connecting or open, `ws.open` returns a
socket that closes at once with `"busy"`. A URL that doesn't start with `wss://` or has
spaces in it is a Lua error.

**Messages.** ≤ 4 KB each way, in order. `sock:send` sends a text message (a table as
JSON) and returns `true`, or `nil` and `"not open"` (still connecting, or closed), `"too
big"` or `"too fast"` (8 messages already waiting to go out). A message from the host
arrives as a string (a binary message too, byte for byte); one over 4 KB closes the
socket with `"too big"`. Messages are delivered between callbacks like everything else,
and the badge stops reading from the host while 8 wait for the app, so a fast host is
slowed down, not dropped.

**Closing.** Every socket ends with one `cb("close", reason, code)`; `code` is the
host's close code when the host closed it (1000 normal, 1001 going away, ...), else
nil. The socket also closes when the app exits (no callback then).

| `reason` | |
|---|---|
| `"closed"` | the app called `sock:close()` |
| `"host closed"` | the host closed it (see `code`) |
| `"lost"` | the connection dropped without a close |
| `"offline"` | no Wi-Fi, or it went |
| `"unreachable"` | no connection: no such host, a TLS error, or the host refused the WebSocket |
| `"not enough memory"` | no room for a secure connection beside the app (see "Memory") |
| `"too big"` | a message over 4 KB came in |
| `"busy"` | another socket is connecting or open |
| `"too fast"` | over 20 opens a minute |
| `"not linked"`, `"denied"`, `"host not allowed"`, `"bad url"` | as for `http` |

To reconnect, call `ws.open` again from the `"close"` callback, after a few seconds if
it failed (opens count toward 20 a minute).

**While a socket is open** (from `ws.open` until its `"close"`), the badge isn't
connected to the badge server. The app's other server calls (`lamps`, `items`,
`profile`, `quest`, `http`, `rpc`) fail at once with `"socket open"`, `badge.online()` is
false, and the badge's own syncs (its owner, bridges, X Dex and trade uploads) wait.
Badges nearby over the air (`mesh`) keep working; bridged ones pause. So call the
server first, then open the socket, and close it when you need the server again: the
connection to the badge server comes back by itself once the socket's `"close"` has
run.

**Memory.** Starting a secure connection takes up to about 40 KB for a moment (22 KB
to `echo.websocket.org`), and an open one keeps about 5 KB. The badge closes its server
connection first, then starts the socket only with 56 KB free, the same rule as for
its own connection, so other things don't run out meanwhile. From 2.0.0 that memory is
PSRAM, which an app's heap never takes below 512 KB, so there always is room. Below
2.0.0 it was the heap the app shares: with Wi-Fi joined, an app whose Lua heap is around
10 KB when it calls `ws.open` has that room, one at 20 KB doesn't (`lua mem` shows the
heap), so there open the socket first and load big data after it's up; without the room
it closes with `"not enough memory"`. In the emulator the
socket is the browser's: everything above holds, except that memory is never short and
a failed connection is always `"unreachable"` (browsers don't say why).

**What the host receives.** The badge adds a query parameter to the URL, last:
`badge_ticket=<ticket>`. It tells your server the app, badge and player, as `http`'s
signed headers do (a browser can't set headers on a WebSocket, so the emulator couldn't
send those). The ticket is seven fields joined with `.`:

```
<app>.<badge>.<user>.<time>.<request>.<key>.<signature>
snake.7K2Q9M.2f1c...-....1790000000000.4c1f0e9a7b2d6e83.9d2b7e41c0a8f356.Lw3n...
```

The app id, the badge id (an emulator's starts with `e`), the owner's user id, unix
ms, 16 random hex, the signing key's id, and an Ed25519 signature (base64url, no
padding) by the same key as `http`'s (`GET https://badge-api.solana.com/api/http/key`) over these lines
joined with `"\n"`:

```
badge-ws-v1
api.example.com                        the host the badge connects to
snake                                  the app
7K2Q9M                                 the badge
2f1c...-...                            the owner's user id
1790000000000                          the time
4c1f0e9a7b2d6e83                       the request
```

Check it when the connection opens, as for a request: the signature, that it names
your app, that the time is within a minute of your clock and that you haven't seen the
request in that minute, the last in one step after the signature (one ticket, one
socket). With `badgeKey()` and `once()` from the `http` example:

```ts
/** The badge and player opening a socket to your app, or null. host: your wss host name. */
export async function fromBadgeSocket(req: Request, app: string, host: string) {
  const ticket = new URL(req.url).searchParams.get('badge_ticket') ?? '';
  const [a, badge, user, time, request, keyId, sig, extra] = ticket.split('.');
  if (extra !== undefined || a !== app || !sig || !request ||
      !(Math.abs(Date.now() - Number(time)) < 60_000)) return null;
  const key = await badgeKey(keyId);
  if (!key) return null;
  const signed = ['badge-ws-v1', host, app, badge, user, time, request].join('\n');
  const raw = Uint8Array.from(atob(sig.replace(/-/g, '+').replace(/_/g, '/')), (c) => c.charCodeAt(0));
  if (!(await crypto.subtle.verify('Ed25519', key, raw, new TextEncoder().encode(signed)))) return null;
  if (!once(request, Number(time))) return null; // a ticket used already
  return { badge, user };
}

// Bun: Bun.serve({ async fetch(req, server) { const who = await fromBadgeSocket(req, 'snake', 'api.example.com');
//   return who && server.upgrade(req, { data: who }) ? undefined : new Response('no', { status: 403 }); }, websocket: {...} })
```

As with `http`, the ticket proves which badge and account opened the socket through
your app's id, not that your exact code did, and anything in `main.lua` is public.

```lua
-- A chat room on your own server (hosts = {"chat.example.com"}).
local list = ui.list(0, 0, ui.W, ui.H - 16, {"Connecting..."}, {})
ui.hint("SELECT wave   CANCEL exit")
local lines, sock = {}, nil

local function add(text)
  table.insert(lines, 1, text)
  lines[9] = nil
  list:items(lines)
end

local function connect()
  sock = ws.open("wss://chat.example.com/rooms/lobby", function(event, data)
    if event == "open" then
      add("Connected")
    elseif event == "message" then
      local m = json.decode(data)              -- check what arrives: it may not be what you expect
      if type(m) == "table" and type(m.from) == "string" and type(m.text) == "string" then
        add(m.from .. ": " .. m.text)
      end
    else
      add("Closed: " .. data)
      timer.after(5000, connect)               -- and again in 5 s
    end
  end)
end
connect()

function on_key(k)
  if k ~= "select" then return false end
  -- The server learns the player from the ticket, so the message only says what happened.
  local ok, err = sock:send({text = "wave"})
  if not ok then ui.toast(err) end
  return true
end
```

![Triton One](img/triton-on-dark.svg#gh-dark-mode-only)
![Triton One](img/triton-on-light.svg#gh-light-mode-only)

### `rpc` (permission `rpc`, Solana OS 0.5.2)

Solana's JSON-RPC API on mainnet and devnet, so apps can read the chain and send
transactions. **The badges' RPC is sponsored by [Triton One](https://triton.one)**: their
nodes answer every call. The badge server holds the node addresses and makes each call
for the app, as with `http`, so it needs Wi-Fi like every server call, works the same in
the emulator, and needs no `hosts`.

> **Never create or store a keypair on the badge.** A badge is not a wallet. It has no secure storage: the microSD card comes out and reads on any computer, an app's files and `storage` are plain files that the console lists and copies, and badges get lent, lost and passed around. A secret key in a script, a bundled file, `storage`, a message or a QR code is as good as published, and so is everything it holds. Sign transactions where keys are kept safe (a wallet, or your own server with its own key) and give the badge only the signed transaction to send.

```lua
rpc.call(method, params, cb)                        -- mainnet
rpc.call(method, params, {cluster = "devnet"}, cb)  -- "mainnet" (the default) or "devnet"
-- cb(true, result) or cb(false, "offline")
rpc.call("getBalance", {"Gh9ZwEmdLJ8DscKNTkTqPbNwLNNBjuSzaG9Vp2KGtKJr"}, function(ok, r)
  -- r = {context = {slot = 371234567}, value = "5000000000"}
end)
```

- `method`: one of the methods below, spelled as Solana spells it.
- `params`: a table, sent as the call's params array (`{address, {commitment =
  "confirmed"}}`), or nil for none. ≤ 2 KB as JSON.
- `result` is the call's `result`, decoded: a table, string, number, boolean, or nil
  (JSON's null, e.g. `getAccountInfo`'s `value` for an account that doesn't exist, or
  `getTransaction` for a signature the node doesn't know).

**Big numbers come back as strings.** A badge's Lua has 32-bit integers, and
`json.decode` would turn a bigger whole number into a float good to about 7 digits:
1.5 SOL is 1,500,000,000 lamports, but 5 SOL doesn't fit. So the badge server turns
every whole number beyond ±2,147,483,647 into its exact decimal string. `getBalance`
of an account with 5 SOL answers `value = "5000000000"`, with 1 SOL `value =
1000000000` (a number). Slots and block heights still fit and stay numbers. Fractions
stay numbers (32-bit floats): for token amounts use `uiAmountString`. `tostring()`
gives the digits either way, so string functions turn lamports into SOL exactly:

```lua
local function sol(lamports)                 -- "5000000000" or 1500000000 -> "5" or "1.5"
  local s = string.rep("0", 10) .. tostring(lamports)
  local whole = s:sub(1, -10):gsub("^0+", "")
  local frac = s:sub(-9):gsub("0+$", "")
  return (whole == "" and "0" or whole) .. (frac ~= "" and "." .. frac or "")
end
```

**Methods.** Reads, and `sendTransaction`:

| | |
|---|---|
| Accounts | `getAccountInfo`, `getBalance`, `getMultipleAccounts` (≤ 5 addresses), `getMinimumBalanceForRentExemption` |
| Tokens | `getTokenAccountBalance`, `getTokenAccountsByOwner`, `getTokenLargestAccounts`, `getTokenSupply` |
| Transactions | `getSignaturesForAddress` (`limit` ≤ 10, and 10 if you don't set one), `getSignatureStatuses` (≤ 10), `getTransaction` (`maxSupportedTransactionVersion` is 0 unless you set it), `getFeeForMessage`, `simulateTransaction`, `sendTransaction` |
| The chain | `getSlot`, `getBlockHeight`, `getBlockTime`, `getEpochInfo`, `getLatestBlockhash`, `isBlockhashValid`, `getRecentPrioritizationFees`, `getRecentPerformanceSamples` (≤ 10 samples, and 10 if you don't give a limit), `getHealth`, `getVersion`, `getGenesisHash` |

Anything else fails with `"method not allowed"`: the rest either can't answer in a
badge-sized reply (`getBlock`, `getProgramAccounts`, `getLargestAccounts`) or isn't
for apps (`requestAirdrop`: use a faucet).

**Answers** are ≤ about 3.5 KB. A bigger one fails with `"too big"` instead of arriving
cut, because half a JSON answer is of no use. So ask for what fits: `dataSlice =
{offset = 0, length = 64}` with `encoding = "base64"` for an account's data,
`encoding = "jsonParsed"` for token accounts, a smaller `limit`.

**Sending transactions.** `sendTransaction` takes one signed, serialized transaction
as base64 (≤ 1,232 bytes, so ≤ 1,644 characters) and answers its signature, a string.
The badge server sends it with `encoding = "base64"` (it refuses any other encoding);
the rest of the config (`skipPreflight`, `preflightCommitment`, `maxRetries`,
`minContextSlot`) goes as you set it. `simulateTransaction` takes the same and answers
what the node would do (`err`, `logs`, `unitsConsumed`), which can be over 3.5 KB with
many logs. The signatures must already be in the transaction: build and sign it where
the key lives (your server, through `http`, or a wallet), never on the badge (see the
warning above). A player's phone wallet can sign what the badge builds: see [Build on
the badge, sign on a phone](#build-on-the-badge-sign-on-a-phone). A sent transaction isn't confirmed yet: check it with
`getSignatureStatuses`. A transaction the node refuses fails with the node's error and,
in brackets, why: `"rpc -32002: Transaction simulation failed (AccountNotFound)"`, or
`(BlockhashNotFound)`, or `({"InstructionError":[0,{"Custom":1}]})` for a program's error.

`ok` is `false` with `"offline"`, `"not linked"`, `"denied"` (the app lacks the
permission, or isn't in the store and has no test run, as for `http`), `"busy"`,
`"socket open"`, `"too fast"` (over 30 calls a minute, or 6 `sendTransaction`s), `"rate
limited"` (the RPC node is limiting the badge server: wait a few seconds and try again),
`"timeout"` (no answer within 6 s), `"unavailable"` (this badge server has no node for
that cluster, or the node didn't answer), `"method not allowed"`, `"bad params"`,
`"request too big"`, `"too big"`, or the node's own error as `"rpc <code>:
<message> (<why>)"`, cut to about 120 characters. A method that isn't a string, params that
aren't a table and an unknown cluster are Lua errors.

```lua
-- Watch a wallet: its SOL, and its latest transactions.
local ADDR = "Gh9ZwEmdLJ8DscKNTkTqPbNwLNNBjuSzaG9Vp2KGtKJr"
local bal = ui.label(0, 8, "...", {size=20, w=ui.W, align="center"})
local list = ui.list(0, 44, ui.W, ui.H - 60, {"Loading..."}, {})
ui.hint("SELECT refresh   CANCEL exit")

local function refresh()
  rpc.call("getBalance", {ADDR}, function(ok, r)
    bal:text(ok and sol(r.value) .. " SOL" or r)          -- sol() from above
  end)
  rpc.call("getSignaturesForAddress", {ADDR, {limit = 5}}, function(ok, r)
    if not ok then list:items({r}) return end
    local rows = {}
    for _, s in ipairs(r) do
      rows[#rows + 1] = {title = s.signature:sub(1, 12) .. "...", right = s.err and "failed" or "ok"}
    end
    list:items(#rows > 0 and rows or {"No transactions"})
  end)
end
refresh()

function on_key(k)
  if k == "select" then refresh() return true end
  return false
end
```

### `crypto` and `solana` (always allowed, Solana OS 2.1.0)

SHA-256, the two Ed25519 checks that need no key, base58 and program-derived addresses,
done by the badge itself: in Lua they would take most of a callback's instructions, or
more. No permission is needed, since nothing here leaves the badge. Both tables are `nil`
below Solana OS 2.1.0: check `if crypto then`.

```lua
crypto.sha256(data)                     -- the 32 bytes of data's SHA-256
crypto.on_curve(bytes)                  -- true when the 32 bytes are a point of the Ed25519 curve
crypto.verify(key, message, signature)  -- true when signature (64 bytes) is key's (32 bytes) over message

solana.encode(bytes)                    -- base58, as Solana writes addresses and signatures
solana.decode(text)                     -- the bytes, or nil and "not base58" or "too long"
solana.pda(seeds, program)              -- a program-derived address (32 bytes) and its bump
```

Everything is a string of bytes (`string.pack`, `string.byte`), as with `ble`: an address
is its 32 bytes, and `solana.encode` turns it into the text `rpc` and people read.

- **`crypto.sha256(data)`** hashes a string of any length.
- **`crypto.on_curve(bytes)`** tells the two kinds of Solana address apart: an address that
  is a point of the curve can have a key (a wallet), one that isn't is a program-derived
  address. Anything but 32 bytes is `false`.
- **`crypto.verify(key, message, signature)`** checks an Ed25519 signature: a
  transaction's (the message is the transaction's message bytes) or the one a wallet makes
  for `signMessage`. It checks what Solana checks of a transaction's signature
  (ed25519-dalek's `verify_strict`, 2.1.0): it is `false` for a signature that doesn't
  match, a key or a signature of the wrong length, a signature whose second half isn't
  below the curve's group order, and a key or a signature's first half (`R`) that is one
  of the curve's eight points of small order, in any encoding. Such a key belongs to no
  one: with the neutral point (`"\1" .. string.rep("\0", 31)`) as the key, a made-up
  signature would otherwise "verify" every message. `crypto.on_curve` still says `true`
  for those points, since they are on the curve. It says that the holder of `key` signed `message`, so the key has to
  come from somewhere you trust: your server's public key can ship in `main.lua` (a public
  key is no secret), and then a badge can check a statement your server signed after
  another badge handed it over the mesh or Bluetooth, with Wi-Fi off.
- **No signing and no keys.** There is nothing here to make a key pair or a signature with,
  on purpose: a badge is not a wallet (see the warning under `rpc`).
- **`solana.encode(bytes)`** takes up to 128 bytes (more is a Lua error); a 64-byte
  signature is 88 characters or fewer. **`solana.decode(text)`** returns `nil` and
  `"not base58"` or `"too long"` (over 128 bytes) instead of raising, since text from
  outside is untrusted. Check the length of what comes back: an address is 32 bytes.
- **`solana.pda(seeds, program)`** gives what `findProgramAddress` gives everywhere else:
  the address made from the seeds, the program's address and the highest bump (255 down)
  that lands off the curve, and that bump. `seeds` is a list of up to 15 strings of at most
  32 bytes each, `program` is 32 bytes; anything else is a Lua error. It returns `nil` if no
  bump works, which doesn't happen in practice. A number among the seeds is its bytes, as
  the program wrote them: a `u64` is `string.pack("<i8", n)` for an `n` a badge's 32-bit
  integers hold, a `u8` is `string.char(n)`.

**What they cost.** The work is done in C, so it is charged to the callback's 200 000
instructions by the time it takes:

| | Instructions |
|---|---|
| `crypto.sha256` | 50, plus 1 per 4 bytes |
| `crypto.on_curve` | 5 000 |
| `crypto.verify` | 40 000, plus 1 per 2 bytes of message |
| `solana.pda` | 5 000 for each bump it tries (two on average), charged before trying it |
| `solana.encode`, `solana.decode` | 20, plus n × n / 16 for n bytes or characters (84 for an address) |

So one callback fits four signature checks, or about nineteen addresses: do more over
several timers or ticks. An address whose search tries many bumps costs that many times
5 000 (seeds that need many can be found ahead of time): past the callback's budget the
search stops with "Script took too long", like a loop would, and about 40 tries fit one
callback (~0.2 s on a badge). Measured on a badge (2.1.0): a signature check takes 57 ms, an
on-curve test 4.7 ms, an address 7 ms (4.8 ms a bump tried), and the screen doesn't redraw during a callback: check signatures one at a
time, not in a loop. None of them takes any of the app's heap beyond the strings they
return.

```lua
-- Is there a proposal for transaction 12 of a Squads multisig? Its account sits at an
-- address made from the multisig's address and the transaction's number.
-- app.json: "permissions": ["rpc"]
local SQUADS = solana.decode("SQDS4ep65T869zMMBKyuUq6aD6EgTu8psMjkvj52pCf")
local MULTISIG = solana.decode("Gh9ZwEmdLJ8DscKNTkTqPbNwLNNBjuSzaG9Vp2KGtKJr")   -- your multisig's address
local label = ui.label(8, 8, "Looking...")

local proposal, bump = solana.pda(
  {"multisig", MULTISIG, "transaction", string.pack("<i8", 12), "proposal"}, SQUADS)
rpc.call("getAccountInfo", {solana.encode(proposal), {encoding = "base64", dataSlice = {offset = 0, length = 0}}},
  function(ok, r)
    if not ok then label:text(r) return end
    label:text(r.value and "Proposal 12 exists" or "No proposal 12 yet")
  end)
```

### `service` (permission `background`, Solana OS 2.3.0)

A service keeps part of your app running while the app is closed: it answers other
badges over the mesh, polls your server, counts something, and tells the owner when
there is news. It is a separate script, **`service.lua`** at the top of your app's
folder, run in a fresh Lua state of its own: nothing of the app's UI or globals stays
alive, and the app and its service talk by messages and through `storage`.

```
pinger/
  main.lua              the app: its screen, and service.start()
  service.lua           the service: no screen, runs in the background
  lib/proto.lua         modules both can require
  app.json              "permissions": ["background", "mesh"]
```

**In the app** (`main.lua`):

```lua
service.start(function(ok, err) end)  -- start it; the first time the badge asks the owner:
                                      -- cb(true) | cb(false, "denied") | cb(false, "full")
                                      -- (3 already running) | cb(false, "low battery") | ...
service.stop()                        -- stop it; it won't start at boot either
service.running()                     -- true while it runs
service.send(value)                   -- to the service's on_message (JSON-encodable, ≤ 1 KB);
                                      -- false when it isn't running or 8 are waiting
function on_service(msg) end          -- the service sent msg while the app is open
```

**In the service** (`service.lua`): the top-level chunk runs once when it starts, then
these globals are called when defined:

```lua
function on_tick(ms) end              -- every 1000 ms; ms since the last tick
function on_message(msg) end          -- the app sent msg (service.send in main.lua)
function on_mesh(from, msg) end       -- as in an app (needs "mesh"), while the radio is on the mesh channel
function on_meet(from, data) end      -- as in an app (needs "mesh"); return a table to sign
function on_mail(m) end               -- mail for this app arrived (needs "mail"; m as mail.get gives it)
function on_stop() end                -- it is being stopped: save what you need (100 000 instructions, 1 s)

service.send(value)                   -- to the app's on_service, only while the app is open: false otherwise
service.status("3 new pings")         -- the line Services shows (≤ 48 printable ASCII; "" = "Running")
service.notify("Ping from 7K2Q9M", {led = 0x14F195})  -- a toast "<App>: Ping from 7K2Q9M",
                                      -- optional LED flash; one every 10 s, else false
service.stop()                        -- end itself (starts at boot no more, until the app starts it again)
```

**What a service may use**: `timer` (4 timers, ≥ 1000 ms), `storage` (the app's own,
shared with the app: both see each other's writes at once), `fs` and `require`, `json`,
`sys.millis`/`time`/`battery`/`memory`, `badge`, `crypto` and `solana`, and with their
permissions `mesh` (`send`, `nearby`, `meet`, `meets`; not `channel`, `scan`, `pause`,
`bridge`), `http` and `rpc` (one call waiting at a time; `"offline"` while the badge is),
and the reads `lamps.balance`, `items.list`, `items.all`, `profile.me`, `profile.get`,
`profile.items` and `profile.pages` (with `inventory`),
`mail.list` and `mail.get` (with `mail`, which also brings `on_mail(m)`, below), and
`files.dir`, `files.list`, `files.size` and `files.read` (with `files`).
Everything else is `nil` in a service: `ui`, `led` (a notice can flash), `sys.keys`,
`sys.exit`, `sys.tick` (a service keeps its 1 s tick), `wifi`, `ble`, `ws`, `quest`,
`mail.send`, `files.write`, `files.remove`, and everything that spends or changes: `lamps.spend`, `transfer`, `deposit`, `award`, `items.grant`, `update`,
`remove`. A service can never spend, and never puts text in another attendee's Mailbox:
only the app on the screen can, with the owner there.

**Lifecycle.**

- `service.start()` works only from the app on the screen. The first time, the badge asks
  the owner in its own dialog, which no app can answer: "Let <App> run in the
  background? It keeps running after you close it and after a restart, until you stop it
  in Services." SELECT allows it, CANCEL is `"denied"`. The answer is kept; the dialog
  comes back only after the owner stopped the service in Services, or when an update of
  the app asks for `background` for the first time.
- It keeps running when the app closes, and starts again about 10 s after the badge
  boots, until it's stopped: by the owner (Services, `svc stop`), by `service.stop()`
  from either side, by removing the app, or when a quest app expires.
- The app and its service can run at once; each gets its own callbacks. A message to
  the side that isn't there is dropped (`service.send` returns false): keep state in
  `storage`. Mesh messages for the app reach both; a meet (`on_meet`) goes to one side
  only, the app while it's open with the mesh, else the service (two answers would sign
  two statements).
- With 3 services running, `service.start` answers `"full"` without asking the owner.
- An error in the service stops it and its message becomes the status (`Failed:
  service.lua:12: attempt to index a nil value`, cut to the 48 characters Services
  shows; the console has all of it); `on_stop` still runs after a script error, not
  after a limit. Three failures in a row turn off starting at boot. A start counts as done
  after a minute of running: if the badge stops before (a crash in the service's start, a
  watchdog, or the power; not a restart the badge makes itself, as for an update), the
  next boot skips it (`Skipped: the badge restarted as it started`),
  and three such restarts in a row turn starting at boot off (`Off: it kept restarting the
  badge`), so a service can never keep a badge in a restart loop. That guard needs the
  record of each start in `services.json`: when the card can't write it, the app's own
  `service.start` still runs the service (`cb(true)`), but nothing else starts one (at
  boot, after an update, from Services: `Not started: the card can't record it`). `print` goes to the
  console tagged `[<app id>:svc]`, charged like a C app's output (32 instructions a
  byte).
- An update stops the service while the files change and starts the new version if it
  was running. Below 15 % battery (not on USB) services stop (`Stopped: battery low`)
  and start at the next boot or on USB. When the badge runs low on memory the most
  recently started service stops first (`Stopped: the badge was low on memory`).

**Limits.**

| | |
|---|---|
| Services | 3 app services running on a badge at once (`"full"` past that). Each costs about 2 KB of the badge's SRAM; they share one 12 KB task |
| Memory | 1 MB of Lua heap each, in PSRAM ("Out of memory: The service needs more than 1 MB."). `sys.memory()` reports it |
| CPU | 100 000 instructions per callback, and a second of wall-clock time per callback for work Lua does in C (a big `string.rep`): "Service took too long". 2 000 000 instructions a minute in all (about 0.7 s of the badge's processor a minute, 1 %): past it the service stops with `Stopped: used too much of the processor` |
| Tick | `on_tick` every 1000 ms; timers ≥ 1000 ms, 4 at a time |
| Events | 8 waiting per service (mesh messages, app messages, network answers); mesh messages past that are dropped |
| Messages | `service.send` ≤ 1 KB of JSON either way, 8 waiting on each side |
| Notices | one every 10 s, ≤ 80 characters |
| Radio | a service never moves the radio. It hears the mesh only while the radio is on the mesh channel anyway (Wi-Fi off, or joined on the mesh channel, MESH.md "Which channels"); it never suspends Wi-Fi, keeps it awake or turns it on |
| Battery | none of its own beyond the CPU above: the radio listens anyway. A service ticking once a second that does little costs too little to measure |

Badges below 2.3.0 have no `service`: the store refuses `background` apps to them
(server/API.md "Background apps").

A whole one: [`examples/pinger/`](examples/pinger/) answers mesh pings while its app is
closed.

**How the badge runs it** (what Solana OS implements; you don't need it to write one):

- One "svc" task (12 KB stack, below the app's "lua" task in priority, made with the
  first service and ended with the last) runs every service's callbacks one at a time,
  round robin, without the screen's lock (no UI). Each service is a `lua_State` with its
  own allocator capped at 1 MB in PSRAM, an event queue of 8, its timers and its share of
  the minute's budget; its state, queue and the copies of its messages and answers live
  in PSRAM too, so it costs SRAM only while a callback parses JSON. Starting one needs
  PSRAM and 64 KB of SRAM free for malloc; below 40 KB (the app's UI keeps 32 KB) or
  when PSRAM nears the app's 512 KB reserve, the newest one stops. Between callbacks a
  service holds no open file (the card takes 5 at once).
- `service.lua` always compiles on the badge (the store's compiled code is mapped for
  the app's own launch); modules it `require`s too.
- The app and its service share one copy of `storage`: a write on either side is seen
  by the other's next `get`, and `data.json` always gets the newest.
- Solana OS's own code can call a service too (`lua_host_call` in
  `main/lua/lua_host.h`: Mailbox's `on_mail(msg)`): such a callback reaches the service,
  and the app if it's on screen, when the script defines it.
- `/sdcard/hh/services.json` remembers the owner's answers:
  `{"<app id>": {"allowed": true, "boot": true, "fails": 0, "ver": "1.2.0"}}`. A
  missing or broken file means nothing is allowed. Services that were running when the
  badge restarted start ~10 s after boot, one a second.
- In the Services app (`os_service.h`) an app service is `app:<app id>`, named after the
  app, with its Lua heap as its memory; the owner's stop there turns `allowed` off.
- Stopping calls `on_stop` (100 000 instructions, 1 s), then closes the state; a service
  stuck in a callback is ended by its instruction budget. A stop never waits on the
  service.

### `mail` (permission `mail`, Solana OS 2.3.0)

Notifications that land in another attendee's **Mailbox** (the badge's built-in inbox,
docs/HHGTS.md "Mailbox"), and the ones your app received. A notification has a short
title and text that the owner reads in Mailbox, and an optional small `data` table only
your app sees. Mail goes only between badges whose owners **both installed your app**, and
your app's server can send it too (server/API.md "Mailbox", with your app key):

```lua
mail.send("7K2Q9M", "Your turn", "Alice played e4", {game = 12, move = "e4"})
                      -- to a badge id (from mesh.nearby, on_mesh, a meet): true when queued,
                      -- or nil, err ("bad id", "title: 1-32 printable ASCII", "text: ...",
                      -- "data: a table of <= 128 bytes as JSON", "not linked", "full")
mail.list()           -- this app's mail on this badge, newest first (<= 50):
                      -- { {id = 41, from = "7K2Q9M", by = "@alice", title = "Your turn",
                      --    at = 1790000000, read = false}, ... }
mail.get(41)          -- one of them in full: {id, from, by, title, text, data, at, exp, read}, or nil
function on_mail(m) end   -- a mail for this app arrived while it's open (m as mail.get gives it);
                          -- also when the owner opens your app from Mailbox on one of its mails
```

- **Sending** goes through the badge's outbox: `mail.send` checks the arguments, queues the
  mail and returns at once, offline too; it uploads when the badge is next online, even
  after your app closed. Your app learns that it was queued, not that it arrived: the server
  quietly drops mail the recipient muted or blocked, and refuses it when the recipient's
  owner doesn't have your app or a cap is reached (the outbox drops it then; the reasons are
  in server/API.md "Mailbox"). Reply in kind from the receiving badge, or check on your
  server.
- `from` is the sending badge's id, `"server"` for your app's server, or `"solana"` for an
  admin's mail that names your app; `by` is the sender's owner label (`"@alice"`) for badge
  mail, else `nil`. Times are unix seconds.
- **What counts**: nothing in mail is proof of anything. Anyone with your app can put any
  title, text and data in it. Use meets (`mesh.meet`) for proof two badges met, and never
  trust `data` with anything that matters: the server moves no !LAMPs or items for mail,
  and neither should your app.
- **Limits**: title 1-32 and text 0-200 printable ASCII characters (no newlines), `data` a
  table that encodes to ≤ 128 bytes of JSON (or `nil`); 30 sends a day per owner across all
  apps, 5 a day to the same person from one app, a send a second; none while 40 uploads
  wait in the badge's outbox (`"full"`, as meets). Mail expires after 7 days.
- `mail.list` and `mail.get` read the card (cost as `fs` reads); without a card they see
  only what came since the badge started. The owner reads, deletes and mutes in Mailbox;
  your app can't mark mail read or delete it.
- The Mailbox shows your app's mail as `<App>: <sender>` with its title, and opens your
  app on it if it's installed: then `on_mail` runs right after your top-level chunk, with
  that mail. Mail for an app that isn't installed only reads in Mailbox.
- A background service (`service.lua`, permission `background`) gets `on_mail(m)` too,
  while it runs, and may call `mail.list` and `mail.get`. `mail.send` is `nil` in a service
  (`service` above): mail to another attendee goes only from the app on the screen. A mail
  reaches the app on screen and the service once each, each through its own `on_mail`.

**From your server.** Your app's server sends with the app key (the one that reads your
meets, made on `/apps/mine`), to one badge, one person (a `user` id from a meet), or everyone
who has your app (3 times a day). No permission is needed for it, and only people who
installed your app can get it:

```ts
// Bun or any fetch: a notification to one badge's owner
const res = await fetch('https://badge-api.solana.com/api/app/mail', {
  method: 'POST',
  headers: { Authorization: `Bearer ${process.env.APP_KEY}`, 'Content-Type': 'application/json' },
  body: JSON.stringify({ to: { badge: '7K2Q9M' }, title: 'Your turn', text: 'Alice played e4', data: { game: 12 } })
});
console.log(await res.json()); // { send: 81, recipients: 1 }
// later: GET /api/app/mail/81 -> { send: 81, recipients: 1, delivered: null, ... }
// (an app's server never learns who read it, or who muted the app)
```

`to: { all: true }` reaches everyone with the app (minus who muted it; `recipients` counts them all); `ttl` (seconds, 1 hour
to 30 days) sets when it expires. The answers and limits are in server/API.md "Mailbox".

Badges below 2.3.0 have no `mail`: the store refuses `mail` apps to them (server/API.md
"Mail apps"). In the emulator mail goes through the same server: two tabs
(`/emulator` and `/emulator?badge=2`) signed in as two people can mail each other.

### `gossip` (permission `gossip`, Solana OS 2.4.0)

Gossip, the event game on the badge mesh (the parent repo's docs/GOSSIP.md): every badge
spreads and catches *programs* from the badges around it, and the OS runs it. Apps with
the permission `gossip` read the badge's part of it and hear `on_gossip(ev)`; app services
read it too. Only the Lab (`gossip-lab`) acts. Nothing here reaches the server by itself or
blocks. The store refuses `gossip` apps to badges below 2.4.0.

```lua
gossip.me()           -- the node, or nil while the badge is unclaimed:
                      -- {id = "7K2Q9M", on = true, synced = 1794795000, incubating = 1,
                      --  cls = "r", clsAt = 1794880000, stake = 120, rank = 1, credits = 3,
                      --  slots = {"59qk...", false, false}, act = "59qk...", inbox = {...},
                      --  held, deployed, deploys, adopted, boost = 0,
                      --  fx = {{k = "fee", until = 1794795600, p = nil}, ...},
                      --  imm = {{p = "G2Qq...", until = ...}, ...},
                      --  items = {{id, k, n, pick = true (Upgrade Authority+: takes a trait)}, ...}, itemsN,
                      --  epoch = {n, ends, obj = {{id, text, have, need}}},
                      --  pub = false, earns = false, why = "Link X at <site>/profile to earn",
                      --  lead = false (the "May become a leader" setting), name_nodes = 50 (adoptions
                      --  before a program can be named), close = -55 (the dBm a role claim needs, 2.4.0),
                      --  site = "https://..." (the website's address from hello, "" before the first),
                      --  ach = {n = 3, pts = 45, last = {"Voter", ...}}}
gossip.programs()     -- {slots = {p, false, p}, inbox = {p, ...}}: each p a program table (below)
gossip.program(a)     -- a program this badge holds a record of, else nil:
                      -- {a, par = "" (a root), gen, name = nil (unnamed), nodes (when known),
                      --  verified = true, spam = false, genesis, hybrid,
                      --  colour = 0xF040FF, hue, body, eyes, marks, mark_seed,
                      --  traits = {2, 4, 0}, vir, hard, stab, rarity = "rare"}
gossip.genome(a)      -- the looks and traits alone, for any valid address (pure), else nil
gossip.nearby()       -- the radar, strongest first, <= 20 heard in the last 30 s:
                      -- {{id, name, kind = "badge" | "role", role = "l" (roles only), real = true,
                      --   class = "v" (nil: none), rssi = -48, st = "59qk..." ("" none), boost = 0,
                      --   spam = true (st is live spam a Validator may cure; nil otherwise)}, ...}
gossip.hearing()      -- <= 8 programs being heard: {{a, pct = 0..100,
                      --   state = "hearing" | "exposed" | "incubating", from = "9XQ3TT", rssi}, ...}
function on_gossip(ev) end
                      -- {t = "exposed", a, from, rarity}   the badge caught a program (a record waits)
                      -- {t = "result", n, p, r = "adopted" | "forked" | "seen" | "immune" | "blocked"
                      --   | "missed" | "unknown" | "own" | "bad" | "cool", fork, stake}   the server's word on one
                      -- {t = "result", n, act = "deploy", ok = true, error}   the answer to an action
                      -- {t = "node", seq}   gossip.me() changed
                      -- {t = "event", kind = "rank" | "milestone" | "drop" | "lamps" | "named" | "cured"
                      --   | "mev" | "boss" | "page" | "achievement", text, col = 0x9945FF}   the badge also
                      --   shows it as a notice
```

- **Times are unix seconds** (`clsAt`, `fx.until`, `imm.until`, `epoch.ends`, `synced`,
  and the role side's `since`, `until`), with `left` in seconds where a countdown is
  wanted: Lua's numbers on the badge are 32-bit, so the server's milliseconds wouldn't
  survive. `synced` is the last sync since the badge started, 0 before one; until then the
  badge has no clock and doesn't spread.
- `on_gossip` is a function the badge calls by name when the app defines it (C apps: the
  export `on_gossip`, C_API.md), on the app on screen and on every app service with the
  permission, with each event below and the role side's.
- **Looks are trustworthy, the rest waits for the server.** A program's genome decodes from
  its address alone (the same on the badge, the server, the web and in the Lab); `verified`
  is true once the server's signed record checked on the badge, which happens off the app's
  time a little after the record arrives. Unverified programs still accumulate exposure:
  only the server adopts. Everything `gossip.nearby()` says beyond the id and the signal is
  what other badges claim, unchecked.
- **Acting** (`gossip-lab` only; other apps get `nil, "lab only"`). Each call queues one
  action for the next sync, which the OS asks for soon, and returns `true`, or `nil` and why
  (`"busy"` with 4 waiting, `"gossip is off"`, or the argument that's wrong). The answer
  arrives as `on_gossip {t = "result", act = ...}`; the node changes with `{t = "node"}`.
  Slots are numbered from 1.

```lua
gossip.set_class("v")         -- "r" RPC Node, "v" Validator, "i" Indexer, "b" Builder (once a day)
gossip.activate(a)            -- the program it gossips
gossip.place(a, 2)            -- from the inbox (or another slot) into slot 2
gossip.drop(3)                -- empty slot 3
gossip.deploy()               -- its own root program; deploy(item) with a Deploy Buffer or Genesis Relic
gossip.use(item, {p = a, q = b, trait = 4})   -- an item (docs/GOSSIP.md §9), with what it needs
gossip.name(a, "Turbine Toad")  -- 1-16 printable ASCII, a program of its own with name_nodes adoptions
gossip.public(true)           -- "Show me on the Network board"
gossip.lead(true)             -- the "May become a leader" setting (Settings shows it too)
```

- **Costs**: each read charges the callback about 300 instructions plus one per 4 bytes it
  returns; `genome` and `program` 400 more. An app service keeps `me`, `programs`,
  `program`, `genome`, `nearby` and `hearing` (and the role side's reads), and hears
  `on_gossip` like the app.
- Off (Settings > Gossip, or the Gossip service stopped): `gossip.me().on` is false,
  nothing is heard and the badge's HELLO carries nothing of Gossip's. Emulated badges play
  between tabs of one browser and earn nothing.

The role side (offers, sessions, the boss fight, claims, bids and cures):

A badge whose owner turned on Settings > May become a leader can be offered a **role** by
the server: a second identity it broadcasts for a while (Leader `l`, MEV Bot `m`, Anonymous
Deployer `a`, Byzantine Leader `z`, the boss). The OS's role service runs it: the station on
the air, its HELLO tags, answering claims, the boss's metronome. Apps read it and the Lab
answers the offer or ends it; no app ever holds the role's key or signs for it.

```lua
-- Reading (any app with gossip, app services too)
gossip.role()        -- nil, or one of:
                     --   {offer = {sid, ro, until, left = s to accept by itself | answer = true/false}}
                     --   {sid, ro, id, n, since, until, left, st = {addresses}, boost, on_air,
                     --    on_air_p (the program in its HELLO this moment, when on the air: a Leader's
                     --    block turns every 10 s), boss = {bpm, beats, min, minBeats, spam} (a boss: min = the fewest voters
                     --    that can root the block, minBeats = the first beat it can be rooted at),
                     --    block = {{p, by}, ...} (a Leader), tally}
                     --   {ended = {sid, ro, by, stake, lamps}}   (for 10 minutes after the end)
gossip.boss()        -- the fight heard nearby, or nil: {id, sid, beat, ms, bpm, fork, p, phase, rssi, real,
                     --    minBeats (no root before this beat)}

-- Acting (the Lab only; others get nil, "lab only"): true, or nil and why
gossip.role_answer(yes)        -- answer the offer now (it accepts by itself after 30 s): "no offer", "answered"
gossip.role_end()              -- decline the offer, or end the session: "no role"
gossip.block({{p = a, by = id}, ...})   -- a Leader's block, <= 4 programs and whose bid each was:
                                         -- "not a leader", "at most 4 programs", "bad program or id"
gossip.bids()                  -- a Leader's bids heard, latest per badge (<= 16):
                               -- {{from, name, p, fee, rssi, age}, ...}, or nil when not a Leader
gossip.fork()                  -- the boss's owner throws a fork: "not the boss", "no fight", "too soon", "too late"
gossip.voters()                -- the boss's voters: {voters, conf, fin, p, all, beat, beats, minBeats, fork, phase}, or nil
                               -- (voters, conf, fin: heard in the last 10 s and in two vote periods at least)
gossip.bid(role, a, fee)       -- bid program a to a Leader near you (fee: queue a Priority Fee Action too):
                               -- "not near", "not a leader", "bad program", "busy"
gossip.claim(role, c)          -- "I am here, with this count": the service repeats it until the role
                               -- answers (on_gossip ack), then uploads it: "not near", "too far", "busy"
gossip.cure(id)                -- a Validator removes a nearby badge's spam (on_gossip cured):
                               -- "not a validator", "not near", "curing", "busy"
gossip.vote(votes, lockout)    -- during a fight: your votes in time and lockout (0-32); the service
                               -- sends a summary every ~2 s: "no fight near", "over"
```

- **The offer.** It shows in `gossip.role()` with `left`; the owner is notified ("Gossip: you
  are up as Leader. Open the Lab, or decline there"), and `on_gossip({t = "role", state =
  "offer"})` fires. Without an answer the badge accepts after 30 s. Turning the setting off
  declines it, as does `role_end()`. A badge that is a Collaborator station or a badge
  beacon at that moment declines by itself (one second identity per badge).
- **The session.** `id` is the role's id on the air (a station, MESH.md "Gossip"), `n` its
  name, `st` the program(s) it may gossip (a Leader's block as the server accepted it,
  10 s each in turn), `since`/`until` unix seconds (`left`: seconds to go). It ends at
  `until`, by the owner, under 15 % battery off USB, when a Collaborator takeover or a badge
  beacon takes the second identity, or 90 s without a sync; then `{ended = ...}` shows what
  it paid. A boss whose fight is over stays on the air 90 s after the server ended it, to
  answer the players' claims. A badge keeps one role id from session to session.
- **Claims.** The role's service answers a claim by itself, at most once per claimant per
  5 s, and where the role needs closeness (MEV Bot, Anonymous Deployer) only when it hears
  you at the close range or stronger (Cfg `close`, −55 dBm); `gossip.claim` checks your side
  first ("too far"). The answer is tagged with the role's key; the server checks it.
- **The fight.** `gossip.boss()` moves `beat` and `ms` (to the next beat) forward to the
  moment you read it. `phase`: 0 before the first beat (`ms` counts down to it), 1 the
  fight, 2 the players rooted the block, 3 the boss won. `fork` is the beat that is a fork
  (−1 none): a vote on it resets your lockout. `p` is the share of voters now confirmed
  (0-100); `real` says the server lists that role as live. Vote with SELECT inside ±120 ms
  of a beat and give the service your count and lockout (earned: `lockout ≤ 2^(votes-1)`,
  or the boss ignores the summary); at the end it claims for you with `c` = your votes in
  time, retrying for 90 s (`on_gossip` `ack` when the boss answered). The block can't be
  rooted before beat `minBeats` (the server's `bossMinBeats`, 60 by default): before it, a
  room at 2/3 finalized hasn't won yet. The result is `phase`. While you vote (or your own
  boss fights), Gossip holds its notices and shows the last three after the fight.
- **Events** (`on_gossip(ev)`, the role side): `{t = "role", state = "offer" | "session" |
  "off" | "declined" | "ended", sid, ro, by?, stake?, lamps?}`; `{t = "boss", id, sid, beat,
  ms, bpm, fork, p, phase, rssi, real}` at most four a second (the boss's own owner gets
  `{t = "boss", own = true, sid, phase, voters, fin}` at the end); `{t = "bid", from, name,
  p, fee, rssi}` (a Leader); `{t = "ack", role, c, r, ok}` (`ok = false`: no answer after
  7 tries, a fight's after 90 s); `{t = "cured", peer, p, ok}` (a Validator; `ok = false`: no answer).
- **Services** keep `gossip.role` and `gossip.boss`; the acting calls are the Lab's.
- **Rates.** The service sends at most one `gossip` message a second for the badge (two in a
  fight) and four for a role, whatever the Lab asks.

### Build on the badge, sign on a phone

A player's phone wallet can sign and send a transaction the badge builds. Phantom and
Solflare scan [Solana Pay](https://github.com/solana-foundation/pay/blob/main/typescript/packages/solana-pay/spec/SPEC.md)
*transaction requests*: a QR code holding a link to a server (`solana:https://…`),
which hands the wallet a transaction for its account. The player sees what it does,
approves it, and the wallet signs and sends it. The transaction itself doesn't fit in
the QR code, so your server sits in between:

1. The app builds the transaction's message on the badge. It leaves a placeholder
   where the wallet's address goes (it can't know which wallet will scan) and puts a
   random *reference* address among the accounts.
2. It posts the message to your server with `http`. The server keeps it and answers
   with a `solana:` link.
3. The app shows the link with `ui.qr`, and the player scans it in their wallet app.
4. The wallet asks your server for the transaction, naming its account. The server
   writes that account over the placeholder and a fresh blockhash into the empty slot,
   and hands the transaction over.
5. The wallet signs and sends it. Nothing comes back to the badge: it finds the
   transaction on chain by its reference (`getSignaturesForAddress`).

No key is on the badge or on your server: the only signature is the wallet's.

`lib/solana.lua`, addresses and messages (copy it into your app; on a badge that has
`solana`, see above, `solana.decode` and `solana.encode` do its `decode` and `encode` faster):

```lua
-- lib/solana.lua: Solana addresses and transaction messages, built on the badge.
local M = {}
local B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"

function M.decode(s)                 -- base58 address -> its 32 bytes
  local b = {}                       -- little-endian while it builds
  for i = 1, #s do
    local carry = B58:find(s:sub(i, i), 1, true) - 1
    for j = 1, #b do
      carry = carry + b[j] * 58
      b[j] = carry & 0xFF
      carry = carry >> 8
    end
    while carry > 0 do b[#b + 1] = carry & 0xFF; carry = carry >> 8 end
  end
  for i = 1, #s do                   -- each leading "1" is a zero byte
    if s:sub(i, i) ~= "1" then break end
    b[#b + 1] = 0
  end
  local out = {}
  for i = #b, 1, -1 do out[#out + 1] = string.char(b[i]) end
  return table.concat(out)
end

function M.encode(bytes)             -- 32 bytes -> base58
  local d = {}
  for i = 1, #bytes do
    local carry = bytes:byte(i)
    for j = 1, #d do
      carry = carry + d[j] * 256
      d[j] = carry % 58
      carry = carry // 58
    end
    while carry > 0 do d[#d + 1] = carry % 58; carry = carry // 58 end
  end
  local out = {}
  for i = 1, #bytes do
    if bytes:byte(i) ~= 0 then break end
    out[#out + 1] = "1"
  end
  for i = #d, 1, -1 do out[#out + 1] = B58:sub(d[i] + 1, d[i] + 1) end
  return table.concat(out)
end

function M.hex(s) return ("%02x"):rep(#s):format(s:byte(1, -1)) end

-- 32 random bytes: an address to find a transaction by, never a key pair.
function M.random_address()
  local b = {}
  for i = 1, 32 do b[i] = math.random(0, 255) end
  return string.char(table.unpack(b))
end

-- Solana's compact length: 7 bits a byte, low bits first.
local function len(n)
  local s = ""
  repeat
    local b = n & 0x7F
    n = n >> 7
    s = s .. string.char(n > 0 and b | 0x80 or b)
  until n == 0
  return s
end

M.SYSTEM = ("\0"):rep(32)            -- 11111111111111111111111111111111
M.MEMO = M.decode("MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr")
M.WALLET = ("\1"):rep(32)            -- stands in for the wallet; the server fills it in

-- Send SOL from the wallet. Extra addresses ride along read-only (a reference).
function M.transfer(to, lamports, ...)
  local accounts = {{M.WALLET, w = true}, {to, w = true}}
  for _, a in ipairs({...}) do accounts[#accounts + 1] = {a} end
  return {program = M.SYSTEM, accounts = accounts, data = string.pack("<I4i8", 2, lamports)}
end

function M.memo(text) return {program = M.MEMO, accounts = {}, data = text} end

-- A legacy message whose only signer, and fee payer, is the wallet. Instructions
-- are {program = address, accounts = {{address, w = true}, {address}}, data = bytes},
-- with w on the accounts it changes. The blockhash is left for the server.
function M.message(instructions)
  local keys, at, w = {M.WALLET}, {[M.WALLET] = 0}, {}
  local function add(k)
    if not at[k] then keys[#keys + 1] = k; at[k] = #keys - 1 end
  end
  for _, ix in ipairs(instructions) do
    for _, a in ipairs(ix.accounts) do if a.w then w[a[1]] = true; add(a[1]) end end
  end
  local writable = #keys
  for _, ix in ipairs(instructions) do
    for _, a in ipairs(ix.accounts) do add(a[1]) end
    add(ix.program)
  end
  local out = {string.char(1, 0, #keys - writable), len(#keys), table.concat(keys), ("\0"):rep(32),
               len(#instructions)}
  for _, ix in ipairs(instructions) do
    local idx = {}
    for i, a in ipairs(ix.accounts) do idx[i] = string.char(at[a[1]]) end
    out[#out + 1] = string.char(at[ix.program]) .. len(#idx) .. table.concat(idx) .. len(#ix.data) .. ix.data
  end
  return table.concat(out)
end

return M
```

`main.lua`:

```lua
-- Pay 0.01 SOL on devnet from the player's phone wallet. The badge builds the
-- transaction, your server hands it to the wallet, the wallet signs and sends it.
-- app.json: "permissions": ["http", "rpc"], "hosts": ["pay.example.com"]
local sol = require("lib.solana")
local SERVER = "https://pay.example.com"
local TO = sol.decode("Gh9ZwEmdLJ8DscKNTkTqPbNwLNNBjuSzaG9Vp2KGtKJr")

local info = ui.label(198, 16, "Preparing...", {w = 114})
ui.hint("CANCEL exit")
if not ui.qr then info:text("Needs Solana OS 0.5.3") return end

local ref = sol.random_address()            -- the payment's reference: the badge finds it by this
local REF = sol.encode(ref)
local msg = sol.message({
  sol.transfer(TO, 10000000, ref),          -- 0.01 SOL; at most 2147483647 lamports (32-bit integers)
  sol.memo("Paid from badge " .. (badge.id() or "?")),
})
local qr, poll

local function stop(text)
  timer.cancel(poll)
  poll = nil
  info:text(text)
end

local function watch()                      -- every 3 s: has the reference shown up on chain?
  if sys.millis() > 300000 then             -- the server forgets the transaction after 5 minutes
    return stop("Timed out. Open the app again to retry.")
  end
  rpc.call("getSignaturesForAddress", {REF, {limit = 1, commitment = "confirmed"}},
    {cluster = "devnet"}, function(ok, r)
      if not (ok and poll and r[1]) then return end      -- not yet: ask again next round
      stop(r[1].err and "The payment failed." or "Paid! Scan to see it on the explorer.")
      qr:text("https://explorer.solana.com/tx/" .. r[1].signature .. "?cluster=devnet")
    end)
end

http.post(SERVER .. "/tx", {message = sol.hex(msg)}, function(ok, res)
  local link = ok and res.status == 200 and (json.decode(res.body) or {}).url
  if not link then
    info:text("The server said no: " .. (ok and res.status or res))
    return
  end
  qr = ui.qr(8, 11, 180, link)
  info:text("Pay 0.01 SOL: scan this in Phantom or Solflare.")
  poll = timer.every(3000, watch)
end)
```

Your server (Bun, `bun add bs58`), at `pay.example.com` in this example:

```ts
// pay.example.com: keeps each badge's transaction for 5 minutes and hands it to
// the wallet that scans it, as a Solana Pay transaction request. Bun; bun add bs58.
import bs58 from 'bs58';
import { fromBadge } from './badge'; // the check from "What your server receives" (http)

const APP = 'phone-pay';
const ORIGIN = 'https://pay.example.com';
const RPC = 'https://api.devnet.solana.com';
const PROGRAMS = new Set(['11111111111111111111111111111111', 'MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr']);
const pending = new Map<string, { message: Uint8Array; badge: string; at: number }>();

/** A badge's transaction while it's under 5 minutes old, else undefined. Drops the old ones. */
function live(id: string) {
  for (const [k, p] of pending) if (Date.now() - p.at > 300_000) pending.delete(k);
  return pending.get(id);
}

/** A legacy message with the wallet as its one signer, calling only PROGRAMS; else null. */
function read(m: Uint8Array) {
  let i = 0;
  const byte = () => { if (i >= m.length) throw 0; return m[i++]; };
  const len = () => { // compact length
    let n = 0;
    for (let s = 0; s < 21; s += 7) { const b = byte(); n |= (b & 0x7f) << s; if (b < 0x80) return n; }
    throw 0;
  };
  const skip = () => { const n = len(); i += n; };
  try {
    if (byte() !== 1 || byte() !== 0) return null; // 1 signer, writable (a v0 message starts with 0x80)
    byte();
    const keys: Uint8Array[] = [];
    for (let n = len(); n > 0; n--, i += 32) keys.push(m.subarray(i, i + 32));
    const blockhash = i;
    i += 32;
    for (let n = len(); n > 0; n--) {
      if (!PROGRAMS.has(bs58.encode(keys[byte()]))) return null;
      skip(); // account indexes
      skip(); // data
    }
    return i === m.length && keys.every((k) => k.length === 32) ? { keys, blockhash } : null;
  } catch {
    return null;
  }
}

/** 32 bytes, or null when the node is down or says no (the wallet can ask again). */
async function latestBlockhash(): Promise<Uint8Array | null> {
  try {
    const res = await fetch(RPC, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'getLatestBlockhash', params: [{ commitment: 'confirmed' }] }),
    });
    const hash = bs58.decode(String((await res.json()).result.value.blockhash));
    return hash.length === 32 ? hash : null;
  } catch {
    return null;
  }
}

Bun.serve({
  routes: {
    // The badge (through the badge server's http): keep its message, answer the link to show.
    '/tx': {
      POST: async (req) => {
        const body = await req.text();
        const from = await fromBadge(req, body, APP);
        if (!from) return new Response('who are you?', { status: 403 });
        let hex = '';
        try {
          hex = String(JSON.parse(body).message);
        } catch {}
        const message = Uint8Array.from(Buffer.from(hex, 'hex'));
        if (message.length > 1100 || message.length * 2 !== hex.length || !read(message)) {
          return Response.json({ error: 'not a transaction this server serves' }, { status: 400 });
        }
        live(''); // drops the expired ones
        const id = Buffer.from(crypto.getRandomValues(new Uint8Array(9))).toString('base64url');
        pending.set(id, { message, badge: from.badge, at: Date.now() });
        return Response.json({ url: `solana:${ORIGIN}/tx/${id}` });
      },
    },
    // The wallet (Solana Pay): first who's asking, then the transaction for its account.
    '/tx/:id': {
      GET: (req) =>
        live(req.params.id)
          ? Response.json({ label: 'Badge payment', icon: `${ORIGIN}/icon.png` })
          : Response.json({ error: 'expired' }, { status: 404 }),
      POST: async (req) => {
        let wallet: Uint8Array;
        try {
          wallet = bs58.decode(String((await req.json()).account));
        } catch {
          wallet = new Uint8Array();
        }
        const p = live(req.params.id); // after the body: it may have expired meanwhile
        if (!p) return Response.json({ error: 'expired' }, { status: 404 });
        const m = p.message.slice(); // a copy: the wallet may ask again
        const { keys, blockhash } = read(m)!;
        if (wallet.length !== 32 || keys.some((k) => Buffer.compare(k, wallet) === 0)) {
          return Response.json({ error: 'this wallet cannot pay this' }, { status: 400 });
        }
        keys[0].set(wallet); // the wallet signs and pays the fee
        const hash = await latestBlockhash();
        if (!hash) return Response.json({ error: 'the network is busy: try again' }, { status: 503 });
        m.set(hash, blockhash);
        const tx = new Uint8Array(1 + 64 + m.length); // 1 signature, still empty, then the message
        tx[0] = 1;
        tx.set(m, 65);
        return Response.json({ transaction: Buffer.from(tx).toString('base64'), message: `From badge ${p.badge}` });
      },
    },
  },
});
```

- **Your server signs nothing, but it answers in your name.** The wallet shows the
  player your domain, the label and its own simulation of what the transaction does.
  Anyone can run their own script under your app's id and post any message, so serve
  only what you mean to: here, `read()` lets through only a legacy message whose one
  signer is the wallet and which calls only the System and Memo programs. The
  `X-Badge-*` headers tell you which badge posted it.
- **Devnet first.** The wallet sends the transaction on the network it's set to, so
  switch it to devnet before you scan (Phantom: testnet mode in its developer settings;
  Solflare: its network setting). The app's `rpc` cluster and the server's `RPC` must
  match it.
- **Scan in the wallet app**, with its own scan button. A phone's camera app may not
  open `solana:` links, or may open them in whichever wallet was installed first.
- **Memory:** while it waits, the example holds about 25 KB of Lua heap. Compiled on
  the badge (Run on badge) it peaks at 36 KB (of 6 MB, or 96 KB below 2.0.0); installed
  from the store it runs from flash and needs less.
- The message travels as hex, which keeps the Lua short: messages up to about 1,000
  bytes fit in `http`'s 2 KB. A transfer is limited to 2,147,483,647 lamports, because
  the badge's integers are 32-bit.
- **A payment alone needs no server.** A Solana Pay *transfer request* in the QR code,
  `solana:<recipient>?amount=0.01&reference=<address>&label=Badge`, has the wallet
  build the transfer itself. Watch for the reference in the same way.

## Example

```lua
-- Tap counter that pays 1 !LAMP per 100 taps (needs an award allowance: ask for one when you publish).
local taps = storage.get("taps") or 0
local title = ui.label(0, 60, "Taps", {size=20, w=ui.W, align="center"})
local count = ui.label(0, 100, tostring(taps), {size=32, w=ui.W, align="center", color="green"})
ui.hint("SELECT tap   CANCEL exit")

function on_key(k)
  if k ~= "select" then return false end
  taps = taps + 1
  count:text(tostring(taps))
  led.fill(0x14, 0xF1, 0x95)
  timer.after(80, led.off)
  if taps % 100 == 0 then
    lamps.award(1, "100 taps", function(ok, r)
      if ok and r.awarded > 0 then ui.toast("+1 !LAMP")
      elseif ok and r.needsX then ui.toast("Link X to earn !LAMPs") end
    end)
  end
  return true
end

function on_exit() storage.set("taps", taps) end
```

And one with files: flashcards, one deck per file, loaded when you pick it.

```lua
-- decks/*.txt: "question|answer" per line. lib/cards.lua: function M.parse(line) -> q, a
local cards = require("lib.cards")
local names = {}
for _, f in ipairs(fs.list("decks")) do
  if f:sub(-4) == ".txt" then names[#names + 1] = f:sub(1, -5) end
end
local card = ui.label(10, 80, "", {size=20, w=300, align="center"})
local deck, i, flipped
local menu                                      -- declared first: its callback uses it

menu = ui.list(0, 0, ui.W, ui.H - 16, names, {on_select=function(_, name)
  deck, i, flipped = {}, 1, false
  for line in fs.lines("decks/" .. name .. ".txt") do
    local q, a = cards.parse(line)
    if q then deck[#deck + 1] = {q, a} end
  end
  menu:show(false)
  card:text(deck[1] and deck[1][1] or "Empty deck")
  ui.hint("SELECT flip   RIGHT next   CANCEL decks")
end})
ui.hint("SELECT open   CANCEL exit")

function on_key(k)
  if not deck then return false end            -- on the deck list, CANCEL exits
  if k == "back" then
    deck = nil                                  -- the collector frees the cards
    card:text("")
    menu:show(true)
    ui.hint("SELECT open   CANCEL exit")
  elseif k == "select" and deck[i] then
    flipped = not flipped
    card:text(deck[i][flipped and 2 or 1])
  elseif k == "right" and deck[i + 1] then
    i, flipped = i + 1, false
    card:text(deck[i][1])
  end
  return true
end
```

## Develop and publish

- Write the app in the editor on `/apps/submit`, or drop a folder or .zip on it:
  `main.lua`, `app.json` if you have one (it fills in the form) and the files it
  needs. New text files can be made right there.
- **Try in emulator** runs it on an emulated badge in the page, with every file on the
  emulated card. `print()` and errors show below it. For `mesh` and `ble` apps, open
  `/emulator` and `/emulator?badge=2` in two tabs of one browser: their badges hear,
  find and connect to each other ([Connectivity: Testing each path](CONNECTIVITY.md#testing-each-path)).
- **Run on badge** copies it onto a real badge over USB (desktop Chrome or Edge, Web
  Serial), sending only the files that changed, starts it and shows the badge's
  console, `print()` included. The badge needs a microSD card and Solana OS 0.5.0 or
  later (it checks `info` first; an app with `ble` needs 1.0.0). It replaces an installed app with the
  same id. This is how to test with a gadget or a phone over Bluetooth: the emulator
  has no radio to reach them.
- **Test runs.** Signed in, both start a test run before the app does, so everything
  that goes through the badge server works with every permission the draft asks for:
  `http`, `ws` and `rpc` with its hosts (once your account has X linked), !LAMPs,
  items, profile and quests, before you publish. The emulator joins its demo Wi-Fi for this; a real badge needs Wi-Fi on and
  to be yours or unclaimed. It lasts 12 hours on that badge (Run again renews it with
  the draft as it is then) and acts for you with your real !LAMPs and items: a purchase
  in your own app pays you back, and `lamps.award` gives only what the published app's
  award limit allows (nothing for an app not in the store yet). Only the app's author
  can test an app id that's taken.
- **Publish** needs an account with X linked (at `/profile`); building and trying an
  app don't. It puts the app in the store, once every Lua file compiles with the badge's Lua
  (see "Compiled by the store") and an automatic check has read the code: it looks for
  apps that harm players (taking !LAMPs by deception, asking for passwords, sending
  data away, hiding what they do), usually within a minute. What it flags waits for an
  organiser; until then only you see the app, or the store keeps your previous version.
  `lamps.award` pays nothing until an organiser grants the app an allowance (its award
  cap: the most each player earns from it in a UTC day, counted per X account, since only
  players with X linked earn; 0 to 1,000,000 !LAMPs). **Ask for one when you publish**: with
  the `lamps` permission, the submit page has a "!LAMPs allowance" field for the amount and a
  line on why (what players earn for, how often). A version that asks for an amount other
  than the app's allowance (more, less or none) always waits for an organiser, even when only
  its details changed; approving it grants the amount the organiser decides (what you asked,
  unless they change it, with a note saying why), and turning it down keeps the allowance
  as it was. You get a notice in your badge's Mailbox either way. Leave the field empty to
  keep what the app has. Your apps and the app's page show every app's allowance (none
  too), how much it paid today and what came of your last request; only you and the
  organisers see them. A test run pays only within the published app's allowance, never
  the one you asked for.
  An app with an award cap (`lamps.award`) waits for an organiser after every change to
  its code, files, permissions or hosts, even when the check passes; the store keeps
  your previous version and its cap meanwhile. An account (and an X account) has at
  most 25 apps (an organiser can raise that), with 5 submits a minute and 20 versions a day.
  Each update is a new version; the badge offers it as an update and downloads only
  the files that changed. Once it's published, everyone's badge checks `http`, `ws` and
  `rpc` against the published version's permissions and hosts.

## Console (development)

`lua list`, `lua run <id>`, `lua rm <id> [file]` on the badge console (and in the
emulator page's Console). The web submit page uses `lua run` to test an app in the
emulator. `lua mem` shows the last run's peak heap and instruction count, and whether
its code was the store's (run in place) or compiled on the badge; `lua
install <id>` installs or updates an app from the App Store, as the store app does.
For a C app, `lua aot <id>` says why it runs native code or its WebAssembly: this
build's target and the manifest's, whether the store's key is cached, whether the
signature verifies, `app.aot`'s size against a flash slot, and what runs; `lua aotkey`
fetches the store's key (Wi-Fi on). Neither writes anything but the key
([C_API.md "Security"](C_API.md#security)).
For bridges: `bridge` lists them, `bridge show` opens the code, `bridge code` prints it,
`bridge <code or link>` bridges with the badge that shows it, `bridge end` closes them.

To copy an app over the serial console, `<file>` is `main.lua`, `app.json` or a
bundled path like `lib/quiz.lua` (for a C app, `app.wasm` instead of `main.lua`):

- `lua put <id> <file> begin|<base64>|end` writes a file a line at a time (at most 128
  base64 characters per line), easy to type or script.
- `lua recv <id> <file> <bytes> <sha256>` is the fast way, for tools. The badge answers
  `recv: ready <n>`: send the next `n` characters of the file's base64 (the rest, at the
  end) and wait for `recv: more <n>` before sending more, so the card is never written
  while data is arriving. It ends with `recv: ok <bytes>` once the size and SHA-256
  match, or `recv: error <why>` (5 s without data is a timeout), keeping the old file.
- `lua sums <id>` prints `<sha256> <bytes> <file>` for each file of an installed app,
  then `sums: <n> files` (0 if it isn't installed), so a tool can skip what's already
  there. Run on badge works this way.

These refuse while the app runs (`open home` closes it) and check `app.json` like an
install does. Writing or removing any file of an app deletes its compiled code from
the store (`code.bin`, or a C app's native `app.aot`): from then on it runs from its
source (a C app: `app.wasm`, interpreted) until the store installs it again.
