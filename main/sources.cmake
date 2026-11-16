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

set(SOLANA_OS_SRCS
    core/os.c core/launcher.c core/calibrate.c core/ui.c core/ui_theme.c
    core/home/home_grid.c core/home/home_list.c core/home/home_bento.c core/home/home_rpg.c
    core/home/home_icons.c core/home/home_dial.c core/home/home_pearl.c core/home/home_term.c core/home/home_blocks.c core/home/home_controls.c core/home/home_epaper.c core/home/home_stream.c core/home/home_lumen.c core/ui_stream.c
    core/os_cloud.c core/os_settings.c core/os_wifi_boot.c
    core/os_mesh.c core/os_bridge.c core/os_api.c core/os_socket.c core/os_data.c core/os_cmd.c core/os_ota.c
    core/ed25519.c
    game/game.c game/xdex.c game/trade.c game/quests.c game/beacon.c
    apps/registry.c
    apps/wifi/wifi_app.c
    apps/badge/badge_app.c
    apps/guide/guide_app.c apps/xdex/xdex_app.c apps/trade/trade_app.c apps/quests/quests_app.c
    apps/beacon/beacon_app.c
    apps/espnow/espnow_app.c
    apps/debug/debug_app.c
    apps/settings/settings_app.c
    apps/themes/themes_app.c
    apps/update/update_app.c
    apps/about/about_app.c
    apps/store/store_app.c
    apps/apps/apps_app.c
    lua/lua_host.c lua/lua_ui.c lua/lua_sys.c lua/lua_net.c lua/lua_json.c lua/lua_pkg.c lua/lua_cmd.c
    lua/lua_fs.c lua/lua_install.c lua/lua_code.c lua/lua_ws.c lua/lua_img.c lua/lua_png.c lua/lua_gif.c lua/lua_meet.c
    lua/lua_ble.c lua/lua_crypto.c
    ${SOLANA_OS_LUA_VENDOR_SRCS}
    assets/sol_mono_12.c assets/sol_font_14.c assets/sol_font_20.c assets/sol_font_32.c
    assets/pearl_num_46.c assets/pearl_font_13.c assets/pearl_font_11.c assets/pearl_mono_9.c
    assets/apple_font_10.c assets/apple_font_12.c assets/apple_font_14.c assets/apple_num_26.c
    assets/apple_num_48.c assets/blocks_font_13.c assets/blocks_font_15.c assets/blocks_mono_9.c
    assets/blocks_num_41.c assets/blocks_text_12.c assets/paper_font_16.c assets/paper_italic_17.c
    assets/paper_mono_11.c assets/paper_mono_9.c assets/paper_num_46.c assets/rpg_bold_10.c
    assets/rpg_font_9.c assets/rpg_num_26.c assets/term_mono_11.c assets/term_mono_9.c
    assets/term_mono_bold_11.c assets/img_line_app.c assets/img_line_apps.c assets/img_line_badge.c
    assets/img_line_debug.c assets/img_line_espnow.c assets/img_line_guide.c assets/img_line_quests.c
    assets/img_line_settings.c assets/img_line_store.c assets/img_line_trade.c assets/img_line_wifi.c
    assets/img_line_xdex.c assets/img_list_orb.c assets/img_rpg_gem.c
    assets/img_logomark_16.c assets/img_logomark_40.c assets/img_logomark_72.c
    assets/img_foundation.c assets/img_foundation_black.c
    assets/img_wall_pearl.c assets/img_wall_black_pearl.c assets/img_wall_aurora.c
    assets/img_pearl_hero.c assets/img_pearl_wifi.c assets/img_pearl_quest.c
    assets/img_black_pearl_hero.c assets/img_black_pearl_wifi.c assets/img_black_pearl_quest.c
    assets/stream_disp_14.c assets/stream_disp_24.c assets/stream_disp_30.c assets/stream_disp_64.c
    assets/stream_caps_8.c assets/stream_bold_11.c assets/img_stream_hero.c
    assets/img_stream_radar.c assets/img_stream_cards.c assets/img_stream_orb.c
    assets/img_lumen_hero.c assets/img_lumen_wifi.c assets/img_lumen_quest.c
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
)
