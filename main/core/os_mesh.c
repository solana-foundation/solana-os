// Mesh service (docs/MESH.md), see os_mesh.h.
//
// One "mesh" task owns the protocol: it drains the frames the radio hands
// over (os_radio's rx callback only copies them into a queue), reassembles
// fragments, drops duplicates, keeps the peer table from HELLOs, raises the
// background toasts and calls subscribers. It also broadcasts our HELLO every
// 2 s +- 0.5 s while the radio is on the mesh channel. Senders fragment on
// their own task under a tx lock.
//
// RAM is fixed: two reassembly slots, a small peer table and a dedup ring,
// all static. Portable: the emulator compiles this file unchanged over its
// BroadcastChannel radio, so SHA-256/HMAC are implemented here too.
//
// Bridged badges (os_bridge.c) sit in the same peer table without a MAC:
// sends to them go to os_bridge, and what they send comes back through
// os_mesh_bridge_rx() onto this task, into the same checks and handlers.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "os.h"
#include "os_cloud.h"
#include "os_bridge.h"
#include "os_cmd.h"
#include "os_mesh.h"
#include "os_radio.h"
#include "os_wifi.h"

static const char *TAG = "os_mesh";

#define HDR_LEN         16
#define FRAG_DATA       232             // HDR_LEN + FRAG_DATA <= OS_RADIO_FRAME_MAX
#define FRAG_MAX        8
#define REASM_SLOTS     2
#define REASM_MS        2000            // incomplete messages are dropped after 2 s
#define DEDUP_N         32
#define DEDUP_MS        10000
#define PEER_MAX        20
#define SUB_MAX         12
#define RXQ_LEN         6
#define BRQ_LEN         8               // bridged messages waiting for the mesh task
#define HELLO_MS        1500            // + 0..1000 ms jitter
#define MAINTAIN_MS     250
#define SYNC_MS         1000            // identity and channel from os_cloud
#define TX_RETRIES      25              // ~50 ms of "driver queue full"
#define BEACON_TOAST_MS (10 * 60 * 1000)    // per character
#define BEACON_GAP_MS   30000               // between any two beacon toasts
#define ASK_TOAST_MS    15000               // per peer, swap/trade requests
#define ASK_GAP_MS      8000                // between any two swap/trade toasts
#define NEST_MAX        8                   // deeper JSON is dropped before anyone parses it
#define MAC_CHANGE_N    3                   // HELLOs in a row from a new MAC before an id moves...
#define MAC_CHANGE_MS   6000                // ...over at least this long (a real peer says hello every 1.5-2.5 s)
#define MAC_IDS_MAX     2                   // live ids per radio MAC: a badge, and the beacon it turns into

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint8_t len;
    uint8_t ch;                 // where it was heard (0 = unknown)
    uint8_t data[OS_RADIO_FRAME_MAX];
} rx_t;

typedef struct {
    uint8_t mac[6];
    uint16_t msg;
    uint8_t type, cnt;
    uint8_t ch;                 // where its first fragment was heard
    uint8_t got;                // bitmask of received fragments
    uint16_t len;
    int64_t t0;                 // 0 = free
    char src[OS_MESH_ID_LEN + 1];
    char buf[OS_MESH_MSG_MAX + 1];
} reasm_t;

typedef struct {
    uint8_t mac[6];
    uint16_t msg;
    int64_t t;
} seen_t;

typedef struct {
    os_mesh_type_t type;
    os_mesh_handler_t cb;
    void *ctx;
} sub_t;

typedef struct {
    uint8_t type;
    char from[OS_MESH_ID_LEN + 1];
    char *json;                 // malloc'd
} brx_t;

static QueueHandle_t s_rxq, s_brq;
static SemaphoreHandle_t s_lock;        // peers, state, counters
static SemaphoreHandle_t s_tx_lock;     // message counter + fragment order
static SemaphoreHandle_t s_sub_lock;    // recursive: held while handlers run
static TaskHandle_t s_task;

static os_mesh_peer_t s_peers[PEER_MAX];
// Per entry of s_peers (same index): pinning, bridging and the MAC-change vote.
static struct {
    bool pinned;
    bool bridged;                       // over a bridge: no radio frame speaks for it
    uint8_t next_mac[6];                // a different MAC claiming this id...
    uint8_t next_n;                     // ...in this many HELLOs in a row
    int64_t next_ms;                    // ...since then
} s_meta[PEER_MAX];
static reasm_t s_reasm[REASM_SLOTS];
static seen_t s_seen[DEDUP_N];
static int s_seen_next;
static sub_t s_subs[SUB_MAX];
static char s_one[FRAG_DATA + 1];       // single-fragment messages, mesh task only
static uint8_t s_mac[6];
static uint16_t s_msg;

// What the mesh task last learned from os_cloud.
static char s_id[OS_MESH_ID_LEN + 1];
static char s_label[25];
static int s_level, s_channel;
static bool s_has_key;
static int s_server_ch, s_override;     // s_lock: the server's channel, and the user's choice (0 = none)
static int s_app_ch;                    // s_lock: a Lua app's own channel (0 = none), over both

// A scan (os_mesh_scan), s_lock. Made when one starts, freed when its results
// are taken or it is stopped: ~1.2 KB only while an app scans.
#define SCAN_APP_MAX    32
#define SCAN_STUCK_MS   3000                // a hop the radio never reached is given up after dwell + this
typedef struct {
    uint8_t ch[OS_MESH_SCAN_CH_MAX];
    int n, idx;
    int dwell;
    int64_t hop_ms;                         // when this hop began
    int64_t next_ms;                        // when it ends: 0 until the radio is on its channel
    bool active;
    bool took_wifi;                         // the scan suspended Wi-Fi (joined on the mesh channel)
    int wifi_ch;                            // the network's channel then (0 = still joining)
    char app[SCAN_APP_MAX + 1];
    os_mesh_scan_done_t done;
    void *ctx;
    int nhits;
    os_mesh_scan_hit_t hits[OS_MESH_SCAN_HITS];
} scan_t;
static scan_t *s_scan;

// Badge beacon mode (os_mesh_beacon), s_lock: the id frames go out with and
// the HELLO to broadcast ("" = none).
#define BEACON_HELLO_MAX 200
static char s_beacon[OS_MESH_ID_LEN + 1];
static char s_beacon_hello[BEACON_HELLO_MAX + 1];

static int s_acquired;
static bool s_took_wifi;                // our acquire suspended Wi-Fi
static bool s_keep_wifi;                // os_mesh_keep_wifi(): bridges need the network
static bool s_log;                      // console "mesh log on"

static struct {
    uint32_t tx_msgs, tx_frames, tx_err, hello_tx;
    uint32_t rx_frames, rx_bad, rx_dup, rx_drop, rx_msgs, rx_multi, reasm_lost, rx_spoof, rx_deep;
} s_n;

static struct {
    char who[17];
    int64_t t;
} s_toasted[8];
static int64_t s_last_beacon_toast, s_last_ask_toast;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// ---------------------------------------------------------------------------
// SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104)

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t buf[64];
    size_t n;
} sha256_t;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_t *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d;
    s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void sha256_init(sha256_t *s)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof(iv));
    s->bytes = 0;
    s->n = 0;
}

static void sha256_update(sha256_t *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->bytes += len;
    while (len) {
        size_t take = 64 - s->n < len ? 64 - s->n : len;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        len -= take;
        if (s->n == 64) {
            sha256_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32])
{
    uint64_t bits = s->bytes * 8;
    uint8_t pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56) {
        sha256_update(s, &pad, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = bits >> (56 - i * 8);
    }
    sha256_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = s->h[i] >> 24;
        out[i * 4 + 1] = s->h[i] >> 16;
        out[i * 4 + 2] = s->h[i] >> 8;
        out[i * 4 + 3] = s->h[i];
    }
}

