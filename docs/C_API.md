# Solana OS C apps

From Solana OS 2.3.0 an App Store app can be written in C. The store compiles it to
WebAssembly and to the badge's native code, and the badge runs it in a sandbox next to
the Lua apps. A C app has exactly the [Lua API](LUA_API.md): the same modules, the same
permissions, the same limits (only the CPU limit is its own: "Limits"), reached through
`badge.h`, a header of typed C functions
(`ui_label()`, `timer_every()`, `http_get()`, ...). This file is the C reference and the
contract between the SDK, the store and the badge. The web shows it at `/apps/docs/c`;
the SDK (headers, examples, the build script) is in `sdk/c/` and `tools/cc/` of
[solana-os](https://github.com/solana-foundation/solana-os), and as a zip on the site.

What a function *does* (what a label looks like, when a message arrives, what an error
means) is in the Lua reference; this page says how to call it from C and what is
different. Where they disagree on behaviour, the Lua reference is right and this page
has a bug.

## C or Lua?

Choose C when the app computes a lot per frame: a game that draws every pixel itself, a
simulation, an emulator, a decoder, number crunching. A C app draws a whole frame into
its own memory and puts it on the screen in one call (`ui_canvas_blit`), and once the
store has installed it, its loops run as the badge's own machine code. Choose C, too, to
bring existing C code to the badge (a parser, a codec, game logic).

Choose Lua for everything else: it is shorter to write, the store shows reviewers less
code, and most badge apps spend their time waiting for keys and the network, where C
gains nothing. Each call into the API from C goes through the same Lua host a Lua app
uses, plus a bridge, so an app made mostly of API calls (labels, lists, `http`) is no
faster in C, and can be slower: on a badge the cheapest API call took 4 µs from native
code and 7 µs interpreted, but setting a label's text took 357 µs from native C, 625 µs
interpreted and 181 µs from Lua ("Speed and limits: C against Lua").

C is fast only in its native form, which only a store install brings (see "What runs
where"). Everywhere else (the emulator, Try in emulator, Run on badge) the badge
interprets the WebAssembly, and there C computes at about Lua's speed: faster where it
works on its own memory (copying, drawing into its own frame), slower in API calls.

| | Lua app | C app |
|---|---|---|
| Source | `main.lua` + `.lua` modules | `main.c` + `.c`/`.h` files |
| API | the Lua modules | the same, through `badge.h` |
| Runs as | Lua bytecode, from flash | native code (`app.aot`) in place from flash, or the WebAssembly in WAMR's interpreter |
| Its own memory | Lua tables and strings | a WebAssembly linear memory: `malloc`, arrays, structs (≤ 4 MB) |
| Drawing | `ui.canvas` calls | the same, plus whole RGB565 frames with `ui_canvas_blit` |
| Badges | Solana OS 0.5.0+ | Solana OS 2.3.0+ |

## The package

An app is a folder, as for Lua ([LUA_API.md "The package"](LUA_API.md#the-package)):

```
starfield/
  main.c                compiled with every other .c file
  app.json              the manifest, with "lang": "c"
  lib/maze.c            compiled too
  lib/maze.h            #include "lib/maze.h"
  art/ship.png          data: ui_load("art/ship.png")
  levels/1.json         data: fs_read(), fs_json_push()
```

- `main.c` (≤ 64 KB) and `app.json`, whose `"lang": "c"` makes it a C app. Without
  `lang` the store takes the folder for a Lua app.
- Bundled files under the Lua limits: at most 64 besides `main.c` and `app.json`, each
  ≤ 256 KB, ≤ 1 MB together, with the same path rules (lowercase letters, digits, `_`,
  `-` and `.`, `/` between folders; ≤ 64 characters, ≤ 4 levels; no name starting or
  ending with `.`). `app.wasm` and `app.aot` at the top of the folder are taken: the
  badge keeps an app's code under those names, so no app, C or Lua, may bundle a file
  called that (the store refuses it; `bin/app.wasm` is fine).
- Every bundled `.c` file is compiled with `main.c` into one module; `.h` files are for
  `#include` (paths from the file that includes them, or from the app's folder). Every
  other file is data the app reads at run time with `fs_*`, `ui_load()`, `ui_image()`
  and `ui_anim()`.
- The badge never gets the C source. The store installs `/sdcard/hh/apps/<id>/`:
  `app.json`, `app.wasm` and, when the badge can run it, `app.aot`, the app's `storage`
  in `data.json`, and the data files under `files/`. Source and headers stay in the store.

The manifest is the Lua one ([LUA_API.md](LUA_API.md#the-package)) with `"lang": "c"`:

```json
{
  "id": "starfield-c",
  "name": "Starfield",
  "version": "1.0.0",
  "author": "@you",
  "summary": "Fly through the stars and catch the green ones.",
  "icon": "play",
  "accent": "9945FF",
  "category": "games",
  "lang": "c",
  "permissions": ["led"]
}
```

The store adds what it compiled to the copy a badge installs (`code`, server/API.md "C
apps"):

```json
"code": {
  "wasm": {"sha256": "<64 hex digits>", "size": 4300},
  "aot": {"target": "rv32imac-xip-wamr2.4.5-2", "sha256": "<64 hex digits>", "size": 9892,
          "sig": "<the store's Ed25519 signature, base64>"}
}
```

`aot` is there only when the store has a native form for the badge's current target (see
"Security" for `sig`).

**C apps need Solana OS 2.3.0 or later.** The store offers them only to badges that run
it, and says so in the app's details on older ones, as it does for `ble` apps and 1.0.0.

## How it is compiled

The store builds every version you publish with one pinned toolchain, and refuses a
version that doesn't build, naming the file and line as the compiler does
(`lib/maze.c:12:3: error: use of undeclared identifier 'wall'`). The same flags and
checks are in the SDK, so a local build matches the store's.

1. **C to WebAssembly.** clang from [wasi-sdk](https://github.com/WebAssembly/wasi-sdk)
   24.0 (clang 18.1.2) compiles `main.c` and the other `.c` files and links them with
   wasi-libc into one module, `app.wasm`: a *reactor* (no `main()`; the badge calls
   `_initialize` once, then `on_start`), `-O2`, unused code removed, no debug
   information.
2. **The checks.** The module may import only the functions of `badge_abi.h` and the
   few WASI calls below; must export `on_start`; must have one memory whose maximum is
   at most 4 MB; may use only the WebAssembly features the badge runs (below); and must
   be at most 1.5 MB. A module that fails one is refused, by the store when it builds
   and by the badge when it loads.
3. **WebAssembly to native code.** `wamrc` from [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime)
   2.4.5 compiles `app.wasm` to RISC-V code for the badge's ESP32-C5, `app.aot`: built
   to run in place from flash (no relocations in the code), with every memory access
   bounds-checked. The store keeps it only if it fits a flash slot (at most 1 MB minus
   4 KB, 1 044 480 bytes) and is clean to run in place; otherwise the version is published
   with `app.wasm` alone and badges interpret it. The store signs each `app.aot` it serves.

The flags are data files in `sdk/c/toolchain/`, which the store reads too:

| File | Holds |
|---|---|
| `versions` | `ABI=1`, `WASI_SDK=24.0`, `WAMR=2.4.5`, `AOT_TARGET=rv32imac-xip-wamr2.4.5-2` |
| `cflags` | the compiler's flags, one per line |
| `ldflags` | the linker's (through clang) |
| `wamrcflags` | wamrc's: RV32IMAC, `-O3` with `--size-level=3`, run in place (`--xip`), bounds checks on, no SIMD or reference types, and the helpers the badge's WAMR provides for a CPU without an FPU (software floating point, 64-bit division, count trailing zeros) |
| `imports` | the functions a module may import, `module.name` per line |

`AOT_TARGET` names the native code those flags make. A badge runs an `app.aot` only if
its target is exactly the badge's own; any change to `wamrcflags` or to WAMR gets a new
target, and the store rebuilds the native forms. (`-1` named a helper wrongly, so wamrc
ignored it and code using `printf` or `qsort` wasn't clean to run in place; `-2` fixes
that.) The native code also assumes the signatures of the badge's WASI imports, so
changing one of those needs a new target too.

**WebAssembly features.** The badge's WAMR is a small embedded build: it runs the
WebAssembly MVP plus **bulk memory** (`memory.copy`, `memory.fill`), **sign extension**,
**non-trapping float-to-int** and **mutable globals**, and nothing else: no SIMD, threads
or atomics, reference types, multi-value, exceptions, tail calls, extended constants,
memory64 or several memories. The cflags turn exactly those four on and the rest off,
and the check decodes every instruction of the module, so an `__attribute__((target("simd128")))`
or inline assembly is refused too. wasi-sdk 24 is the newest release whose wasi-libc stays
inside that set: from 25 on, its libc and compiler-rt are built with reference types,
whose longer `call_indirect` encoding this WAMR build refuses.

**Memory.** 32 KB of stack for C's locals, placed first in memory so an overflow traps
instead of corrupting data. The memory starts as small as the app's data and stack need
(64 KB for most of the examples, 128 KB for `starfield` with its 34 KB frame buffer) and
grows as `malloc` asks, up to 4 MB, where `malloc` returns `NULL` (see "Limits" for the
badge's own cap).

### Building locally

`tools/cc/fetch.sh` downloads the pinned toolchain (wasi-sdk and wamrc, each checked
against a SHA-256 in the script) into `tools/cc/.toolchain/` (macOS on Apple Silicon,
where wamrc runs under Rosetta 2, and Linux x86_64). `tools/cc/badge-cc` builds an app
folder the store's way:

```sh
sh tools/cc/fetch.sh                                  # once, ~220 MB
tools/cc/badge-cc sdk/c/examples/hello -o hello.wasm  # compile, link, check
tools/cc/badge-cc myapp -o app.wasm --aot app.aot     # and the native code, to check it builds
tools/cc/badge-cc --check app.wasm --report           # the checks alone; imports, exports, features
```

It prints the compiler's messages with paths from the app's folder, warns about what
the store would refuse in the package (too many files, bad paths, no `"lang": "c"`), and
exits 1 when the build or a check fails. The `app.aot` it makes is only a check that
wamrc accepts the module: a badge takes native code from the store alone (see
"Security").

## What runs where

| Where the app runs | What runs | Limit per callback |
|---|---|---|
| A badge, installed from the App Store (listed or unlisted) | `app.aot`, native, in place from flash | 200 ms of native time |
| A badge, installed from the store without a native form (none for its target, or one that doesn't verify) | `app.wasm` in WAMR's interpreter | 2 000 000 WebAssembly instructions |
| A badge, from Run on badge or the serial console | `app.wasm`, interpreted | 2 000 000 instructions |
| The emulator, Try in emulator | `app.wasm`, interpreted | 2 000 000 instructions |

A store install always writes `app.wasm`, and adds `app.aot` when its target is the
badge's own, it fits a flash slot and the store's signature verifies. At launch the badge
copies the native form into a 1 MB slot of its flash (unless it is there already) and runs
it from there, as it does with a Lua app's compiled code: no native code is ever copied
into RAM. At every launch it checks the signature again and that the slot holds exactly
the signed file, and runs `app.wasm` if anything fails ("Security"). The app behaves the
same either way (the same API, messages and memory cap); only the speed and the CPU limit
differ.

Measured on a prototype badge (ESP32-C5 rev 1.2, Solana OS 2.3.0, USB power, Wi-Fi off):

| | Native (`app.aot`) | Interpreted (`app.wasm`) |
|---|---|---|
| Signature check, every launch | 57 to 76 ms | |
| Copy into a flash slot, first launch only | 34 to 58 ms for 1 to 4 KB, 290 to 380 ms for 46 to 76 KB | |
| Later launches: map the slot and hash it (46 KB) | 3.6 to 3.8 ms | |
| `lua run` until `hello` runs | about 460 ms the first time, about 120 ms after | about 90 ms |
| The starfield example: drawing one frame | 7 ms | 33 ms |
| The starfield example: frames a second | 19.2 to 19.8 (the screen is the limit) | 12.6 to 13.2 |

How fast each form is, workload by workload, against Lua and against the same C built
into the firmware: "Speed and limits: C against Lua" below.

The interpreter ran a plain loop at about 4 500 WebAssembly instructions a millisecond,
and the benchmark's workloads at 3 100 to 6 500, so its 2 000 000 a callback are 0.3 to
0.65 s of work. The emulator runs the same interpreter and counts the same instructions
(far faster than a badge, so its times don't carry over): `lua mem` there prints the most
a callback of your app used ("most wasm instructions in one callback"), which is what a
badge would count.

For an author that means: test the logic and the UI in the emulator or with Run on badge,
interpreted; to see how fast the app really is, install it from the store (an unlisted
app is enough), and check with `lua mem` on the badge that it ran `app.aot (native)`.

## Speed and limits: C against Lua

The benchmark in `sdk/c/bench/` does the same work as a C app and as a Lua app, both
printing their results on the console, and `sdk/c/bench/run.py` drives them and prints
the table, so anyone can run it again (its README says how). Each workload adds up a
checksum, and every column below got the same one. Four columns:

- **C in the firmware**: the benchmark's C (`kernels.c`) compiled into a test build of
  the firmware with GCC 15 at `-O2` and run from a console command, its data in PSRAM
  like an app's. This is "native" to compare against; at the firmware's own `-Os` it ran
  within 5 % of `-O2` on every workload but the 80 × 54 frame (20 % slower).
- **C app, native**: the same C built by the store's toolchain (`app.aot`, the flags in
  `wamrcflags`), run in place from a flash slot.
- **C app, interpreted**: the same app's `app.wasm`.
- **Lua app**: the same work in Lua, written as a careful Lua programmer would (locals,
  plain arrays, methods looked up once).

Measured on HYQ5VS, a prototype badge (ESP32-C5 rev 1.2), on USB power with Wi-Fi off,
running a test build of Solana OS 2.3.0 that also takes a native file over the console,
signed by the server's own signing code with a test key (the released build takes native
code from the store only). A second pass with Wi-Fi joined gave numbers within 3 % of
these for about half the cells and within 10 % for all but two Lua rows that draw on a
canvas (the 320 × 216 frame, 28 %, and the game, 18 %). Times are the work's own: C reads
the microsecond clock, Lua `sys.millis()`, averaged over 3 s of callbacks. Memory is in
binary units (1 MB = 1 048 576 bytes), as the caps are.

### Speed

Lower is better. "Native ÷ firmware" is how much slower the C app's native form is than
the same C in the firmware: the measure of "near native". The last two columns are how
many times the native form is faster than the interpreter and than Lua.

| Workload | C in the firmware | C app, native | C app, interpreted | Lua app | Native ÷ firmware | Interpreted ÷ native | Lua ÷ native |
|---|---|---|---|---|---|---|---|
| Mandelbrot 320 × 240, integers (12 fractional bits), ≤ 32 iterations: ms a frame | 50.4 | 62.5 | 5 283 | 5 307 | 1.24 | 85 | 85 |
| The same with 32-bit floats (Lua's numbers): ms a frame | 1 709 | 2 142 | 5 964 | 6 738 | 1.25 | 2.8 | 3.1 |
| The same with doubles (C only): ms a frame | 2 714 | 3 228 | 8 031 | | 1.19 | 2.5 | |
| Sieve of the primes to 4096: ms | 0.24 | 0.30 | 20.7 | 19.5 | 1.24 | 69 | 65 |
| SHA-256 written in the language: ms per 64 KB | 22.0 | 43.4 | 1 298 | 2 107 | 1.97 | 30 | 49 |
| SHA-256 through the API (`crypto.sha256`, 1 KB a call): ms per 64 KB | | 51.0 | 73.8 | 9.4 | | 1.4 | 0.19 |
| Fill a 1 MB buffer in a loop, 32-bit words: ms per MB | 78.8 | 76.9 | 465 | 646 | 0.98 | 6.0 | 8.4 |
| Copy within it (`memcpy`; Lua `table.move`): ms per MB | 122 | 120 | 128 | 251 | 0.98 | 1.1 | 2.1 |
| A 320 × 216 frame computed per pixel and shown: ms a frame | 10.5 | 27.5 (10.2 drawing, 17.3 the blit) | 338 | 3 573 | 0.97 | 12 | 130 |
| The same on an 80 × 54 canvas at 4×: ms a frame | 0.25 | 1.70 (0.60 drawing, 1.1 the blit) | 22.4 | 211 | 2.41 | 13 | 124 |
| Set a label's text (16 labels): µs a call | | 357 | 625 | 181 | | 1.8 | 0.51 |
| Replace a list's 20 rows: µs a call | | 2 775 | 3 746 | 1 220 | | 1.35 | 0.44 |
| A game tick: 256 particles moved, bounced and drawn 2 × 2 on a 160 × 108 canvas at 2×: ms a tick | 2.42 | 7.92 (2.65 the tick, 5.3 the blit) | 30.7 | 20.7 | 1.09 | 3.9 | 2.6 |

C draws into its own RGB565 buffer and shows it with one `ui_canvas_blit`; Lua draws
with `c:pixel` (the frames) and `c:clear` plus a `c:rect` a particle (the game), the best
the canvas offers. The firmware column computes the same pixels into PSRAM without showing
them, so "native ÷ firmware" compares the drawing alone. What reached the screen, in
frames a second (a tick is every 30 ms, so 33 at most):

| | C app, native | C app, interpreted | Lua app |
|---|---|---|---|
| 320 × 216 frame per pixel | 12.7 | 2.0 | about 0.3 |
| 80 × 54 frame at 4× | 21.6 | 15.7 | 2.3 |
| The game tick | 19.7 | 13.5 | 15.7 |

A Lua canvas pixel cost 37 to 52 µs (the two passes), most of it the canvas call rather
than Lua's own 20-odd instructions: that is why Lua's frames are slow, and why its game,
a few hundred calls a tick, keeps up.

### How much fits in one callback

The budgets differ (Lua: 200 000 VM instructions; interpreted C: 2 000 000 WebAssembly
instructions; native C: 200 ms of native time, not counting time inside the badge's API;
any C callback: 3 s), so the same work fits differently. Measured by growing the work 6 %
a callback until the badge stopped the app (the last size that ran, and its time):

| Work in one callback | C app, native | C app, interpreted | Lua app |
|---|---|---|---|
| Mandelbrot rows (integers) | 760 rows, 195 ms | 14 rows, 296 ms | 3 rows, 58 ms |
| SHA-256 blocks of 64 bytes | 4 990 blocks, 202 ms | 231 blocks, 287 ms | 36 blocks, 72 ms |
| Label updates | 2 169, 0.70 s | 4 119, 2.5 s | 10 206, 1.8 s |
| In one 30 ms tick at the measured speed: Mandelbrot rows | 115 | 1.4 | 1.4 |

Each stopped at its own budget: "Native code ran over 200 ms in one callback", "Over
2000000 wasm instructions in one callback", and Lua's "main.lua:21: Script took too long".
A label update costs native C about a quarter of its time in native time (the rest is
the API's, which doesn't count) and Lua about 20 instructions, so one callback can hold the
screen for 0.7 s (native) to 2.5 s (interpreted) before its budget ends it: spread UI work
over ticks.

### Near native

The native form ran at 0.97 to 1.25 times the firmware's time on eight of the ten workloads
the firmware column has, and at 1.97 and 2.41 times on two. Why, workload by workload, and
what other `wamrc` settings did in the same test build (native time ÷ firmware time; the
store's settings are unchanged):

| Workload | Store's settings | Plus `+unaligned-scalar-mem` | Bounds checks off (not safe) | Why |
|---|---|---|---|---|
| Mandelbrot, integers | 1.24 | 1.22 | 1.23 | No memory traffic and no helpers in the loop: what is left is how LLVM (from WebAssembly) and GCC (from C) laid the loop out |
| Mandelbrot, floats / doubles | 1.25 / 1.19 | 1.30 / 1.16 | 1.27 / 1.19 | Every float operation is a software helper. Run in place, native code calls each through a table to WAMR's wrapper, which calls the helper: two calls where the firmware makes one |
| Sieve | 1.24 | 1.25 | 0.81 | A bounds check on every byte it stores |
| SHA-256 | 1.97 | 1.24 | 1.78 (both: 1.04) | Its 64-word schedule lives in the app's memory, and wamrc can't assume a 32-bit access is aligned, so each one becomes four byte loads or stores |
| Fill, copy, 320 × 216 frame | 0.97 to 0.98 | 0.96 to 1.01 | 0.97 to 0.98 | PSRAM's speed is the limit for both |
| 80 × 54 frame | 2.41 | 2.33 | 2.16 (both: 1.95) | The frame stays in the cache, so the loop's code counts: byte stores, bounds checks, and the memory base added to every address |
| Game tick | 1.09 | 1.00 | 1.03 | Byte stores into the frame |

`+unaligned-scalar-mem` (in `--cpu-features`) lets wamrc use 32-bit loads and stores where
WebAssembly doesn't promise alignment: native files were 25 % smaller (89 → 67 KB for
`bench-c`), SHA-256 1.6 times faster, a label update 18 % faster (`badge.h` runs in the
app too), nothing slower beyond noise. On HYQ5VS a test app wrote and read 32-bit words at
every offset of 12 KB of its memory (PSRAM) and all came back right. **On a production
badge (4YGWAV, ESP32-C5 rev 1.0) it was the other way round**: the Game Boy core's CPU loop
took 46.7 ms a frame with it against 27.1 ms without (1.7 times slower; "Heavy apps and
emulators"), so the store's flags stay without it. Turning bounds checks off would save
about 30 % in an emulator's loop (18.9 ms there) but would let a bad pointer restart the
badge, so they stay on.

### Limits

What each limit allowed, measured with the benchmark apps (same badge and build; the
rules are in "Limits" above and in [LUA_API.md "Limits"](LUA_API.md#limits)):

| | Lua app | C app |
|---|---|---|
| Memory it really got | 5.50 MB of 64 KB strings (5.54 MB counted), then "Out of memory: The app needs more than 6 MB." | 3.94 MB of `malloc` (63 blocks of 64 KB), then `malloc` returned `NULL` at the linear memory's 4 MB maximum, with 4.05 MB counted natively, 4.10 MB interpreted; the app ran on |
| The badge's SRAM, app open and idle with 17 labels | 17.0 KB (of the 121.5 KB malloc can still get at the launcher) | 17.3 KB native, 17.0 KB interpreted |
| PSRAM, the same | 48 KB | 122 KB native, 165 KB interpreted (WAMR's instance, the linear memory, and interpreted a copy of the module) |
| Launch, `lua run` until the app's first line (17 labels built) | 0.26 to 0.27 s | 0.21 to 0.27 s either form; 0.70 s natively the first time, copying its 89 KB into a flash slot |
| Recursion in one callback (a small function) | 26 127 levels; 32 658 ran out of instructions ("Script took too long") | interpreted 378 levels, 472 stopped with "The app ran out of stack (recursion too deep) (wasm operand stack overflow)"; native 303, 378 stopped with "The app ran out of stack (recursion or callbacks nested too deeply) (native stack overflow)" |
| UI objects | 128, then "too many UI objects (128)" | the same |
| Work in one callback | 200 000 instructions: 58 to 72 ms of computing, longer in API calls ("How much fits") | 2 000 000 instructions interpreted (0.3 s of computing), 200 ms native, 3 s whatever it does |

C's linear memory stops at 4 MB, under the 6 MB cap, so a Lua app can hold more. Native C
recurses least: its calls run on the badge's 12 KB task stack.

### What it means

Native C is near native: within 25 % of C built into the firmware on most work, never
more than 2.4 times. Against Lua it computes 50 to 130 times faster on integers and
pixels, 3 times faster with floats (which no FPU makes slow in both: floats cost native C
34 times its integer time, Lua 1.3 times), and twice as fast at copying memory; through
the API it is no faster, and setting labels or list rows took it twice Lua's time. So
choose C for apps that compute per pixel or per sample, and make few API calls in their
loops; choose Lua for apps that are mostly UI and network. And interpreted C (the
emulator, Run on badge, a badge without the store's native file) runs at about Lua's
speed.

## Heavy apps and emulators

Can a C app emulate a Game Boy, a Game Boy Color, a Game Boy Advance? The `gameboy` rows
were measured again on ESP-IDF master (Solana OS 2.3.0, production badge 4ZT2BX, the store's
signed native code run in place: the signature check takes 58 ms at launch, the first copy
into a flash slot 665 ms) and changed little; the rest is as measured on a
production badge (4YGWAV: ESP32-C5 rev 1.0, 240 MHz, no FPU, 8 MB quad PSRAM at 80 MHz;
USB power, Wi-Fi off), Solana OS 2.3.0 test builds on ESP-IDF 6.0.1, before the move to
master (2.2.0), 2026-11-15; `ui_frame` on master is measured under "`ui_frame`" below:

- **Game Boy and Game Boy Color: yes, for most games, as a store install (native code).**
  The `gameboy` example (Walnut-CGB, MIT) runs real homebrew at full speed: 56 to 60
  emulated frames a second (the machine's 59.7) and 45 to 56 of them on the screen. A game
  that keeps the CPU busy every frame runs at 23 to 35 fps, and one in the Color's double
  speed at 14 to 18: those don't reach full speed.
- **Interpreted (Try in emulator, Run on badge, a badge without the native form): no.** One
  Game Boy frame took over 3 s of the interpreter, the ceiling of a callback: under 0.3 fps.
- **Game Boy Advance: no.** The only permissively licensed core at hand (SkyEmu's, MIT, an
  accurate one) ran 0.7 fps with drawing as a C app, and 2 to 2.6 fps even built into the
  firmware with no sandbox at all. A playable GBA needs about 10 to 25 times that.

### What runs where, frame by frame

The same core and the same glue code (Walnut-CGB 1.2.4, its whole ROM in memory) built
three ways: into a test firmware with GCC 15 at `-O2` (the chip with no sandbox), as a C
app's native form (the store's `wamrcflags`), and interpreted. Milliseconds per emulated
frame; "shown" adds the call that puts it on the screen:

| ROM (all permissively licensed or made for this) | Firmware: CPU only | Firmware: drawn | C app native: CPU only | C app native: drawn | + shown, no copy | + shown, copied | + shown at 1.5x | Interpreted |
|---|---|---|---|---|---|---|---|---|
| Busy loop, Game Boy (never halts) | 8.75 | 15.5 | 27.9 | 39.7 | 1.1 | 6.6 | 11.6 | over 3 000 |
| Busy loop, Game Boy Color double speed | 18.2 | 26.8 | 55.0 | 67.1 | 1.0 | 6.5 | 11.8 | |
| *Libbet and the Magic Floor* (a game, zlib; runs in Color mode) | 1.21 | 9.4 | 4.9 | 15.4 | 1.4 | 6.9 | 17.0 | |
| *cgb-acid2* (a still picture, MIT) | 0.82 | 10.3 | 3.5 | 15.8 | 1.0 | 6.6 | 16.1 | |

What reached the screen, frames a second of wall-clock time (60 is full speed):

| ROM | C app native, every frame drawn and shown | Every other frame shown | The `gameboy` example (adaptive: draws the last frame of each tick) |
|---|---|---|---|
| Busy loop, Game Boy | 23.3 | 27.8 | |
| Busy loop, Color double speed | 14.2 | 15.9 | |
| Libbet | 51.9 | 56.6 | 56 to 60 emulated, 45 to 56 shown (1x); 22 to 28 shown at 1.5x. On ESP-IDF master (2.3.0, a production badge, installed from the store, run in place): 53.5 to 60.1 emulated, 48 to 59 shown |
| cgb-acid2 | 54.2 | 58.1 | On ESP-IDF master: 59.6 to 59.9 emulated, 48 to 49.5 shown |
| A 1 MB MBC5 ROM switching banks every few instructions | | | 21 emulated, 7 shown |

So a C app's native code is 2.5 to 4 times the firmware's for an emulator's CPU loop and
1.3 to 1.6 times for its drawing, much further from native than the benchmark's 1.0 to 1.25
("Speed and limits"). Measured, with the same app built with other `wamrc` settings (busy
Game Boy, CPU only / Libbet, drawn, ms a frame):

| | Busy, CPU | Libbet, drawn |
|---|---|---|
| The store's settings | 27.1 | 16.2 |
| `--cpu-features=+unaligned-scalar-mem` | 46.7 | (5.45 CPU only) |
| `--size-level=1` / `0` | 27.2 / 27.5 | 16.7 / 16.6 |
| `--disable-llvm-jump-tables` | byte for byte the same file: run in place, the code has none anyway | |
| `--bounds-checks=0` (not safe, measured only) | 18.9 | 12.0 |

- **Bounds checks** are about 30 % of the native form's time in an emulator, whose every
  step reads and writes the emulated machine's memory. They stay: without them a bad
  pointer restarts the badge.
- **No jump tables.** Code run in place may not point into its own read-only data (no
  relocations in the code), so `wamrc --xip` compiles a `switch` as a tree of comparisons,
  not a table. An emulator's opcode switch is the worst case. The firmware's core built
  with `-fno-jump-tables` was up to 19 % slower (busy loop, CPU only 10.4 ms against 8.75),
  so this is part of the gap, not most of it.
- **The rest** is WebAssembly itself: every function pointer call (the core reads the ROM
  through one, for every opcode) is a checked indirect call, values are 32-bit locals
  LLVM can't keep in registers across the checks, and helpers go through a table.
- **Not the memory**: the core's state in SRAM instead of PSRAM was no faster in the
  firmware (16.6 ms drawn against 15.5): the cache holds it.
- **`+unaligned-scalar-mem`** made the production badge 1.7 times slower (rev 1.0 handles
  misaligned accesses slowly), where the prototype (rev 1.2) was faster: it stays off.

### Game Boy Advance

| SkyEmu's GBA core, ms a frame | Firmware: CPU only | Firmware: drawn | C app native: CPU only | C app native: drawn |
|---|---|---|---|---|
| Busy loop (made for this: 128 sprites, a tiled background, never halts) | 126 | 506 | 448 | 1 400 |
| *240p Test Suite* menu (GPL-2.0, measured only) / a game that waits for VBlank | 16.0 | 386 to 396 | 233 | |

The GBA's CPU runs at 16.8 MHz and its picture is 240 x 160 with four layers and 128
sprites; this core emulates both a cycle at a time. Even in the firmware it reaches 2 to 8
fps; in the sandbox 3 to 4 times less, and a drawn frame (1.4 s) is longer than a callback
may run (the test lifted the limits). What a GBA would need:

- **A core written for speed on this chip**: an ARM interpreter that runs a block of
  instructions per dispatch and a scanline renderer, like those that run full speed on
  similar CPUs elsewhere. No permissively licensed one exists today (the fast ones, gpSP
  and mGBA, are GPL and MPL).
- **A JIT** (ARM to RISC-V): the fast GBA emulators on ESP32 chips use one. A C app can't:
  apps get no executable memory, and their native code comes only signed from the store.
  It would have to be firmware: an emulator built into Solana OS as a service, which
  is a different product (the OS vouching for a large, complex core) and still tight on a
  single 240 MHz core whose SRAM (about 90 KB free while an app runs) can't hold the GBA's
  288 KB of work RAM or a code cache. esp-gbx, the most complete emulator for the
  ESP32-C5 (Apache-2.0), builds the GBA for the ESP32-P4 and S31 only and the GB/GBC for
  the C5.
- **Memory**: a GBA ROM is 4 to 32 MB; a C app's memory stops at 4 MB, so ROM pages come
  from the card (`files_read`), 16 KB at a time.

Not worth doing on this badge: even a firmware core with a JIT would be at the edge, and
the sandbox's guarantees (no executable memory, signed code only) are what C apps rest on.

### Writing one

The `gameboy` example (`sdk/c/examples/gameboy/`) is the pattern for any heavy app:

1. **The owner's files** (`"permissions": ["files"]`): the app lists its folder
   (`files_list`), and reads the game with `files_read` straight into its own memory: whole
   when it fits (a 1 MB ROM took 0.92 s, 256 KB a tick, with a progress bar), otherwise
   16 KB banks into a cache as big as memory allows, read as the game asks for them. A
   cache miss costs a 16 KB read from the card and 4 100 host steps, so a game that switches among more
   banks than the cache holds can hit the 200 000 host steps of a callback (about 48 misses):
   make the cache big (the linear memory may grow to 4 MB). Saves go to `save/<game>.sav`
   (`files_write`), checked every 30 s and at exit.
2. **Pacing**: `sys_tick(16)`; each tick adds `ms` to an account and emulates whole
   16.74 ms frames while there is time for one (at most 3), drawing only the last. When the
   app keeps up, every frame is shown; when it can't, it shows fewer but the game keeps its
   speed.
3. **The screen**: render into two 64-byte-aligned big-endian buffers in turn, and
   `ui_frame` each one unscaled (160 x 144 at (80, 37)): no copy, 1 to 1.4 ms. 1.5x
   (240 x 216, the app area's height) looks better and costs 11 to 17 ms a shown frame.
4. **Keys**: the badge has a d-pad, SELECT, CANCEL and HOME, and HOME always leaves. The
   example reads `sys_keys()` every tick: SELECT is A, CANCEL is B, both at once tapped is
   START, and held for a second opens its menu (the Game Boy's SELECT, the screen size,
   save, quit). `on_key` returns 1 so CANCEL doesn't close the app.
5. **No sound**: the badge has no speaker. The core keeps the sound registers games read
   and synthesises nothing (`ENABLE_SOUND 0`).
6. **Memory and launch**: the example takes 86 KB of WebAssembly, 181 KB native (a flash
   slot holds 1 MB), 0.4 to 1.4 MB of linear memory with the cache, and left 91 KB of SRAM
   for malloc and 7.4 MB of PSRAM free while running. The first launch copies the native
   code into a flash slot (0.6 s); later ones check the signature (58 ms).

Test it interpreted only for the menus (it is far too slow there), and install it from the
store, unlisted is enough, to play. Commercial games are copyrighted: an owner uses the
example with games they own. The SDK ships no game and no BIOS or boot ROM.

## Lifecycle

The badge loads the module, calls `_initialize` (wasi-libc's set-up: static
constructors), then these exports when they are defined. Only `on_start` is required;
`badge_abi.h` declares them all with their export names, so defining one is enough.

```c
void on_start(void);                      // once, at launch: build the UI here
int32_t on_key(int32_t key, int32_t repeat); // KEY_UP ... KEY_BACK; return 1 if handled
void on_tick(int32_t ms);                 // every 30 ms; ms since the last tick
void on_mesh(void);                       // a message from this app on another badge ("mesh")
int32_t on_meet(void);                    // another badge asks for a meet ("mesh")
void on_exit(void);                       // the app is closing (HOME, CANCEL, sys_exit)
void on_service(void);                    // the app's own service sent a message ("background", 2.3.0)
void on_mail(void);                       // a notification for this app ("mail", 2.3.0)
```

A C app with the `background` permission is also its own service: the badge starts a
second, separate instance of the same module at `on_service_start` instead of
`on_start` (see ["`service`"](#service-permission-background-solana-os-230-lua)).

They are the Lua globals of the same names ([LUA_API.md "Lifecycle"](LUA_API.md#lifecycle)),
with the same timing and rules:

- `on_key(key, repeat)`: `key` is `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`,
  `KEY_SELECT` or `KEY_BACK` (CANCEL), `repeat` 1 for an auto-repeat. Return 0 for
  `KEY_BACK` to exit. HOME always returns to the launcher and never reaches the app. A
  focused list, `ui_confirm` or `ui_input` takes the keys it uses first.
- `on_tick(ms)`: move things by `ms`, never by counting ticks. For several keys at once
  read `sys_keys()` here (`KEY_HELD(sys_keys(), KEY_LEFT)`).
- `on_mesh()`: the sender's badge id and the message are its arguments on the stack:
  `badge_arg(1)` and `badge_arg(2)` (see "Values on the stack").
- `on_meet()`: `badge_arg(1)` is who asks, `badge_arg(2)` their data. To sign, return
  `mesh_meet_accept("{\"caught\":1}")`; return 0 to refuse.
- `on_exit()`: runs when the app closes normally (HOME, CANCEL, `sys_exit()`). It is
  not called after the app stopped on an error (its memory can't be trusted then) or
  after `exit()`, which ends the app at once.

The screen below the status bar is `UI_W` × `UI_H` (320 × 218) pixels, (0, 0) at the
top-left. A runtime error (a trap: an out-of-bounds access, a division by zero,
`unreachable`, `abort()`), `badge_fail()`, a mistake in using the ABI, running out of
memory or out of time stops the app with the error screen ("When an app stops"); CANCEL
returns to the launcher. An API call the badge refuses doesn't stop it: the wrapper
returns `BADGE_ERR` and `badge_error()` says why.

Callbacks never run at the same time: one export or callback at a time, each to the end.
There are no threads.

### Limits

Everything in [LUA_API.md "Limits"](LUA_API.md#limits) holds for a C app too (UI objects,
label text, images, timers, storage, files, reads, mesh, Bluetooth, HTTP, RPC, WebSocket,
crypto), plus:

| | |
|---|---|
| Memory | One cap, the Lua app's: 6 MB in PSRAM. It covers the app's linear memory (≤ 4 MB: its data, its 32 KB stack and `malloc`), WAMR's copy of the module (interpreted; native code stays in flash), its instance and its own 32 KB stack, and the Lua state behind the API (labels' text, loaded images, tables you get). `sys_memory()` reports the sum. When the cap would be passed by growing the linear memory, the growth is refused: `malloc` returns `NULL` and the app runs on. Anything else past it stops the app with "Out of memory". Measured in the emulator: the SDK's examples run with 121 to 221 KB in all. On a badge the benchmark got 3.94 MB of `malloc` before the 4 MB maximum ("Speed and limits: C against Lua") |
| The badge's memory | Measured on a badge (Solana OS 2.3.0's C apps on ESP-IDF 6.0.1, Wi-Fi off): 125.4 KB of SRAM free for `malloc` and 8.36 MB of PSRAM at the launcher; with `hello` running, 110.2 KB and 8.24 MB native, 112.0 KB and 8.22 MB interpreted. Twenty native launches and exits left the free heap (157 KB) and its largest block (96 KB) as they were |
| CPU, interpreted | 2 000 000 WebAssembly instructions per callback (about 0.45 s of a plain loop on a badge, "What runs where"). A callback's nested calls into the app share it (a list's `on_select` called from inside `on_key`, a C function called through `sos_call`), and so do `_initialize` and `on_start` at launch. `memset`, `memcpy` and `memmove` (WebAssembly's `memory.fill`, `memory.copy` and `memory.init`) cost one instruction per 4 bytes, so a callback moves at most 8 MB with them: on a badge `memset` writes about 12 KB a millisecond and `memmove` about 8 KB, while the interpreter runs 2 200 to 4 500 plain instructions, so 4 bytes take about an instruction's time. Past it: "App took too long". Measured: stuck apps stopped at exactly 2 000 000, with no watchdog warning; a loop of 3.75 MB `memset`s 0.96 s after launch and of `memmove`s 1.27 s (both counting 0.3 s to grow the memory first), of 64 KB `memory.init`s 1.0 s |
| CPU, native | 200 ms of native time per callback, shared the same way. The app's own code counts, and so do `memset`, `memcpy` and `memmove` (the badge runs them in 4 KB pieces and stops between two); time inside the badge's API functions and while the linear memory grows doesn't (the API's work is bounded by the host's steps, below). Past it: "App took too long". Measured: a plain loop stopped after 204 to 210 ms of native time, a loop calling an API function after 204 to 210 ms, a spinning callback nested inside an API call after 203 to 207 ms, a loop spending its time in the software floating-point helpers after 207 to 295 ms, a loop of 3.75 MB `memset`s or `memmove`s after 200 to 201 ms, of `memory.init`s after 207 ms |
| Host work | What the API functions do for the app counts against the host's 200 000 steps a callback, as for a Lua app, in both forms: reading files, `crypto`, drawing on a canvas, `ui_canvas_blit`, and the calls themselves. Every call into the badge costs 1 step and `sos_call` 4 more (a Lua call's own instructions); what one copies costs 1 step per 4 bytes (strings in and out: `sos_push_str`, `sos_to_str`, the keys of `sos_get` and `sos_set`); console output (`printf` and `badge_print()`, or Lua's `print` reached through the API) 32 steps a byte, since the console is a 115 200-baud serial line that writes about 11 bytes a millisecond, so a callback prints about 6 KB; `arc4random()` and `getentropy()` 6 steps a byte. Past it: "App took too long". Measured on a badge: a loop pushing 2.5 MB strings stopped at the first one (nothing copied), loops of 1 KB `fd_write`s or `sos_log`s after 6 KB of output, a 64 KB `print` before any of it, and the SDK's API probe (92 checks and their output in one `on_start`) used 119 886 steps |
| One callback, whatever it does | 3 s of wall-clock time, in both forms. The limits above stop an app long before; this is the backstop for anything they miss. Past it: "App took too long". Measured with those limits raised in a test build: a plain native loop, a native `memset` loop, native and interpreted loops calling the badge, and an interpreted `memset` loop each stopped 3.0 to 3.3 s after the callback began |
| Stack | 32 KB of the linear memory for C's locals. Interpreted, the function calls themselves use WAMR's 32 KB stack (in the app's heap); native, the badge's 12 KB task stack, shared with the badge's code that calls the app, so deep recursion stops sooner there. Measured with `hello` running: 7.2 KB of the task stack left native, 7.0 KB interpreted; at the deepest nesting of callbacks tested, 2.4 KB native, 3.8 KB interpreted. A small recursive function went 303 levels deep native and 378 interpreted, where Lua went past 26 000 |
| Module | `app.wasm` ≤ 1.5 MB. The native form was 2.2 to 3.0 times the size of the module for the SDK's examples (`hello`: 21 KB of WebAssembly, 46 KB native); it must fit a 1 MB flash slot, or the store publishes `app.wasm` alone |
| The stack frame | 64 values per frame (see "The raw ABI"), 1024 kept values (`sos_ref`) at a time |
| Callbacks waiting | 64 in `badge.h` (timers, server calls, lists, sockets, ...); past that a call fails with "badge.h: more than 64 callbacks waiting" |
| Numbers | C has 64-bit integers and doubles in its own memory; values crossing into the API are 32-bit integers and 32-bit floats, as in Lua. The badge's CPU has no floating-point unit: floats work, slowly (a Mandelbrot frame took native C 34 times as long with floats as with integers, "Speed and limits: C against Lua"); keep hot loops in integers |

### When an app stops

The error screen's title and message, as the serial console also prints them
(`[lua:<id>] <title>: <message>`):

| What happened | Title | Message |
|---|---|---|
| A load or store outside the linear memory (a bad pointer, C's stack overflowing its 32 KB) | App error | The app read or wrote outside its memory (out of bounds memory access) |
| Integer division by zero | App error | The app divided by zero (integer divide by zero) |
| `INT_MIN / -1` | App error | Integer overflow in a division (INT_MIN / -1) (integer overflow) |
| A float too big for the integer it is converted to | App error | A float too big for an integer (invalid conversion to integer) |
| Recursion too deep for WAMR's stack (interpreted) | App error | The app ran out of stack (recursion too deep) (wasm operand stack overflow) |
| The badge task's stack running out (callbacks nested through `sos_call`; native code's own recursion) | App error | The app ran out of stack (recursion or callbacks nested too deeply) (native stack overflow) |
| A function pointer that points nowhere (0, garbage) | App error | The app called a bad function pointer (uninitialized element), or (undefined element) past the end of the function table |
| A function pointer of the wrong type | App error | The app called a function pointer of the wrong type (indirect call type mismatch) |
| `abort()`, `__builtin_trap()`, a failed `assert` | App error | The app crashed (unreachable code: abort(), __builtin_trap()) (unreachable) |
| `badge_fail("...")` | App error | your text ("The app stopped" for an empty one) |
| An index outside the frame | App error | sos_to_int: index 5 is outside the frame (1 values) |
| A pointer or length outside the linear memory, given to the badge | App error | sos_push_str: the pointer 0x7ffffff0 (100 bytes) is outside the app's memory |
| More than 64 values in a frame | App error | sos_push_int: more than 64 values in a frame |
| A value given to `sos_push_fn` that isn't a C callback | App error | sos_push_fn: no function there (function 12345) |
| A kept value that isn't one | App error | sos_unref: 77 is not a kept value |
| An import the badge doesn't have | App error | The app imports env.launch_rockets, which the badge doesn't have |
| Built for a newer ABI | App error | This app needs a newer Solana OS (its C ABI is 2, this badge's 1). Update in Settings > Software update. |
| No `on_start`, or an export of the wrong type | App error | The app has no on_start / The app's on_key has the wrong type (see badge_abi.h) |
| A start function (the SDK never makes one; WAMR would run it with no limit while it loads the module) | App error | The app has a start function, which the badge doesn't run (rebuild it with the SDK) |
| An export WAMR would run by itself at load, `__wasm_call_ctors` or `__post_instantiate` (the SDK's constructors run from `_initialize`) | App error | The app exports __wasm_call_ctors, which the badge doesn't run (rebuild it with the SDK) |
| Native code put on the card as `app.wasm` | App error | app.wasm is native code, not WebAssembly: only the store's signed app.aot runs natively |
| An `app.wasm` that isn't WebAssembly at all | App error | app.wasm isn't WebAssembly |
| The 6 MB cap | Out of memory | The app needs more than 6 MB. |
| The badge's own memory running short first | Out of memory | The badge is low on memory. |
| Too many interpreted instructions (bulk memory operations included) | App took too long | Over 2000000 wasm instructions in one callback |
| Too much native time | App took too long | Native code ran over 200 ms in one callback |
| Too much host work: API calls, copies, console output | App took too long | Over 200000 host steps in one callback (copies, output, fs, crypto, drawing) |
| A callback past 3 s, whatever it was doing | App took too long | The app ran over 3 s in one callback |
| The native form can't run (no store key on the badge yet, a signature that doesn't verify, a changed `app.aot`) and there is no `app.wasm` | App error | The app can't start: the store's signature doesn't verify, and it has no app.wasm. Install it again from the App Store |

On a badge (Solana OS 2.3.0), every trap above, a bad pointer, an index outside the frame,
the 6 MB cap, an unknown import, a newer ABI, a start function, native code as `app.wasm`
and each kind of stuck code in "Limits" ended on its error screen with no restart, in both
forms. When the native form can't run and `app.wasm` is
there, nothing shows: the app runs interpreted, and the console logs why
(`native code not used (...)`).

The messages name the function (`sos_to_int`, `sos_push_str`, ...) of the raw ABI that
`badge.h` called for you. When the linear memory can't grow (the 4 MB maximum, or the
6 MB cap) nothing stops: `memory.grow` answers -1 and `malloc` returns `NULL`. `exit()`
isn't an error: the app just closes.

## Standard library

wasi-libc, the C library of wasi-sdk (musl-based), is linked into the app, so ordinary C
works: `string.h`, `stdlib.h` (`malloc`, `qsort`, `strtol`, `rand`), `stdio.h`
formatting (`snprintf`, `sscanf`), `math.h`, `ctype.h`, `time.h`, 64-bit arithmetic. The
badge provides only these WASI calls underneath it:

| Call | On the badge |
|---|---|
| `fd_write` | `stdout` and `stderr` go to the serial console a line at a time, like Lua's `print` (`[<app id>] ...`, control characters as `?`); the console shows them, Run on badge and Try in emulator show them below the app. One call takes at most 4 buffers and 1 KB (stdio writes the rest with the next), and output costs host steps ("Limits") |
| `clock_time_get` | `time()` and `clock_gettime(CLOCK_REALTIME)` are the server's clock. Until the badge has it, `clock_gettime(CLOCK_REALTIME)` fails and `time()` returns 0, as `sys_time()` does; the monotonic clock counts from boot |
| `random_get` | `getentropy()`, `arc4random()`: the badge's hardware random numbers, at most 4 KB a call (more fails with `EINVAL`; `getentropy()` asks for 256 bytes at most) |
| `proc_exit` | `exit()` ends the app at once, without `on_exit` |
| `fd_close`, `fd_seek`, `fd_fdstat_get`, `environ_*`, `args_*` | answered, doing nothing useful (stdio asks them at start); no environment, no arguments |

So: **no files** (`fopen` and friends need WASI calls the badge doesn't provide, and the
module is refused: read the app's files with `fs_read()`), **no sockets** (`http_*`,
`ws_*`), **no threads**, **no sleeping** (`sleep()`, `nanosleep()`: use `timer_after()`),
no `getenv`, no signals. `printf` works and adds about 17 KB to the module; `badge_print()`
logs a line without it. Formatting is slow interpreted: one `snprintf` with two numbers
took about 3 700 WebAssembly instructions. `rand()` is the same sequence every launch
unless you seed it (`srand(sys_millis())`, or `arc4random()`). `main()` is never called.

## The API: badge.h

```c
#include "badge.h"   // includes badge_abi.h, stdint.h, stddef.h, string.h
```

Every function is one call of the Lua API and behaves as the Lua reference says; the
headings below link there. `badge.h` is a header only (every function `static inline`),
so there is nothing to link, and the linker keeps only what the app calls.

### Conventions

- **Names.** `module_function()` for `module.function` (`ui.label` → `ui_label`,
  `mesh.send` → `mesh_send`), and methods as `type_method()` (`l:text()` →
  `ui_label_text(l, ...)`).
- **Indices count from 0** in C: LEDs 0..7, list rows, results (`ui_list_selected()`).
  The raw ABI (`sos_geti`, `badge_arg`) keeps the stack's 1-based indices.
- **Status.** Functions that act return 0, or `BADGE_ERR` (-1) when the API refused or
  failed; `badge_error()` then holds the reason (`"too fast: 4 messages per second"`,
  `"not a mesh channel"`, or a Lua error's text, which counts from 1 as Lua does:
  `led_set(8, ...)` says `"bad argument #1 to '?' (LEDs are 1..8)"`) until the next
  failure. Readers return the value, with a documented default when there is none.
- **Objects** are handles: `ui_obj` (labels, rects, bars, gradients, lists, QR codes,
  images, canvases, animations), `ui_img` (a `ui_load()` result), `ble_link`,
  `ws_socket`. All are `badge_obj { int32_t ref; }`; a constructor returns one with
  `.ref == 0` on failure. `ui_obj_remove(&o)` removes an object and frees its handle;
  `badge_obj_free(&o)` frees a handle without removing anything (a loaded image, a link
  or socket you're done with). Handles you never free are freed when the app exits.
- **Strings out** go into a buffer you pass, with its size: at most `cap - 1` bytes and a
  NUL. The function returns the full length, so a result `>= cap` says it was cut, and
  `BADGE_ERR` when there is no string (`badge_id()` before the first hello). Binary data
  (`fs_read`, Bluetooth values) comes the same way, length first.
- **Tables in** are JSON text: `mesh_send("*", "{\"ping\":1}")`, `storage_set_json()`,
  `http_post_json()`, `items_grant()`. The badge decodes it (`json.decode`), so a table
  arrives as Lua would send it. Or build one on the stack: `badge_new_table()`,
  `badge_set_int("x", 3)`, ..., then `mesh_send_top("*")`.
- **Tables out** stay on the stack as a `badge_val`, valid until the callback that got
  them returns. Documented fields come in structs (`mesh_peer`, `http_response`,
  `ble_dev`, `profile_t`, ...) or through typed helpers; anything else through
  `badge_field_int(v, "name", default)`, `badge_field_str()`, `badge_push_field()`, or
  as JSON text with `json_encode()`.
- **Callbacks** are a function pointer and a `void *user` you choose, handed back on
  every call. Server calls, dialogs and timers that fire once free their slot when they
  run; `timer_every()` keeps it until `timer_cancel()`; a list's `on_select` until
  `ui_obj_remove()`; a socket's and a link's until their close event; a served
  Bluetooth service and a scan until the app exits or replaces them.
- **Colours** are `0xRRGGBB` or a theme colour that follows Dark/Light: `COL_TEXT`,
  `COL_DIM`, `COL_BG`, `COL_PANEL`, `COL_BORDER`, `COL_PURPLE`, `COL_GREEN`, `COL_PINK`,
  `COL_LPURPLE`, `COL_BLUE`, `COL_AQUA`, `COL_LIME`, `COL_YELLOW`, `COL_ORANGE`. In option
  structs 0 means "the default"; use `COL_BLACK` there for black. `COL_NONE` is "no
  colour" (see-through). `RGB(r, g, b)` builds `0xRRGGBB`; `RGB565(r, g, b)` and
  `RGB565_HEX(0xRRGGBB)` build a pixel for `ui_canvas_blit`.
- **Option structs** may be `NULL` for all defaults, and zeroed fields are defaults, so
  designated initializers read well:
  `ui_label(0, 40, "Hi", &(ui_label_opts){.size = 32, .color = COL_GREEN})`.

Every wrapper leaves the stack as it found it, except those named `*_push`, which leave
one value on top for you to read and pop.

### `ui` ([Lua](LUA_API.md#ui))

```c
int ui_bg(uint32_t color);
ui_obj ui_label(int x, int y, const char *text, const ui_label_opts *o);
//   ui_label_opts { int size; uint32_t color; int mono; int w; int align; }
//   size 12 | 14 | 20 | 32; align UI_LEFT | UI_CENTER | UI_RIGHT (needs w)
int ui_label_text(ui_obj l, const char *text);
int ui_label_color(ui_obj l, uint32_t color);
ui_obj ui_rect(int x, int y, int w, int h, const ui_rect_opts *o);
//   ui_rect_opts { uint32_t color; int radius; uint32_t border; }  border: a 1 px border's colour
int ui_rect_size(ui_obj r, int w, int h);
int ui_rect_color(ui_obj r, uint32_t color);
ui_obj ui_bar(int x, int y, int w, int h, int value);      // progress 0..100, the Solana gradient
int ui_bar_set(ui_obj b, int value);
ui_obj ui_gradient(int x, int y, int w, int h);

int ui_obj_pos(ui_obj o, int x, int y);                    // any object
int ui_obj_show(ui_obj o, int show);
int ui_obj_remove(ui_obj *o);                              // and frees the handle
void badge_obj_free(badge_obj *o);

ui_obj ui_list(int x, int y, int w, int h, const char *const *items, int n,
               ui_select_fn on_select, void *user);        // void on_select(void *user, int index)
ui_obj ui_list_rows(int x, int y, int w, int h, const ui_row *rows, int n,
                    ui_select_fn on_select, void *user);   // ui_row { title, sub, right }
int ui_list_items(ui_obj list, const char *const *items, int n);
int ui_list_set_rows(ui_obj list, const ui_row *rows, int n);
int ui_list_selected(ui_obj list);                         // the row, from 0, or -1
int ui_list_select(ui_obj list, int index);

ui_obj ui_qr(int x, int y, int size, const char *text);    // 25..218 px, text <= 256 bytes
int ui_qr_text(ui_obj q, const char *text);
int ui_hint(const char *text);                             // pinned to the bottom (16 px)
int ui_toast(const char *text);
int ui_confirm(const char *text, ui_confirm_fn cb, void *user);    // void cb(void *user, int yes)
int ui_input(const char *prompt, int maxlen, ui_input_fn cb, void *user); // text NULL: cancelled
int ui_clear(void);                                        // every object; free handles you keep
```

#### Images ([Lua](LUA_API.md#images))

```c
ui_img ui_load(const char *path);                          // a bundled PNG or GIF, loaded once
ui_img ui_load_rows(const char *const *rows, int n, const ui_pal *pal, int npal); // pixel art
//   ui_pal { char ch; uint32_t color; }  adds or changes a character (COL_NONE: see-through)
int ui_img_size(ui_img img, int *w, int *h);
ui_obj ui_image(int x, int y, const char *path, int scale);           // scale 1..16 (0: 1)
ui_obj ui_image_of(int x, int y, ui_img img, int scale);              // views share the pixels
ui_obj ui_image_rows(int x, int y, const char *const *rows, int n, const ui_pal *pal, int npal, int scale);
int ui_image_set(ui_obj o, const char *path);
int ui_image_set_img(ui_obj o, ui_img img);
int ui_image_scale(ui_obj o, int scale);                   // images and animations

ui_obj ui_anim(int x, int y, const char *path, const ui_anim_opts *o); // a bundled GIF, playing
//   ui_anim_opts { int scale; int loop; }  loop: UI_LOOP_FILE (what the GIF says),
//   UI_LOOP_FOREVER, UI_LOOP_ONCE, or a number of times
int ui_anim_play(ui_obj a);
int ui_anim_stop(ui_obj a);
int ui_anim_playing(ui_obj a);
```

#### Canvas and blit

```c
ui_obj ui_canvas(int x, int y, int w, int h, int scale, uint32_t color); // color 0: see-through
int ui_canvas_clear(ui_obj c, uint32_t color);             // COL_NONE: see-through
int ui_canvas_pixel(ui_obj c, int x, int y, uint32_t color);
int ui_canvas_rect(ui_obj c, int x, int y, int w, int h, uint32_t color);  // filled
int ui_canvas_line(ui_obj c, int x1, int y1, int x2, int y2, uint32_t color);
int ui_canvas_image(ui_obj c, ui_img img, int x, int y);   // see-through pixels leave it as it was
int ui_canvas_image_path(ui_obj c, const char *path, int x, int y);
int32_t ui_canvas_get(ui_obj c, int x, int y);             // 0xRRGGBB, or -1
int ui_canvas_blit(ui_obj c, const uint16_t *px, int x, int y, int w, int h);
```

`ui_canvas_blit` is why games are written in C: it copies `w` × `h` RGB565 pixels (row by
row, little-endian, `RGB565()`) from the app's memory onto the canvas at (`x`, `y`),
clipped, in one call (`sos_blit` underneath, which skips the stack), at most 4096 pixels
a side. Draw the whole frame into your own buffer in `on_tick`, then blit it. A canvas
costs 2 bytes a pixel of the app's heap whatever its scale, so a 160 × 108 canvas shown
at 2× fills the screen for 34 KB (and a 34 KB buffer of your own). The canvas redraws on
screen after the callback. A blit costs the host 1 step per 16 pixels written (1 080 of
the 200 000 for 160 × 108). Filling a full 320 × 216 canvas took 16.4 to 16.7 ms on a
badge, from native code and interpreted alike: the copy is the badge's own code.

The pixel value `0x0001` is the canvas's own mark for see-through, so a blit writes it as
`0x0000` (black): a blit never makes a pixel see-through. `ui_canvas_get` gives the
colour back from RGB565, each channel's top bits repeated (`0x9945FF` reads `0x9C45FF`,
`0xFF0000` stays `0xFF0000`).

```c
static uint16_t frame[160 * 108];
static ui_obj canvas;

void on_start(void) { canvas = ui_canvas(0, 0, 160, 108, 2, COL_BLACK); }

void on_tick(int32_t ms)
{
    static uint32_t t;
    t += (uint32_t)ms;
    for (int y = 0; y < 108; y++)
        for (int x = 0; x < 160; x++)
            frame[y * 160 + x] = RGB565(x + t / 8, y * 2, (x ^ y) + t / 16);
    ui_canvas_blit(canvas, frame, 0, 0, 160, 108);
}
```

Interpreted, this `on_tick` is about 478 000 WebAssembly instructions (counted in the
emulator): more than 0.1 s a frame on a badge even at a plain loop's pace ("What runs
where"), against a tick every 30 ms. An app that computes every pixel needs its native
form. One that draws less holds up interpreted: the `starfield` example (220 stars into a
160 × 108 frame) drew a frame in 7 ms native and 33 ms interpreted, and ran at 19.4 and
12.6 to 13.2 frames a second.

#### Frames straight to the screen (Solana OS 2.3.0)

```c
int ui_frame(const uint16_t *px, int w, int h, int x, int y, int dw, int dh, int flags);
//   w x h RGB565 pixels, scaled (nearest) to dw x dh at (x, y) of the app area
//   flags UI_FRAME_BE: big-endian pixels (RGB565_BE(), the screen's own byte order)
//   1 shown, 0 not shown (a dialog, the keyboard or the badge's panel is over the app),
//   BADGE_ERR when dw x dh at (x, y) isn't inside the app area (UI_W x UI_H)
```

A canvas is the UI's: `ui_canvas_blit` copies your frame into it, and the badge's UI
library redraws it later, through its image path, after your callback has returned, so
you wait for all of it (17.3 ms for a 320 x 216 frame). `ui_frame` skips the UI: the frame
goes to the screen's SPI DMA during the call, and the call returns while the screen is
still taking it, so the next frame is computed meanwhile. 1 to 1024 pixels a side; the
whole destination must be inside the app area.

- **No copy**: a big-endian frame, unscaled, in a 64-byte-aligned buffer whose size
  (`w * h * 2`) is a multiple of 64 is read by the DMA straight from your memory. Don't
  write to it until the next `ui_frame`: draw the next frame into a second buffer
  (`static uint16_t fb[2][160 * 144] __attribute__((aligned(64)));`). Anything else
  (little-endian, scaled, unaligned) is converted into a buffer of the badge's first.
  Measured on a production badge on ESP-IDF 6.0.1, a 160 x 144 frame cost the app 1.0 to
  1.4 ms without a copy, 6.5 to 6.9 ms with one, and 11.6 to 17 ms scaled to 240 x 216 (the
  DMA of the bigger frame, 20.7 ms at 40 MHz, is then the limit). On ESP-IDF master
  (`tests/c/t-c-fbench`, the prototype HYQ5VS, interpreted), sending frame after frame, each
  160 x 144 frame took 9.2 ms without a copy (the SPI wire at 40 MHz: the next frame waits
  for the last), 15.0 ms with one and 31.4 ms scaled to 240 x 216; `lua mem` counted every
  unscaled big-endian frame as sent without a copy. The DMA reads PSRAM, which is out of
  reach while the badge writes its flash; the only writes while an app draws are small
  settings commits, and 1 200 frames under 16 of them showed no error, hang or restart
  (`tests/c/t-c-fstress`): at worst a frame could show wrong pixels until the next one.
- **The UI on top**: while your `ui_confirm` or `ui_input`, the badge's own confirm, the
  keyboard or the bridge panel is open, `ui_frame` shows nothing and returns 0; while a
  toast shows, only the rows above it are drawn. The UI also paints over the frame whenever
  it redraws something in that area (a label you put there, the toast going away): keep
  UI objects out of the frame's rectangle, and the next frame paints the area again.
  `ui_bg(COL_BLACK)` gives the UI's redraws the frame's background.
- **Pacing**: the screen takes one frame at a time (a 160 x 144 one in 9.2 ms, 240 x 160
  in 15.4 ms, the whole app area in 27.9 ms); a `ui_frame` while the last one is still on
  its way waits for it. Ask `sys_tick(16)` for a tick every frame of a 60 Hz game.
- **Cost**: host steps as a blit, 1 per 16 pixels converted, or 1 per 256 sent with no
  copy. A screenshot (`shot` on the console) shows a converted frame, not one sent with no
  copy.
- **In the emulator** the frame goes into the page's screen, with the same rules.

### `led` (permission `led`, [Lua](LUA_API.md#led-permission-led))

```c
#define LED_COUNT 8                        // 0..5 across the top, 6..7 on the right edge
int led_set(int i, int r, int g, int b);   // 0..255 each; the badge scales to its cap
int led_hex(int i, uint32_t hex);
int led_fill(int r, int g, int b);
int led_off(void);
```

### `timer` ([Lua](LUA_API.md#timer))

```c
int32_t timer_after(int32_t ms, timer_fn fn, void *user);  // void fn(void *user); the id (> 0) or BADGE_ERR
int32_t timer_every(int32_t ms, timer_fn fn, void *user);
void timer_cancel(int32_t id);
```

8 timers at a time; a delay under 20 ms is taken as 20 ms. Anywhere `badge.h` takes a
callback, `NULL` is allowed: the call still happens, and nothing runs when it answers.

### `sys`, `badge`, `json` ([Lua](LUA_API.md#sys-badge-json))

```c
uint32_t sys_millis(void);                 // ms since the app started (no stack: fast)
uint32_t sys_keys(void);                   // keys held now: KEY_HELD(sys_keys(), KEY_UP) (fast)
int32_t sys_time(void);                    // unix seconds from the server's clock, 0 if never synced
int32_t sys_battery(void);                 // percent, -1 on USB or unknown (the status bar's number, filtered)
int sys_memory(int32_t *used, int32_t *max);   // the whole app: Lua state and WAMR's share ("Limits")
int32_t sys_tick(int32_t ms);              // on_tick every ms (16..1000, default 30) from now on; the old period (2.3.0)
void sys_exit(void);
int32_t badge_id(char *buf, uint32_t cap);     // "7K2Q9M"; BADGE_ERR before the first hello
int32_t badge_owner(char *buf, uint32_t cap);  // "@toly"; BADGE_ERR while unclaimed
int badge_online(void);
int json_push(const char *text);           // json.decode, pushed (see "Values on the stack")
int32_t json_encode(badge_val v, char *buf, uint32_t cap);
const char *key_name(int32_t key);         // "up", ... "back"
void badge_print(const char *line);        // a console line, as Lua's print, without printf (<= 1024 bytes)
void badge_fail(const char *msg);          // stop with this on the error screen
```

### `storage` (always allowed, [Lua](LUA_API.md#storage-always-allowed))

```c
int32_t storage_get_int(const char *key, int32_t dflt);
int32_t storage_get_str(const char *key, char *buf, uint32_t cap);  // BADGE_ERR when unset
int32_t storage_get_json(const char *key, char *buf, uint32_t cap); // any value as JSON ("null" when unset)
badge_val storage_push(const char *key);   // the value itself, pushed: pop it
int storage_set_int(const char *key, int32_t v);
int storage_set_bool(const char *key, int v);
int storage_set_str(const char *key, const char *v);
int storage_set_json(const char *key, const char *json);   // tables, lists
int storage_remove(const char *key);
```

16 KB per app, kept on the card across launches and removed with the app. Keep what
lives only in C memory in sync: save in `on_exit` or as it changes.

### `fs` (always allowed, [Lua](LUA_API.md#fs-and-require-always-allowed))

```c
int fs_exists(const char *path);
int32_t fs_size(const char *path);         // -1 when there's no such file
int32_t fs_read(const char *path, void *buf, uint32_t cap);         // the whole file (<= 256 KB)
int32_t fs_read_at(const char *path, int32_t offset, int32_t len, void *buf, uint32_t cap);
int32_t fs_list(const char *path, void (*fn)(void *user, const char *name), void *user);
int32_t fs_lines(const char *path, void (*fn)(void *user, const char *line, uint32_t len), void *user);
int fs_json_push(const char *path);        // the decoded file, pushed
```

The app's bundled data files, read-only (`.c` and `.h` files aren't among them: the
badge has no source). `fs_read` returns the file's length: give a buffer one byte
longer to get all of it (binary is fine; the NUL after it is extra). `fs_list` and
`fs_lines` call `fn` for each entry or line before they return. Reading counts toward the
host's 200 000 steps a callback as in Lua (500 a file opened, 1 per 4 bytes). A missing
file is `0` from `fs_exists`, `-1` from `fs_size` and `BADGE_ERR` from the others.

### `files` (permission `files`, Solana OS 2.3.0, [Lua](LUA_API.md#files-permission-files-solana-os-230))

```c
int32_t files_dir(char *buf, uint32_t cap);    // "hh/files/<id>/": the folder, as the owner sees it on the card
int32_t files_size(const char *name);          // bytes, or -1
int32_t files_read(const char *name, uint32_t offset, void *buf, uint32_t len);  // <= 256 KB: bytes read, 0 at the end
int files_write(const char *name, uint32_t offset, const void *data, uint32_t len); // "save/<name>" only
int files_remove(const char *name);            // a save only
int32_t files_list(void (*fn)(void *user, const char *name, int32_t size), void *user);
```

The app's own folder on the card, for files the badge's owner copied there (an emulator's
games) and the app's saves in `save/`: the rules are the Lua reference's. `files_read`
reads straight into your memory (`sos_file_read` underneath, no stack and no copy), so
paging a big file costs only the card (and 4 100 host steps per 16 KB): a 1 MB file was
read whole, 256 KB a tick, in 0.92 s on a badge. A missing `"files"` permission stops the
app at the first `files_read` (as a bad pointer does); a read of more than 256 KB too. In a
service instance (`background`) the reads work; `files_write` and `files_remove` return
`BADGE_ERR`: only the app on the screen writes.

### `wifi` (permission `wifi`, [Lua](LUA_API.md#wifi-permission-wifi))

```c
int wifi_status(void);                     // WIFI_OFF, WIFI_CONNECTING, WIFI_ON, WIFI_FAILED
int32_t wifi_ssid(char *buf, uint32_t cap);
int wifi_on(void);
int wifi_off(void);
```

### `mesh` (permission `mesh`, [Lua](LUA_API.md#mesh-permission-mesh))

```c
int mesh_nearby(mesh_peer *out, int max);  // how many there are (may be > max)
//   mesh_peer { char id[8], name[32], kind[8]; int32_t level, rssi; int bridged, app; }
int mesh_send(const char *to, const char *json);   // to: a badge id, or "*" / NULL for all
int mesh_send_top(const char *to);         // the table on top of the stack (popped)
int mesh_bridge(void);
int mesh_pause(void);
int mesh_resume(void);
int mesh_info(mesh_info_t *out);
//   mesh_info_t { int32_t channel, event; char band[4]; int online, paused, scanning; }
int mesh_channel(int n);                   // 0: back to the event's channel
int mesh_scan(const mesh_scan_opts *o, mesh_scan_fn cb, void *user);
//   mesh_scan_opts { const int *channels; int nchannels; const char *band; int dwell; }
//   void cb(void *user, badge_val channels, int count), read during the callback with:
int mesh_scan_channel(badge_val channels, int i, mesh_channel_t *out);
//   mesh_channel_t { int32_t channel, badges, beacons, app, npeers; char band[4]; }
int mesh_scan_peer(badge_val channels, int i, int j, mesh_peer *out);

int mesh_meet(const char *id, const char *data_json, mesh_meet_fn cb, void *user);
//   void cb(void *user, const char *peer, badge_val their_data, const char *err)
int32_t mesh_meet_accept(const char *data_json);   // in on_meet(): return mesh_meet_accept(...)
int mesh_meets(int32_t after, badge_result_fn cb, void *user);  // {items = {...}, next}
```

Messages are JSON objects of ≤ 200 bytes, 4 a second. `on_mesh()` receives them:

```c
void on_mesh(void)
{
    char from[8];
    badge_str(badge_arg(1), from, sizeof from);
    int32_t score = badge_field_int(badge_arg(2), "score", -1);   // check: anything can arrive
    if (score >= 0) { /* ... */ }
}
```

Meets, bridges, channels and scans work as the Lua reference says ("Meets", "Bridged
badges", "Choosing a channel"); the [Connectivity guide](CONNECTIVITY.md) says when to use
which.

### `ble` (permission `ble`, Solana OS 1.0.0, [Lua](LUA_API.md#ble-permission-ble-solana-os-100))

```c
int ble_on(void);                          // BADGE_ERR: "not enough memory", "unavailable"
int ble_off(void);
int ble_state(void);                       // 1 on, 0 off
int32_t ble_address(char *buf, uint32_t cap);
int ble_advertise(const ble_adv *a);       // NULL stops
//   ble_adv { const char *name, *service; const void *data; uint32_t data_len; int connectable, interval; }
int ble_scan(const ble_scan_opts *o, ble_scan_fn cb, void *user);   // void cb(void *user, const ble_dev *dev)
//   ble_scan_opts { const char *service, *name; int active, seconds, duty; }
//   ble_dev { char addr[18]; int32_t rssi; char name[30]; char services[4][37]; int nservices;
//             int32_t company; uint8_t data[32]; int data_len; int connectable, random; }
int ble_scan_stop(void);

ble_link ble_connect(const ble_dev *dev, ble_event_fn cb, void *user);
//   void cb(void *user, const ble_event *ev): BLE_EV_OPEN, BLE_EV_NOTIFY (chr, data, len),
//   BLE_EV_CLOSE (reason), always the last
int ble_link_read(ble_link l, const char *service, const char *chr, ble_read_fn cb, void *user);
//   void cb(void *user, const uint8_t *data, uint32_t len, const char *err)
int ble_link_write(ble_link l, const char *service, const char *chr, const void *data, uint32_t len);
int ble_link_write_cb(ble_link l, const char *service, const char *chr, const void *data, uint32_t len,
                      ble_done_fn cb, void *user);         // void cb(void *user, const char *err)
int ble_link_subscribe(ble_link l, const char *service, const char *chr, int on);
int ble_link_close(ble_link l);
int ble_link_state(ble_link l);            // BLE_CONNECTING, BLE_OPEN, BLE_CLOSED

int ble_serve(const ble_service *svc, ble_event_fn cb, void *user);
//   ble_service { const char *service; const ble_char *chars; int nchars; }
//   ble_char { const char *uuid; int read, write, notify; const void *value; uint32_t value_len; }
//   events: BLE_EV_OPEN (addr), BLE_EV_WRITE (chr, data, len), BLE_EV_SUBSCRIBE (chr, on), BLE_EV_CLOSE
int ble_set(const char *chr, const void *data, uint32_t len);
int ble_notify(const char *chr, const void *data, uint32_t len);
int ble_drop(void);
```

Strings in an event (`chr`, `addr`, `reason`) and its data are valid until the callback
returns: copy what you keep. Everything a peer sends is untrusted.

### `lamps` (permission `lamps`, [Lua](LUA_API.md#lamps-permission-lamps))

```c
int lamps_balance(lamps_fn cb, void *user);
int lamps_spend(int32_t amount, const char *sku, lamps_fn cb, void *user);
int lamps_transfer(const char *badge_id, int32_t amount, const char *memo, lamps_fn cb, void *user);
int lamps_deposit(const char *address, int32_t amount, const char *memo, lamps_fn cb, void *user);
int lamps_award(int32_t amount, const char *reason, lamps_fn cb, void *user);
//   void cb(void *user, const lamps_result *r, const char *err)
//   lamps_result { int32_t balance, awarded; int needs_x; char id[24]; }
```

Spending, transfers and deposits show the badge's own confirmation, which no app can
answer. `lamps_award` pays nothing until the app has an award allowance, which you
ask for when you publish: the submit page's "!LAMPs allowance" field
([LUA_API.md "Develop and publish"](LUA_API.md#develop-and-publish)).

### `items` (permission `items`, [Lua](LUA_API.md#items-permission-items))

```c
int items_list(badge_result_fn cb, void *user);    // result: a list of items
int items_all(badge_result_fn cb, void *user);
int items_grant(const char *kind, const char *name, const char *data_json, badge_result_fn cb, void *user);
int items_update(const char *id, const char *data_json, badge_result_fn cb, void *user);
int items_remove(const char *id, badge_result_fn cb, void *user);
void item_info(badge_val item, item_t *out);       // item_t { char id[40], kind[32], name[48]; int tradable; }
//   its data: badge_field_json(item, "data", buf, cap)
```

`badge_result_fn` is `void cb(void *user, badge_val result, const char *err)`: `err` is
`NULL` and `result` the answer, or `err` the reason (`"offline"`, `"not linked"`,
`"denied"`, `"busy"`, `"socket open"`, ...). Walk a list with `badge_len(result)` and
`badge_push_index(result, i)` (pop each).

### `profile` (permission `profile`, [Lua](LUA_API.md#profile-permission-profile))

```c
int profile_me(profile_fn cb, void *user);
int profile_get(const char *badge_id, profile_fn cb, void *user);
//   void cb(void *user, const profile_t *p, const char *err)
//   profile_t { char name[48], x[32], x_name[64], solana[48]; int32_t level, lamps, pages; char badge[8]; }
```

Empty strings where Lua has `nil` (no X, no linked or public Solana address).

The player's items and Guide pages (permission `inventory`, Solana OS 2.3.0; the badge's own
owner only, [Lua](LUA_API.md#the-players-items-and-guide-pages-permission-inventory-solana-os-230)
has what each field means, the paging and the limits):

```c
int profile_items(const char *cursor, badge_result_fn cb, void *user);
//   result: {items = [...], next = "..." | nil, total = n}
int profile_pages(const char *cursor, badge_result_fn cb, void *user);
//   result: {pages = [...], next = "..." | nil, owned = n, total = n}
void profile_item_info(badge_val item, profile_item_t *out);
//   profile_item_t { char id[24], kind[33], name[41], app[25], quest[49]; int tradable; }  ("" for nil)
void profile_page_info(badge_val page, profile_page_t *out);
//   profile_page_t { char id[49], title[49], category[16], rarity[16]; int32_t number, count; }
```

`cursor` is `NULL` (or `""`) for the first page, then the result's `next`, read with
`badge_field_str(result, "next", buf, cap)` (it returns 0 or less at the end). An app that
shows how many of the Guide's pages its player has:

```c
#include <stdio.h>
#include "badge.h"

static ui_obj s_line;

static void on_pages(void *user, badge_val r, const char *err)
{
    char next[160], text[64];
    (void)user;
    if (err) {
        ui_label_text(s_line, err);
        return;
    }
    if (badge_field_str(r, "next", next, sizeof next) > 0) {   // more to come: walk on
        profile_pages(next, on_pages, NULL);
        return;
    }
    snprintf(text, sizeof text, "You hold %d of the Guide's %d pages",
             (int)badge_field_int(r, "owned", 0), (int)badge_field_int(r, "total", 0));
    ui_label_text(s_line, text);
}

void on_start(void)
{
    s_line = ui_label(8, 8, "Counting your pages...", &(ui_label_opts){.size = 20, .w = 304});
    profile_pages(NULL, on_pages, NULL);
}
```

(`owned` and `total` come with every page, so an app that only wants the count can stop at
the first answer; walking on is how it would read each page's `count` or `rarity`, with
`badge_push_field(r, "pages")`, `badge_len` and `badge_push_index`.)

### `quest` (permission `quest`, [Lua](LUA_API.md#quest-permission-quest))

```c
int quest_report(const char *event, const char *data_json, badge_result_fn cb, void *user);
int quest_active(badge_result_fn cb, void *user);  // result: a list
void quest_info(badge_val q, quest_t *out);        // quest_t { char id[40], title[64], step[64]; }
```

### `http` (permission `http`, [Lua](LUA_API.md#http-permission-http-solana-os-046))

```c
int http_get(const char *url, http_fn cb, void *user);
int http_get_headers(const char *url, const http_header *h, int n, http_fn cb, void *user);
int http_post(const char *url, const char *body, http_fn cb, void *user);       // text
int http_post_json(const char *url, const char *json, http_fn cb, void *user);  // sent as JSON
int http_request(const http_req *q, http_fn cb, void *user);
//   http_req { method, url, headers, nheaders, body, body_len, json }
//   void cb(void *user, const http_response *res, const char *err)
//   http_response { int32_t status; char type[64]; int truncated; badge_val body, location; }
int32_t http_body(const http_response *res, char *buf, uint32_t cap);
```

`err` is set when there was no answer (`"offline"`, `"host not allowed"`, `"timeout"`,
...); any answer, a 404 included, comes as `res` with its `status`. Only the hosts in
`app.json`; the badge server makes the request and signs who asked (see the Lua
reference for what your server receives).

### `ws` (permission `http`, Solana OS 0.5.1, [Lua](LUA_API.md#ws-permission-http-solana-os-051))

```c
ws_socket ws_open(const char *url, const char *const *protocols, int nprotocols, ws_fn cb, void *user);
//   void cb(void *user, const ws_event *ev): WS_EV_OPEN, WS_EV_MESSAGE (data, len), WS_EV_CLOSE
//   (reason, code: the host's close code or -1), always the last
int ws_send(ws_socket w, const char *text);
int ws_send_bytes(ws_socket w, const void *data, uint32_t len);
int ws_send_json(ws_socket w, const char *json);
int ws_close(ws_socket w);
int ws_state(ws_socket w);                 // WS_CONNECTING, WS_OPEN, WS_CLOSED
```

### `rpc` (permission `rpc`, Solana OS 0.5.2, [Lua](LUA_API.md#rpc-permission-rpc-solana-os-052))

```c
int rpc_call(const char *method, const char *params_json, const char *cluster,
             badge_result_fn cb, void *user);       // cluster NULL: mainnet, or "devnet"
```

`params_json` is the params array as JSON (`"[\"Gh9Zw...\", {\"commitment\":\"confirmed\"}]"`)
or `NULL`. Integers beyond 32 bits come back as decimal strings (`"5000000000"`): read
them with `badge_field_str()` and `strtoll()`, which C has. Never put a keypair on a badge
(see the Lua reference's warning).

### `crypto` and `solana` (always allowed, Solana OS 2.1.0, [Lua](LUA_API.md#crypto-and-solana-always-allowed-solana-os-210))

```c
int crypto_sha256(const void *data, uint32_t len, uint8_t out[32]);
int crypto_on_curve(const uint8_t bytes[32]);
int crypto_verify(const uint8_t key[32], const void *msg, uint32_t len, const uint8_t sig[64]);
int32_t solana_encode(const void *bytes, uint32_t len, char *out, uint32_t cap);  // base58, <= 128 bytes
int32_t solana_decode(const char *text, uint8_t *out, uint32_t cap);   // how many bytes, or BADGE_ERR
int solana_pda(const badge_bytes *seeds, int nseeds, const uint8_t program[32],
               uint8_t out[32], int *bump);         // badge_bytes { const void *p; uint32_t len; }
```

They cost the host steps the Lua reference lists. Plain C can do SHA-256 or base58
itself too, in its own memory; the badge's versions are its own code, much faster than
the interpreter (the SHA-256 row of "What runs where").

### `mail` (permission `mail`, Solana OS 2.3.0, [Lua](LUA_API.md#mail-permission-mail-solana-os-230))

```c
int mail_send(const char *to, const char *title, const char *text, const char *data_json);
                                           // to a badge id: 0 when queued; data_json NULL or a JSON object
int mail_count(void);                      // this app's mail on the badge (<= 50), newest first
int mail_item(int i, mail_t *out);         // the i-th (0-based): 0, or BADGE_ERR past the end
//   mail_t { int32_t id, at, exp, read; char from[8], by[25], title[33], text[201], data[129]; }
int mail_get(int32_t id, mail_t *out);     // one by id
void on_mail(void);                        // export: badge_arg(1) is the mail (a table, as Lua's mail.get)
```

`mail_item` and `mail_get` fill `data` with the mail's data as compact JSON (`""` when it
has none). The Lua reference's rules hold as written: only between badges whose owners both
installed the app, queued through the outbox, the same caps, nothing in it trusted. In a
service instance (`background`) `mail_count`, `mail_item`, `mail_get` and the `on_mail`
export work; `mail_send` returns `BADGE_ERR`: only the app on the screen sends.

### `gossip` (permission `gossip`, Solana OS 2.4.0, [Lua](LUA_API.md#gossip-permission-gossip-solana-os-240))

The Lua module's functions as `gossip_*` in `badge.h`, with the same rules: reads for any
app with `gossip` (services too), the acting calls for the Lab (`gossip-lab`) only.

```c
gossip_me_t me;            gossip_me(&me);                  // 0, or BADGE_ERR "not linked"
                           gossip_me_json(buf, cap);        // the whole Node as JSON
gossip_program_t p;        gossip_program(addr, &p);        // a held record: 0, or "unknown"
                           gossip_genome(addr, &p);         // looks alone: 0, or "bad address"
                           gossip_slot(i, &p);  gossip_inbox(i, &p);   // 1 filled, 0 empty or past the end
gossip_peer_t r[20];       int n = gossip_nearby(r, 20);    // strongest first
gossip_heard_t h[8];       int k = gossip_hearing(h, 8);    // GOSSIP_HEARING / _EXPOSED / _INCUBATING
// in on_gossip():         gossip_event_kind(buf, cap);  gossip_event_json(buf, cap);
// the Lab only (0, or BADGE_ERR "lab only", "busy", ...):
gossip_set_class('v');  gossip_activate(a);  gossip_place(a, slot);  gossip_drop(slot);
gossip_deploy(NULL);    gossip_use(item, p, q, trait /* -1 none */);  gossip_name(a, "Turbine Toad");
gossip_public(1);       gossip_lead(1);
```

- `gossip_me_t` holds the Node's numbers, `act`, `nslots`/`ninbox`, `why`, `lead` (the
  "May become a leader" setting), `name_nodes` (adoptions before a program can be named)
  `ach_n`/`ach_pts` (achievements) and `site` (the website's address from hello, `""` before
  the first); `gossip_me_json` also has `close` (the dBm a role
  claim needs), and `gossip.nearby()`'s entries read through `sos_global` a badge's `spam`
  (live spam a Validator may cure); its items (with `pick` for the Upgrade Authority+
  that takes a trait), effects, immunity, objectives and the newest achievements' names are
  in `gossip_me_json`. Times are unix seconds, countdowns (`left`) seconds.
- `gossip_program_t`: `a`, `par`, `name`, `gen`, `nodes` (-1 unknown), `verified`, `spam`,
  `colour` (0xRRGGBB), `hue`, `body`, `eyes`, `marks`, `mark_seed`, `traits[3]` and
  `ntraits`, `vir`, `hard`, `stab`, `rarity` (`GOSSIP_COMMON` .. `GOSSIP_LEGENDARY`).
- **Slots are 0-based in C** (1-based in Lua, 0-based on the wire). The export `on_gossip`
  (void, the event as `badge_arg(1)`) is called by name, like a Lua app's function: any C
  app with the permission that exports it gets every event of the Lua reference's
  `on_gossip` (the node side's `exposed`, `result`, `node`, `event`, the role side's
  `role`, `boss`, `bid`, `ack`, `cured`), with the same fields. No ABI change.
- Each wrapper reads through the Lua module (one call and a table walk): read once per
  frame at most, not per creature.

The role side:

```c
gossip_role_t r;
if (gossip_role(&r) == 1 && !strcmp(r.kind, "offer")) {   // "offer", "session", "ended"
    // r.ro, r.left (seconds before it accepts by itself) ...
    gossip_role_answer(1);                     // or gossip_role_end() to decline
}
gossip_boss_t b;
if (gossip_boss(&b) == 1 && b.phase == 1) {    // the fight heard nearby, beat/ms moved to now
    gossip_vote(votes, lockout);               // the OS sends a summary every ~2 s
}
void on_gossip(void) { /* badge_arg(1): {t = "boss", ...}: gossip_boss_arg(&b) */ }
```

| Function | Lua | Returns |
|---|---|---|
| `gossip_role(&r)` | `gossip.role()` | 1 filled, 0 none. `r.kind` `""`/`"offer"`/`"session"`/`"ended"`; `since`, `until` unix seconds, `left` seconds |
| `gossip_boss(&b)`, `gossip_boss_arg(&b)` | `gossip.boss()`, an `on_gossip` `boss` event | 1 filled, 0 none (`gossip_boss_arg` in `on_gossip`); `b.min_beats`: no root before this beat |
| `gossip_voters(&v)` | `gossip.voters()` | 1 while this badge is the boss, else 0 (`v.min_beats`, and `r.min_beats` of a boss's `gossip_role`: the first beat the block can be rooted at; `r.on_air_p`: the program in the role's HELLO now) |
| `gossip_bids(out, max)` | `gossip.bids()` | how many filled (`gossip_bid_t`), 0 when not a Leader |
| `gossip_role_answer(yes)`, `gossip_role_end()` | `gossip.role_answer`, `gossip.role_end` | 0, or `BADGE_ERR` with `badge_error()` |
| `gossip_bid(role, addr, fee)`, `gossip_claim(role, c)`, `gossip_cure(id)` | the same | 0 or `BADGE_ERR` |
| `gossip_vote(votes, lockout)`, `gossip_fork()` | the same | 0 or `BADGE_ERR` |
| `gossip_block(picks, n)` | `gossip.block` | 0 or `BADGE_ERR` (`gossip_pick_t {p, by}`, ≤ 4) |

The rules are the Lua reference's: `gossip_role` and `gossip_boss` for any app with
`gossip` and in a service; the rest for the Lab (`gossip-lab`) only, others get
`BADGE_ERR` and `"lab only"` (`gossip_bids` and `gossip_voters` return `BADGE_ERR` too).
Nothing here blocks or reaches the server; the role key never reaches the app.

### `service` (permission `background`, Solana OS 2.3.0, [Lua](LUA_API.md#service-permission-background-solana-os-230))

A C app's service is a second instance of the same module, with its own linear memory,
started at `on_service_start` instead of `on_start` (after `_initialize`). Nothing of the
app's instance is shared: the two talk by messages and through `storage`. The Lua
reference's rules hold as written: the owner's first yes, starting at boot, what stops
it, what it may call, and the limits. In C, one rule for what a service can't use:

- the functions of the modules the Lua reference leaves out (`ui_*`, `led_*`, `wifi_*`,
  `ble_*`, spending, ...) return `BADGE_ERR`, and `badge_error()` says
  `"attempt to call a nil value"`: their module isn't there;
- the screen and the buttons answer as if there were none: `ui_frame` returns -1 (not
  drawn) and `sys_keys()` returns 0 (nothing held), whatever the owner is pressing in
  another app; a service never reads the keys.

```c
// In the app (on_start / on_key / ...):
int service_start(badge_result_fn cb, void *user);  // cb(user, ok, result): result "denied", "full", ...
int service_stop(void);
int service_running(void);                 // 1 while it runs
int service_send(const char *json);        // to the service's on_message; BADGE_ERR when not running / full
void on_service(void);                     // export: badge_arg(1) is the service's message

// In the service instance:
void on_service_start(void);               // export, required for "background": the service's on_start
void on_tick(int32_t ms);                  // every 1000 ms in the service
void on_message(void);                     // export: badge_arg(1) is the app's message
void on_mesh(void);  int32_t on_meet(void); // as in the app
void on_stop(void);                        // export: being stopped (same limits as on_exit)
int service_status(const char *line);      // the Services row's line (≤ 48 printable ASCII)
int service_notify(const char *text, uint32_t led);  // toast "<App>: text", led 0xRRGGBB or 0
int sys_is_service(void);                  // 1 in the service instance, 0 in the app
```

`service_send` from the service reaches the app's `on_service` only while the app is
open. **One WebAssembly runtime**: the badge runs one C program at a time, so a C
service pauses while any C app is on screen (`Paused: a C app is open`) and starts
again when it closes; a C app's own `service_start` answers OK and its service starts
once the app closes (`Starts when the C app closes`). So a C app and its service never
run at once: they meet in `storage`. Two C services don't run at once either (the
second answers `"busy"`); a C service runs next to Lua apps and Lua services. Limits besides the Lua table's: 1 MB in all per service (its linear memory, WAMR's
copy of an interpreted module, its instance and stacks), 250 000 interpreted
instructions or 30 ms of native time per callback (about what Lua's 100 000 take), and
2 000 000 interpreted instructions or 700 ms of native time a minute. The store refuses a `background` C app
whose module doesn't export `on_service_start`.

## Values on the stack

Between C and the Lua API values travel on a stack (see "The raw ABI"). Most apps never
see it, except for tables that callbacks receive and for fields beyond the typed
structs:

```c
badge_val badge_arg(int32_t n);            // a callback's argument n (from 1)
badge_val badge_top(void);                 // the top value
void badge_pop(int32_t n);
int32_t badge_type(badge_val v);           // SOS_NIL, SOS_BOOL, SOS_INT, SOS_NUM, SOS_STR, SOS_TABLE, ...
int32_t badge_int(badge_val v);  float badge_num(badge_val v);  int badge_bool(badge_val v);
int32_t badge_str(badge_val v, char *buf, uint32_t cap);
uint32_t badge_len(badge_val v);           // a string's bytes, a list's length
int32_t badge_field_type(badge_val t, const char *key);
int32_t badge_field_int(badge_val t, const char *key, int32_t dflt);
float badge_field_num(badge_val t, const char *key, float dflt);
int badge_field_bool(badge_val t, const char *key);
int32_t badge_field_str(badge_val t, const char *key, char *buf, uint32_t cap);
int32_t badge_field_json(badge_val t, const char *key, char *buf, uint32_t cap);
badge_val badge_push_field(badge_val t, const char *key);  // pushes t[key]: pop it
badge_val badge_push_index(badge_val t, int32_t i);        // pushes t[i + 1]: pop it
void badge_new_table(void);                // then badge_set_int/num/bool/str/bytes(key, ...)
```

A `badge_val` is a position in the current frame: valid until the function that got it
returns, and only while what is above it hasn't been popped. Nested tables:

```c
static void got_meets(void *user, badge_val res, const char *err)
{
    if (err) { ui_toast(err); return; }
    badge_val items = badge_push_field(res, "items");
    for (uint32_t i = 0; i < badge_len(items); i++) {
        badge_val m = badge_push_index(items, (int32_t)i);
        char peer[8];
        badge_field_str(m, "peer", peer, sizeof peer);
        /* ... */
        badge_pop(1);                       // m
    }
    badge_pop(1);                           // items
}
```

## The raw ABI

`badge.h` is built on `badge_abi.h`, the ABI itself (`SOS_ABI 1`): the functions a module
imports from module `"badge"` and the exports the badge calls. It is a small, checked
version of Lua's C API over the app's Lua state, so a new Lua function is reachable from C
the day it ships, before `badge.h` has a wrapper:

```c
// mesh.scan's "band" option, the raw way: mesh.scan({band = "5"}, cb)
sos_global("mesh.scan", 9);                // the function
sos_new_table();
sos_push_str("5", 1);
sos_set(-2, "band", 4);                    // {band = "5"}
sos_push_fn(my_callback, my_data);         // int32_t my_callback(void *ud, int32_t nargs)
if (sos_call(2, 0) != 0) {                 // failed: the error's text is on top
    char err[100];
    sos_to_str(-1, err, sizeof err);
    sos_pop(1);
}
```

| Group | Functions |
|---|---|
| The frame | `sos_top`, `sos_pop`, `sos_push_value`, `sos_remove` |
| Push | `sos_push_nil`, `sos_push_bool`, `sos_push_int`, `sos_push_num` (a 32-bit float), `sos_push_str` (bytes, copied), `sos_new_table`, `sos_push_fn` (a C function as a value), and `badge.h`'s `sos_push_cstr` |
| Read | `sos_type`, `sos_to_bool`, `sos_to_int`, `sos_to_num`, `sos_to_str` (into a buffer; returns the full length), `sos_len` |
| Tables and objects | `sos_get` (a field, or an object's method), `sos_set`, `sos_geti`, `sos_seti` (from 1), `sos_next` |
| The API | `sos_global` (a dotted name: `"ui.label"`, `"ui.W"`), `sos_call` |
| Kept values | `sos_ref` (pops and keeps; 1024 at a time), `sos_unref`, `sos_push_ref` |
| No stack | `sos_blit`, `sos_frame` and `sos_file_read` (2.3.0), `sos_keys`, `sos_millis`, `sos_log`, `sos_fail`, `sos_host_abi` |

- **The frame.** Each call into the app has its own frame: `on_start` starts empty, a
  callback starts with its arguments at 1..nargs. Indices are 1-based from the bottom or
  negative from the top (-1 the top); an index outside the frame stops the app (only
  `sos_type` answers `SOS_NONE` for one). At most 64 values a frame.
- **Calling.** Push the function (`sos_global`, or an object's method with `sos_get`),
  then its arguments (for a method the object first, as `self`), then
  `sos_call(nargs, nresults)`: 0 with `nresults` values pushed (`SOS_MULTRET`: all;
  count with `sos_top()`), or non-zero with one value, the error's text, which you pop.
  The non-zero value is the host's error code: 2 for an ordinary error (what an API
  function raises). Out of memory, running out of time and a trap in a nested callback
  always stop the app instead, whatever the caller wanted.
- **Names.** `sos_global` takes a dotted name of 1 to 96 characters; longer stops the
  app. A name that doesn't exist pushes nil.
- **C callbacks.** `sos_push_fn(fn, ud)` makes a function value that calls
  `int32_t fn(void *ud, int32_t nargs)` with its arguments at 1..nargs; it returns how
  many results it left on top. The value lives as long as the badge holds it. `fn` must
  be a function of the app with exactly that type; 0 or anything else stops the app.
- **Straight to the badge.** `sos_log` writes at most 1024 bytes a line (the rest is
  cut); `sos_blit` takes at most 4096 pixels a side and writes `0x0001` as `0x0000`
  ("Canvas and blit"); `sos_fail` stops the app with its text; `sos_keys` and
  `sos_millis` cost no stack.
- **The exports** are declared in `badge_abi.h` with their export names (`on_start`,
  `on_key`, `on_tick`, `on_exit`, `on_mesh`, `on_meet`), so defining one exports it and
  leaving one out is fine. `on_meet` signs by pushing a table and returning 1
  (`mesh_meet_accept` does both). `badge_abi()` (weak, defined in the header) tells the
  badge which ABI the app was built for; a badge refuses a newer one than its own.
- **One budget a callback.** Every call into the app made while a callback runs (a
  function value of `sos_push_fn` that an API function calls, a list's selection inside
  `on_key`) shares that callback's budget ("Limits"); at launch `_initialize` and
  `on_start` share one.

## Example

A tap counter that keeps its count, flashes the LEDs and pays 1 !LAMP per 100 taps (the
[Lua example](LUA_API.md#example), in C; `app.json`: `"lang": "c"`, `"permissions":
["led", "lamps"]`):

```c
#include <stdio.h>

#include "badge.h"

static ui_obj count_label;
static int32_t taps;

static void show(void)
{
    char text[16];
    snprintf(text, sizeof text, "%ld", (long)taps);
    ui_label_text(count_label, text);
}

static void lights_off(void *user)
{
    (void)user;
    led_off();
}

static void awarded(void *user, const lamps_result *r, const char *err)
{
    (void)user;
    if (err) return;
    if (r->awarded > 0) ui_toast("+1 !LAMP");
    else if (r->needs_x) ui_toast("Link X to earn !LAMPs");
}

void on_start(void)
{
    taps = storage_get_int("taps", 0);
    ui_label(0, 60, "Taps", &(ui_label_opts){.size = 20, .w = UI_W, .align = UI_CENTER});
    count_label = ui_label(0, 100, "", &(ui_label_opts){.size = 32, .color = COL_GREEN, .w = UI_W, .align = UI_CENTER});
    ui_hint("SELECT tap   CANCEL exit");
    show();
}

int32_t on_key(int32_t key, int32_t repeat)
{
    if (key != KEY_SELECT || repeat) return key != KEY_BACK;
    taps++;
    show();
    led_fill(0x14, 0xF1, 0x95);
    timer_after(80, lights_off, NULL);
    if (taps % 100 == 0) lamps_award(1, "100 taps", awarded, NULL);
    return 1;
}

void on_exit(void)
{
    storage_set_int("taps", taps);
}
```

More in `sdk/c/examples/`: `hello` (labels, keys, a timer), `starfield` (a game drawn a
frame at a time with `ui_canvas_blit` and `sys_keys`), `tally` (storage), `sol-price`
(`http` with a callback), `ping` (`mesh_send` and `on_mesh`), `sketch` (a canvas and a
bundled PNG) and `gameboy` (a Game Boy and Game Boy Color emulator: `ui_frame`, `files`,
`sys_tick`; "Heavy apps and emulators"). `sh sdk/c/examples/build.sh` builds them all.

## Develop and publish

As for Lua ([LUA_API.md "Develop and publish"](LUA_API.md#develop-and-publish)): the same
store, permissions, test runs, code check, review, allowances and limits. What differs:

- **Write** the app in the editor on `/apps/submit` with the language set to C, or drop
  a folder or .zip with `main.c` and `app.json`. The submit page compiles it on the
  server and shows the compiler's errors with their file and line.
- **Try in emulator** compiles the draft on the server and runs `app.wasm` in the
  emulator's WAMR interpreter, with every data file on the emulated card. `printf` and
  errors show below it.
- **Run on badge** compiles the draft on the server and copies `app.wasm` and the data
  files to a real badge over USB (Web Serial), which needs Solana OS 2.3.0. A draft
  runs in the interpreter there: only the store's install brings native code, so a
  published app that computes a lot runs much faster than its test run ("What runs where").
- **Locally**, build with `badge-cc` and copy the module over the serial console, which
  takes `app.wasm` like any app file (LUA_API.md "Console"):
  `lua recv <id> app.wasm <bytes> <sha256>` (and `app.json`, data files), then
  `lua run <id>`. The console never takes `app.aot`, and any console write to an
  installed app deletes its `app.aot`, which would no longer match.
- **`lua mem`** after a run says which form ran (`app.wasm (interpreted)` or
  `app.aot (native)`), the app's heap peak, and the most instructions (interpreted) or
  native time a callback used, against the limit; native, also the flash slot, whether
  this launch copied the code in, and how long the signature check and the slot took.
- **`lua aot <id>`** says why an installed app runs `app.aot` or `app.wasm`, without
  launching it: this build's target and the manifest's, whether the store's key is
  cached, whether the signature verifies, `app.aot`'s size against a flash slot, and what
  runs (`runs: app.wasm, interpreted: the store's key isn't on this badge yet`).
  `lua aotkey` fetches the store's key, with Wi-Fi on. When a launch finds no key or a
  signature the cached key doesn't verify (the store may have changed its key), the app
  runs `app.wasm` and, if the badge is online, it asks the server for the key again in the
  background, once per boot, so the next launch can verify; `lua aot <id>` then says so
  (`key refresh: refreshed: the server sent a new key 12 s ago`).
- **Speed.** To measure the real thing, publish the app unlisted, install it from the
  store on a badge and read `lua mem` there.
- **Publish** submits the source: the store compiles it itself, so the code check and the
  reviewers read the code that runs. Each version is compiled once with the toolchain in
  `versions`; when the native target changes, the store builds the native form again.

## Security

- **The sandbox.** A C app is a WebAssembly module: it reaches only its own linear
  memory, and the badge only through the imports listed in `toolchain/imports`, every
  one of which checks its indices, pointers and lengths. A module importing anything
  else doesn't load. Everything an app can do goes through the same Lua API, permissions
  and limits as a Lua app's, and the same confirmations (spending, transfers) that no app
  can answer.
- **WebAssembly from anyone.** The interpreter enforces the sandbox whoever compiled the
  module, so a badge runs an `app.wasm` from anywhere: the store, Run on badge, the
  console.
- **Native code only from the store, signed.** `app.aot` is machine code for the badge's
  CPU, and the sandbox then rests on the compiler that made it: the store's `wamrc`, from
  the module the store itself built and checked, with bounds checks on every memory
  access. So the badge runs an `app.aot` only when the store's installer wrote it (the
  console, Run on badge and quest downloads never write one, and any console write to
  the app deletes it), its target is the badge's own, its SHA-256 and size match the
  manifest's `code.aot`, and the manifest's `sig` is the store's Ed25519 signature of
  `sos-aot1|<target>|<sha256 in hex>`, checked against the store's public key (`GET
  /api/device/aotkey`, which the badge caches) at install and again at every launch,
  before the file is mapped; then the badge hashes the flash slot through its mapping, so
  it runs exactly the signed bytes. A file edited on the card doesn't run: it no longer
  matches what was signed, and the badge runs `app.wasm` instead. Each file must also be
  what its name says before WAMR's loader sees it (which would take either format from
  its first bytes): `app.wasm` WebAssembly, `app.aot` native code. Measured: with no key
  cached, a wrong key or a changed `app.aot`, the app ran from `app.wasm`; native code
  copied onto the card as `app.wasm` was refused, with no restart.
- **Your code is readable.** Anyone who installs a C app can read its `app.wasm` (and
  decompile it): an API key in it is no secret. Keep keys on your server and call it
  with `http`, as in Lua.
- **Input is hostile.** Mesh messages, Bluetooth values, socket messages and HTTP bodies
  can be anything. Check lengths before copying, use the buffer sizes `badge.h` asks
  for, and never trust a field's presence: the `badge_field_*` helpers return your
  default when it is missing or of another type.

### What players are told

Native code is where a C app asks for more trust than a Lua app: a Lua app's every step
goes through the interpreter, while a C app's sandbox rests on the store's compiler and
the runtime, and the chip has no isolation of its own to fall back on. A flaw there
would let a C app reach everything on the badge. So players are told, wherever they
decide to get or run a C app (docs/HHGTS.md "One look" keeps the canonical texts):

- **On the badge**, the App Store's details of a C app and the Apps library's info of an
  installed one with native code show, under the title *C apps can be more dangerous*:
  "Lua apps run in an interpreter. A C app runs as native code that only the store's
  compiler keeps in its sandbox: a flaw there could reach your whole badge." Installing
  or updating a C app from the App Store first asks the same in the OS's confirm dialog
  ("Install <name>?", SELECT yes, CANCEL no), once per install; a Lua app never asks.
  An installed C app without native code for the badge says instead "C app: it runs as
  WebAssembly in the badge's interpreter, not as native code." (`lua install` on the
  console doesn't ask: it is a developer's tool.)
- **On the site**, a C app's page shows the warning above its Buy or Add button: "A Lua
  app is a script: everything it does goes through the badge's interpreter. A C app from
  the store runs as native code on the badge's processor, with every memory access
  bounds-checked by the store's compiler and the same permissions and limits as a Lua
  app. The chip itself can't isolate it, though: a flaw in the compiler or the runtime
  would let it reach everything on the badge, its keys and !LAMPs actions included, so
  get it only if you trust its author." with a link here. The first Buy or Add of a C
  app in a browser session asks for an explicit "I understand".
- **Sideloads** (Try in emulator, Run on badge) of someone else's C app run the
  WebAssembly, so they say: "Here this C app runs as WebAssembly in the badge's
  interpreter, not as native code, with the same permissions and limits as a Lua app. It
  is still code someone else wrote: run it only if you trust its author."

The submit form tells authors that players see this.
