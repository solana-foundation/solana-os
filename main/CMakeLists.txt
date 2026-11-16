include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS ${SOLANA_OS_SRCS} ${SOLANA_OS_DEVICE_SRCS} ${SOLANA_OS_WAMR_AOT_SRCS}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS ${SOLANA_OS_WAMR_INCLUDE_DIRS}
    PRIV_REQUIRES bsp nvs_flash esp_wifi esp_netif esp_event console esp_timer mbedtls
                  esp_app_format app_update esp_partition spi_flash fatfs vfs esp_http_client esp-tls tcp_transport lwip esp_driver_uart bt)

# cJSON (espressif/cjson) parses mesh JSON with a depth cap of 16:
# CONFIG_CJSON_NESTING_LIMIT in sdkconfig.defaults.

# WAMR (C apps, sources.cmake): the shared settings, plus the AOT runtime
# the badge has and the emulator doesn't (lua/lua_aot.h).
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${SOLANA_OS_WAMR_DEFS} WASM_ENABLE_AOT=1)
# WAMR's own sources print uint32 (uint32_t, an unsigned long in this
# toolchain's newlib) with %u throughout; that is harmless on ilp32, and the
# only warning class it raises. Its files alone get -Wno-format.
set(WAMR_FILES ${SOLANA_OS_WAMR_SRCS} ${SOLANA_OS_WAMR_AOT_SRCS})
list(TRANSFORM WAMR_FILES PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
set_source_files_properties(${WAMR_FILES} PROPERTIES COMPILE_OPTIONS "-Wno-format")
