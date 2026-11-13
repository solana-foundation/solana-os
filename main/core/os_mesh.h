#pragma once

// Badge-to-badge and badge-to-beacon messaging over ESP-NOW (docs/MESH.md).
//
// Portable: framing, fragmentation, presence (HELLO) and the peer table live in
// os_mesh.c; the raw radio is os_radio (esp_now on the badge, a
// BroadcastChannel between browser tabs in the emulator). A "mesh" task owns
// the radio. It broadcasts HELLO every ~2 s while the radio is on the mesh
// channel and reassembles incoming messages.
//
// Badges that share no radio talk over a bridge through the server instead
// (os_bridge.h, docs/MESH.md "Bridges"). A bridged badge is a peer like any
// other (bridged = true): os_mesh_send() to its id, or to everyone, goes over
// the bridge, and what it sends arrives through the same handlers.
//
// Handlers run on the mesh task, NOT the OS task: copy what you need into your
// own state (under your own lock) and render it from on_tick, or hop over with
// os_post(). Keep them short: the mesh task has ~2 KB of stack to spare, so
// no SD I/O or HTTP there, and never take the LVGL lock (os_toast() and
// os_post() are fine). After os_mesh_unsubscribe() returns, the handler is
// not running and won't be called again.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OS_MESH_ID_LEN      6
#define OS_MESH_MSG_MAX     1600        // assembled JSON message, bytes
#define OS_MESH_PEER_TTL_MS 30000       // peers vanish 30 s after their last HELLO
#define OS_MESH_TAG_LEN     16          // hex chars of an HMAC tag (8 bytes)

typedef enum {
    OS_MESH_HELLO = 1,
    OS_MESH_XDEX = 2,
    OS_MESH_TRADE = 3,
    OS_MESH_RELAY = 4,
    OS_MESH_RELAY_RESP = 5,
    OS_MESH_APP = 6,
    OS_MESH_TYPE_MAX,
} os_mesh_type_t;

typedef enum {
    OS_PEER_BADGE,
    OS_PEER_BEACON,
} os_peer_kind_t;

typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    uint8_t mac[6];
    os_peer_kind_t kind;
    char name[25];          // badge: owner label ("" while unclaimed); beacon: character name
    char title[33];         // beacon: character title
    char character[17];     // beacon: character id
    uint32_t color;         // beacon: character accent 0xRRGGBB
    char nonce[17];         // beacon: current relay nonce
    int level;              // badge: Level
    int8_t rssi;            // 0 when bridged
    int64_t last_ms;        // esp_timer ms of the last frame heard (bridged: of the last sync)
    bool bridged;           // over a bridge (no MAC), not the radio
} os_mesh_peer_t;

void os_mesh_init(void);                    // after os_wifi_init() and os_cloud_init()

// Radio on the mesh channel or a bridge open, and this badge has an id and a mesh key.
bool os_mesh_ready(void);
int os_mesh_channel(void);                  // the channel peers are expected on

// A mesh channel (docs/MESH.md "Which channels"): 2.4 GHz 1 to 13, or a 5 GHz
// channel without radar rules (36, 40, 44, 48, 149, 153, 157, 161, 165). The
// driver would also send on the radar (DFS) channels 52 to 144, so every
// channel that comes in (hello, NVS, console, the ESP-NOW app) goes through
// this first. Portable.
bool os_mesh_channel_ok(int channel);
// The band of a Wi-Fi channel, for people and mesh.info(): "2.4" or "5".
const char *os_mesh_band(int channel);

// Use `channel` for the mesh instead of the one the server announced, for
// beacons whose access point is on another channel (the ESP-NOW app's Mesh
// tab finds them). It lasts until reboot, or until the server announces a
// different channel; 0 goes back to the server's. Applied within a second.
// False (and nothing changes) for a channel that isn't a mesh channel.
bool os_mesh_set_channel(int channel);
int os_mesh_server_channel(void);           // what the server announced, 0 before the first hello
bool os_mesh_channel_overridden(void);

// A Lua app's own channel (LUA_API.md "Choosing a channel"): while set it
// wins over the user's and the server's, and Wi-Fi joined on another channel
// is suspended for the mesh holder (as by os_mesh_acquire). 0 goes back. The
// Lua host clears it when the app ends. False for a channel that isn't a mesh
// channel. Applied at once.
bool os_mesh_app_channel(int channel);
// The channel without the app's choice: the user's, else the server's.
int os_mesh_event_channel(void);

