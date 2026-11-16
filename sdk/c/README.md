# Solana OS C SDK

Write App Store apps for the Solana BP26 badge in C. The store compiles your C to
WebAssembly and to the badge's native code; the badge (Solana OS 2.3.0 and later) runs
it in a sandbox with the same API, permissions and limits as a Lua app. A store install
runs the native code, in place from the badge's flash; everything else (the emulator,
Run on badge, your own build copied over the console) interprets the WebAssembly, which
is many times slower at computing (on a badge the starfield example drew a frame in 7 ms
native, 33 ms interpreted: `docs/C_API.md` "What runs where"). The full
reference is [`docs/C_API.md`](../../docs/C_API.md) (on the web: `/apps/docs/c`); the API's
behaviour is documented once, in [`docs/LUA_API.md`](../../docs/LUA_API.md).

```
sdk/c/
  include/badge.h        the typed API: ui_label(), timer_every(), http_get(), ...
  include/badge_abi.h    the ABI underneath it (SOS_ABI 1)
  toolchain/             the store's pinned versions and flags: versions, cflags,
                         ldflags, wamrcflags, imports
  examples/              six complete apps, and build.sh to build them all
  bench/                 the same workloads as a C app and a Lua app, and run.py to
                         time them on a badge (docs/C_API.md "Speed and limits")
  tests/                 api/ (every function of badge.h, compiled), libc/ (wasi-libc
                         on WAMR), host/ (what badge.h's functions return, checked on a
                         real host: the emulator or a badge), mock-host.ts (runs an
                         app.wasm against a fake badge)
tools/cc/
  fetch.sh               downloads the pinned toolchain into tools/cc/.toolchain/
  badge-cc               builds an app folder the way the store does, and checks it
  wasm-check.awk         the module checks badge-cc runs (the store runs the same
                         checks, ported to TypeScript)
```

In the SDK zip from the site the same folders sit side by side (`include/`, `toolchain/`,
`examples/`, `tests/`, `tools/cc/`, `docs/C_API.md`); the commands below work from its
top folder with `sdk/c/` left out.

## Quickstart

You need macOS on Apple Silicon (with Rosetta 2 for wamrc: `softwareupdate
--install-rosetta`) or Linux x86_64, and `curl`, `tar` and a POSIX shell.

```sh
sh tools/cc/fetch.sh                           # once: wasi-sdk 24.0 + wamrc 2.4.5, ~220 MB, checked
tools/cc/badge-cc sdk/c/examples/hello -o hello.wasm
sh sdk/c/examples/build.sh                     # every example (+ native code, + the mock host with bun)
```

A new app is a folder with `main.c` and `app.json` (any other file is bundled; `app.wasm`
and `app.aot` at the top are names the badge keeps for the app's code, so the store
refuses them):

```json
{ "id": "my-app", "name": "My App", "version": "1.0.0", "lang": "c",
  "icon": "star", "accent": "14F195", "category": "demos", "permissions": [] }
```

```c
#include "badge.h"

static ui_obj label;

void on_start(void)
{
    label = ui_label(0, 90, "Hello, badge", &(ui_label_opts){.size = 20, .w = UI_W, .align = UI_CENTER});
    ui_hint("SELECT change   CANCEL exit");
}

int32_t on_key(int32_t key, int32_t repeat)
{
    (void)repeat;
    if (key == KEY_SELECT) {
        ui_label_text(label, "Hello again");
        return 1;
    }
    return key != KEY_BACK; // returning 0 for CANCEL exits
}
```

```sh
tools/cc/badge-cc my-app -o my-app.wasm
```

`badge-cc` compiles `main.c` and every other `.c` file in the folder (headers from
`include/` and from the app's folder), links, and checks the module as the store and
the badge will: only the allowed imports, `on_start` exported, at most 4 MB of memory,
no WebAssembly feature the badge doesn't run, at most 1.5 MB. Errors name files the way
your folder does. `--aot app.aot` also runs wamrc, to check the native build works;
`--check app.wasm --report` lists a module's imports, exports and features.

## Try it

- **On the site**: paste or drop the folder on `/apps/submit` with the language set to C.
  Try in emulator and Run on badge compile it on the server and run it.
- **On your badge, from here**: copy the module over the serial console with the
  console's `lua recv <id> app.wasm <bytes> <sha256>` (and `app.json` and your data
  files), then `lua run <id>` (docs/LUA_API.md "Console"). Your own build always runs in
  the interpreter; native code comes only from the store, signed. `lua mem` after a run
  shows which form ran, the heap peak and the most instructions a callback used (the
  limit is 2 000 000 interpreted, about 0.45 s of a plain loop on a badge; the emulator
  counts exactly as a badge does).
- **At full speed**: publish the app unlisted and install it from the store on a badge.
  The store installs `app.wasm` and adds its signed `app.aot`; the badge checks the
  signature at every launch and falls back to `app.wasm` if it fails. Native code has
  200 ms of its own time per callback (time inside API calls doesn't count).
- **Without a badge**: `bun sdk/c/tests/mock-host.ts my-app.wasm` runs the app against a
  fake badge that checks every stack operation and delivers every callback. It proves
  the app uses the API correctly, not that it looks right, and its answers are stand-ins:
  what the real API returns is checked by `tests/host`.

## What the examples show

| Example | Shows |
|---|---|
| `hello` | labels, keys, a timer, `printf` to the console |
| `starfield` | a game drawn a frame at a time: its own RGB565 buffer, `ui_canvas_blit`, `sys_keys`, integer maths |
| `tally` | `storage`: numbers and JSON, `ui_confirm` |
| `sol-price` | `http_get` with a callback, decoding a JSON answer (permission `http`, a host) |
| `ping` | `mesh_nearby`, `mesh_send`, `on_mesh`, a list (permissions `mesh`, `led`) |
| `sketch` | a canvas, pixel art, a bundled PNG loaded once and stamped |

## Licence

MIT, as Solana OS (`LICENSE` at the repository's top). `fetch.sh` downloads, and nothing
here includes, wasi-sdk (Apache-2.0 WITH LLVM-exception; wasi-libc: MIT, Apache-2.0 and
BSD licences) and WAMR's wamrc (Apache-2.0 WITH LLVM-exception); on macOS also zstd
(BSD-3-Clause), from Homebrew's bottle, for wamrc. Apps you build link wasi-libc: keep
its notice if you distribute the module outside the store.
