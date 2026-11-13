cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# LVGL's list of dirty areas (32 by default; it isn't in Kconfig). Past its end
# LVGL redraws the whole screen, and a moved object takes two entries before
# overlapping ones are joined: 17 moving sprites did that. 256 holds every
# object a Lua app can make; bsp_display.c merges them (merge_dirty). 3.8 KB.
# The badge's only: the emulator (emulator/CMakeLists.txt) keeps LVGL's 32.
idf_build_set_property(COMPILE_DEFINITIONS "LV_INV_BUF_SIZE=256" APPEND)
project(solana_os)
