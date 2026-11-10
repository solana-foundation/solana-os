#pragma once

// Bluetooth LE for the running Lua app (docs/LUA_API.md "ble", Solana OS 1.0.0).
//
// The badge has one radio. Wi-Fi (either band) and ESP-NOW keep working while
// BLE is on: the radio is shared in time, so a busy scan slows Wi-Fi a little.
// BLE costs about 30 KB of the badge's heap while it is on (measured on 1.0.0:
// NimBLE with both roles, one link, no pairing, controller code in flash; up
// to ~39 KB with a link open and a scan running), so it is OFF until an app
// asks, has one owner at a time, and goes off when that app ends.
//
// One link at a time, either way round: the badge connects to a device
// (central: os_ble_connect), or serves one service and a device connects to it
// (peripheral: os_ble_serve + a connectable advertisement). No pairing, no
// encryption: everything a peer sends is hostile, and nothing sent is secret.
//
// The owner calls from its own task (the Lua task). Events wait here until it
// takes them (os_ble_next), and wake() tells it there is one, as os_socket
// does. What peers send (SCAN, NOTIFY, WRITE, SUBSCRIBE) waits only while
// fewer than OS_BLE_EVENTS_MAX events do; beyond that it is dropped, newest
// first (BLE has no flow control a peer must honour; a WRITE still becomes
// the value). OPEN, CLOSE and DONE are never dropped.
//
// Lengths on a link: a notification and a write without response carry at
// most the link's MTU - 3 bytes, a read's answer MTU - 1 (NimBLE asks for an
// MTU of 256; it is 23 until the peer agrees), cut there; a write with
// response longer than MTU - 3 goes as a long write.
//
// Portable interface: core/os_ble.c on the badge (NimBLE, device-only), and
// emulator/platform/os_ble_emu.c in the emulator, where the "air" is a
// BroadcastChannel shared by the origin's tabs, like os_radio_emu.c, so two
// emulated badges find, connect to and serve each other.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define OS_BLE_DATA_MAX     244     // bytes per read, write or notification
#define OS_BLE_ADV_MAX      24      // app bytes in an advertisement (20 with a 16-bit service, 6 with a 128-bit one)
#define OS_BLE_NAME_MAX     29      // bytes of an advertised name
#define OS_BLE_CHARS_MAX    4       // characteristics of the served service
#define OS_BLE_SERVICES_MAX 4       // service UUIDs reported per advertisement
#define OS_BLE_EVENTS_MAX   8       // events waiting for the owner
#define OS_BLE_OPS_MAX      4       // reads, writes with response and subscribes waiting for their answer
#define OS_BLE_MIN_FREE     (64 * 1024)     // os_ble_start refuses below this much free heap

// A UUID as written: 16-bit "180f" -> len 2, {0x18, 0x0f}; 128-bit
// "6e400001-b5a3-f393-e0a9-e50e24dcca9e" -> len 16, {0x6e, 0x40, ...}.
typedef struct {
    uint8_t len;                    // 2 or 16
    uint8_t b[16];
} os_ble_uuid_t;

// An address as printed, "aa:bb:cc:dd:ee:ff" -> val[0] = 0xaa.
typedef struct {
    uint8_t val[6];
    bool random;                    // a random address (most phones), else public
} os_ble_addr_t;

typedef enum {
    OS_BLE_EV_SCAN,                 // an advertisement: addr, rssi, connectable, name, services, company, data
    OS_BLE_EV_OPEN,                 // the link is up: central (we connected) or not (a device connected to us), addr
    OS_BLE_EV_CLOSE,                // the link ended, or the connect failed: central, reason
    OS_BLE_EV_NOTIFY,               // central: the peer notified: chr, data
    OS_BLE_EV_WRITE,                // served: the peer wrote: chr, data
    OS_BLE_EV_SUBSCRIBE,            // served: the peer subscribed (ok) or unsubscribed: chr
    OS_BLE_EV_DONE,                 // central: the answer to op: ok, then data (a read) or reason
} os_ble_ev_kind_t;

typedef struct {
    os_ble_ev_kind_t kind;
    os_ble_addr_t addr;
    int8_t rssi;
    bool connectable;
    bool central;
    bool ok;
    char name[OS_BLE_NAME_MAX + 1];             // printable ASCII, "" when the advertisement has none
    os_ble_uuid_t services[OS_BLE_SERVICES_MAX];
    uint8_t n_services;
    uint16_t company;                           // manufacturer data's company id, 0 when there is none
    os_ble_uuid_t chr;
    uint32_t op;
    // CLOSE: "closed" (os_ble_disconnect), "peer closed", "lost" (the peer went
    // out of range), "unreachable" (the connect got no answer in 10 s), "off"
    // (os_ble_stop). DONE, not ok: "no such characteristic", "not allowed" (the
    // peer refused), "lost" (the link ended first), "timeout".
    const char *reason;                         // static
    // SCAN: the manufacturer data after its two company bytes. NOTIFY, WRITE,
    // DONE of a read: the value. malloc'd, the taker frees it; NULL when empty.
    uint8_t *data;
    size_t len;
} os_ble_ev_t;

// Any task, without locks held: an event is waiting for ctx.
typedef void (*os_ble_wake_t)(void *ctx);

// Boot (app_main.c, and app_main_emu.c in the emulator), before the console
// starts: registers the "ble" console command (os_cmd). BLE stays off.
void os_ble_init(void);

// false: this build has no BLE (os_ble_start then answers ESP_ERR_NOT_SUPPORTED).
bool os_ble_available(void);

