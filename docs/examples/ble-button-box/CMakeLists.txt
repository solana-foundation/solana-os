# A button box for the Solana BP26 badge: any ESP32 with Bluetooth LE (README.md).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(ble_button_box)
