# Changelog

Solana OS releases, newest first, from this repository's history. The version is
`CONFIG_APP_PROJECT_VER` in `sdkconfig.defaults`; a badge takes an over-the-air
update only to a newer one. Dates are commit dates. The contracts these builds
implement (`docs/LUA_API.md`, `docs/MESH.md`, `server/API.md`) and the server and
site changes that came with them live in the parent repository.

## 2.3.1 (2026-11-16)

Fixes from the review of solana-os#23 (2.3.0 on main). Nothing new for apps.

- **A service's code waits for its record.** `start()` (`lua_svc.c`) put a fresh slot where
  the svc task could boot it while `services.json` was still being written, so a restart in
  that window left no "starting" mark and the next boot could start the same failing service
  again. The slot is held (`held`) until the write is done (or the app on screen asked for it,
  which runs without a record, as before); the svc task neither boots nor ends a held slot.
- **The Mailbox belongs to one server.** Its index keeps the server's origin (`server`); a
  badge moved to another server with the same owner drops the old server's messages and starts
  collecting from 0, where it used to ask the new server for ids after the old server's last
  and could acknowledge mail it never collected. A page or an acknowledgement answered by the
  old server after a change (`os_cloud_server_gen()`) counts as a failed collection. An index
  from 2.3.0, without `server`, is the current server's. The origin is read with
  `os_cloud_get_server()`, which copies it under the lock and allocates nothing, so a badge
  short of memory can't read it as "" and take its own server for another; with no server
  configured the mail stays and keeps its server's name.
