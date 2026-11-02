# Solana OS: ESP-IDF v6.0.1's esp_http_client with one upstream fix, esp-idf
# 44987dfd14 "fix(esp_http_client): fix broken connection reuse" (on
# release/v6.0, after the v6.0.1 tag). In 6.0.1 every request on a kept-alive
# connection opened a new TLS connection over the old one without closing it: a
# socket and ~4 KB leaked per reused request, so a badge went offline for good
# after ~9 server hellos. As a project component it replaces IDF's. Drop this
# directory once ESP-IDF ships the fix (6.0.2).
if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 0 OR NOT IDF_VERSION_PATCH EQUAL 1)
    message(FATAL_ERROR "components/esp_http_client is ESP-IDF v6.0.1's with a fix: on "
                        "v${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}, check whether it has "
                        "esp-idf 44987dfd14, then drop the directory")
endif()

if(NOT ${IDF_TARGET} STREQUAL "linux")
    set(req lwip esp_event)
else()
    set(req linux esp_event)
endif()

idf_component_register(SRCS "esp_http_client.c"
                            "lib/http_auth.c"
                            "lib/http_header.c"
                            "lib/http_utils.c"
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "lib/include"
                    # lwip is a public requirement because esp_http_client.h includes sys/socket.h
                    REQUIRES ${req}
                    PRIV_REQUIRES tcp_transport http_parser)