static void hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32])
{
    uint8_t k[64] = {0}, pad[64];
    sha256_t s;
    if (klen > 64) {
        sha256_init(&s);
        sha256_update(&s, key, klen);
        sha256_final(&s, k);
    } else {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, msg, mlen);
    sha256_final(&s, out);
    for (int i = 0; i < 64; i++) {
        pad[i] = k[i] ^ 0x5c;
    }
    sha256_init(&s);
    sha256_update(&s, pad, 64);
    sha256_update(&s, out, 32);
    sha256_final(&s, out);
}

static void to_hex(const uint8_t *b, int n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 15];
    }
    out[n * 2] = 0;
}

static void tag_with(const uint8_t key[16], const char *msg, char out[OS_MESH_TAG_LEN + 1])
{
    uint8_t mac[32];
    hmac_sha256(key, 16, msg, strlen(msg), mac);
    to_hex(mac, OS_MESH_TAG_LEN / 2, out);
}

void os_mesh_tag(const uint8_t key[16], const char *msg, char out[OS_MESH_TAG_LEN + 1])
{
    tag_with(key, msg ? msg : "", out);
}

bool os_mesh_sign(const char *msg, char out[OS_MESH_TAG_LEN + 1])
{
    uint8_t key[16];
    if (!msg || !os_cloud_mesh_key(key)) {
        out[0] = 0;
        return false;
    }
    tag_with(key, msg, out);
    memset(key, 0, sizeof(key));
    return true;
}

void os_mesh_nonce(char out[17])
{
    uint8_t b[8];
    esp_fill_random(b, sizeof(b));
    to_hex(b, 8, out);
}

// ---------------------------------------------------------------------------
// Peers

// Printable ASCII only (the fonts have no other glyphs), at most n - 1 chars.
static void clean_copy(char *out, size_t n, const char *in)
{
    size_t j = 0;
    for (const char *p = in ? in : ""; *p && j + 1 < n; p++) {
        if (*p >= 0x20 && *p < 0x7F) {
            out[j++] = *p;
        }
    }
    out[j] = 0;
}

static os_mesh_peer_t *find_peer_locked(const char *id)
{
    for (int i = 0; i < PEER_MAX; i++) {
        if (s_peers[i].id[0] && !strcmp(s_peers[i].id, id)) {
            return &s_peers[i];
        }
    }
    return NULL;
}

static bool live(const os_mesh_peer_t *p, int64_t now)
{
    return p->id[0] && p->last_ms && now - p->last_ms < OS_MESH_PEER_TTL_MS;
}

// The entry for id, creating it if needed. A full table (a crowded hall)
// gives up a silent entry first, else its weakest peer if the newcomer is
// stronger: "nearby" means close. NULL when the newcomer loses. A radio MAC
// (mac non-NULL) gets at most MAC_IDS_MAX live ids: one sender making up ids
// can't fill the table and push real badges out.
static os_mesh_peer_t *peer_slot_locked(const char *id, const uint8_t *mac, int rssi, bool *created)
{
    os_mesh_peer_t *p = find_peer_locked(id);
    *created = false;
    if (p) {
        return p;
    }
    int64_t now = now_ms();
    if (mac) {
        int same = 0;
        for (int i = 0; i < PEER_MAX; i++) {
            if (!s_meta[i].bridged && live(&s_peers[i], now) && !memcmp(s_peers[i].mac, mac, 6)) {
                same++;
            }
        }
        if (same >= MAC_IDS_MAX) {
            return NULL;
        }
    }
    os_mesh_peer_t *silent = NULL, *weak = NULL;
    for (int i = 0; i < PEER_MAX && !p; i++) {
        os_mesh_peer_t *q = &s_peers[i];
        if (!q->id[0]) {
            p = q;
        } else if (s_meta[i].pinned || s_meta[i].bridged) {
            continue;                   // an app's partner, or a bridge: never evicted
        } else if (!live(q, now)) {
            silent = !silent || q->last_ms < silent->last_ms ? q : silent;
        } else if (!weak || q->rssi < weak->rssi) {
            weak = q;
        }
    }
    p = p ? p : silent ? silent : weak && weak->rssi < rssi ? weak : NULL;
    if (p) {
        memset(p, 0, sizeof(*p));
        memset(&s_meta[p - s_peers], 0, sizeof(s_meta[0]));
        strlcpy(p->id, id, sizeof(p->id));
        *created = true;
    }
    return p;
}

static int by_rssi(const void *a, const void *b)
{
    return ((const os_mesh_peer_t *)b)->rssi - ((const os_mesh_peer_t *)a)->rssi;
}

int os_mesh_peers(os_mesh_peer_t *out, int max)
{
    int n = 0;
    int64_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PEER_MAX && n < max; i++) {
        if (live(&s_peers[i], now)) {
            out[n++] = s_peers[i];
        }
    }
    xSemaphoreGive(s_lock);
    qsort(out, n, sizeof(*out), by_rssi);
    return n;
}

bool os_mesh_pin(const char *id, bool pin)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    os_mesh_peer_t *p = id ? find_peer_locked(id) : NULL;
    if (p) {
        s_meta[p - s_peers].pinned = pin;
    }
    xSemaphoreGive(s_lock);
    return p != NULL;
}

bool os_mesh_peer(const char *id, os_mesh_peer_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    os_mesh_peer_t *p = id ? find_peer_locked(id) : NULL;
    if (p && live(p, now_ms())) {
        *out = *p;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

// ---------------------------------------------------------------------------
// State

int os_mesh_channel(void)
{
    return s_channel;
}

bool os_mesh_channel_ok(int channel)
{
    switch (channel) {
    case 36: case 40: case 44: case 48:
    case 149: case 153: case 157: case 161: case 165:
        return true;
    default:
        return channel >= 1 && channel <= 13;
    }
}

const char *os_mesh_band(int channel)
{
    return channel > 14 ? "5" : "2.4";
}

bool os_mesh_set_channel(int channel)
{
    if (channel && !os_mesh_channel_ok(channel)) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_override = channel > 0 ? channel : 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "mesh channel %s %d", channel > 0 ? "set to" : "back to the server's,", channel > 0 ? channel : s_server_ch);
    return true;
}

int os_mesh_server_channel(void)
{
    return s_server_ch;
}

bool os_mesh_channel_overridden(void)
{
    return s_override != 0;
}

int os_mesh_event_channel(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int ch = s_override ? s_override : s_server_ch;
    xSemaphoreGive(s_lock);
    return ch ? ch : s_channel;
}

// The ESP-NOW scanner borrowed the radio (Wi-Fi suspended, not by us).
static bool paused(void)
{
    return os_wifi_state() == OS_WIFI_SUSPENDED && !s_took_wifi;
}

static bool on_channel(void)
{
    return !paused() && s_channel && os_radio_channel() == s_channel;
}

bool os_mesh_ready(void)
{
    return (on_channel() || os_bridge_count() > 0) && s_id[0] && s_has_key;
}

static void use_channel(int ch);

// Pick up identity and channel changes from os_cloud (mesh task).
static void sync_cloud(void)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return;
    }
    os_cloud_get_info(in);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_id, strlen(in->id) == OS_MESH_ID_LEN ? in->id : "", sizeof(s_id));
    clean_copy(s_label, sizeof(s_label), in->claimed ? in->owner : "");
    s_level = in->level;
    s_has_key = in->has_key;
    int ch = in->mesh_channel;
    if (ch != s_server_ch) {
        // The server moved the mesh: a channel the user picked gives way to it.
        if (s_override && s_server_ch && ch) {
            ESP_LOGI(TAG, "the server announced channel %d: dropping the chosen channel %d", ch, s_override);
            s_override = 0;
        }
        s_server_ch = ch;
    }
    if (s_override) {
        ch = s_override;
    }
    if (s_app_ch) {
        ch = s_app_ch;                      // the running app's, until it ends
    }
    bool beacon = s_beacon[0] != 0;
    xSemaphoreGive(s_lock);
    free(in);
    if (beacon) {
        // A beacon's radio follows its AP (MESH.md "Badge beacons").
        os_wifi_info_t w;
        os_wifi_get_info(&w);
        if (w.state == OS_WIFI_CONNECTED && w.channel > 0) {
            ch = w.channel;
        }
    }
    use_channel(ch);
}

