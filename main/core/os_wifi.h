#pragma once

// System Wi-Fi service. Owns the driver (STA, dual-band), joins the saved
// network at boot (os_wifi_boot) and when the user turns Wi-Fi on, and lets radio-hungry apps (ESP-NOW
// sniffer, mesh apps) borrow the radio with suspend/resume. While no network
// is joined the radio waits on the park channel (the mesh channel), so
// ESP-NOW keeps working with Wi-Fi "off".

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

typedef enum {
    OS_WIFI_IDLE,           // off, no saved network, or disconnected
    OS_WIFI_CONNECTING,
    OS_WIFI_CONNECTED,
    OS_WIFI_FAILED,         // gave up; see os_wifi_fail_reason()
    OS_WIFI_SUSPENDED,      // radio lent to an app
} os_wifi_state_t;

typedef struct {
    os_wifi_state_t state;
    char ssid[33];
    char ip[16];
    int rssi;
    int channel;
    const char *fail_reason;
    bool enabled;           // os_wifi_enabled()
} os_wifi_info_t;

esp_err_t os_wifi_init(void);
os_wifi_state_t os_wifi_state(void);
void os_wifi_get_info(os_wifi_info_t *out);

// Saved networks (os_wifi_saved.c, portable), most recent first: a network is
// saved, or moves to the front, once a join gets an IP. The oldest drops off
// past OS_WIFI_SAVED_MAX. os_wifi_saved_ssid/pass are the most recent one, the
// network "Wi-Fi on" joins.
#define OS_WIFI_SAVED_MAX 5
bool os_wifi_saved_ssid(char *out, size_t n);
bool os_wifi_saved_pass(char *out, size_t n);
int os_wifi_saved_count(void);
bool os_wifi_saved_get(int i, char *ssid, size_t ssid_n, char *pass, size_t pass_n);
bool os_wifi_saved_find(const char *ssid, char *pass, size_t pass_n);  // pass may be NULL
void os_wifi_saved_put(const char *ssid, const char *pass);
bool os_wifi_saved_remove(const char *ssid);

// Async scan (both bands). Poll os_wifi_scan_done(), then fetch results
// sorted by RSSI.
esp_err_t os_wifi_scan_start(void);
bool os_wifi_scan_done(void);
int os_wifi_scan_results(wifi_ap_record_t *out, int max);

// Wi-Fi starts off; os_wifi_boot() then joins the most recent saved network if
// it shows up within 30 s, else an older saved one in range (docs/HHGTS.md).
// The radio runs either way (ESP-NOW needs it), but no network is joined while
// Wi-Fi is off: until then, or until the user or a Lua app with the "wifi"
// permission turns it on. On = join the most recent saved network (no-op
// without one); off = leave it. Enabled stays true through retries and failures until it is
// turned off, the network is forgotten, or the badge reboots.
void os_wifi_set_enabled(bool on);
bool os_wifi_enabled(void);
// Bumped by every os_wifi_connect/_ch, os_wifi_set_enabled and os_wifi_forget
// call: background code that drives Wi-Fi (os_wifi_boot.c) compares it to see
// whether anyone else has since, even if Wi-Fi ended up as it was.
uint32_t os_wifi_changes(void);

// At boot, after os_mesh_init(): with a saved network, turn Wi-Fi on and turn it
// off again if it hasn't joined within 30 s. With older networks saved too, one
// that isn't found (it gives up, or 30 s pass) makes way for the most recent
// older one a scan finds, which gets 30 s of its own. Portable (os_wifi_boot.c).
void os_wifi_boot(void);

// Join a network (turns Wi-Fi on). Credentials are saved to NVS once the
// connection gets an IP. An AP of the SSID on the mesh channel is preferred
// (online and on the mesh at once), else the strongest one anywhere.
esp_err_t os_wifi_connect(const char *ssid, const char *pass);
// Same, preferring an AP on `channel` (the band row the user picked; 0 = as above).
esp_err_t os_wifi_connect_ch(const char *ssid, const char *pass, int channel);
void os_wifi_forget(void);              // every saved network; also turns Wi-Fi off

// Channel the radio waits on while no network is joined (os_mesh sets the
// mesh channel). 0 = leave the radio where it is.
void os_wifi_set_park_channel(int channel);

void os_wifi_suspend(void);
void os_wifi_resume(void);
