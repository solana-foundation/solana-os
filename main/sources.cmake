# Source lists shared by the badge build (main/CMakeLists.txt) and the browser
# emulator (firmware/emulator/). Add new apps and portable core files to
# SOLANA_OS_SRCS so both builds pick them up; only code that talks to real
# hardware or ESP-IDF services belongs in SOLANA_OS_DEVICE_SRCS, and each of
# those needs an emulator counterpart.

# Lua 5.5 for store apps (lua/vendor/LICENSE): the core and the libraries the
# sandbox offers, no io/os/package/debug.
set(SOLANA_OS_LUA_VENDOR_SRCS)
foreach(f lapi lauxlib lbaselib lcode lcorolib lctype ldebug ldo ldump lfunc lgc llex lmathlib lmem
          lobject lopcodes lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio)
    list(APPEND SOLANA_OS_LUA_VENDOR_SRCS lua/vendor/${f}.c)
endforeach()

# WAMR 2.4.5 for C apps (lua/wamr/README.md, Apache-2.0 WITH LLVM-exception):
# the classic interpreter, its loader and runtime, over Solana OS's platform
# layer (lua/lua_wasm_port.c, wamr_port/). Both builds compile these; the
# badge adds the AOT runtime (SOLANA_OS_WAMR_AOT_SRCS). The definitions and
# include directories go to every source of both builds (they are WAMR's
# own names); each build adds its WASM_ENABLE_AOT and the emulator its
# switch dispatch.
set(SOLANA_OS_WAMR_SRCS
    lua/wamr/core/iwasm/common/wasm_runtime_common.c lua/wamr/core/iwasm/common/wasm_native.c
    lua/wamr/core/iwasm/common/wasm_exec_env.c lua/wamr/core/iwasm/common/wasm_memory.c
    lua/wamr/core/iwasm/common/wasm_loader_common.c
    lua/wamr/core/iwasm/interpreter/wasm_runtime.c lua/wamr/core/iwasm/interpreter/wasm_loader.c
    lua/wamr/core/iwasm/interpreter/wasm_interp_classic.c
    lua/wamr/core/shared/utils/bh_assert.c lua/wamr/core/shared/utils/bh_common.c
    lua/wamr/core/shared/utils/bh_hashmap.c lua/wamr/core/shared/utils/bh_list.c
    lua/wamr/core/shared/utils/bh_log.c lua/wamr/core/shared/utils/bh_vector.c
    lua/wamr/core/shared/utils/bh_leb128.c lua/wamr/core/shared/utils/bh_bitmap.c
    lua/wamr/core/shared/mem-alloc/mem_alloc.c lua/wamr/core/shared/mem-alloc/ems/ems_alloc.c
    lua/wamr/core/shared/mem-alloc/ems/ems_hmu.c lua/wamr/core/shared/mem-alloc/ems/ems_kfc.c
)
set(SOLANA_OS_WAMR_INCLUDE_DIRS
    lua/wamr_port lua/wamr/core/iwasm/include lua/wamr/core/iwasm/common lua/wamr/core/iwasm/interpreter
    lua/wamr/core/iwasm/aot lua/wamr/core/shared/utils lua/wamr/core/shared/platform/include
    lua/wamr/core/shared/mem-alloc
)
# A sandbox for untrusted modules: the full validating loader, no libc of
# WAMR's (the WASI calls are lua/lua_wasi.c's), no threads, no shared
# memory; the features the SDK builds for (sdk/c/toolchain/cflags: MVP,
# mutable globals, sign extension, non-trapping float-to-int, bulk memory;
# no reference types, so a module using them doesn't load); and instruction
# metering for the per-callback budget. The badge and the emulator are both
# RISCV32_ILP32 here, so alignment rules match. LW_AOT_TARGET is the SDK's
# AOT_TARGET (sdk/c/toolchain/versions): the label of the native code the
# store's wamrc makes, which a badge runs only when it is its own.
set(SOLANA_OS_WAMR_DEFS
    BUILD_TARGET_RISCV32_ILP32 WASM_ENABLE_INTERP=1 WASM_ENABLE_FAST_INTERP=0 WASM_ENABLE_JIT=0
    WASM_ENABLE_FAST_JIT=0 WASM_ENABLE_LIBC_BUILTIN=0 WASM_ENABLE_LIBC_WASI=0 WASM_ENABLE_THREAD_MGR=0
    WASM_ENABLE_SHARED_MEMORY=0 WASM_ENABLE_MULTI_MODULE=0 WASM_ENABLE_MINI_LOADER=0
    WASM_ENABLE_BULK_MEMORY=1 WASM_ENABLE_REF_TYPES=0 WASM_ENABLE_SIMD=0 WASM_ENABLE_GC=0
    WASM_ENABLE_EXCE_HANDLING=0 WASM_ENABLE_TAIL_CALL=0 WASM_ENABLE_MEMORY64=0 WASM_ENABLE_MULTI_MEMORY=0
    WASM_ENABLE_LOAD_CUSTOM_SECTION=0 WASM_ENABLE_CUSTOM_NAME_SECTION=0 WASM_ENABLE_DUMP_CALL_STACK=0
    WASM_ENABLE_PERF_PROFILING=0 WASM_ENABLE_MEMORY_PROFILING=0 WASM_ENABLE_INSTRUCTION_METERING=1
    WASM_ENABLE_SPEC_TEST=0 WASM_ENABLE_WAMR_COMPILER=0 WASM_ENABLE_DEBUG_INTERP=0 WASM_ENABLE_SHARED_HEAP=0
    WASM_ENABLE_MODULE_INST_CONTEXT=0 WASM_ENABLE_QUICK_AOT_ENTRY=0 WASM_ENABLE_AOT_STACK_FRAME=0
    WASM_ENABLE_LIB_PTHREAD=0 WASM_ENABLE_LIB_WASI_THREADS=0 BH_DEBUG=0
    # Solana OS (lua/wamr/README.md): bytes of bulk memory work (memory.fill,
    # .copy, .init) per instruction the interpreter charges. Measured on a
    # badge: memset 12 KB/ms and memmove 8 KB/ms in PSRAM, the interpreter
    # 2 200 to 2 900 instructions/ms, so 4 bytes take about one instruction's
    # time and a callback's 2 000 000 cover at most 8 MB (~1 s).
    SOLANA_OS_WASM_BULK_BYTES=4
    BH_MALLOC=wasm_runtime_malloc BH_FREE=wasm_runtime_free
)
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/../sdk/c/toolchain/versions SOLANA_OS_AOT_TARGET REGEX "^AOT_TARGET=")
# Read at configure time: a new label must configure again, or a build would
# keep the old one and turn down every app.aot the store makes for the new.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../sdk/c/toolchain/versions)
string(REPLACE "AOT_TARGET=" "" SOLANA_OS_AOT_TARGET "${SOLANA_OS_AOT_TARGET}")
if(NOT SOLANA_OS_AOT_TARGET MATCHES "^[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "sdk/c/toolchain/versions has no AOT_TARGET=<label>")
endif()
list(APPEND SOLANA_OS_WAMR_DEFS "LW_AOT_TARGET=\"${SOLANA_OS_AOT_TARGET}\"")
# The AOT runtime, badge only (main/CMakeLists.txt sets WASM_ENABLE_AOT=1):
# nothing reaches it until lua/lua_aot.c says AOT is usable. No emulator
# counterpart: the emulator's WAMR is built without it.
set(SOLANA_OS_WAMR_AOT_SRCS
    lua/wamr/core/iwasm/aot/aot_loader.c lua/wamr/core/iwasm/aot/aot_runtime.c
    lua/wamr/core/iwasm/aot/aot_intrinsic.c lua/wamr/core/iwasm/aot/arch/aot_reloc_riscv.c
    lua/wamr/core/iwasm/common/arch/invokeNative_riscv.S
)

set(SOLANA_OS_SRCS
    core/os.c core/launcher.c core/calibrate.c core/ui.c core/os_cloud.c core/os_settings.c core/os_wifi_boot.c
    core/os_mesh.c core/os_bridge.c core/os_api.c core/os_socket.c core/os_data.c core/os_cmd.c core/os_ota.c
    core/os_battery.c core/battery_calc.c                                                  # one battery reading (2.4.0)
    core/ed25519.c
    core/os_service.c core/os_service_test.c game/services.c apps/services/services_app.c   # background services (2.3.0)
    game/mailbox.c apps/mailbox/mailbox_app.c lua/lua_mail.c                               # Mailbox (2.3.0)
    game/gossip.c game/gossip_genome.c lua/lua_gossip.c                                  # Gossip, the node service (2.4.0, fw-node)
    game/gossip_role.c lua/lua_gossip_role.c                                             # Gossip roles (2.4.0, fw-roles)
    game/gossip_role_app.c game/gossip_role_boss.c                                       # Gossip roles: claims, bids, cures; the fight (fw-roles)
    game/collab.c apps/collab/collab_app.c          # Collaborator (2.3.0)
    game/game.c game/xdex.c game/trade.c game/quests.c game/beacon.c
    apps/registry.c
    apps/wifi/wifi_app.c
    apps/badge/badge_app.c
    apps/guide/guide_app.c apps/xdex/xdex_app.c apps/trade/trade_app.c apps/quests/quests_app.c
    apps/beacon/beacon_app.c
    apps/espnow/espnow_app.c
    apps/debug/debug_app.c
    apps/settings/settings_app.c
    apps/update/update_app.c
    apps/about/about_app.c
    apps/store/store_app.c
    apps/apps/apps_app.c
    lua/lua_host.c lua/lua_ui.c lua/lua_sys.c lua/lua_net.c lua/lua_json.c lua/lua_pkg.c lua/lua_cmd.c
    lua/lua_fs.c
    lua/lua_files.c lua/lua_install.c lua/lua_code.c lua/lua_ws.c lua/lua_img.c lua/lua_png.c lua/lua_gif.c lua/lua_meet.c
    lua/lua_ble.c lua/lua_crypto.c lua/lua_wasm.c lua/lua_wasi.c lua/lua_wasm_port.c lua/lua_aot_trust.c
    lua/lua_svc.c  # background services (2.3.0)
    ${SOLANA_OS_LUA_VENDOR_SRCS}
    ${SOLANA_OS_WAMR_SRCS}
    assets/sol_mono_12.c assets/sol_font_14.c assets/sol_font_20.c assets/sol_font_32.c
    assets/img_logomark_16.c assets/img_logomark_40.c assets/img_logomark_72.c
    assets/img_foundation.c assets/img_foundation_black.c
)

set(SOLANA_OS_DEVICE_SRCS
    app_main.c
    core/os_wifi.c
    core/os_http.c
    core/os_ws.c
    core/os_ota_flash.c
    core/os_radio.c
    core/os_ble.c
    core/console.c
    lua/lua_xip.c
    lua/lua_aot.c
)