static bool wifi_joined(const os_wifi_info_t *w)
{
    return w->state == OS_WIFI_CONNECTED || w->state == OS_WIFI_CONNECTING;
}

// Move the mesh to ch (any task). Peers heard on the old channel are gone
// (unicasts to them would go nowhere), and a mesh holder takes the radio
// from a network on another channel, as its acquire would have.
static void use_channel(int ch)
{
    bool suspend = false;
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = ch != s_channel;
    if (changed) {
        s_channel = ch;
        for (int i = 0; i < PEER_MAX; i++) {
            if (!s_meta[i].bridged && !s_meta[i].pinned) {
                s_peers[i].last_ms = 0;
            }
        }
        if (s_acquired > 0 && !s_keep_wifi && !s_took_wifi && wifi_joined(&w) &&
                !(w.state == OS_WIFI_CONNECTED && w.channel == ch)) {
            suspend = s_took_wifi = true;
        }
    }
    xSemaphoreGive(s_lock);
    if (changed) {
        ESP_LOGI(TAG, "mesh channel %d", ch);
        os_wifi_set_park_channel(ch);
    }
    if (suspend) {
        ESP_LOGI(TAG, "Wi-Fi is off the new mesh channel: suspending it while the mesh app runs");
        os_wifi_suspend();
    }
}

// An empty frame wakes the mesh task now (any task but the mesh task).
static void wake(void)
{
    rx_t w = { .len = 0 };
    if (s_rxq) {
        xQueueSend(s_rxq, &w, 0);
    }
}

bool os_mesh_app_channel(int channel)
{
    if (channel && !os_mesh_channel_ok(channel)) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool same = s_app_ch == channel;
    s_app_ch = channel;
    int ch = channel ? channel : s_override ? s_override : s_server_ch;
    bool beacon = s_beacon[0] != 0;
    xSemaphoreGive(s_lock);
    if (same) {
        return true;                        // already applied (or nothing to undo)
    }
    ESP_LOGI(TAG, "app channel %s %d", channel ? "set to" : "cleared, back to", ch);
    if (!beacon && ch) {
        use_channel(ch);                    // now, not at the next sync
    }
    return true;
}

// Where the radio should be: the scan's channel while one runs, else the mesh's.
static int radio_target_locked(void)
{
    return s_scan && s_scan->active ? s_scan->ch[s_scan->idx] : s_channel;
}

// While our acquire (or a scan) holds the radio, os_wifi does not park it: we
// do. Otherwise os_wifi parks it, on the scan's channel while one runs.
static void hold_channel(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int target = radio_target_locked();
    bool took = s_took_wifi;
    xSemaphoreGive(s_lock);
    if (!target) {
        return;
    }
    if (took) {
        if (os_radio_channel() != target) {
            os_radio_set_channel(target);   // fails until the disconnect settles; retried
        }
    } else {
        os_wifi_set_park_channel(target);   // nothing while a network is joined or the radio is lent out
    }
}

// ---------------------------------------------------------------------------
// Scans

// A scan ends (s_lock held): give back the Wi-Fi it lent, unless the mesh
// holder needs it suspended now (mesh.channel or the server moved the mesh off
// the network's channel meanwhile, and the lent Wi-Fi kept use_channel from
// taking it): then s_took_wifi stays, the holder's, and its release resumes it.
// True when the caller should resume Wi-Fi (after s_lock).
static bool scan_return_wifi_locked(scan_t *sc)
{
    if (!sc || !sc->took_wifi) {
        return false;
    }
    sc->took_wifi = false;
    if (s_acquired > 0 && !s_keep_wifi && sc->wifi_ch != s_channel) {
        return false;
    }
    s_took_wifi = false;
    return true;
}

bool os_mesh_scanning(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_scan && s_scan->active;
    xSemaphoreGive(s_lock);
    return on;
}

esp_err_t os_mesh_scan(const uint8_t *channels, int n, int dwell_ms, const char *app, os_mesh_scan_done_t done,
                       void *ctx)
{
    if (!channels || n < 1 || n > OS_MESH_SCAN_CH_MAX || dwell_ms < 500 || dwell_ms > 5000 || !done ||
            (app && strlen(app) > SCAN_APP_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < n; i++) {
        if (!os_mesh_channel_ok(channels[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    scan_t *sc = calloc(1, sizeof(*sc));
    if (!sc) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(sc->ch, channels, n);
    sc->n = n;
    sc->dwell = dwell_ms;
    sc->hop_ms = now_ms();
    sc->active = true;
    strlcpy(sc->app, app ? app : "", sizeof(sc->app));
    sc->done = done;
    sc->ctx = ctx;

    os_wifi_info_t w;
    os_wifi_get_info(&w);
    scan_t *old = NULL;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_scan && s_scan->active) {
        err = ESP_ERR_NOT_FINISHED;
    } else if (!s_acquired || s_keep_wifi || s_beacon[0]) {
        err = ESP_ERR_INVALID_STATE;        // no holder, a bridge keeps Wi-Fi joined, or a beacon
    } else {
        if (!s_took_wifi && wifi_joined(&w)) {
            sc->took_wifi = s_took_wifi = true;     // joined on the mesh channel: lend it to the scan
            sc->wifi_ch = w.state == OS_WIFI_CONNECTED ? w.channel : 0;
        }
        old = s_scan;                       // results nobody took
        s_scan = sc;
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        free(sc);
        return err;
    }
    free(old);
    ESP_LOGI(TAG, "scan: %d channels, %d ms each%s", n, dwell_ms, sc->took_wifi ? " (Wi-Fi suspended meanwhile)" : "");
    if (sc->took_wifi) {
        os_wifi_suspend();
    }
    wake();
    return ESP_OK;
}

void os_mesh_scan_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    scan_t *sc = s_scan;
    s_scan = NULL;
    bool resume = scan_return_wifi_locked(sc);
    xSemaphoreGive(s_lock);
    if (sc && sc->active) {
        ESP_LOGI(TAG, "scan stopped");
    }
    free(sc);
    if (resume) {
        os_wifi_resume();
    }
    wake();
}

bool os_mesh_scan_ready(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ready = s_scan && !s_scan->active;
    xSemaphoreGive(s_lock);
    return ready;
}

int os_mesh_scan_take(uint8_t channels[OS_MESH_SCAN_CH_MAX], int *n_ch, os_mesh_scan_hit_t *out, int max)
{
    int n = 0;
    *n_ch = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    scan_t *sc = s_scan && !s_scan->active ? s_scan : NULL;
    if (sc) {
        s_scan = NULL;
    }
    xSemaphoreGive(s_lock);
    if (sc) {
        memcpy(channels, sc->ch, sc->n);
        *n_ch = sc->n;
        n = sc->nhits < max ? sc->nhits : max;
        memcpy(out, sc->hits, n * sizeof(*out));
        free(sc);
    }
    return n;
}

// Mesh task, every MAINTAIN_MS: the next hop once this one has lasted its
// dwell (counted from when the radio got there), done at the end.
static void scan_step(int64_t now)
{
    os_mesh_scan_done_t done = NULL;
    void *ctx = NULL;
    bool resume = false;
    int radio = os_radio_channel();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    scan_t *sc = s_scan;
    if (sc && sc->active) {
        if (!sc->next_ms && radio == sc->ch[sc->idx]) {
            sc->next_ms = now + sc->dwell;
        }
        bool over = sc->next_ms ? now >= sc->next_ms : now - sc->hop_ms > sc->dwell + SCAN_STUCK_MS;
        if (over && sc->idx + 1 < sc->n) {
            sc->idx++;
            sc->hop_ms = now;
            sc->next_ms = 0;
        } else if (over) {
            sc->active = false;
            done = sc->done;
            ctx = sc->ctx;
            resume = scan_return_wifi_locked(sc);
        }
    }
    xSemaphoreGive(s_lock);
    if (resume) {
        os_wifi_resume();
    }
    if (done) {
        ESP_LOGI(TAG, "scan done");
        done(ctx);
    }
}

// A HELLO or one of the scanning app's APP messages, heard during a scan
// (mesh task). False when no scan runs.
static bool scan_note(uint8_t type, const char *src, int rssi, int ch, const char *json)
{
    char app[SCAN_APP_MAX + 1];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_scan && s_scan->active;
    strlcpy(app, on ? s_scan->app : "", sizeof(app));
    xSemaphoreGive(s_lock);
    if (!on || (type != OS_MESH_HELLO && (type != OS_MESH_APP || !app[0] || !strstr(json, app)))) {
        return on;
    }
    if (!ch) {
        ch = os_radio_channel();            // the radio didn't say: where it is now
    }
    cJSON *j = cJSON_Parse(json);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return on;
    }
    bool hello = type == OS_MESH_HELLO;
    const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(j, "app"));
    if (!hello && (!a || strcmp(a, app))) {
        cJSON_Delete(j);
        return on;
    }
    const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(j, "k"));
    const cJSON *lv = cJSON_GetObjectItem(j, "lv");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    scan_t *sc = s_scan;
    bool visiting = false;
    for (int i = 0; sc && sc->active && i < sc->n && !visiting; i++) {
        visiting = sc->ch[i] == ch;
    }
    os_mesh_scan_hit_t *h = NULL;
    for (int i = 0; visiting && i < sc->nhits && !h; i++) {
        if (sc->hits[i].channel == ch && !strcmp(sc->hits[i].id, src)) {
            h = &sc->hits[i];
        }
    }
    if (visiting && !h && sc->nhits < OS_MESH_SCAN_HITS) {
        h = &sc->hits[sc->nhits++];
        memset(h, 0, sizeof(*h));
        h->channel = ch;
        strlcpy(h->id, src, sizeof(h->id));
        h->kind = OS_PEER_BADGE;
    }
    if (h) {
        h->rssi = rssi;
        if (hello) {
            h->hello = true;
            h->kind = k && !strcmp(k, "c") ? OS_PEER_BEACON : OS_PEER_BADGE;
            clean_copy(h->name, sizeof(h->name), cJSON_GetStringValue(cJSON_GetObjectItem(j, "n")));
            h->level = h->kind == OS_PEER_BADGE && cJSON_IsNumber(lv) ? (int16_t)lv->valuedouble : 0;
        } else {
            h->app = true;
        }
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(j);
    return on;
}

void os_mesh_acquire(void)
{
    bool suspend = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_acquired++ == 0) {
        os_wifi_info_t w;
        os_wifi_get_info(&w);
        bool joined = w.state == OS_WIFI_CONNECTED || w.state == OS_WIFI_CONNECTING;
        suspend = joined && !(w.state == OS_WIFI_CONNECTED && w.channel == s_channel) && !s_keep_wifi;
        s_took_wifi = suspend;
    }
    bool first = s_acquired == 1;
    xSemaphoreGive(s_lock);
    if (first) {
        os_radio_stay_awake(true);
    }
    if (suspend) {
        ESP_LOGI(TAG, "Wi-Fi is off the mesh channel: suspending it while a mesh app runs");
        os_wifi_suspend();
        xTaskNotifyGive(s_task);
    }
}

