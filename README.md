# Solana OS

Firmware for the Solana Breakpoint 2026 conference badge (ESP32-C5, ILI9341
320x240, microSD, 6 buttons, 8 WS2812s): an app launcher, Wi-Fi (2.4 and 5 GHz), an
ESP-NOW badge-to-badge mesh, Bluetooth LE for apps, the *Hitchhiker's Guide to Solana*
game apps and a sandboxed Lua 5.5 app host. The same sources also build to WebAssembly for a
browser emulator (`emulator/`) and for the store's Lua compiler (`tools/luac/`).

Stack: ESP-IDF v6.0.1, C, LVGL 9, Lua 5.5. What changed in each release: [CHANGELOG.md](CHANGELOG.md).

## Lua apps and memory

From 2.0.0 Solana OS uses the 8 MB of PSRAM in the badge's ESP32-C5HR8 (`main/core/os_mem.h`):
a Lua app gets up to 6 MB of heap there, and Wi-Fi's buffers and TLS live there too, so the
chip's ~160 KB of free SRAM is left for task stacks, DMA, LVGL and Bluetooth (about 30 KB
while an app has it on, so it is off until an app calls `ble.on()`). Before 2.0.0 there was
no PSRAM in use: a Lua app (96 KB of heap since 0.5.13, 48 KB before) shared one heap with
Wi-Fi, TLS, the mesh and Bluetooth, and much of what follows was written for that budget
(CHANGELOG.md says where each KB came from). Which radio an app should use for what, and how
to plan its memory: [docs/CONNECTIVITY.md](docs/CONNECTIVITY.md). Since 0.5.0:

- Store apps arrive compiled: the App Store compiles them with this firmware's
  own Lua (`tools/luac/`, the same `main/lua/vendor/` built to wasm) and the
  badge runs the bytecode **in place from flash** (`main/lua/lua_xip.c`: slots
  in the otherwise unused `storage` partition, mapped through the flash cache).
  Instructions, line info and long strings never enter the heap and nothing is
  parsed at launch. Bytecode is only ever loaded from a store install whose
  SHA-256 matched; sideloaded apps compile from source.
- Libraries are lazy (`luaL_newlib`, a vendor patch): their tables start empty
  and a function costs RAM only once a script uses it.
- Source is compiled before the libraries are opened, the string table grows
  at two strings per slot, and the collector runs young collections at 10 %
  growth.

Measured on a badge (Lua heap peak, `lua mem`): snake 30.9 KB on 0.4.5, 17.5 KB
on 0.5.0; magic-8 22.8 KB, then 13.6 KB. While snake runs the badge keeps
~15 KB more heap free, and its largest free block doubled (15 KB to 33 KB).

## Build and flash

```sh
source ~/.espressif/v6.0.1/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbserial-110 -b 460800 flash
```

`tools/badge.py` drives the badge's serial console from the host and saves
`shot` screenshots as PNGs (`python3 tools/badge.py --reset "shot"`).

## Layout

| Path | What |
|---|---|
| `components/bsp/` | Board drivers: display, microSD, buttons, RGB LEDs, battery |
| `main/core/` | The OS: app manager, Wi-Fi, mesh, Bluetooth (`os_ble`), server link, OTA, UI kit |
| `main/apps/` | Built-in apps (one folder each, registered in `apps/registry.c`) |
| `main/game/` | Game services (X Dex, trades, quests) |
| `main/lua/` | Lua host and the vendored Lua 5.5.1 (patches listed in `vendor/LICENSE`) |
| `emulator/` | WebAssembly build of the same firmware |
| `tools/` | Console driver, asset generators, web export, the store's Lua compiler (`luac/`) |

## Part of solana-bp26-badges

This repo is a git submodule (at `firmware/`) of solana-bp26-badges, Solana
Foundation's private repo for the server, website, beacon firmware and the
protocol contracts (`docs/MESH.md`, `server/API.md`). The Lua app API (`docs/LUA_API.md`) lives
here, next to the host that implements it; the site renders it at `/apps/docs`.
So do the quest-building guide (`docs/QUESTS.md`, at `/apps/docs/quests`) and
the connectivity guide with its examples (`docs/CONNECTIVITY.md`, `docs/examples/`, at
`/apps/docs/connectivity`).
A few tools assume that
layout: `tools/export_web.ts` / `tools/build_web.sh`, `tools/gen_assets.sh`
(brand fonts) and the emulator build (writes to `../web/static/emulator`).
The firmware itself builds standalone.

`sdkconfig.defaults` points the badge at the production server
(`CONFIG_SOLANA_OS_SERVER_URL`); override it there or on the console with
`server <url>`.

## License

MIT (see `LICENSE`), except the fonts and logos in `main/assets/`, which the
MIT license doesn't cover: the fonts are rendered from ABC Diatype and ABC
Diatype Semi-Mono, and the logos are Solana Foundation brand assets. Both stay
subject to their owners' terms; don't reuse them outside this firmware.
