# C vs Lua: the benchmark

The same work written twice, as a C app (`c/`) and as a Lua app (`lua/`), run on a
badge and compared. The results, measured on a badge, are in
[docs/C_API.md "Speed and limits: C against Lua"](../../../docs/C_API.md#speed-and-limits-c-against-lua).

```
bench/
  c/kernels.c, kernels.h   the workloads in plain C (no badge.h): the C app's
                           and a test build's "C in the firmware" column
  c/main.c, app.json       the C app (bench-c): menu, phases, console lines
  lua/main.lua, app.json   the Lua app (bench-lua): the same, step by step
  run.py                   drives either app over the serial console, prints the table
```

## The workloads

Each workload does its work in *units* small enough for one Lua callback, and both
apps add up the same checksum, so a row whose checksums differ is a bug.

| Workload | A unit | Checksum |
|---|---|---|
| `mandel` | one row of a 320 × 240 Mandelbrot, integers with 12 fractional bits, ≤ 32 iterations (rows in a scattered order) | `000cbe4f` |
| `mandelf` | the same row with 32-bit floats (Lua's numbers on the badge) | `000cbe79` |
| `mandeld` | the same row with doubles (C only: Lua has no 64-bit floats) | `000cbe7b` |
| `sieve` | the primes up to 4096 (564 of them) | `000008d0` |
| `sha` | one 64-byte block of SHA-256, written out in the language | `7486da8f` |
| `shaapi` | SHA-256 of 1 KB through the badge's own `crypto.sha256` | `a460f668` |
| `fill` | 16 K 32-bit words of a 1 MB buffer written in a loop | `fffe0000` |
| `copy` | 64 KB of a 1 MB buffer copied (`memcpy`; Lua: `table.move`) | `fffe0000` |
| `draw` | one row of a 320 × 216 frame computed per pixel (C: its own RGB565 buffer, `ui_canvas_blit` at the end of each callback; Lua: `c:pixel` on a `ui.canvas`) | `86d02100` |
| `draw80` | the same on an 80 × 54 canvas shown at 4×, the size LUA_API.md suggests for Lua | `078da030` |
| `labels` | one `label:text()` with a new number (16 labels) | |
| `list` | one `list:items()` with 20 rows | |
| `game` | a whole game tick: 256 particles moved, bounced and drawn 2 × 2 on a 160 × 108 canvas at 2×, then shown | `0141146c` |

and three limits: `alloc` (64 KB blocks until the app can't get more), `stack`
(recursion, 25 % deeper each callback until it stops) and `ui` (labels until the badge
refuses one).

A run of one workload: **CAL** doubles the units in one callback until they take
8 ms; **CHECK** does a fixed amount of work spread over callbacks and prints the
checksum; **RUN** spends 3 s doing chunks of units, one chunk a tick (a whole frame a
tick when it fits); **PROBE** does one callback at least as big as the run's, then the
app closes, so `lua mem` reports the most any callback counted against its limit (Lua
instructions; for C, WebAssembly instructions or native time, and host steps), and the
app prints its longest callback's work time next to it (`max_us`). Their ratio is what a
limit counts per microsecond of that work, so `run.py table` estimates how many average
units fit in one callback without stopping the app. The estimate is low when something
else counted more than the work (the app's start prints its menu: about 28 000
interpreted instructions, more than a callback of label updates); a ramp measures it.
SELECT runs a workload; RIGHT runs it and then a **ramp** instead of the probe: 6 %
more units every callback until the badge stops the app, printing each size that ran.

The console lines (each app's lines start with `[bench-c] ` or `[bench-lua] `):

```
MENU mandel,mandelf,...           READY lang=c ms=626        (ms since the app started)
CHK wl=mandel chk=000cbe4f
BENCH wl=mandel lang=c units=5310 us=1382536 wall=3012 frames=0 blit=0
PROBE wl=mandel n=76 us=19427 max_us=19427                    FIT wl=mandel n=760 us=194543
ALLOC lang=c got=4128768 used=4251551 max=6291456 stop=malloc_null
STACK lang=c ok=303 us=100        UI lang=c labels=128 err=too many UI objects (128)
DONE wl=mandel
```

`us` is the time spent in the work (drawing: the blit included, and `blit` alone),
`wall` the whole run. C reads the monotonic clock in microseconds; Lua has only
`sys.millis()`, whole milliseconds, which even out over the hundreds of callbacks of a
run.

## Running it

```sh
tools/cc/badge-cc sdk/c/bench/c -o /tmp/bench-c.wasm      # from firmware/
cd sdk/c/bench
python3 run.py push --port /dev/cu.usbserial-110 lua
python3 run.py push --port /dev/cu.usbserial-110 c --wasm /tmp/bench-c.wasm
python3 run.py run --port /dev/cu.usbserial-110 --settle 35 --wifi-off --out r.jsonl bench-lua
python3 run.py run --port /dev/cu.usbserial-110 --settle 35 --wifi-off --out r.jsonl bench-c
python3 run.py table r.jsonl
```

Use the system `python3` with pyserial, as for `tools/badge.py`; opening the port
restarts the badge, so `--settle 35` waits for its boot-time Wi-Fi attempt to end, and
`--wifi-off` turns Wi-Fi off until the next restart (the saved network stays). Name
workloads after the app to run only those; `--ramp mandel` grows that one until the
badge stops it. `table` prints, per workload and column, the time of a unit, frames a
second for the drawn ones, how many units fit in one callback and which limit sets
that, and the checksum.

A C app copied over the console runs interpreted (`app.wasm`). For the native column,
publish `bench-c` to the App Store (unlisted is enough), install it on the badge, check
with `lua mem` that it says `app.aot (native)`, and run `run.py run ... bench-c`
without pushing. The "C in the firmware" column needs a test build of Solana OS with
`kernels.c` compiled in behind a console command (`run.py run --fw`): it isn't in the
released firmware.

The emulator runs both apps too, but its Lua and its WebAssembly interpreter are many
times faster than a badge's, so its numbers mean nothing here, and a workload whose
units were sized on its clock can go past a badge-sized budget and stop.