void os_mesh_keep_wifi(bool keep)
{
    bool resume = false, suspend = false;
    scan_t *ended = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (keep != s_keep_wifi) {
        s_keep_wifi = keep;
        if (keep) {
            if (s_scan && s_scan->active) {
                // A joined radio can't hop: the scan ends here, without its
                // callback (as mesh.pause() ends one), not with channels it never heard.
                ended = s_scan;
                s_scan = NULL;
            }
            resume = s_took_wifi;           // the scan's lend included
            s_took_wifi = false;
        } else if (s_acquired > 0) {
            // A mesh app is still open: give it the radio, as its acquire would have.
            os_wifi_info_t w;
            os_wifi_get_info(&w);
            bool joined = w.state == OS_WIFI_CONNECTED || w.state == OS_WIFI_CONNECTING;
            suspend = s_took_wifi = joined && !(w.state == OS_WIFI_CONNECTED && w.channel == s_channel);
        }
    }
    xSemaphoreGive(s_lock);
    if (ended) {
        ESP_LOGI(TAG, "a bridge needs Wi-Fi: scan stopped");
        free(ended);
    }
    if (resume) {
        ESP_LOGI(TAG, "a bridge needs Wi-Fi: resuming it");
        os_wifi_resume();
    }
    if (suspend) {
        ESP_LOGI(TAG, "no more bridges: suspending Wi-Fi again while the mesh app runs");
        os_wifi_suspend();
        xTaskNotifyGive(s_task);
    }
}

void os_mesh_beacon(const char *id)
{
    bool on = id && strlen(id) == OS_MESH_ID_LEN;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_beacon[0] != 0;
    strlcpy(s_beacon, on ? id : "", sizeof(s_beacon));
    s_beacon_hello[0] = 0;
    xSemaphoreGive(s_lock);
    if (on != was) {
        if (on) {
            ESP_LOGI(TAG, "playing beacon %s", id);
        } else {
            ESP_LOGI(TAG, "beacon mode off");
        }
        os_radio_stay_awake(on);            // modem sleep misses frames
    }
    if (s_task) {
        xTaskNotifyGive(s_task);            // pick up the channel now
    }
}

void os_mesh_beacon_hello(const char *json)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_beacon_hello, json && strlen(json) <= BEACON_HELLO_MAX ? json : "", sizeof(s_beacon_hello));
    xSemaphoreGive(s_lock);
}

bool os_mesh_beacon_on(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_beacon[0] != 0;
    xSemaphoreGive(s_lock);
    return on;
}

void os_mesh_release(void)
{
    bool resume = false, last = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_acquired > 0 && --s_acquired == 0) {
        last = true;
        resume = s_took_wifi;
        s_took_wifi = false;
    }
    xSemaphoreGive(s_lock);
    if (last) {
        os_radio_stay_awake(false);
    }
    if (resume) {
        os_wifi_resume();
    }
}

// ---------------------------------------------------------------------------
// Sending