// Turns BLE on for ctx (unique, as for os_socket). May block up to a second
// while the stack starts. ESP_ERR_NO_MEM: under OS_BLE_MIN_FREE free,
// ESP_ERR_INVALID_STATE: another owner has it.
esp_err_t os_ble_start(os_ble_wake_t wake, void *ctx);

// Ends everything ctx started (advertising, scan, link, service) and turns BLE
// off: its heap comes back and no more events arrive for ctx. Waiting events
// are freed. Safe when ctx never started it, from any task.
void os_ble_stop(void *ctx);

bool os_ble_active(void);                       // on, for anyone
bool os_ble_address(os_ble_addr_t *out);        // this badge's own address; false while off

// --- being found -----------------------------------------------------------

typedef struct {
    const char *name;               // NULL: "Badge <id>" ("Badge" before the badge has an id)
    const os_ble_uuid_t *service;   // NULL: none
    const uint8_t *data;            // app bytes, sent as manufacturer data with company 0xFFFF
    size_t len;
    bool connectable;               // devices may connect (to the served service)
    uint16_t interval_ms;           // 0: 100 ms; 20 to 10 000
} os_ble_adv_t;

// Starts or replaces the advertisement; NULL stops it. The advertisement holds
// the flags, the service and the data; the name goes in the scan response
// (active scanners see it). ESP_ERR_INVALID_SIZE: the name or data is too long
// (OS_BLE_NAME_MAX, OS_BLE_ADV_MAX and its smaller limits with a service),
// ESP_ERR_INVALID_STATE: not started by ctx.
esp_err_t os_ble_advertise(void *ctx, const os_ble_adv_t *adv);

// --- finding ---------------------------------------------------------------

typedef struct {
    const os_ble_uuid_t *service;   // only devices advertising this service; NULL: all
    const char *name_prefix;        // only names starting with this; NULL: all
    bool active;                    // ask each device for its scan response (its name)
    uint32_t seconds;               // 0: until stopped
    uint8_t duty;                   // percent of the time the radio listens, 10 to 100; 0: 30
} os_ble_scan_t;

// Starts or replaces the scan; NULL stops it. Each device is reported at most
// once a second (OS_BLE_EV_SCAN). A scan ends by itself after `seconds`, and
// when the badge connects. ESP_ERR_INVALID_STATE: not started by ctx, or a
// connect is in progress.
esp_err_t os_ble_scan(void *ctx, const os_ble_scan_t *scan);

// --- being connected to (peripheral) ----------------------------------------

typedef struct {
    os_ble_uuid_t uuid;
    bool read, write, notify;       // what a connected device may do with it
} os_ble_chr_t;

// The one service this badge offers, once per os_ble_start and before any
// link: 1 to OS_BLE_CHARS_MAX characteristics. A device's reads are answered
// from the value last set, without an event; its writes arrive as
// OS_BLE_EV_WRITE (and become the value; over OS_BLE_DATA_MAX it is refused).
// ESP_ERR_INVALID_STATE: already serving, or a link exists.
esp_err_t os_ble_serve(void *ctx, const os_ble_uuid_t *service, const os_ble_chr_t *chrs, int n);

// Sets a served characteristic's value (≤ OS_BLE_DATA_MAX). notify: also sends
// it to the connected device: ESP_ERR_INVALID_STATE when none is connected,
// ESP_ERR_NOT_ALLOWED when it hasn't subscribed, ESP_ERR_NO_MEM when the stack
// has no buffer for it now ("busy"). ESP_ERR_NOT_FOUND: no such characteristic.
esp_err_t os_ble_set(void *ctx, const os_ble_uuid_t *chr, const uint8_t *data, size_t len, bool notify);

// --- connecting (central) ----------------------------------------------------

// OS_BLE_EV_OPEN or OS_BLE_EV_CLOSE follows. ESP_ERR_INVALID_STATE: a link
// exists or is being made, either way round, or the last link's CLOSE hasn't
// been taken yet. Connecting stops a scan and a connectable advertisement
// (a device connecting to us meanwhile would take the one link); advertise
// again after the CLOSE.
esp_err_t os_ble_connect(void *ctx, const os_ble_addr_t *addr);

// Each queues one operation on the link we opened and returns its id in *op;
// OS_BLE_EV_DONE with that id follows (a write without response has none, and
// *op is 0). The service and characteristic are looked up on first use and
// remembered for the link. ESP_ERR_INVALID_STATE: no open central link,
// ESP_ERR_INVALID_SIZE: over OS_BLE_DATA_MAX, ESP_ERR_NO_MEM: OS_BLE_OPS_MAX
// already wait.
esp_err_t os_ble_read(void *ctx, const os_ble_uuid_t *service, const os_ble_uuid_t *chr, uint32_t *op);
esp_err_t os_ble_write(void *ctx, const os_ble_uuid_t *service, const os_ble_uuid_t *chr, const uint8_t *data,
                       size_t len, bool response, uint32_t *op);
esp_err_t os_ble_subscribe(void *ctx, const os_ble_uuid_t *service, const os_ble_uuid_t *chr, bool on, uint32_t *op);

// Ends the link, whichever side opened it: OS_BLE_EV_CLOSE "closed" follows.
void os_ble_disconnect(void *ctx);

// --- events -------------------------------------------------------------------

// The next event for ctx, oldest first; false: none waiting. The taker frees
// ev->data.
bool os_ble_next(void *ctx, os_ble_ev_t *ev);