// Scans (LUA_API.md "Choosing a channel"): the mesh task hops through
// `channels`, `dwell_ms` on each, and only listens. HELLOs and APP messages
// for `app` heard meanwhile are noted per channel; nothing is handled or sent
// (no HELLOs, os_mesh_send fails), even on the mesh channel. done(ctx) runs on the mesh task
// at the end (post from it, nothing more); os_mesh_scan_stop(), or a bridge
// taking Wi-Fi (os_mesh_keep_wifi), ends it without calling done. Needs a mesh holder (os_mesh_acquire), no bridge
// keeping Wi-Fi and no beacon: ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FINISHED
// while another scan runs, ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG.
#define OS_MESH_SCAN_CH_MAX     22
#define OS_MESH_SCAN_HITS       24
typedef struct {
    uint8_t channel;
    char id[OS_MESH_ID_LEN + 1];
    os_peer_kind_t kind;
    char name[25];
    int8_t rssi;
    int16_t level;
    bool app;               // sent this app's APP messages
    bool hello;             // its HELLO was heard (else only an APP message)
} os_mesh_scan_hit_t;
typedef void (*os_mesh_scan_done_t)(void *ctx);
esp_err_t os_mesh_scan(const uint8_t *channels, int n, int dwell_ms, const char *app, os_mesh_scan_done_t done,
                       void *ctx);
void os_mesh_scan_stop(void);
bool os_mesh_scanning(void);
// A scan ended and its results wait for os_mesh_scan_take (false once taken or stopped).
bool os_mesh_scan_ready(void);
// After done: the channels visited and what was heard, then the results are
// freed. Returns the hits (<= max), *n_ch the channels (<= OS_MESH_SCAN_CH_MAX).
int os_mesh_scan_take(uint8_t channels[OS_MESH_SCAN_CH_MAX], int *n_ch, os_mesh_scan_hit_t *out, int max);

// Peers heard in the last OS_MESH_PEER_TTL_MS, strongest first.
int os_mesh_peers(os_mesh_peer_t *out, int max);
bool os_mesh_peer(const char *id, os_mesh_peer_t *out);
// Keep a peer in the table whatever else is heard (a trade partner, an
// accepted X Dex peer); unpin when done. False if the id isn't in the table.
// Only HELLOs add peers and set their MAC: other messages from an unknown id
// or another MAC are dropped before any handler sees them.
bool os_mesh_pin(const char *id, bool pin);

// Send a JSON object (NUL-terminated, ≤ OS_MESH_MSG_MAX) to a peer id, or to
// everyone with to_id == NULL. Fragments as needed. Never blocks for long.
esp_err_t os_mesh_send(os_mesh_type_t type, const char *to_id, const char *json);

typedef void (*os_mesh_handler_t)(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx);
int os_mesh_subscribe(os_mesh_type_t type, os_mesh_handler_t cb, void *ctx);   // handle, or -1
void os_mesh_unsubscribe(int handle);

// tag(meshKey, msg) as 16 lowercase hex + NUL (docs/MESH.md "Tags").
// false when the badge has no mesh key yet.
bool os_mesh_sign(const char *msg, char out[OS_MESH_TAG_LEN + 1]);
void os_mesh_nonce(char out[17]);           // 16 random lowercase hex + NUL

// Apps that talk over the mesh call acquire in on_enter and release in
// on_exit. If Wi-Fi is joined on a channel other than the mesh channel, the
// first acquire suspends Wi-Fi and the last release resumes it, except while
// os_mesh_keep_wifi() holds it.
void os_mesh_acquire(void);
void os_mesh_release(void);

// For os_bridge. Bridges need the network: while kept, mesh apps don't suspend
// Wi-Fi (a suspension of ours ends at once; it comes back if a mesh app is
// still open when the hold ends). Any task.
void os_mesh_keep_wifi(bool keep);

// tag(key, msg) under another 16-byte key (a beacon's scan key, MESH.md
// "Scan codes").
void os_mesh_tag(const uint8_t key[16], const char *msg, char out[OS_MESH_TAG_LEN + 1]);

// Badge beacons (game/beacon.c, docs/MESH.md "Badge beacons"). While a beacon
// id is set, every frame this badge sends carries it as src and the HELLO it
// broadcasts is the beacon's (os_mesh_beacon_hello(); none until then), not
// the badge's. Nearby and request toasts stop, the radio stays awake, and the
// mesh channel follows the joined Wi-Fi network's, as a beacon's radio follows
// its AP: Wi-Fi is never suspended for it. NULL or "" ends it. Don't mix it
// with os_mesh_acquire() (the Beacon app is the only app open meanwhile).
void os_mesh_beacon(const char *id);
void os_mesh_beacon_hello(const char *json);    // <= 200 bytes, copied; NULL = silent
bool os_mesh_beacon_on(void);

typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    char name[25];          // owner label, as a HELLO's "n"
    int level;
} os_mesh_bridged_t;
// The open bridges after a sync (OS task): listed as live badges with
// bridged = true, never evicted; ids missing from the list stop being peers.
// While bridged, radio frames claiming that id are dropped.
void os_mesh_set_bridged(const os_mesh_bridged_t *list, int n);
// A message the server carried from bridged badge `from`: checked and
// delivered on the mesh task like radio input. Any task; json is copied.
void os_mesh_bridge_rx(os_mesh_type_t type, const char *from, const char *json);