static esp_err_t send_to(const uint8_t *mac, os_mesh_type_t type, const char *json)
{
    size_t len = strlen(json);
    int cnt = (len + FRAG_DATA - 1) / FRAG_DATA;
    if (!len || len > OS_MESH_MSG_MAX || cnt > FRAG_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    char id[OS_MESH_ID_LEN + 1];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(id, s_beacon[0] ? s_beacon : s_id, sizeof(id));
    xSemaphoreGive(s_lock);
    if (!id[0]) {
        return ESP_ERR_INVALID_STATE;       // MESH.md: no server-assigned id, no transmit
    }

    uint8_t f[HDR_LEN + FRAG_DATA];
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uint16_t msg = s_msg++;
    for (int idx = 0; idx < cnt && err == ESP_OK; idx++) {
        int n = idx < cnt - 1 ? FRAG_DATA : (int)(len - FRAG_DATA * (cnt - 1));
        f[0] = 'H';
        f[1] = 'G';
        f[2] = 1;
        f[3] = type;
        memcpy(f + 4, id, OS_MESH_ID_LEN);
        f[10] = msg & 0xFF;
        f[11] = msg >> 8;
        f[12] = idx;
        f[13] = cnt;
        f[14] = len & 0xFF;
        f[15] = len >> 8;
        memcpy(f + HDR_LEN, json + idx * FRAG_DATA, n);
        for (int tries = 0;; tries++) {
            err = os_radio_send(mac, f, HDR_LEN + n);
            if (err != ESP_ERR_NO_MEM || tries == TX_RETRIES) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (err == ESP_OK) {
            s_n.tx_frames++;
        }
    }
    xSemaphoreGive(s_tx_lock);
    if (err == ESP_OK) {
        s_n.tx_msgs++;
    } else {
        s_n.tx_err++;
        ESP_LOGW(TAG, "send type %d: %s", type, esp_err_to_name(err));
    }
    return err;
}

esp_err_t os_mesh_send(os_mesh_type_t type, const char *to_id, const char *json)
{
    if (type <= 0 || type >= OS_MESH_TYPE_MAX || !json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!to_id || !strcmp(to_id, "*")) {
        // Everyone in range, and every bridged badge.
        esp_err_t err = on_channel() ? send_to(NULL, type, json) : ESP_ERR_INVALID_STATE;
        if (type != OS_MESH_HELLO && os_bridge_count() > 0 && os_bridge_send(type, NULL, json) == ESP_OK) {
            err = ESP_OK;
        }
        return err;
    }
    uint8_t mac[6];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    os_mesh_peer_t *p = find_peer_locked(to_id);
    bool bridged = p && s_meta[p - s_peers].bridged;
    if (p) {
        memcpy(mac, p->mac, 6);
    }
    xSemaphoreGive(s_lock);
    if (bridged) {
        return os_bridge_send(type, to_id, json);
    }
    if (!on_channel()) {
        return ESP_ERR_INVALID_STATE;
    }
    return p ? send_to(mac, type, json) : ESP_ERR_NOT_FOUND;
}

static void send_hello(void)
{
    char json[64];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_beacon[0]) {
        // Playing a beacon: its HELLO, or silence until it has one.
        static char bj[BEACON_HELLO_MAX + 1];       // mesh task only
        strlcpy(bj, s_beacon_hello, sizeof(bj));
        xSemaphoreGive(s_lock);
        if (bj[0] && send_to(NULL, OS_MESH_HELLO, bj) == ESP_OK) {
            s_n.hello_tx++;
        }
        return;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "k", "b");
    cJSON_AddStringToObject(j, "n", s_label);
    cJSON_AddNumberToObject(j, "lv", s_level);
    xSemaphoreGive(s_lock);
    bool ok = j && cJSON_PrintPreallocated(j, json, sizeof(json), false);
    cJSON_Delete(j);
    if (ok && send_to(NULL, OS_MESH_HELLO, json) == ESP_OK) {
        s_n.hello_tx++;
    }
}

// ---------------------------------------------------------------------------
// Subscribers

int os_mesh_subscribe(os_mesh_type_t type, os_mesh_handler_t cb, void *ctx)
{
    int h = -1;
    xSemaphoreTakeRecursive(s_sub_lock, portMAX_DELAY);
    for (int i = 0; i < SUB_MAX; i++) {
        if (!s_subs[i].cb) {
            s_subs[i] = (sub_t){ .type = type, .cb = cb, .ctx = ctx };
            h = i;
            break;
        }
    }
    xSemaphoreGiveRecursive(s_sub_lock);
    if (h < 0) {
        ESP_LOGE(TAG, "no room for another subscriber");
    }
    return h;
}

// Waits for a handler that is running right now, so after this returns the
// callback is never called again (its ctx may be freed).
void os_mesh_unsubscribe(int handle)
{
    if (handle < 0 || handle >= SUB_MAX) {
        return;
    }
    xSemaphoreTakeRecursive(s_sub_lock, portMAX_DELAY);
    s_subs[handle].cb = NULL;
    xSemaphoreGiveRecursive(s_sub_lock);
}

static void dispatch(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json)
{
    xSemaphoreTakeRecursive(s_sub_lock, portMAX_DELAY);
    for (int i = 0; i < SUB_MAX; i++) {
        if (s_subs[i].cb && s_subs[i].type == type) {
            s_subs[i].cb(type, from, json, s_subs[i].ctx);
        }
    }
    xSemaphoreGiveRecursive(s_sub_lock);
}

// ---------------------------------------------------------------------------
// Receiving (mesh task)

static bool seen(const uint8_t *mac, uint16_t msg, int64_t now)
{
    for (int i = 0; i < DEDUP_N; i++) {
        if (s_seen[i].t && now - s_seen[i].t < DEDUP_MS && s_seen[i].msg == msg && !memcmp(s_seen[i].mac, mac, 6)) {
            return true;
        }
    }
    return false;
}

static void mark_seen(const uint8_t *mac, uint16_t msg, int64_t now)
{
    seen_t *s = &s_seen[s_seen_next];
    s_seen_next = (s_seen_next + 1) % DEDUP_N;
    memcpy(s->mac, mac, 6);
    s->msg = msg;
    s->t = now;
}

// Toast once per `who` within `quiet_ms`.
static bool toast_due(const char *who, int64_t quiet_ms, int64_t now)
{
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(s_toasted[i].who, who)) {
            if (now - s_toasted[i].t < quiet_ms) {
                return false;
            }
            slot = i;
            break;
        }
        if (s_toasted[i].t < s_toasted[slot].t) {
            slot = i;
        }
    }
    strlcpy(s_toasted[slot].who, who, sizeof(s_toasted[slot].who));
    s_toasted[slot].t = now;
    return true;
}

static bool app_open(const char *id)
{
    const os_app_t *app = os_current();
    return app && app->id && !strcmp(app->id, id);
}

// Is this HELLO from the MAC we know for its id? A live id moves to another
// MAC only after MAC_CHANGE_N HELLOs in a row from it over MAC_CHANGE_MS (one
// forged frame, or a quick burst between the real peer's HELLOs, can't hijack
// a peer: its next HELLO starts the count again), and a live beacon id never
// turns into a badge.
static bool hello_mac_ok_locked(os_mesh_peer_t *p, bool created, const uint8_t *mac, bool beacon, int64_t now)
{
    int i = p - s_peers;
    if (s_meta[i].bridged) {
        return false;                       // the server speaks for a bridged id, not the air
    }
    if (created || !live(p, now) || !memcmp(p->mac, mac, 6)) {
        s_meta[i].next_n = 0;
        return true;
    }
    if (p->kind == OS_PEER_BEACON && !beacon) {
        return false;
    }
    if (memcmp(s_meta[i].next_mac, mac, 6) || !s_meta[i].next_n) {
        memcpy(s_meta[i].next_mac, mac, 6);
        s_meta[i].next_n = 0;
        s_meta[i].next_ms = now;
    }
    if (s_meta[i].next_n < UINT8_MAX) {
        s_meta[i].next_n++;
    }
    if (s_meta[i].next_n < MAC_CHANGE_N || now - s_meta[i].next_ms < MAC_CHANGE_MS) {
        return false;
    }
    s_meta[i].next_n = 0;
    return true;
}

// Only HELLOs create peers and set their MAC, kind and name. Returns false
// when the HELLO is not accepted (then nobody hears about it).
static bool on_hello(const os_mesh_peer_t *heard, const char *json, int64_t now)
{
    cJSON *j = cJSON_Parse(json);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        s_n.rx_bad++;
        return false;
    }
    const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(j, "k"));
    const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(j, "n"));
    bool beacon = k && !strcmp(k, "c");
    char toast[48] = "";

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool created;
    os_mesh_peer_t *p = peer_slot_locked(heard->id, heard->mac, heard->rssi, &created);
    if (!p || !hello_mac_ok_locked(p, created, heard->mac, beacon, now)) {
        if (p && !created) {
            s_n.rx_spoof++;
        }
        xSemaphoreGive(s_lock);
        cJSON_Delete(j);
        return false;                       // table full of closer peers, or not the MAC we know
    }
    bool fresh = created || !live(p, now);
    memcpy(p->mac, heard->mac, 6);
    p->rssi = heard->rssi;
    p->last_ms = now;
    p->kind = beacon ? OS_PEER_BEACON : OS_PEER_BADGE;
    clean_copy(p->name, sizeof(p->name), n);
    if (beacon) {
        const char *col = cJSON_GetStringValue(cJSON_GetObjectItem(j, "col"));
        clean_copy(p->title, sizeof(p->title), cJSON_GetStringValue(cJSON_GetObjectItem(j, "t")));
        clean_copy(p->character, sizeof(p->character), cJSON_GetStringValue(cJSON_GetObjectItem(j, "ch")));
        clean_copy(p->nonce, sizeof(p->nonce), cJSON_GetStringValue(cJSON_GetObjectItem(j, "nonce")));
        p->color = col ? (uint32_t)strtoul(col, NULL, 16) & 0xFFFFFF : COL_TEXT;
        p->level = 0;
        if (fresh && p->name[0] && now - s_last_beacon_toast > BEACON_GAP_MS && !app_open("quests") && !s_beacon[0] &&
                toast_due(p->character[0] ? p->character : p->id, BEACON_TOAST_MS, now)) {
            s_last_beacon_toast = now;
            snprintf(toast, sizeof(toast), "%s is nearby", p->name);
        }
    } else {
        const cJSON *lv = cJSON_GetObjectItem(j, "lv");
        p->level = cJSON_IsNumber(lv) ? (int)lv->valuedouble : 0;
        p->title[0] = p->character[0] = p->nonce[0] = 0;
        p->color = 0;
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(j);
    if (toast[0]) {
        os_toast("%s", toast);
    }
    return true;
}

