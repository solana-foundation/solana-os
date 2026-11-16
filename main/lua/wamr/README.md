# WAMR 2.4.5 (vendored)

The WebAssembly runtime that runs C apps (Solana OS 2.2.0, docs/C_API.md):
a subset of [WebAssembly Micro Runtime](https://github.com/bytecodealliance/wasm-micro-runtime),
tag `WAMR-2.4.5`, under Apache-2.0 WITH LLVM-exception ([LICENSE](LICENSE); third-party parts in [ATTRIBUTIONS.md](ATTRIBUTIONS.md)).

## What is here

The tree keeps WAMR's layout, `core/` only:

- `core/config.h`, `core/version.h`
- `core/iwasm/include/`: the public headers
- `core/iwasm/common/`: runtime, natives, exec env, memory, loader helpers;
  `arch/invokeNative_riscv.S` (the badge's AOT calls)
- `core/iwasm/interpreter/`: the classic interpreter and the full (validating) loader
- `core/iwasm/aot/`: the AOT loader and runtime, `arch/aot_reloc_riscv.c`
  (badge build only: `SOLANA_OS_WAMR_AOT_SRCS`)
- `core/iwasm/compilation/aot.h`, `aot_stack_frame.h`: headers the AOT runtime includes
- `core/shared/utils/`, `core/shared/mem-alloc/`, `core/shared/platform/include/`

Not here: the fast interpreter, the JITs, libc-builtin, libc-wasi, threads,
the C API (`wasm_c_api.c`), WAMR's platform ports. Solana OS's platform layer
is `../lua_wasm_port.c` with `../wamr_port/platform_internal.h`; the build
settings (`WASM_ENABLE_*`) are in `../../sources.cmake`.

## Changes

Each is marked "Solana OS" in the source.

- `iwasm/interpreter/wasm_interp_classic.c`: instruction metering counts one
  budget across nested calls. Stock WAMR reads the limit into a local at each
  interpreter entry and never writes it back, so a native that calls into
  wasm again (the C app bridge does: `sos_call` -> API -> callback) started
  the nested call with the full budget. The count is now handed to the
  native before the call and taken back after it, and written back on
  return and on a trap.
- `iwasm/interpreter/wasm_interp_classic.c`: bulk memory operations cost by
  length. `memory.fill`, `memory.copy` and `memory.init` charge one instruction
  per `SOLANA_OS_WASM_BULK_BYTES` bytes (4, set in `../../sources.cmake`)
  before the work, and trap as the budget does when it would run out; stock
  WAMR charged each one instruction whatever its length (3 000 fills of
  3.75 MB in one callback were charged 19 563). The rate: on a badge `memset`
  writes ~12 KB and `memmove` ~8 KB a ms in PSRAM while the interpreter runs
  2 200 to 4 500 instructions, so 4 bytes take about an instruction's time and
  a callback's 2 000 000 instructions cover at most 8 MB (~1 s). They also trap
  when the exec env's count reads 0: the badge's watchdog writes 0 there to
  stop an app (the interpreter copies its count there only around natives).
- `iwasm/aot/aot_runtime.c`: `aot_memset` and `aot_memmove` (what native code's
  `memory.fill` and `memory.copy` call through the XIP symbol table's
  `memset`, `memmove` and `memcpy`) call `lw_aot_memset`/`lw_aot_memmove`
  (`../lua_aot.c`), which work in 4 KB pieces and stop the app between two;
  `aot_memory_init` copies through `aot_memmove` instead of `bh_memcpy_s`
  (both ranges are checked before).
- `iwasm/common/wasm_runtime_common.c`, `iwasm/include/wasm_export.h`:
  `wasm_runtime_get_instruction_count_limit()`, what is left of the budget
  (for `lua mem` and the per-callback count).
- `iwasm/interpreter/wasm_loader.c`: a variable only the fast interpreter
  uses is declared only for it (`-Wunused-variable`).
- `iwasm/aot/arch/aot_reloc_riscv.c`: the intrinsics' symbol table is
  `const`, so it stays in flash instead of 1.1 KB of every badge's SRAM.
- `shared/platform/include/platform_wasi_types.h`: skipped when the sysroot
  already declares WASI's types (Emscripten's `<wasi/api.h>`, in the
  emulator); WAMR uses them only in libc-wasi.

On the badge WAMR's files are compiled with `-Wno-format` (main/CMakeLists.txt):
they print `uint32` (an `unsigned long` in the toolchain's newlib) with `%u`.

## Updating

Copy the same files from the new tag over these, re-apply the changes above
(search for "Solana OS"), bump `WAMR` in `sdk/c/toolchain/versions` (with the
SDK, whose wamrc must be the same version) and `AOT_TARGET` with it, and run
the C app tests (`main/lua/tests/c/`).