- **Saved data survives a replace cut short** (`os_data.c`):
  - `os_data_write` puts a leftover `.tmp` (the only complete copy) back in place before
    writing; when that rename fails it now writes nothing, where it used to truncate the copy.
    The check opens the file instead of `stat`ing it (CodeQL's time-of-check alert).
  - `os_data_remove` removes the `.tmp` too, so a removed file (the Guide's cache, a mail)
    doesn't come back through `os_data_read`.
  - An app update copies `data.json` through `os_data_read`/`os_data_write`
    (`lua_install.c`), so storage that only exists as `data.json.tmp` moves to the new version
    instead of being deleted with the old folder.
- **`storage` frees its copy on an error.** Building the table from the shared copy
  (`lh_storage_load`, a refresh after the other side's write) runs protected and frees the
  JSON copy before raising "not enough memory" again; the copy is outside the Lua heap, so an
  app near its limit leaked up to 16 KB per failed decode.
- **`files` reads what it checked**: `lh_files` opens the file, then `fstat`s it (CodeQL).
- **`ui_frame`'s cheap rate only without a copy.** The host charged 1 step per 256 pixels for
  every unscaled big-endian frame, also an unaligned one the badge copies (16 times the work).
  One predicate, `bsp_display_frame_no_copy()` in `bsp.h`, now decides both the charge and the
  path, in both builds (the emulator copies every frame but charges the same).
- Measured on a production badge (4ZT2BX, C5 rev 1.0): `tests/t-svc-store` (an app and its
  service, 200 keys each, all kept, read back from the card after a relaunch) and
  `tests/t-ownfiles` pass, the heap back at the launcher's after each; the emulator's
  `tests/svc-run.ts` passes (42 checks). Not tested on hardware: a server change with mail
  waiting, a failed `.tmp` rename, an update over a `data.json.tmp`, a storage decode that runs
  out of memory.

## 2.3.0 (2026-11-15)

C apps, background services with the first two built on them (Mailbox and
Collaborator), and apps' reads of the player's inventory, on 2.2.0's ESP-IDF master. Two
server features of the same release show on the badge unchanged: the update quest and
`lamps.award` allowances (below).

- **C apps.** Store apps written in C (`docs/C_API.md`; the SDK in `sdk/c/`): the store
  compiles an app's C to WebAssembly, and the badge runs it in
  [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) 2.4.5's classic
  interpreter, vendored in `main/lua/wamr/` (a subset, Apache-2.0 WITH LLVM-exception;
  its changes are listed in its README and marked "Solana OS"). The Lua host stays the
  host: a C app gets the usual Lua state with every API module, its "main chunk" is the
  module (`main/lua/lua_wasm.c`), and the module's imports (`sdk/c/include/badge_abi.h`,
  `SOS_ABI` 1) are a small checked Lua C API over that state, so a C app has exactly the
  Lua API, permissions and limits, and the host's events, timers, sockets, Bluetooth,
  error screen and `lua mem` work for it unchanged. wasi-libc's few WASI calls are
  `main/lua/lua_wasi.c` (stdout and stderr to the console as `print`, the clock, random
  bytes, `proc_exit` as `sys.exit()`). A module importing anything else doesn't load, and
  the error names the import; one built for a newer ABI is refused with an update hint.
- **Checked, and errors never cross WAMR.** Every index, pointer and length is checked, at
  most 64 values a frame and 1024 kept values (`sos_ref`); an app's mistake stops it with
  a readable message. Imports that can raise a Lua error run protected and turn it into a
  WAMR exception; the Lua error is raised only once WAMR has unwound (the one jump across
  WAMR is the watchdog's, out of stuck native code, below).
  `sos_call` is `lua_pcall`, but out of memory, the budget and a trap in a nested callback
  always stop the app. Traps read as words on the error screen ("The app read or wrote
  outside its memory (out of bounds memory access)", "The app divided by zero", ...),
  titled "App error", "App took too long" or "Out of memory".
- **One heap cap, two budgets.** WAMR's allocations (module, instance, linear memory, its
  32 KB stack) go to the app's counted heap through Solana OS's platform layer
  (`main/lua/lua_wasm_port.c`), so the module and the Lua state share the 6 MB cap in
  PSRAM with its reserve, and `sys.memory()` reports both; `memory.grow` past it answers
  -1 (`malloc` returns NULL). WAMR starts and stops around each C app. CPU, interpreted:
  WAMR's instruction metering, `LUA_HOST_WASM_STEPS_MAX` (2 000 000) wasm instructions a
  callback, shared by the callbacks nested in it (a patch: stock WAMR gave each nested
  call a fresh budget). Native: `LW_AOT_CALLBACK_MS` (200 ms) of native time a callback,
  shared the same way; the badge's own work in an import or in growing memory doesn't
  count. Host work (fs, crypto, canvas drawing) still counts against the 200 000 host
  steps. Over either: "App took too long".
- **Every way a callback spends time is bounded** (from a security review). A callback
  holds the LVGL lock, so the badge's UI, HOME included, waits for it:
  - *Bulk memory, interpreted.* `memory.fill`, `memory.copy` and `memory.init` cost one
    instruction per 4 bytes (`SOLANA_OS_WASM_BULK_BYTES` in `main/sources.cmake`, a patch
    in `wasm_interp_classic.c`), charged before the work. Before, one opcode cost one
    instruction whatever its length: 3 000 fills of 3.75 MB in one callback were charged
    19 563 instructions. Measured on a badge: `memset` 12 KB/ms and `memmove` 8 KB/ms in
    PSRAM against 2 200 to 4 500 plain instructions a ms, so 4 bytes take about an
    instruction's time and a callback moves at most 8 MB.
  - *Bulk memory, native.* wamrc's run-in-place code calls `memset`, `memmove`/`memcpy`
    and `aot_memory_init` through the native symbol table; WAMR's `aot_memset` and
    `aot_memmove` now go to `lw_aot_memset`/`lw_aot_memmove` (`main/lua/lua_aot.c`), which
    work in 4 KB pieces and leave through the innermost `setjmp` once the app must stop
    (a patch in `aot_runtime.c`; `memory.init` uses them too). Their time is native time.
    Before, the program counter sat in the runtime's `memset`, never in the module, so the
    watchdog never fired and a loop of them never stopped. Measured: a loop of 3.75 MB
    `memset`s or `memmove`s stopped after 200-201 ms of native time, of `memory.init`s
    after 207 ms. The other helpers native code can reach (soft-float, 64-bit division,
    the intrinsics, `aot_call_indirect`, `aot_check_app_addr_and_convert`) return within
    microseconds; the watchdog, when the program counter isn't in the module, now tries
    again after a random 0.25 to 1.25 ms instead of 10 ms (a soft-float loop stopped
    after 207-222 ms, passes 3 to 21). Growing the linear memory is refused (`memory.grow`
    answers -1) once the app must stop.
  - *Imports, by length.* What an import does for the app costs host steps
    (`lw_charge`, `main/lua/lua_wasm.h`): 1 a call, 4 more for `sos_call`, 1 per 4 bytes
    copied (`sos_push_str`, `sos_to_str`, `sos_get`/`sos_set` keys), 32 per byte written
    to the console (`sos_log`, `fd_write`, and Lua's `print` when a C app calls it), 6 per
    random byte; `fd_write` takes at most 4 buffers and 1 KB a call (a short count, which
    stdio carries on from) and `random_get` at most 4 KB (`EINVAL` past it). Measured on
    a badge: the console writes 11 bytes a ms, `esp_fill_random` 520 bytes a ms; loops of
    1 KB writes stopped after 6 KB of output, a 2.5 MB push before copying, and the SDK's
    API probe (92 checks in one `on_start`) used 119 886 of the 200 000 steps. Before,
    2 000 pushes of 2.5 MB in one callback cost no host steps. The host-steps message is
    now "Over 200000 host steps in one callback (copies, output, fs, crypto, drawing)".
  - *The ceiling.* One callback of a C app ends after 3 s of wall-clock time whatever it
    does (`LW_CALLBACK_MAX_MS`): every import checks it, and on a badge the watchdog
    (which now watches interpreted apps too) sets the stop flag, aims native code at the
    trampoline and zeroes the interpreter's remaining count, which the interpreter reads
    after every import and at every bulk memory operation. "App took too long" ("The app
    ran over 3 s in one callback"). Measured with the other limits raised in a test build:
    a native loop, a native `memset` loop, native and interpreted loops of imports and an
    interpreted `memset` loop each stopped 3.0 to 3.3 s after the callback began.
- **Nothing runs outside the budget at load.** WAMR runs a module's start function, and
  exports named `__wasm_call_ctors` or `__post_instantiate`, while it instantiates the
  module, on an exec env of its own with no instruction limit: a 114-byte module looped
  there for as long as it liked. The SDK makes none of them (its constructors run from
  `_initialize`, under the budget) and the server refuses a start section, but sideloads
  and edited cards never pass the server: the badge now refuses all three before
  instantiating, for `app.wasm` and `app.aot` alike ("The app has a start function, which
  the badge doesn't run (rebuild it with the SDK)", "The app exports __wasm_call_ctors,
  which the badge doesn't run (rebuild it with the SDK)"). Data and element segments are
  bounded by the file; the initial memory by the heap cap.
- **Each file is the format its name says.** WAMR's loader picks WebAssembly or native
  code from the first bytes, and the badge's WAMR has the AOT runtime, so an AOT file
  sideloaded as `app.wasm` went to the AOT loader unsigned and then jumped into the PSRAM
  heap (a fault and a restart, since that memory isn't executable). The badge now checks
  the magic before the loader sees the bytes: `app.wasm` must start with `\0asm`
  ("app.wasm is native code, not WebAssembly: only the store's signed app.aot runs
  natively", or "app.wasm isn't WebAssembly"), `app.aot` with `\0aot`. Measured: refused
  on a badge, no restart.
- **The store's key, after a rotation.** A launch that finds no key or a signature the
  cached key doesn't verify still runs `app.wasm`, and now, when the badge is online,
  asks the server for the key again in the background, once per boot (never in a loop;
  an offline launch leaves it to a later one), so the next launch can verify; `lua aot
  <id>` shows the refresh and its outcome.
- **The package.** `app.json` `"lang": "c"`; the code is `app.wasm` (interpreted) and,
  when the store made one for this badge's target (`AOT_TARGET` in
  `sdk/c/toolchain/versions`), `app.aot`; no `main.lua`. The App Store installs `app.wasm`
  and, when its target matches, it fits a flash slot (1 MB less 4 KB) and the store's
  signature verifies, `app.aot`, each checked against the manifest's SHA-256; only the
  installer writes `app.aot`. The console's `lua put`/`recv` take `app.wasm` (never
  `app.aot`), any console write deletes `app.aot` as it deletes `code.bin`, `lua sums`
  lists `app.wasm`, `lua list` marks C apps and `lua mem` shows the module, its linear
  memory, and the most wasm instructions (interpreted) or native time (native) in a
  callback. App Store and Apps show a small "C" on C apps.
- **Native C apps.** `app.aot` (wamrc `--xip`, `sdk/c/toolchain/wamrcflags`) runs in place
  from the flash slots Lua's `code.bin` uses (`main/lua/lua_xip.c`, `LUA_XIP_EXEC`: an
  instruction mapping above the PSRAM heap's protected region, where the C5 executes;
  no RAM is made executable, no sdkconfig change). The slot is copied in unless it holds
  the code already, and its bytes are hashed through the mapping at every launch; the
  mapping stays until WAMR unloads the module. The seam is `main/lua/lua_aot.c`; the
  executable-memory hooks it had are gone (a file that isn't `--xip` asks for executable
  RAM and doesn't load).
- **Only the store's native code runs.** The store signs `sos-aot1|<target>|<sha256>`
  with Ed25519 and puts the signature in the manifest's `code.aot.sig`; the badge fetches
  the store's key (`GET /api/device/aotkey`), keeps it in NVS (`cloud`/`aotkey`, dropped
  with the server identity), checks the signature at install (fetching the key again
  once if it fails: rotation) and again at every launch before anything is mapped. No
  key, no signature, a bad one, a file of the wrong size or a slot that doesn't hash to
  the signed SHA-256: the app runs `app.wasm`, and without one the error screen says it
  can't start. The deciding half is portable (`main/lua/lua_aot_trust.c`, both builds:
  the key's fetch and cache, the signed message, the Ed25519 check, the target and slot
  checks, the launch's decision); only running native code and the watchdog are the
  badge's (`main/lua/lua_aot.c`). The installer checks the signature before it asks
  whether the hardware runs native code, so the emulator fetches the key and verifies
  every `code.aot.sig` too (then takes `app.wasm`); `app.json` keeps `code.aot` whichever
  form was installed. A slot found by the signed SHA-256 is mapped without reading the
  card and hashed, so what runs is the signed code even if `app.aot` on the card changed.
- **`lua aot <id>`** says why a C app runs `app.aot` or `app.wasm`: this build's target,
  the manifest's, whether the store's key is cached (its first 8 hex digits), whether
  the signature in `app.json` verifies, `app.aot`'s size, whether it fits a slot and is
  the signed SHA-256, and which form runs and why. `lua aotkey` fetches the key from the
  server now and caches it (one word: `lua aot key` was `lua aot <id>` for an app whose
  id is `key`). Neither ever writes `app.aot`.
- **The C app warning** (`docs/C_API.md` "What players are told", `main/apps/c_warning.h`):
  the App Store's details of a C app and the Apps library's info show "C apps can be more
  dangerous", and installing or updating one asks first in the OS's confirm dialog, once
  per install (never for a Lua app; `lua install` doesn't ask).
- **Stopping stuck native code.** An `esp_timer` checks the running callback's native time
  every 10 ms; past 200 ms it sets a flag every import checks on entry and return, and
  when the app's task is preempted inside the module's code it points the task's saved
  program counter at a trampoline that `longjmp`s to the innermost call into the module.
  Only the module's frames and WAMR's call path lie between (each nested callback has
  its own `setjmp`); the error then travels the normal way and the app ends with "App
  took too long" ("Native code ran over 200 ms in one callback"). WASI imports keep
  WAMR's libc-wasi signatures for native apps: wamrc compiles them as direct calls.
- **Measured on a badge** (HYQ5VS, prototype, ESP32-C5 rev 1.2; USB; Wi-Fi off). Every
  regression app in `main/lua/tests/c/` interpreted and native: each trap, bad pointer
  and index, the memory cap, the unknown-import and wrong-ABI cases on the error screen,
  never a restart. A stuck loop stopped natively after 204-210 ms (first watchdog pass);
  a callback that spins, called through `pcall` from native code, after 203-207 ms; a
  soft-float loop (the program counter mostly in the helpers) after 236-295 ms (pass 4
  to 10); a loop calling an import after 208-210 ms of native time. Interpreted, the same
  apps stop at 2 000 000 instructions, with no task-watchdog warning. Launch of a native
  app: the signature check 57-76 ms every time; the slot copy at first launch 34-58 ms
  for 1-4 KB, 290-380 ms for 46-76 KB; later launches map and hash a 46 KB slot in
  3.6-3.8 ms (about 120 ms from `lua run` to the app booted, against about 460 ms the
  first time and 90 ms for the same app interpreted). Through the bridge: the cheapest
  import 4 us native, 7 us interpreted; a full 320 x 216 canvas `sos_blit` 16.4-16.7 ms
  either way (17.3 ms in the benchmark below, which also has the label figures). The
  interpreter ran a plain loop at about 4 500 wasm instructions a ms. A starfield frame (examples/starfield's drawing, 160 x
  108 at 2x) takes 7 ms native, 33 ms interpreted: 19.4 and 12.6-13.2 frames a second
  (the screen is the limit natively). Lua task stack left (of 12 KB): 7.2 KB native and
  7.0 KB interpreted for hello, 2.4 KB native and 3.8 KB interpreted at the deepest
  nesting the test reaches (it ends with "The app ran out of stack"). SRAM free for
  malloc: launcher 125.4 KB, hello running 110.2 KB native / 112.0 KB interpreted, the
  speed test 113.0 / 113.0 KB; PSRAM free 8.36 MB at the launcher, 8.24 / 8.22 MB with
  hello. 20 launch and exit cycles of a native app: SRAM free 157 KB before and after,
  largest block 96 KB before and after. A Lua app with `code.bin` ran in place from a
  slot next to native code in the others; `t-crypto` and `t-coro` pass.
- **The Lua task yields** a tick at least once a second even when frames leave no wait
  between ticks: a C app drawing the whole screen otherwise starved the idle task and
  the task watchdog warned (seen on a badge).
- **The installer's acceptance step, on a badge** (a test build running the installer's
  own `install_c_code` on a task of its stack size and priority, with apps signed by the
  server's code, `server/src/aotsign.ts`): the store's signature verifies, `app.wasm`
  and `app.aot` are hashed and kept; a tampered `app.aot` is refused. The task's stack
  (5 120 bytes, unchanged) never had less than 748 bytes left over a measurement frame
  about 300 bytes bigger than the worker's; reading the manifest (`lua_pkg_load`) and
  the card and logging go deepest, not the signature check or SHA-256 (both stay above
  it). The console's 5 KB REPL task keeps 1.4 KB after `lua aot`. On the final build:
  hello and starfield native from their slots, a stuck loop stopped after 206 ms (pass
  1), a changed signature and a changed `app.aot` (its slot doesn't hash to the signed
  SHA-256) run `app.wasm`.
- **C against Lua, measured** (`sdk/c/bench/`: the same thirteen workloads as a C app
  and a Lua app with matching checksums, and `run.py`, which drives them over the
  console; the tables are in `docs/C_API.md` "Speed and limits: C against Lua"). On
  HYQ5VS (USB, Wi-Fi off, a test build that also took a native file over the console,
  and with `kernels.c` in the firmware at GCC `-O2` behind a console command, kept out of
  the tree): the native form ran at 0.97-1.25 times the firmware's time on eight of ten
  workloads (integer Mandelbrot 62.5 / 50.4 ms a frame), 1.97 on SHA-256 and 2.41 on a
  frame small enough to stay in the cache (byte-wise 32-bit accesses and bounds checks);
  interpreted C ran at about Lua's speed on integers (Mandelbrot 5 283 / 5 307 ms) and
  85 times slower than native; native C was 50-130 times faster than Lua on integer and
  pixel work, 3.1 with floats, 2.1 copying, and slower than Lua through the API (a
  label's text 357 us native, 625 interpreted, 181 in Lua). Measured limits: Lua got
  5.50 MB of strings before "The app needs more than 6 MB", C 3.94 MB of `malloc` before
  `NULL`; 17 KB of SRAM for an open app in all three; recursion 303 levels native, 378
  interpreted, over 26 000 in Lua. A second full pass with Wi-Fi joined matched within
  10 % (two Lua canvas rows 18-28 %). wamrc's `+unaligned-scalar-mem` made native files
  25 % smaller and SHA-256 1.6 times faster, and misaligned 32-bit accesses worked on
  this badge; not adopted (it needs a new `AOT_TARGET` and a production badge's check).
- **What it costs.** About 120 KB of flash (2.1.0's image 2 340 704 bytes, now
  2 463 216). Nothing while no C app runs.
- **Not measured on a badge yet:** a real store install over the network (fetching the
  key from a server that serves it, then downloading `app.aot`; checked end to end in
  the emulator against a real test server instead, `server/test/cemu.test.ts`), the TLS
  floor with a C app open (the benchmark ran a full pass with Wi-Fi joined and the badge
  registered with its server), a production badge (C5 rev 1.0), battery.
- **`tools/cc/badge-cc --aot`** refuses a native file a badge can't run in place (not
  `--xip`, or a `.rela.text` code relocation group), so a local build fails where the
  store would.
- Tests: `main/lua/tests/c/` (regression apps for every import, the callbacks, each trap,
  the limits, the console and a store install against a stand-in server, and the SDK's
  `sdk/c/tests/host` probe of `badge.h`, 92 checks; `build.sh`, then
  `bun main/lua/tests/c/run.ts`, headless in the emulator), and
  `server/test/cemu.test.ts` (the emulated badge against a real test server: publish,
  install with the key fetched and the signature verified, run, `lua aot`, tampering).
- **Heavy C apps and emulators**, measured on a production badge (4YGWAV, ESP32-C5 rev 1.0)
  on ESP-IDF 6.0.1, before the move to master; the tables are in `docs/C_API.md` "Heavy
  apps and emulators".
- **`ui_frame` / `sos_frame`**: a C app's RGB565 frame straight to the screen's SPI DMA,
  past LVGL (`bsp_display_frame`), returning while the screen still takes it. An unscaled
  big-endian frame in a 64-byte-aligned buffer goes with no copy (1.0 to 1.4 ms for
  160 x 144, against 6.5 to 6.9 copied and 17.3 ms for a 320 x 216 `ui_canvas_blit`);
  others are converted (scaled, nearest) into a PSRAM buffer kept until the app exits.
  Nothing while a dialog, the keyboard or the OS panel is up; only the rows above a toast.
  The linear memory is 64-byte aligned (`lua_wasm_port.c`) and `memory.grow` waits for a
  frame in flight, as the app's exit does, so the DMA never reads memory that moved.
  Screenshots show copied frames.
  On ESP-IDF master the SPI bus takes a whole screen per transaction and the panel's DMA
  reads PSRAM directly (`psram_dma_direct`): master's esp_lcd stopped splitting big buffers
  and bounced PSRAM ones through SRAM. Measured there: 9.2 ms a 160 x 144 frame without a
  copy (the wire at 40 MHz), 15.0 copied, 31.4 scaled 1.5x (`tests/c/t-c-fbench`).
- **`files`** (permission `files`, Lua and C): the app's own folder `hh/files/<id>/` for
  files the owner copies there (read-only) and its saves in `save/` (1 MB a file, 32
  files); names, never paths. C reads go straight into the app's memory
  (`sos_file_read`). The console takes them: `lua recv <id> files/<name> ...` (32 MB).
  Wiped with `/sdcard/hh` by a reset or a new owner. A background service reads them
  (`files.dir`, `list`, `size`, `read`) but doesn't write; only the app does.
- **`profile.items` / `profile.pages`** (permission `inventory`, bit 13; Lua and C:
  `profile_items`, `profile_pages`, `profile_item_info`, `profile_page_info`): the badge owner's
  items (id, kind, name, the app or quest that made it; never another app's data) and the
  Guide pages they hold (with copies), paged by the server's cursor, 10 an answer. A
  background service may read them. The permission reads "See the items and Guide pages
  you hold" in the store details and Apps info. Regression apps `tests/t-inv`,
  `tests/c/t-c-inv`. Measured on 4YGWAV (production; both installed from a store): 30 items
  in 3 calls and 20 pages in 2, no item's data; 143 KB of SRAM free at the launcher (86 KB
  largest block), ~130 KB while the Lua or the native C app walks (70 KB), 143 KB again
  after each closes.
- **The update quest** (server side, the parent repo's server/API.md "The update quest"):
  when an admin offers a build to every badge, running it pays 100 !LAMPs once per X
  account, with a Mailbox notice from Solana. No firmware change: Quests > Quest log shows
  it as a row ("Update to Solana OS 2.3.0", its step "Settings > Software update on your
  badge"), the last list it got when offline. Seen on 4ZT2BX: 2.2.0 listed it, updated over
  the air, its next hello paid once and the notice arrived.
- **`lamps.award` allowances** (server side, docs only here: `docs/LUA_API.md`,
  `docs/C_API.md`): an author asks for one when publishing; an organiser grants it, or
  another amount, and the author's badge gets a notice from Solana. Seen on 4YGWAV: asked 50,
  granted 30, three awards of 20 paid 20, 10 and 0.
- **`sys.tick(ms)` / `sys_tick()`**: on_tick's period, 16 to 1000 ms; an app that sets it
  is woken by a one-shot timer, so 16 ms is 16, not the scheduler's 20. A service keeps its
  1 s tick: `sys.tick` is nil there.
- **`sdk/c/examples/gameboy`**: a Game Boy / Game Boy Color emulator (Walnut-CGB 1.2.4,
  MIT): native, real homebrew runs at 56 to 60 emulated fps and 45 to 56 shown; games
  that never idle at 23 to 35 (double speed 14 to 18); interpreted it is unplayable (one
  frame over 3 s). GBA: no (SkyEmu's core, 0.7 fps as an app, 2.6 in the firmware).
- **`wamrc +unaligned-scalar-mem` stays off**: on the production badge it made an
  emulator 1.7 times slower (faster on the prototype). Bounds checks cost an emulator
  about 30 %; they stay on.
- **Background services** (`main/core/os_service.h`, the parent repo's docs/HHGTS.md
  "Background services"). One registry of everything that runs while its app is closed:
  built-in services register at boot (id, name, status line, state, start/stop, whether
  the owner may stop it, start at boot remembered in NVS `svc/<id>`, an unread count), and
  app services (`background` permission, `docs/LUA_API.md` "service", `docs/C_API.md`
  "service") show up through one provider hook from the Lua host. The mesh, X Dex
  swaps, Trade invites, the quest relay and the server link are listed (always on).
  `os_service_notify()` is the one way a service tells the owner something (a toast
  `<Service>: <text>`, an optional LED flash, one every 10 s); the status bar shows a bell
  and the count while any service has something unread.
- **Services** (`services`, Purple, after Apps): the list, each service's status, memory and
  start-at-boot, stop (after a confirm) and start. Console: `svc [list|start|stop|boot|test]`;
  `svc test on` and a restart register a test-only service. Measured on a prototype: opening
  and closing Services five times left the largest block as it was (98 304 B); the free heap
  moved by what the console's history costs per command (~40 B).
- **App services** (`main/lua/lua_svc.c`, the `background` permission, `docs/LUA_API.md`
  "service", `docs/C_API.md` "service"): an app's `service.lua` (a C app: a second
  instance at `on_service_start`) keeps running after the app closes, in a state of its own
  on one "svc" task: 1 MB of PSRAM heap, 100 000 instructions and a second per callback,
  2 000 000 a minute, a tick a second, 4 timers >= 1 s, 8 events waiting, 3 services at once.
  Its globals leave out the screen, LEDs, Wi-Fi, Bluetooth, sockets, the radio's channel and
  everything that spends. The owner's yes is an `os_confirm`; `/sdcard/hh/services.json`
  keeps it; services that were running start again ~10 s after boot; stops: the owner
  (Services, `svc stop`), the app, removal, updates (paused around the swap), quest-app
  expiry, three failures, low battery, low memory, a software update. The app and its
  service share `storage`. `lua_host_call()` lets built-in code call a named callback in an
  app's service and app; Mailbox's `on_mail` reaches a service through `lua_svc_call()`, and a
  service reads its mail (`mail.list`, `mail.get`) but can't send any. C services share the one WebAssembly runtime:
  they pause while a C app is open. Measured on a prototype: the svc task's 12 KB stack is
  the SRAM cost (117 060 B malloc-able at the launcher with none, 103 412 with one, 103 944
  with three); 20 stop/start cycles left the largest block as it was.
- **Mailbox** (`game/mailbox.c`, `apps/mailbox/`, `lua/lua_mail.c`; the parent repo's
  docs/HHGTS.md "Mailbox" and server/API.md "Mailbox"). A built-in background service
  (`mailbox`: stoppable, starts at boot) collects the owner's notifications from Solana
  (admins and admin keys), apps' servers and other badges' apps whenever hello says some wait
  (`mail`, 0-99), which includes the first hello on Wi-Fi: pages of `GET /api/device/mail`
  and an ack. Up to 50 on the card (`/sdcard/hh/mail/`, the oldest read one goes first),
  in PSRAM while it runs (~23 KB), the text read on first use; everything from the server is
  cut to its field and printable ASCII. A toast `Mailbox: <sender>: <title>` with a Yellow
  flash, and the status bar's bell. **Mailbox** (Yellow, after My Badge): the list, the
  reader (open the app it came from, delete, mute the app, report and block another
  badge's sender), muted apps; offline it shows the card's copy. Console: `mail [status|
  list|show|sync|hello|read|rm|clear]`.
- **`mail`** for apps (permission `mail`, `docs/LUA_API.md` "mail", `docs/C_API.md` "mail"):
  `mail.send` (through the outbox, offline too; a second apart), `mail.list`, `mail.get`,
  `on_mail` (while the app is open, and when Mailbox opens the app on its mail); C:
  `mail_send`, `mail_count`, `mail_item`, `mail_get`, `mail_arg`, the `on_mail` export.
  `tests/t-mail`.
- **Collaborator** (`collab`, Green, after Services; `main/game/collab.c`, `main/apps/collab/`;
  the parent repo's docs/HHGTS.md "Collaborator", server/API.md "Collaborator", docs/MESH.md
  "Collaborator stations"). The owner lends the badge to the event: off by default, turned on
  in its app after the OS's own consent dialog, then a built-in background service that starts
  at boot until turned off. While on it syncs with the server every 30 s (live, a trickle of
  !LAMPs the server decides); an admin may take it over: APP messages sent from a **station**
  id the server gives (its own HELLO, `k:"s"`, next to the badge's, never the owner's id or
  key), what nearby badges send to the station forwarded back, and, when the owner lends the
  screen, a card, QR code, image (downloaded to the card), colour or countdown and an LED
  pattern on the hidden takeover screen, shown only when the badge is idle and never over an
  install, update, trade, swap or confirm. HOME or CANCEL there ends the takeover (forfeited);
  the status bar's green eye shows whenever it's taken over. Ends under 15 % battery off USB.
  Console: `collab [status|on yes|off|end|sync|lend on|off]`. `os_mesh_station()`,
  `OS_PEER_STATION` (Lua `mesh.nearby()` kind `"station"`), two holders of keep-Wi-Fi, the
  radio kept awake while anyone needs it; `os_service_def_t.mark` (a status-bar symbol) and
  `os_service_mark()`; `os_held()`, `os_last_key_ms()`.
- **Measured on a production badge** (4ZT2BX, C5 rev 1.0, Wi-Fi joined, at the launcher):
  141-143 KB of SRAM free, 110 KB of it for malloc, an 86 KB largest block, idle and with
  Collaborator live; one to three app services cost ~13 KB and split the largest block to
  57 KB while the svc task lives (back to 86 KB once the last one stops); 15 minutes of
  everything at once (Mailbox collecting, Collaborator live, a service, an app opened and
  closed every minute) held steady, no restart. Over the air: 2.2.0 -> 2.3.0 under the
  2.1.0 bootloader and 2.1.0 -> 2.3.0, each confirmed at the launcher; a 2.3.1 that aborts
  before the launcher rolled back; app services stop for an update and start again after
  it. The Game Boy example from the store, native and run in place: Libbet 53.5-60 emulated
  and 48-59 shown fps, cgb-acid2 ~59.8 and ~48.5.
- **From the hardware pass**: a service's crash guard counts only restarts the badge didn't
  make itself (an orderly `reboot` or update within a service's first minute skipped it),
  and why starting at boot went off survives a restart (`services.json` `why`); a long
  Collaborator card title wraps instead of running off the screen; `mesh peers` names
  stations; the console keeps one line of history, not 32 (each line a long-lived strdup in
  SRAM).
- **From the pull request's third review**: an app and its service closing at the same time
  no longer lose the last change of one of them. A state's last save that found the other
  state writing used to leave the change to that writer and close, and the writer, holding
  an older copy and closing too, freed it. Now a last save waits for the write in flight
  (at most 2 s) and then writes the newest copy, so every version committed before it began
  reaches the card; later ones belong to the other state, whose own last save does the same.
  Measured on 4ZT2BX with tests/t-svc-final (the service's last save on the card while the
  app sets a key and exits, 300 rounds): 2.3.0's first build lost the app's key in 20,
  this one in none; a close that waited ended within 280 ms of the other's write.
- **From the pull request's second review**: a state that ended before it loaded `storage`
  (a service whose `service.lua` doesn't compile) no longer releases the app's shared copy,
  which made the app's later `storage.set()` calls succeed without being kept: each state
  releases the reference it took, once; unsaved changes belong to the shared copy, so the
  last state to close writes them whoever made them, and the two never write `data.json` at
  once. A replaced card file whose rename failed is put back in place before the next write.
  The app's own `service.start` runs the service even when the card can't record the start
  (it used to be stopped with a bare "stopped"); a start nobody asked for that can't be
  recorded is refused with a reason (`card error`), and a start of a service already
  running never stops it.
- **From the pull request's review**: an app and its service writing different `storage`
  keys at once no longer lose one (the shared copy is replaced only if nobody replaced it
  since; else rebuilt and tried again; of one key the last writer wins: measured on a
  badge, 15 of 200 keys lost before, none after); a copy that can't be made is an error,
  never "empty", and a failed save stays pending; `data.json` that exists but can't be read
  stops the app instead of starting it empty; a replaced card file that couldn't be renamed
  into place survives as `.tmp` and is read from there. An update restarts the app's
  service only after the installer has finished, and a refused restart is retried (ten
  updates in a row on a badge, the service running after each). Mailbox acknowledges only
  what the card saved (a full or failing card leaves mail on the server); without a card it
  acknowledges what it keeps in RAM. The takeover screen starts its LED pattern when it opens
  and stops it when it closes (`collab` shows the LED state). A service start that can't be
  recorded in `services.json` doesn't happen unless the app on screen asked.
- **From the review**: a C app's `files_read` with a bad name pointer stops the app (it
  could restart the badge, and from a service's start on every boot); a service whose start
  never finished (the badge restarted under it) is skipped at the next boot and turned off
  after three; a C service reads no buttons (`sys_keys` 0) and draws no frame (-1), the
  rest of what it can't use returns `BADGE_ERR` (C_API.md "service"); the services' mesh
  handler copies what it needs under its lock; a Collaborator image arriving during a
  decode waits for it; `sys.tick`'s timer and Collaborator's two are made at boot.

## 2.2.0 (2026-11-15)

Only the move to ESP-IDF master; nothing else changes from 2.1.0.

- **Built with ESP-IDF master** (was 6.0.1), pinned to commit `4109c8923a5`
  (v6.1-dev-9359, 2026-11-12, `IDF_VERSION` 6.3.0) in `esp-idf.commit`: the top
  `CMakeLists.txt` stops a build on any other commit of ESP-IDF (`SOLANA_OS_IDF_UNPINNED=1`
  makes it a warning, to try a newer master). Toolchain GCC 16.1 (was 15.2), C built as
  `gnu23`. `tools/build_web.sh` sources `~/.espressif/master/esp-idf/export.sh`.
- **`components/esp_http_client/` is gone**: master has the keep-alive fix it carried
  (esp-idf 44987dfd14, merged as 0e658fa1311).
- **Over the air from older builds**: a master-built app boots on the 6.0.1 bootloader
  of 2.0.0/2.1.0 badges and on the 5.5 bootloader of badges first flashed before 0.5.4,
  brings PSRAM up and confirms itself after an update. `/stock` writes master's
  bootloader; the partition table is byte-identical to 2.1.0's.
- **Rollback**: master renamed `BOOTLOADER_APP_ROLLBACK_ENABLE` and confirms a new app at
  startup by default; `sdkconfig.defaults` keeps "confirm by app"
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_CONFIRM_BY_APP`), so a build that never reaches the
  launcher still rolls back.
- **The SRAM figure malloc can take**: master gives the 32 KB DMA reserve pool
  `MALLOC_CAP_DEFAULT` at its lowest priority (esp-idf 90a275f29ac), so
  `heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT)` counts it although
  malloc reaches it only after PSRAM. `os_mem_sram_malloc_free()`/`_min()` (`os_mem.h`)
  take it off; the console's `sram:` line and the Lua UI guard (`lh_ui_room`) use them.
- **CS pins**: master's `gpio_config()` reserves output pins, so driving both chip selects
  high before the bus starts (and the card's after sdspi lets go) warned of a conflict
  each time esp_lcd or sdspi took the pin. `bsp_sd.c` drives them as plain outputs.
- **SRAM**: the unused SPI-slave and parallel-IO ISR options no longer pull the GDMA
  control and link-list functions into IRAM (−1.8 KB). Master's Wi-Fi libraries still
  take ~2.5 KB more static SRAM than 6.0.1's: ~3.7 KB less free heap than the same
  source on 6.0.1. The app is 42.7 KB bigger (2,383,376 bytes).
- **Verified on a production badge** (C5 rev 1.0): an over-the-air update 2.1.0 → 2.2.0
  under the 2.1.0 bootloader (confirmed itself at the launcher), a rollback from a build
  that aborts before the launcher, the 5.5.4 bootloader of 0.5.3, and the Lua regression
  apps t-crypto, t-psram and t-uiset.
- `os_http`'s per-connection log line also gives what the handshake took from SRAM.

## 2.1.0 (2026-11-13)

- **A faster screen.** Measured on a prototype badge:
  - The LCD's pixel clock is 40 MHz (`BOARD_LCD_PCLK_HZ`, 20 before): redrawing the whole
    app area every tick went from 15 fps (64 ms a frame) to 25 fps (35 ms). The card keeps
    its own 20 MHz on the same bus.
  - Lua's `on_tick` runs every 30 ms (`LH_TICK_MS`, 50 before) and LVGL refreshes every
    20 ms (`CONFIG_LV_DEF_REFR_PERIOD`, 33 before): the LVGL task sleeps in whole 10 ms
    RTOS ticks plus one, so 33 ms came out as 40. A store game that moves from `on_tick`
    (Cube Shift): at most 20 fps before, 21 to 25 with the clock and the tick, 28 to 31
    with the refresh period too.
  - Dirty areas: LVGL's list holds 256 (`LV_INV_BUF_SIZE`, a compile definition in
    `CMakeLists.txt`; 32 by default, which made LVGL redraw the whole screen once 17
    sprites moved), and `merge_dirty()` in `bsp_display.c` merges nearby ones (each area
    costs ~1 ms with 120 objects on screen, 0.2 ms of it the panel's window commands).
    With 120 objects on screen and sprites in rows, a frame went from 10.9 to 2.8 ms with
    8 moving, 46 to 4.5 ms with 20, 47 to 9 ms with 40 (19 to 30 fps), 50 to 18 ms with 80
    (17 to 28 fps), 52 to 31 ms with 120 (16 to 20 fps); scattered sprites now take 13.5,
    23.5, 37 and 51 ms for 20, 40, 80 and 120. Cube Shift: 30 to 32 fps, frames of 8 to
    12 ms, no full-redraw spikes. The emulator keeps LVGL's 32 entries
    (`bsp_display.c` is the badge's). Costs 3.8 KB of SRAM: the launcher has ~151.5 KB
    free (2.0.1: ~155 KB).
  - Tried and dropped: LVGL built with `-O2` (106 KB more flash, no measurable gain); the
    longer list without merging (80 and 120 sprites got slower: 98 and 149 ms).
  - Not verified: a production panel at 40 MHz (measured on one prototype, colour
    inversion off), how the physical screen looks (screenshots come from LVGL's memory,
    not the panel), battery power. An app that counts ticks instead of using `ms` runs
    1.67 times faster; every app the server seeds uses `ms`.
  - HW Debug > Display shows the clock the build uses.
- **`crypto` and `solana` for Lua apps** (no permission; `docs/LUA_API.md` "crypto and
  solana"). `crypto.sha256`, `crypto.on_curve` (is this a point of the Ed25519 curve) and
  `crypto.verify` (an Ed25519 signature); `solana.encode` and `solana.decode` (base58, up
  to 128 bytes) and `solana.pda` (a program-derived address and its bump, as
  `findProgramAddress` gives them). An app can now find a program's accounts by itself,
  for instance a Squads proposal from the multisig's address and a transaction number,
  where it used to need the addresses worked out elsewhere, and can check a signature made
  by a key it trusts with Wi-Fi off. There is no signing and no key generation: a badge
  holds no keys. `main/core/ed25519.*` (portable C: ten 26-bit limbs, the complete
  point formulas TweetNaCl uses, both scalars walked at once, its own SHA-512),
  `main/lua/lua_crypto.c` (the two modules; SHA-256 is PSA's, as for store installs),
  `tests/t-crypto` (values from Node's SHA-256 and Ed25519, RFC 8032 and
  @solana/web3.js).
- **What it costs.** 7.5 KB of flash (image 2 299 187 to 2 306 659 bytes) and no RAM while
  unused; the deepest call, a signature check, takes about 1.7 KB of the Lua task's stack.
  Counted in QEMU on the ESP32-C5 build of `ed25519.c` (`-Os`): 1.04 million instructions
  for the on-curve test and 11.5 million for a signature, so about 4 to 9 ms and 50 to
  100 ms at 240 MHz. **Measured on a badge** (a prototype, `tests/t-crypto`: every check
  passes): `crypto.verify` 52 ms (51.4 to 53.8, a good signature or a bad one alike),
  `crypto.on_curve` 4.7 ms, `solana.pda` 7.3 ms (with ~2 bumps), `crypto.sha256` of 4 KB
  1.6 ms; the badge's Lua runs about 3 000 VM instructions a millisecond. The instruction
  charges in `lua_crypto.c` (5 000, 40 000 and 10 000 of a callback's 200 000) stay: a
  callback of nothing but one of them takes at most ~260 ms (5 signature checks), within
  the half second they were sized for. The emulator builds them (`bun run firmware`) and
  `tests/t-crypto` passes there too.
- **Example: Squads pager** (`docs/examples/squads-pager`): reads one Squads multisig over
  `rpc`, finds its newest proposals with `solana.pda` and blinks when one waits for the
  wallet linked at `/profile`. Run only against a made-up multisig with `rpc` stubbed, not
  on a badge or in the emulator.
- **Fixes from the review of this release** (Greptile on solana-os#17):
  - **`crypto.verify` refuses small-order keys and R** (`ed25519_verify`, security). With the
    neutral point as the key (`"\1"` and 31 zero bytes) and the neutral point as R with
    s = 0, every message "verified": no private key needed, and the other seven
    small-order points forge one message in eight. It now checks what Solana checks of a
    transaction's signature (ed25519-dalek's `verify_strict`): s below the group order
    (as before), and neither the key nor R of small order, in any encoding (`8P` is the
    neutral point). R is still compared byte for byte with the canonical encoding of
    s*B - h*A, so the few non-canonical R encodings `verify_strict` takes are refused;
    no signer makes them. `crypto.on_curve` is unchanged: those points are on the curve,
    and a program-derived address test mustn't change. Tests: `tests/t-crypto` has six
    new negative vectors (all returned `true` on the build before), and
    `tests/crypto/ed25519_test.py` checks the C code on the host against a Python
    reference: RFC 8032, 200 random signatures, every small-order point and six
    non-canonical encodings as key and as R, and an R of small order with a real key's
    s = h*a (1 053 cases; 47 failed before). Measured on a badge: 57.5 ms a signature
    (52 before); the 40 000-instruction charge stays.
  - **`solana.pda` pays per bump** (`lua_crypto.c`): 5 000 instructions for each bump
    tried, charged before trying it, where it charged 10 000 for the whole search. Seeds
    that need many tries can be found ahead of time: on the build before, ten calls with
    seeds needing 18 ran 180 hashes and curve tests in one callback, 854 ms under the
    LVGL lock, for 100 000 of its 200 000 instructions. Now about 40 tries fit a callback
    (4.8 ms each on a badge: ~0.2 s), then "Script took too long" stops the search, with
    nothing half done. `tests/t-pda`.
  - **Setters can't take the badge's SRAM** (`lua_ui.c`). 2.0.1 checked SRAM only when an
    object was made, so an app could make empty labels and lists until the check refused,
    then fill them: `label:text`, a list's rows, `ui.hint`, `ui.toast` and `qr:text` grew
    SRAM unchecked. They all ask `lh_ui_room` now, as do `ui.hint`'s bar (made without a
    check before), `ui.confirm`'s and `ui.input`'s text and `ui.qr`'s two scratch buffers;
    an app sees the same catchable "label:text: the badge is low on memory" as when it
    makes an object. What a list, the keyboard or an image can grow by on its own (rows
    redrawn on a key: 1.3 KB a list) is kept free by every check. And the check measures
    SRAM that malloc can still take (`OS_MEM_SRAM_MALLOC`): `OS_MEM_SRAM` also counts the
    32 KB pool `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` keeps for DMA, which malloc never
    uses, so 2.0.1's 32 KB floor was mostly that pool. `tests/t-uiset` (60 empty labels,
    lists and rects until refused, then the fills), measured on a prototype with Wi-Fi
    off: before, every fill went through and the SRAM malloc could take fell to 92 bytes
    (small allocations then go to PSRAM); now the fills are refused and it stays at
    40 KB or more. The console's `heap` adds a `sram:` line with that figure. An app gets
    about 32 KB less UI than 2.0.1 allowed (60 labels and six lists with Wi-Fi off). The
    emulator's SRAM is its whole wasm heap, so it refuses nothing there, as for making
    objects.
  - **Server calls' bodies in PSRAM** (`lua_json.c`): the JSON encoder's buffer (16
    queued calls of up to 4 KB each, `storage`, mesh messages, meets) is allocated in
    PSRAM when there is some, so a full queue no longer holds 64 KB of SRAM.
  - **Squads pager**: it read 1 500 bytes of the multisig and took the members that fit for
    all of them, so a wallet among the last of a big multisig was "not a member" and got
    no alerts; it read 420 bytes of each proposal and, past 10 approvals, could ask for a
    signature already given; and a proposal account that existed but didn't parse counted
    as no proposal ("No open proposals"). Lists are now read whole (2 400 bytes a call, at
    most three more: about 260 members or votes, else "Too many members to read"); a
    proposal that exists but can't be read stops the round with "Can't read proposal #n",
    and that stays on screen. `test.lua` runs it on a computer against mocked RPC answers
    (9 cases; 6 failed before). From the second review: the extra reads could pass the 30
    RPC calls a minute a badge gets (a 120-member multisig with eight long proposals: 40
    calls refused in five minutes), so the app now counts its calls and starts a round
    only when one as long as the last fits the minute; and pieces of one list read by
    separate calls could come from two states of the account (a vote landing between
    them made a valid proposal "can't read"), so pieces not answered from one slot are
    checked against the account's first bytes and read again if it moved. 12 cases.
    Still not run on a badge or in the emulator.

## 2.0.1 (2026-11-13)

Fixes from the 2.0.0 review.

- **UI objects check SRAM.** With the Lua heap in PSRAM the allocator's reserve no longer
  guards the SRAM that LVGL's objects use, and one `ui.list` slot is up to 43 LVGL objects:
  an app making lists could run SRAM out while its heap stayed small. Every call that makes
  objects (`ui.label`, `rect`, `bar`, `gradient`, `list`, `qr`, `image`, `canvas`, `anim`,
  `confirm`, `input`) now first checks that what it adds leaves 32 KB of SRAM free
  (`lh_ui_room`, `LUA_HOST_SYS_RESERVE`), else raises "ui.list: the badge is low on memory".
- **A scan is silent on the badge's own channel too.** While `mesh.scan` visited the mesh
  channel, the badge still sent its HELLOs, `mesh.send` worked and `on_mesh` heard app
  messages; elsewhere none of that happened. Now a running scan sends nothing and passes
  nothing on, wherever it is (`on_channel()` in `core/os_mesh.c` counts a scan as off the
  channel): `mesh.send` answers `nil, "scanning"`, `mesh.meet` `"the mesh is off"`, and
  what it hears (HELLOs, the app's messages) only goes into its results.
- **`mesh.resume()` then `mesh.scan()` in one callback scans.** The resume only queued
  its acquire for the OS task, which waits for the LVGL lock the callback holds, so the
  scan found the mesh released and answered `nil, "the mesh is off"`. `mesh.scan` now takes
  a pending resume's acquire itself (`mesh_hold_apply` in `lua/lua_net.c`), and each queued
  pause or resume applies the app's latest ask, so a pause, a resume and a scan in one
  callback no longer end that scan when the pause lands. A pause ends a running scan at
  once (it dropped the scan's callback but left it running until the release landed, so a
  scan right after a pause and a resume answered `"scanning"`). `tests/t-chan` checks all
  three, measured on a badge.

## 2.0.0 (2026-11-12)

- **PSRAM.** Every badge's ESP32-C5HR8 carries 8 MB of quad PSRAM in its package (production
  rev 1.0 and the prototypes' rev 1.2 alike), which Solana OS left off until now. It runs at
  80 MHz, is brought up by the app's startup code (not the bootloader, so a badge updated
  over the air gets it too) and goes into the heap: `malloc()` puts blocks over 16 KB there,
  Wi-Fi and lwIP buffers prefer it (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`), FatFS's
  buffers too, and mbedTLS allocates from it first (`tls_calloc` in `os_http.c`,
  `CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`), so a TLS handshake no longer touches SRAM. A chip
  without PSRAM still boots and gets 1.x's limits (`CONFIG_SPIRAM_IGNORE_NOTFOUND`).
  `core/os_mem.h` says which heap is which; the guards follow it: a new TLS connection
  (`os_http`, `os_ws`) checks PSRAM, `ble.on()` SRAM. Measured: SRAM 163 KB free at the
  launcher with no network joined (1.1.2: 156 KB), 154 KB with Wi-Fi joined and the
  server connection open (HYQ5VS on 1.1.2: 143 KB); a full handshake takes 27 KB of PSRAM;
  Bluetooth still takes about 30 KB of SRAM while on. Boot takes about a second longer,
  the PSRAM's memory test (`CONFIG_SPIRAM_MEMTEST`). On rev 1.0 chips PSRAM isn't
  encrypted even with flash encryption (an ESP32-C5 erratum; badges don't encrypt flash).
- **ESP-IDF's flash driver again, not the ROM's.** With PSRAM mapped, the ROM's esp_flash
  driver (`CONFIG_SPI_FLASH_ROM_IMPL`, used since 0.5.13) overwrites MMU entries in the
  middle of it: entry 129 (`0x42810000`) during `nvs_flash_init()`, 130 later. Those 128 KB
  of PSRAM then read `0x0addbad0` (the C5's answer for an unmapped page) and lose every
  write, so the heap broke as soon as anything reached them (a store access fault in
  `tlsf_malloc`, on both kinds of badge). ESP-IDF's driver keeps every entry: all 8 MB
  write and verify, and a 2 MB erase and write of the other OTA slot with a Lua app holding
  6 MB of PSRAM verified on both sides. It costs about 9 KB of SRAM.
- **Lua apps get 6 MB of heap** (`docs/LUA_API.md` "Limits"; 96 KB before), all of it in
  PSRAM (`lh_alloc` uses `heap_caps_realloc(..., OS_MEM_PSRAM)` and never spills into
  SRAM), and the badge keeps 512 KB of PSRAM for itself while an app runs
  (`LUA_HOST_PSRAM_RESERVE`). Without PSRAM: 96 KB in SRAM and the 32 KB reserve, as in 1.x
  (`lua_host_heap_max()`). With the heap out of SRAM, the app's SRAM-bound limits double:
  128 UI objects (64) and 12 KB of label text (6 KB); and `fs.read`/`fs.json` take up to
  256 KB a call (16 KB), a whole bundled file. `sys.memory()` (new) answers `{used, max}`;
  "Out of memory" says "the app needs more than 6 MB"; `lua mem` names the heap. Measured
  with `tests/t-psram` (new: 124 labels, then 64 KB strings a timer callback at a time): it
  reached 6,243,450 bytes of Lua heap and "not enough memory" at the cap, with 1.9 to
  2.1 MB of PSRAM and 116 KB of SRAM still free, and gave all of it back on exit. Snake
  installed from the store ran from its flash slot as before. `tests/t-heap` is now the
  test for a badge without PSRAM.
- **The console shows both heaps.** `heap`'s first line is SRAM, as before the change
  (the heap whose limits matter most), then `psram: … free, … min, … largest block, …
  total`; `info` adds `PSRAM: 8192 KB, … KB free`; HW Debug > System shows both. The
  emulator emulates 8 MB of PSRAM (`MALLOC_CAP_SPIRAM` in its heap shim), so apps get a
  badge's limits there too.
- Includes 1.1.2 (Invert colours).

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