// "@bob wants to swap cards" / "@bob wants to trade", from any app but the one
// that handles it.
static void maybe_ask_toast(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, int64_t now)
{
    if (!strstr(json, "\"req\"") && !strstr(json, "\"invite\"")) {
        return;                             // cheap filter before parsing
    }
    cJSON *j = cJSON_Parse(json);
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(j, "op"));
    const char *to = cJSON_GetStringValue(cJSON_GetObjectItem(j, "to"));
    bool xdex = type == OS_MESH_XDEX && op && !strcmp(op, "req");
    bool trade = type == OS_MESH_TRADE && op && !strcmp(op, "invite");
    char name[25];
    clean_copy(name, sizeof(name), from->name);
    if (!name[0] && xdex) {
        const cJSON *card = cJSON_GetObjectItem(j, "card");
        clean_copy(name, sizeof(name), cJSON_GetStringValue(cJSON_GetObjectItem(card, "n")));
    }
    bool mine = to && !strcmp(to, s_id);
    cJSON_Delete(j);
    if (!mine || (!xdex && !trade) || app_open(xdex ? "xdex" : "trade") || os_mesh_beacon_on()) {
        return;
    }
    char who[17];
    snprintf(who, sizeof(who), "%s%s", xdex ? "x" : "t", from->id);
    if (now - s_last_ask_toast > ASK_GAP_MS && toast_due(who, ASK_TOAST_MS, now)) {
        s_last_ask_toast = now;
        os_toast("%s wants to %s", name[0] ? name : from->id, xdex ? "swap cards" : "trade");
    }
}

// cJSON parses recursively, ~80 B of stack per level: one frame of nested
// brackets could overflow the mesh task (or any task that parses it later)
// and reboot every badge in range. Nothing deeper than NEST_MAX gets past
// here. Brackets inside strings don't count.
static bool nesting_ok(const char *s, int max)
{
    int depth = 0;
    bool str = false, esc = false;
    for (; *s; s++) {
        if (str) {
            if (esc) {
                esc = false;
            } else if (*s == '\\') {
                esc = true;
            } else if (*s == '"') {
                str = false;
            }
        } else if (*s == '"') {
            str = true;
        } else if (*s == '[' || *s == '{') {
            if (++depth > max) {
                return false;
            }
        } else if (*s == ']' || *s == '}') {
            depth--;
        }
    }
    return true;
}

// mac NULL: a bridged message (os_mesh_bridge_rx), its sender named by the server.
static void deliver(uint8_t type, const uint8_t *mac, const char *src, int rssi, int ch, const char *json, int len,
                    int frags)
{
    bool bridged = mac == NULL;
    int64_t now = now_ms();
    os_mesh_peer_t from = {0};
    if (!nesting_ok(json, NEST_MAX)) {
        s_n.rx_deep++;
        return;
    }
    s_n.rx_msgs++;
    if (frags > 1) {
        s_n.rx_multi++;
    }
    if (s_log) {
        printf("mesh rx type %d from %s (%d B, %s, %d dBm): %.80s\n", type, src, len,
               bridged ? "bridged" : frags == 1 ? "1 frag" : "frags", rssi, json);
    }
    if (!bridged && scan_note(type, src, rssi, ch, json) && (ch ? ch != s_channel : !on_channel())) {
        return;                             // heard on a channel the scan visits: noted, nothing more
    }

    if (type == OS_MESH_HELLO) {
        if (bridged) {
            return;                         // MESH.md: HELLOs never travel over a bridge
        }
        strlcpy(from.id, src, sizeof(from.id));
        memcpy(from.mac, mac, 6);
        from.rssi = rssi;
        if (!on_hello(&from, json, now)) {
            return;
        }
    }
    // Everything else must come from the MAC a HELLO taught us for that id, or
    // over the bridge open with it (and then never from the air).
    xSemaphoreTake(s_lock, portMAX_DELAY);
    os_mesh_peer_t *p = find_peer_locked(src);
    bool known = p && (bridged ? s_meta[p - s_peers].bridged
                               : !s_meta[p - s_peers].bridged && !memcmp(p->mac, mac, 6));
    if (known) {
        if (!bridged) {
            p->rssi = rssi;
        }
        from = *p;
    }
    xSemaphoreGive(s_lock);
    if (!known) {
        s_n.rx_spoof++;
        return;
    }
    if (type == OS_MESH_XDEX || type == OS_MESH_TRADE) {
        maybe_ask_toast(type, &from, json, now);
    }
    dispatch(type, &from, json);
}

static void on_frame(const rx_t *rx)
{
    const uint8_t *f = rx->data;
    int64_t now = now_ms();
    s_n.rx_frames++;
    if (rx->len < HDR_LEN + 1 || f[0] != 'H' || f[1] != 'G') {
        s_n.rx_bad++;
        return;
    }
    uint8_t type = f[3], idx = f[12], cnt = f[13];
    uint16_t msg = f[10] | f[11] << 8, len = f[14] | f[15] << 8;
    int n = rx->len - HDR_LEN;
    if (f[2] != 1 || type < OS_MESH_HELLO || type >= OS_MESH_TYPE_MAX) {
        return;                             // MESH.md: unknown versions and types are ignored
    }
    char src[OS_MESH_ID_LEN + 1];
    for (int i = 0; i < OS_MESH_ID_LEN; i++) {
        char c = f[4 + i];                  // an emulator's id starts with a lowercase 'e'
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (i == 0 && c == 'e'))) {
            s_n.rx_bad++;
            return;
        }
        src[i] = c;
    }
    src[OS_MESH_ID_LEN] = 0;
    int want = idx < cnt - 1 ? FRAG_DATA : (int)len - FRAG_DATA * (cnt - 1);
    if (cnt < 1 || cnt > FRAG_MAX || idx >= cnt || len < 1 || len > OS_MESH_MSG_MAX ||
            (len + FRAG_DATA - 1) / FRAG_DATA != cnt || n != want) {
        s_n.rx_bad++;
        return;
    }
    if (seen(rx->mac, msg, now)) {
        s_n.rx_dup++;
        return;
    }
    if (cnt == 1) {
        mark_seen(rx->mac, msg, now);
        memcpy(s_one, f + HDR_LEN, n);
        s_one[n] = 0;
        deliver(type, rx->mac, src, rx->rssi, rx->ch, s_one, n, 1);
        return;
    }

    reasm_t *r = NULL, *oldest = &s_reasm[0];
    for (int i = 0; i < REASM_SLOTS; i++) {
        reasm_t *s = &s_reasm[i];
        if (s->t0 && s->msg == msg && !memcmp(s->mac, rx->mac, 6)) {
            r = s;
            break;
        }
        if (s->t0 < oldest->t0) {
            oldest = s;
        }
    }
    if (!r) {
        r = oldest;
        if (r->t0) {
            s_n.reasm_lost++;               // every slot busy: the oldest message loses
        }
        memset(r, 0, offsetof(reasm_t, buf));
        memcpy(r->mac, rx->mac, 6);
        memcpy(r->src, src, sizeof(r->src));
        r->msg = msg;
        r->type = type;
        r->cnt = cnt;
        r->len = len;
        r->ch = rx->ch;
        r->t0 = now;
    }
    if (r->type != type || r->cnt != cnt || r->len != len) {
        s_n.rx_bad++;
        return;
    }
    if (r->got & (1u << idx)) {
        s_n.rx_dup++;
        return;
    }
    memcpy(r->buf + idx * FRAG_DATA, f + HDR_LEN, n);
    r->got |= 1u << idx;
    if (r->got == (1u << cnt) - 1) {
        mark_seen(r->mac, r->msg, now);
        r->buf[r->len] = 0;
        r->t0 = 0;
        deliver(r->type, r->mac, r->src, rx->rssi, r->ch, r->buf, r->len, r->cnt);
    }
}

static void expire_reassembly(int64_t now)
{
    for (int i = 0; i < REASM_SLOTS; i++) {
        if (s_reasm[i].t0 && now - s_reasm[i].t0 > REASM_MS) {
            s_reasm[i].t0 = 0;
            s_n.reasm_lost++;
        }
    }
}

// Wi-Fi task (badge) / radio task (emulator): copy and hand over.
static void on_rx(const uint8_t mac[6], const uint8_t *data, int len, int rssi, int channel)
{
    if (len <= 0 || len > OS_RADIO_FRAME_MAX || !memcmp(mac, s_mac, 6)) {
        return;
    }
    rx_t rx;
    memcpy(rx.mac, mac, 6);
    rx.rssi = rssi;
    rx.len = len;
    rx.ch = channel > 0 && channel <= 255 ? channel : 0;
    memcpy(rx.data, data, len);
    if (xQueueSend(s_rxq, &rx, 0) != pdTRUE) {
        s_n.rx_drop++;
    }
}

// ---------------------------------------------------------------------------
// Bridges (os_bridge.c)

void os_mesh_set_bridged(const os_mesh_bridged_t *list, int n)
{
    int64_t now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PEER_MAX; i++) {
        if (!s_meta[i].bridged) {
            continue;
        }
        bool still = false;
        for (int k = 0; k < n && !still; k++) {
            still = !strcmp(s_peers[i].id, list[k].id);
        }
        if (!still) {
            s_meta[i].bridged = false;
            s_peers[i].bridged = false;
            s_peers[i].last_ms = 0;         // gone; a HELLO from the air may bring it back
        }
    }
    for (int k = 0; k < n; k++) {
        bool created;
        os_mesh_peer_t *p = peer_slot_locked(list[k].id, NULL, 0, &created);
        if (!p) {
            continue;
        }
        int i = p - s_peers;
        s_meta[i].bridged = true;
        s_meta[i].next_n = 0;
        memset(p->mac, 0, sizeof(p->mac));
        p->kind = OS_PEER_BADGE;
        clean_copy(p->name, sizeof(p->name), list[k].name);
        p->level = list[k].level;
        p->title[0] = p->character[0] = p->nonce[0] = 0;
        p->color = 0;
        p->rssi = 0;
        p->last_ms = now;
        p->bridged = true;
    }
    xSemaphoreGive(s_lock);
}

void os_mesh_bridge_rx(os_mesh_type_t type, const char *from, const char *json)
{
    size_t len = json ? strlen(json) : 0;
    if (!from || strlen(from) != OS_MESH_ID_LEN || !len || len > OS_MESH_MSG_MAX ||
            (type != OS_MESH_XDEX && type != OS_MESH_TRADE && type != OS_MESH_APP)) {
        s_n.rx_bad++;
        return;
    }
    brx_t b = { .type = type, .json = malloc(len + 1) };
    if (!b.json) {
        s_n.rx_drop++;
        return;
    }
    memcpy(b.json, json, len + 1);
    strlcpy(b.from, from, sizeof(b.from));
    if (xQueueSend(s_brq, &b, 0) != pdTRUE) {
        free(b.json);
        s_n.rx_drop++;
        return;
    }
    rx_t wake = { .len = 0 };               // an empty frame wakes the mesh task now
    xQueueSend(s_rxq, &wake, 0);
}

static void drain_bridged(void)
{
    brx_t b;
    while (xQueueReceive(s_brq, &b, 0)) {
        deliver(b.type, NULL, b.from, 0, 0, b.json, strlen(b.json), 1);
        free(b.json);
    }
}

static void mesh_task(void *arg)
{
    static rx_t rx;                     // 260 B: keep it off the task stack
    int64_t next_hello = now_ms() + HELLO_MS, next_maintain = 0, next_sync = 0;
    for (;;) {
        int64_t now = now_ms();
        int64_t wait = (next_hello < next_maintain ? next_hello : next_maintain) - now;
        if (wait < 0) {
            wait = 0;
        }
        if (xQueueReceive(s_rxq, &rx, pdMS_TO_TICKS(wait))) {
            if (rx.len && !paused()) {
                on_frame(&rx);
            }
        }
        drain_bridged();
        ulTaskNotifyTake(pdTRUE, 0);    // acquire nudges us to take the channel now
        now = now_ms();
        if (now >= next_maintain) {
            next_maintain = now + MAINTAIN_MS;
            if (now >= next_sync) {
                next_sync = now + SYNC_MS;
                sync_cloud();
            }
            scan_step(now);
            hold_channel();
            expire_reassembly(now);
        }
        if (now >= next_hello) {
            next_hello = now + HELLO_MS + esp_random() % 1000;
            if (on_channel() && s_id[0]) {
                send_hello();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Console

static const char *const type_names[OS_MESH_TYPE_MAX] = {
    [OS_MESH_HELLO] = "hello", [OS_MESH_XDEX] = "xdex", [OS_MESH_TRADE] = "trade",
    [OS_MESH_RELAY] = "relay", [OS_MESH_RELAY_RESP] = "relay_resp", [OS_MESH_APP] = "app",
};

static int selftest(void)
{
    static const struct {
        const char *s, *tag;
    } v[] = {
        {"xdex|7K2Q9M|9XQ3TT|0011223344556677", "bc6e8523fe5d601c"},
        {"trade|7K2Q9M|3f2a9c1e00b14d77|7K2Q9M|9XQ3TT|50|it_8f2k,it_k2m1||0||p007,p013", "cfa9b760dfdf0de4"},
        {"relay|7K2Q9M|BCN001|a1b2c3d4e5f60718|17|{\"op\":\"open\"}", "81402ef7e089ca97"},
        {"bridge|7K2Q9M|58000000", "2870ed5904c344a1"},
        {"meet|7K2Q9M|tag-game|9XQ3TT|7K2Q9M|0011223344556677|8899aabbccddeeff|1790000000|1790000002|rr|9|{\"it\":42}|8|{\"ok\":1}",
         "156320afbc968b8a"},
    };
    uint8_t key[16], digest[32];
    char hex[65];
    int fails = 0;
    for (int i = 0; i < 16; i++) {
        key[i] = i;
    }
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, "abc", 3);
    sha256_final(&s, digest);
    to_hex(digest, 32, hex);
    bool ok = !strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fails += !ok;
    printf("%s sha256(\"abc\")\n", ok ? "PASS" : "FAIL");
    for (int i = 0; i < (int)(sizeof(v) / sizeof(v[0])); i++) {
        char tag[OS_MESH_TAG_LEN + 1];
        tag_with(key, v[i].s, tag);
        ok = !strcmp(tag, v[i].tag);
        fails += !ok;
        printf("%s tag %s  (%.40s%s)\n", ok ? "PASS" : "FAIL", tag, v[i].s, strlen(v[i].s) > 40 ? "..." : "");
    }
    // A frame of nested brackets must not reach cJSON, and cJSON itself must
    // refuse deep nesting (CJSON_NESTING_LIMIT in both builds).
    char *deep = malloc(FRAG_DATA + 1);
    if (deep) {
        memset(deep, '[', FRAG_DATA);
        deep[FRAG_DATA] = 0;
        ok = !nesting_ok(deep, NEST_MAX) && nesting_ok("{\"k\":\"b\",\"n\":\"[[[[[[[[[[\",\"d\":[[[1]]]}", NEST_MAX);
        fails += !ok;
        printf("%s nesting scan drops %d nested brackets, passes brackets in strings\n", ok ? "PASS" : "FAIL", FRAG_DATA);
        memset(deep, 0, FRAG_DATA + 1);
        for (int i = 0; i < 40; i++) {
            deep[i] = '[';
            deep[79 - i] = ']';
        }
        cJSON *j = cJSON_Parse(deep);
        ok = j == NULL;
        cJSON_Delete(j);
        fails += !ok;
        printf("%s cJSON refuses 40 nested arrays\n", ok ? "PASS" : "FAIL");
        free(deep);
    }
    printf("mesh selftest: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}

static void print_status(void)
{
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    printf("mesh: id %s  channel %d, %s GHz (radio on %d)  %s%s  key %s  acquired %d%s\n", s_id[0] ? s_id : "-",
           s_channel, os_mesh_band(s_channel), os_radio_channel(), os_mesh_ready() ? "ready" : on_channel() ? "on channel" : "off channel",
           paused() ? " (paused: radio lent out)" : "", s_has_key ? "yes" : "no", s_acquired,
           s_took_wifi ? " (Wi-Fi suspended by mesh)" : "");
    if (s_override) {
        printf("  channel %d chosen on the badge; the server announced %d (\"mesh channel auto\" goes back)\n", s_override,
               s_server_ch);
    }
    if (s_app_ch) {
        printf("  channel %d chosen by the running app (mesh.channel)\n", s_app_ch);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_scan) {
        printf("  scan: %s, channel %d (%d of %d), %d heard%s\n", s_scan->active ? "running" : "done", s_scan->ch[s_scan->idx],
               s_scan->idx + 1, s_scan->n, s_scan->nhits, s_scan->took_wifi ? ", Wi-Fi suspended for it" : "");
    }
    xSemaphoreGive(s_lock);
    if (s_beacon[0]) {
        printf("  playing beacon %s: %s\n", s_beacon, s_beacon_hello[0] ? s_beacon_hello : "(silent)");
    }
    printf("  hello: '%s' Lv %d, %lu sent\n", s_label, s_level, (unsigned long)s_n.hello_tx);
    printf("  tx: %lu msgs, %lu frames, %lu errors\n", (unsigned long)s_n.tx_msgs, (unsigned long)s_n.tx_frames,
           (unsigned long)s_n.tx_err);
    printf("  rx: %lu frames, %lu msgs (%lu multi-fragment), %lu dup, %lu bad, %lu dropped, %lu lost in reassembly\n",
           (unsigned long)s_n.rx_frames, (unsigned long)s_n.rx_msgs, (unsigned long)s_n.rx_multi,
           (unsigned long)s_n.rx_dup, (unsigned long)s_n.rx_bad, (unsigned long)s_n.rx_drop,
           (unsigned long)s_n.reasm_lost);
    printf("  refused: %lu from an unknown or changed MAC, %lu nested too deep\n", (unsigned long)s_n.rx_spoof,
           (unsigned long)s_n.rx_deep);
    printf("  bridges: %d open%s\n", os_bridge_count(), s_keep_wifi ? ", keeping Wi-Fi joined" : "");
    printf("  mesh task stack left %u B\n", (unsigned)uxTaskGetStackHighWaterMark(s_task));
}

static void print_peers(void)
{
    os_mesh_peer_t *list = malloc(PEER_MAX * sizeof(*list));
    int n = list ? os_mesh_peers(list, PEER_MAX) : 0;
    int64_t now = now_ms();
    for (int i = 0; i < n; i++) {
        const os_mesh_peer_t *p = &list[i];
        if (p->bridged) {
            printf("badge  %-6s bridged                    %3llds  %s Lv %d\n", p->id,
                   (long long)((now - p->last_ms) / 1000), p->name[0] ? p->name : "(unclaimed)", p->level);
            continue;
        }
        printf("%s %-6s %02x:%02x:%02x:%02x:%02x:%02x %4d dBm %3llds  %s", p->kind == OS_PEER_BEACON ? "beacon" : "badge ",
               p->id, p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5], p->rssi,
               (long long)((now - p->last_ms) / 1000), p->name[0] ? p->name : "(unclaimed)");
        if (p->kind == OS_PEER_BEACON) {
            printf(" / %s [%s] #%06lX nonce %s\n", p->title, p->character, (unsigned long)p->color, p->nonce);
        } else {
            printf(" Lv %d\n", p->level);
        }
    }
    printf("%d peer%s\n", n, n == 1 ? "" : "s");
    free(list);
}

// mesh send <type> <id|*> <json, ' for "> | big <bytes>
static int cmd_send(int argc, char **argv)
{
    int type = 0;
    for (int i = 1; i < OS_MESH_TYPE_MAX; i++) {
        if (!strcmp(argv[2], type_names[i]) || atoi(argv[2]) == i) {
            type = i;
        }
    }
    if (!type) {
        printf("unknown type '%s'\n", argv[2]);
        return 1;
    }
    char *json = calloc(1, OS_MESH_MSG_MAX + 1);
    if (!json) {
        return 1;
    }
    if (!strcmp(argv[4], "big")) {
        int n = argc > 5 ? atoi(argv[5]) : 600;
        n = n < 40 ? 40 : n > OS_MESH_MSG_MAX ? OS_MESH_MSG_MAX : n;
        int head = snprintf(json, OS_MESH_MSG_MAX, "{\"app\":\"test\",\"d\":{\"pad\":\"");
        for (int i = head; i < n - 4; i++) {
            json[i] = 'a' + (i % 26);
        }
        strcpy(json + n - 4, "\"}}");
    } else {
        for (int i = 4; i < argc; i++) {
            strlcat(json, argv[i], OS_MESH_MSG_MAX + 1);
            if (i + 1 < argc) {
                strlcat(json, " ", OS_MESH_MSG_MAX + 1);
            }
        }
        for (char *c = json; *c; c++) {
            if (*c == '\'') {
                *c = '"';                   // the badge console eats double quotes
            }
        }
    }
    esp_err_t err = os_mesh_send(type, argv[3], json);
    printf("sent %s (%u B): %s\n", type_names[type], (unsigned)strlen(json), esp_err_to_name(err));
    free(json);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_mesh(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "status")) {
        print_status();
    } else if (!strcmp(sub, "peers")) {
        print_peers();
    } else if (!strcmp(sub, "selftest")) {
        return selftest();
    } else if (!strcmp(sub, "log") && argc == 3) {
        s_log = !strcmp(argv[2], "on");
    } else if (!strcmp(sub, "send") && argc >= 5) {
        return cmd_send(argc, argv);
    } else if (!strcmp(sub, "channel") && argc == 3) {
        bool autom = !strcmp(argv[2], "auto");
        int ch = autom ? 0 : atoi(argv[2]);
        if ((!autom && !ch) || !os_mesh_set_channel(ch)) {
            printf("mesh: %s is not a mesh channel%s. Use 1-13, 36, 40, 44, 48, 149, 153, 157, 161, 165 or auto\n",
                   argv[2], ch >= 52 && ch <= 144 ? " (a radar channel)" : "");
            return 1;
        }
    } else if (!strcmp(sub, "acquire")) {
        os_mesh_acquire();
    } else if (!strcmp(sub, "release")) {
        os_mesh_release();
    } else {
        printf("usage: mesh [status|peers|selftest|log on|off|channel <n|auto>|acquire|release|send <type> <id|*> <json>|big <bytes>]\n");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------

void os_mesh_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    s_sub_lock = xSemaphoreCreateRecursiveMutex();
    s_rxq = xQueueCreate(RXQ_LEN, sizeof(rx_t));
    s_brq = xQueueCreate(BRQ_LEN, sizeof(brx_t));
    s_msg = esp_random();                       // a quick reboot must not look like duplicates
    os_radio_mac(s_mac);
    os_cmd_register("mesh", "mesh [status|peers|selftest|log on|off|channel <n|auto>|acquire|release|send <type> <id|*> <json>]",
                    cmd_mesh);
    xTaskCreate(mesh_task, "mesh", 4096, NULL, 3, &s_task);
    esp_err_t err = os_radio_init(on_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radio unavailable: %s", esp_err_to_name(err));
    }
}
