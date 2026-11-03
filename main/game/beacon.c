// Badge beacons, see beacon.h (docs/MESH.md "Badge beacons", server/API.md
// "Badge beacons").
//
// What a beacon's firmware does (beacon/main/cloud.c, relay.c), on the OS
// task: the beacon's hello every `poll` s while the Beacon app is open, nonces
// rotating every 30 s, the character's HELLO handed to os_mesh (which sends
// every frame under the beacon id meanwhile), and RELAYs forwarded to
// POST /api/device/beacon/relay over the badge's own server connection. The
// server's answer goes back byte for byte with "rid" in front, because the
// visiting badge checks the server's tag over the exact strings.
//
// A badge that didn't hear the answer retries the same request with the same
// nonce, which the server would refuse as replayed: answers are cached for
// 20 s, keyed on (badge, rid, req), and a retry of a request still in flight
// is dropped (its answer is coming).

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"
#include "core/os_mesh.h"
#include "core/os_radio.h"
#include "core/os_wifi.h"
#include "game.h"
#include "beacon.h"

static const char *TAG = "beacon";

#define NONCE_MS        30000           // MESH.md §1
#define RETRY_MS        15000           // after a hello that failed
#define CACHE_N         4
#define CACHE_MS        20000
#define JOBS_MAX        2               // relays waiting on the server (os_api queues 8 in all)
#define HOPS_MAX        4               // RELAYs waiting for the OS task
#define REQ_MAX         1400
#define RESP_CAP        2048            // API.md: answers are <= 1400 bytes
#define HELLO_JSON_MAX  200             // os_mesh_beacon_hello()
#define NVS_NS          "beacon"

typedef struct {
    void *tok;                          // os_api ctx, NULL = free
    char badge[OS_MESH_ID_LEN + 1];
    uint32_t rid;
    uint64_t hash;
} job_t;

typedef struct {
    char *answer;                       // NULL = free
    char badge[OS_MESH_ID_LEN + 1];
    uint32_t rid;
    uint64_t hash;
    int64_t at;
} cached_t;

typedef struct {
    os_mesh_peer_t from;
    char json[];
} hop_t;

static beacon_info_t s_info;
static bool s_known;                    // s_info's state came from the server (pair or hello)
static bool s_serving;                  // the Beacon app is open
static volatile bool s_rx;              // the mesh task hands RELAYs over
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_hops;                      // s_mux
static char s_cloud_id[7];              // hello's "beacon"
static char s_nonce[17], s_prev[17];
static int64_t s_nonce_at, s_hello_at;
static void *s_hello_tok, *s_pair_tok;
static volatile bool s_pair_asked;      // console: `beacon pair` posted, not started yet
static char s_pair_error[64];
static uint8_t s_scan_key[16];
static bool s_has_scan;
static char s_scan_url[112];
static char s_aired[HELLO_JSON_MAX + 1];    // what os_mesh broadcasts, "" = silent
static job_t s_jobs[JOBS_MAX];
static cached_t s_cache[CACHE_N];
static bool s_boot_checked;

// ---------------------------------------------------------------------------
// State

static void nvs_open_flag(bool open)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "open", open ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool nvs_was_open(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "open", &v);
        nvs_close(h);
    }
    return v == 1;
}

static void update_phase(void)
{
    beacon_phase_t p;
    if (!s_info.id[0]) {
        p = BEACON_NONE;
    } else if (!s_known) {
        p = BEACON_CHECKING;
    } else if (s_info.code[0]) {
        p = BEACON_PENDING;
    } else if (!s_info.ch[0]) {
        p = BEACON_ASLEEP;
    } else {
        p = BEACON_ON_AIR;
    }
    if (p != s_info.phase) {
        s_info.phase = p;
        s_info.rev++;
    }
}

static void clear_cache(void)
{
    for (int i = 0; i < CACHE_N; i++) {
        free(s_cache[i].answer);
        s_cache[i].answer = NULL;
    }
}

static void cancel_requests(void)
{
    for (int i = 0; i < JOBS_MAX; i++) {
        os_api_cancel(s_jobs[i].tok);
        s_jobs[i].tok = NULL;
    }
    os_api_cancel(s_hello_tok);
    s_hello_tok = NULL;
}

static void air(const char *json)
{
    const char *j = json ? json : "";
    if (strcmp(j, s_aired)) {
        strlcpy(s_aired, j, sizeof(s_aired));
        os_mesh_beacon_hello(s_aired[0] ? s_aired : NULL);
    }
}

// The beacon this badge plays is now `id` ("" = none): everything learned
// about another one goes.
static void set_id(const char *id)
{
    if (!strcmp(id, s_info.id)) {
        return;
    }
    ESP_LOGI(TAG, "%s%s", id[0] ? "beacon " : "not a beacon any more", id);
    int visits = s_info.visits, busy = s_info.busy;
    int64_t last = s_info.last_visit_ms;
    uint32_t rev = s_info.rev;
    memset(&s_info, 0, sizeof(s_info));
    strlcpy(s_info.id, id, sizeof(s_info.id));
    s_info.visits = visits;
    s_info.busy = busy;
    s_info.last_visit_ms = last;
    s_info.rev = rev + 1;
    s_known = false;
    s_has_scan = false;
    s_scan_url[0] = 0;
    air(NULL);
    clear_cache();
    if (s_serving) {
        os_mesh_beacon(id[0] ? id : NULL);
        s_hello_at = 0;
    }
    update_phase();
}

// The server says this badge isn't a beacon (any more): the app goes from the launcher.
static void forget(void)
{
    if (s_info.id[0]) {
        os_toast("This badge isn't a quest beacon any more");
    }
    cancel_requests();
    os_cloud_set_beacon("");
    strlcpy(s_cloud_id, "", sizeof(s_cloud_id));
    set_id("");
}

bool beacon_listed(void)
{
    return s_cloud_id[0] != 0;
}

const beacon_info_t *beacon_info(void)
{
    return &s_info;
}

void beacon_cloud(const char *id)
{
    if (strcmp(id ? id : "", s_cloud_id)) {
        strlcpy(s_cloud_id, id ? id : "", sizeof(s_cloud_id));
        set_id(s_cloud_id);
    }
}

static bool hex16(const char *s, uint8_t out[16])
{
    if (!s || strlen(s) != 32) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) {
            return false;
        }
        out[i / 2] = (uint8_t)(i % 2 ? (out[i / 2] | v) : v << 4);
    }
    return true;
}

static bool printable(const char *s, size_t max)
{
    if (!s || !s[0] || strlen(s) >= max) {
        return false;
    }
    for (; *s; s++) {
        if (*s <= 0x20 || *s >= 0x7F) {
            return false;
        }
    }
    return true;
}

bool beacon_scan_url(char *out, size_t n)
{
    int64_t t = os_cloud_time_ms();
    if (!s_has_scan || !s_scan_url[0] || !s_info.id[0] || t <= 0) {
        return false;
    }
    char msg[48], tag[OS_MESH_TAG_LEN + 1];
    long long w = t / 1000 / 30;
    snprintf(msg, sizeof(msg), "scan|%s|%lld", s_info.id, w);
    os_mesh_tag(s_scan_key, msg, tag);
    snprintf(out, n, "%s%s.%lld.%s", s_scan_url, s_info.id, w, tag);
    return true;
}

// ---------------------------------------------------------------------------
// Hello (server/API.md "Badge beacons")

static void on_hello(int status, const char *body, void *ctx)
{
    if (!ctx || ctx != s_hello_tok) {
        return;
    }
    s_hello_tok = NULL;
    int64_t now = game_now_ms();
    if (status == 404) {
        forget();
        return;
    }
    cJSON *j = cJSON_Parse(body ? body : "");
    const char *id = game_jstr(j, "id");
    if (status != 200 || !cJSON_IsObject(j) || strlen(id) != OS_MESH_ID_LEN) {
        const char *msg = game_jstr(j, "error");
        char err[sizeof(s_info.error)];
        if (!status) {
            strlcpy(err, "Can't reach the server", sizeof(err));
        } else if (msg[0]) {
            game_ascii(err, sizeof(err), msg);
        } else {
            snprintf(err, sizeof(err), "The server answered %d", status);
        }
        cJSON_Delete(j);
        if (strcmp(err, s_info.error)) {
            strlcpy(s_info.error, err, sizeof(s_info.error));
            s_info.rev++;
        }
        s_hello_at = now + RETRY_MS;
        return;
    }
    if (strcmp(id, s_info.id)) {
        // Paired again as another beacon (deleted, then paired): follow the server.
        os_cloud_set_beacon(id);
        strlcpy(s_cloud_id, id, sizeof(s_cloud_id));
        set_id(id);
    }
    beacon_info_t was = s_info;
    const cJSON *pending = cJSON_GetObjectItem(j, "pending");
    game_ascii(s_info.code, sizeof(s_info.code), game_jstr(pending, "code"));
    const cJSON *c = cJSON_GetObjectItem(j, "character");
    game_ascii(s_info.ch, sizeof(s_info.ch), game_jstr(c, "id"));
    game_ascii(s_info.name, sizeof(s_info.name), game_jstr(c, "n"));
    game_ascii(s_info.title, sizeof(s_info.title), game_jstr(c, "t"));
    s_info.color = game_parse_hex(game_jstr(c, "col"), COL_ORANGE);
    if (!s_info.name[0]) {
        s_info.ch[0] = 0;                   // nothing to show for it
    }
    s_info.mesh = game_jint(j, "mesh", 0);
    s_info.error[0] = 0;
    s_has_scan = hex16(game_jstr(j, "scan"), s_scan_key);
    const char *url = game_jstr(j, "scanUrl");
    strlcpy(s_scan_url, printable(url, sizeof(s_scan_url)) ? url : "", sizeof(s_scan_url));
    int poll = game_jint(j, "poll", 60);
    s_hello_at = now + (poll < 10 ? 10 : poll > 300 ? 300 : poll) * 1000LL;
    s_known = true;
    cJSON_Delete(j);
    if (memcmp(&was, &s_info, sizeof(was))) {
        s_info.rev++;
    }
    update_phase();
}

static void send_hello(void)
{
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    uint8_t m[6];
    os_radio_mac(m);
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(b, "mac", mac);
    cJSON_AddNumberToObject(b, "channel", w.state == OS_WIFI_CONNECTED ? w.channel : 0);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    void *tok = os_api_token();
    if (body && os_api_call("POST", "/api/device/beacon/hello", body, 0, on_hello, tok) == ESP_OK) {
        s_hello_tok = tok;
    } else {
        s_hello_at = game_now_ms() + 2000;      // queue full: soon again
    }
    cJSON_free(body);
}

// ---------------------------------------------------------------------------
// Relaying (MESH.md §4)

static uint64_t fnv(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h = (h ^ (uint8_t)*s++) * 0x100000001b3ULL;
    }
    return h;
}

static void send_resp(const char *badge, const char *json)
{
    if (json && os_mesh_send(OS_MESH_RELAY_RESP, badge, json) != ESP_OK) {
        ESP_LOGW(TAG, "answer to %s not sent", badge);
    }
}

// Our own errors carry no tag: the badge shows them as "The beacon is busy".
static void answer_error(const char *badge, uint32_t rid, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "rid", rid);
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    send_resp(badge, s);
    cJSON_free(s);
}

// {"rid":N, + the server's object after its '{', untouched (heap), or NULL.
static char *relay_resp(uint32_t rid, const char *body)
{
    const char *p = body;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
        p++;
    }
    if (*p++ != '{') {
        return NULL;
    }
    size_t n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == '\t')) {
        n--;
    }
    char head[24];
    int h = snprintf(head, sizeof(head), "{\"rid\":%" PRIu32 ",", rid);
    if (n < 2 || p[n - 1] != '}' || h + n > OS_MESH_MSG_MAX) {
        return NULL;
    }
    char *out = malloc(h + n + 1);
    if (out) {
        memcpy(out, head, h);
        memcpy(out + h, p, n);
        out[h + n] = 0;
    }
    return out;
}

static void cache_put(const job_t *j, char *answer)
{
    cached_t *c = &s_cache[0];
    for (int i = 0; i < CACHE_N; i++) {
        if (!s_cache[i].answer) {
            c = &s_cache[i];
            break;
        }
        if (s_cache[i].at < c->at) {
            c = &s_cache[i];
        }
    }
    free(c->answer);
    c->answer = answer;
    strlcpy(c->badge, j->badge, sizeof(c->badge));
    c->rid = j->rid;
    c->hash = j->hash;
    c->at = game_now_ms();
}

static void on_answer(int status, const char *body, void *ctx)
{
    job_t j = {0};
    for (int i = 0; i < JOBS_MAX; i++) {
        if (ctx && s_jobs[i].tok == ctx) {
            j = s_jobs[i];
            s_jobs[i].tok = NULL;
        }
    }
    if (!j.tok) {
        return;
    }
    if (status == 404) {
        answer_error(j.badge, j.rid, "This beacon isn't set up");
        forget();
        return;
    }
    cJSON *r = status == 200 ? cJSON_Parse(body) : NULL;
    const cJSON *ok = cJSON_GetObjectItem(r, "ok");
    char *out = NULL;
    if (status == 200 && cJSON_IsBool(ok) && cJSON_IsString(cJSON_GetObjectItem(r, cJSON_IsTrue(ok) ? "res" : "error"))) {
        out = relay_resp(j.rid, body);
    }
    if (out) {
        ESP_LOGI(TAG, "%s rid %" PRIu32 ": %s", j.badge, j.rid, cJSON_IsTrue(ok) ? "answered" : game_jstr(r, "error"));
        send_resp(j.badge, out);
        cache_put(&j, out);
        if (cJSON_IsTrue(ok)) {
            s_info.visits++;
            s_info.last_visit_ms = game_now_ms();
            game_led_flash(s_info.color);
        }
    } else {
        s_info.busy++;
        cJSON *e = status && status != 200 ? cJSON_Parse(body) : NULL;
        const char *msg = game_jstr(e, "error");
        answer_error(j.badge, j.rid, !status ? "The beacon can't reach the server"
                                     : msg[0] ? msg : status == 200 ? "Answer too long" : "The server didn't answer properly");
        cJSON_Delete(e);
    }
    cJSON_Delete(r);
}

static bool is_hex16(const char *s)
{
    if (strlen(s) != 16) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) {
            return false;
        }
    }
    return true;
}

static void on_relay(void *arg)
{
    hop_t *h = arg;
    portENTER_CRITICAL(&s_mux);
    s_hops--;
    portEXIT_CRITICAL(&s_mux);
    const char *badge = h->from.id;
    cJSON *o = s_serving && s_info.phase == BEACON_ON_AIR ? cJSON_Parse(h->json) : NULL;
    const cJSON *rid = cJSON_GetObjectItem(o, "rid");
    double r = cJSON_IsNumber(rid) ? rid->valuedouble : -1;
    if (!cJSON_IsObject(o) || r < 0 || r > 4294967295.0 || r != (double)(uint32_t)r) {
        cJSON_Delete(o);                    // nothing to answer to
        free(h);
        return;
    }
    job_t job = { .rid = (uint32_t)r };
    strlcpy(job.badge, badge, sizeof(job.badge));
    const char *nonce = game_jstr(o, "nonce"), *tag = game_jstr(o, "tag"), *req = game_jstr(o, "req");
    job_t *slot = NULL;
    for (int i = 0; i < JOBS_MAX; i++) {
        if (s_jobs[i].tok && s_jobs[i].rid == job.rid && !strcmp(s_jobs[i].badge, badge)) {
            goto done;                      // a retry of a request in flight: its answer is coming
        }
        if (!s_jobs[i].tok && !slot) {
            slot = &s_jobs[i];
        }
    }
    if (!is_hex16(nonce) || !is_hex16(tag) || !req[0] || strlen(req) > REQ_MAX) {
        answer_error(badge, job.rid, "Bad request");
        goto done;
    }
    if (strcmp(nonce, s_nonce) && strcmp(nonce, s_prev)) {
        s_info.busy++;
        answer_error(badge, job.rid, "Too far away");      // hasn't heard us lately
        goto done;
    }
    job.hash = fnv(req);
    int64_t now = game_now_ms();
    for (int i = 0; i < CACHE_N; i++) {
        cached_t *c = &s_cache[i];
        if (c->answer && now - c->at < CACHE_MS && c->rid == job.rid && c->hash == job.hash && !strcmp(c->badge, badge)) {
            ESP_LOGI(TAG, "%s rid %" PRIu32 ": repeat, answered from cache", badge, job.rid);
            send_resp(badge, c->answer);
            goto done;
        }
    }
    if (!slot) {
        s_info.busy++;
        answer_error(badge, job.rid, "Busy, try again");
        goto done;
    }
    if (!os_api_online()) {
        s_info.busy++;
        answer_error(badge, job.rid, "The beacon can't reach the server");
        goto done;
    }
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "badge", badge);
    cJSON_AddStringToObject(b, "nonce", nonce);
    cJSON_AddNumberToObject(b, "rid", job.rid);
    cJSON_AddStringToObject(b, "req", req);
    cJSON_AddStringToObject(b, "tag", tag);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    job.tok = os_api_token();
    if (body && os_api_call("POST", "/api/device/beacon/relay", body, RESP_CAP, on_answer, job.tok) == ESP_OK) {
        *slot = job;
    } else {
        s_info.busy++;
        answer_error(badge, job.rid, "Busy, try again");
    }
    cJSON_free(body);
done:
    cJSON_Delete(o);
    free(h);
}

// Mesh task: copy RELAYs from badges and hop over, a few at a time, so a flood
// can't fill the OS queue that buttons and API answers share.
static void on_mesh(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx)
{
    if (!s_rx || from->kind != OS_PEER_BADGE || from->bridged || !strstr(json, "\"rid\"")) {
        return;
    }
    size_t len = strlen(json);
    portENTER_CRITICAL(&s_mux);
    bool room = s_hops < HOPS_MAX && len <= REQ_MAX + 256;
    if (room) {
        s_hops++;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!room) {
        return;
    }
    hop_t *h = malloc(sizeof(hop_t) + len + 1);
    if (h) {
        h->from = *from;
        memcpy(h->json, json, len + 1);
    }
    if (!h || !os_post(on_relay, h)) {
        free(h);
        portENTER_CRITICAL(&s_mux);
        s_hops--;
        portEXIT_CRITICAL(&s_mux);
    }
}

// ---------------------------------------------------------------------------
// Serving

void beacon_serve(bool on)
{
    if (on == s_serving) {
        return;
    }
    s_serving = on;
    nvs_open_flag(on);
    if (on) {
        os_mesh_nonce(s_nonce);
        os_mesh_nonce(s_prev);
        s_nonce_at = game_now_ms();
        s_hello_at = 0;
        s_info.visits = s_info.busy = 0;
        s_info.last_visit_ms = 0;
        s_info.rev++;
        if (s_info.id[0]) {
            os_mesh_beacon(s_info.id);
        }
        // A beacon needs the network: join the saved one if Wi-Fi is off.
        char ssid[33];
        if (!os_wifi_enabled() && os_wifi_saved_ssid(ssid, sizeof(ssid))) {
            os_wifi_set_enabled(true);
        }
        os_api_prewarm();
        s_rx = true;
    } else {
        s_rx = false;
        air(NULL);
        os_mesh_beacon(NULL);
        cancel_requests();
        clear_cache();
    }
}

void beacon_sync(void)
{
    if (s_serving) {
        s_hello_at = 0;
    } else {
        os_cloud_refresh();
    }
}

void beacon_tick(int64_t now)
{
    // A badge that restarted with the app open (power cut, update) opens it again.
    if (!s_boot_checked && os_current() == &os_launcher) {
        s_boot_checked = true;
        if (s_cloud_id[0] && nvs_was_open()) {
            ESP_LOGI(TAG, "was playing beacon %s: opening Beacon again", s_cloud_id);
            os_launch_id("beacon");
        }
    }
    if (!s_serving) {
        return;
    }
    update_phase();
    if (!s_info.id[0]) {
        return;
    }
    if (now - s_nonce_at >= NONCE_MS) {
        s_nonce_at = now;
        strlcpy(s_prev, s_nonce, sizeof(s_prev));
        os_mesh_nonce(s_nonce);
    }
    bool online = os_api_online();
    if (online && !s_hello_tok && now >= s_hello_at) {
        send_hello();
    }
    // On the air only while it can relay: a beacon that can't reach the server stays silent.
    if (s_info.phase == BEACON_ON_AIR && online) {
        cJSON *j = cJSON_CreateObject();
        char col[7];
        snprintf(col, sizeof(col), "%06" PRIX32, s_info.color & 0xFFFFFF);
        cJSON_AddStringToObject(j, "k", "c");
        cJSON_AddStringToObject(j, "n", s_info.name);
        cJSON_AddStringToObject(j, "t", s_info.title);
        cJSON_AddStringToObject(j, "col", col);
        cJSON_AddStringToObject(j, "nonce", s_nonce);
        cJSON_AddStringToObject(j, "ch", s_info.ch);
        char json[HELLO_JSON_MAX + 1];
        bool ok = cJSON_PrintPreallocated(j, json, sizeof(json), false);
        cJSON_Delete(j);
        air(ok ? json : NULL);
    } else {
        air(NULL);
    }
}

// ---------------------------------------------------------------------------
// Console: `beacon pair` is how a badge becomes a beacon (the site's flasher
// and emulator run it). The first line of `beacon` is for them to read.

static void print_status(void)
{
    if (s_pair_tok || s_pair_asked) {
        printf("beacon: pairing\n");
    } else if (!s_info.id[0] && s_pair_error[0]) {
        printf("beacon: error %s\n", s_pair_error);
    } else if (!s_info.id[0]) {
        printf("beacon: none\n");
    } else if (!s_known) {
        printf("beacon: paired %s\n", s_info.id);
    } else if (s_info.code[0]) {
        printf("beacon: pending %s %s\n", s_info.id, s_info.code);
    } else {
        printf("beacon: adopted %s %s\n", s_info.id, s_info.ch[0] ? s_info.ch : "-");
    }
    if (!s_info.id[0]) {
        return;
    }
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    static const char *const phases[] = { "not a beacon", "checking in", "waiting to be adopted", "no character", "on the air" };
    printf("  Beacon app %s, %s%s; Wi-Fi channel %d, mesh channel %d\n", s_serving ? "open" : "closed",
           phases[s_info.phase], s_info.phase == BEACON_ON_AIR && !s_aired[0] ? " (silent: offline)" : "",
           w.state == OS_WIFI_CONNECTED ? w.channel : 0, s_info.mesh ? s_info.mesh : os_mesh_server_channel());
    if (s_info.name[0]) {
        printf("  character %s (%s), %s\n", s_info.name, s_info.ch, s_info.title);
    }
    if (s_serving) {
        printf("  %d visits, %d turned away; nonce %s%s%s\n", s_info.visits, s_info.busy, s_nonce,
               s_info.error[0] ? "; last hello: " : "", s_info.error);
        char url[160];
        if (beacon_scan_url(url, sizeof(url))) {
            printf("  scan %s\n", url);
        }
    }
}

static void on_pair(int status, const char *body, void *ctx)
{
    if (!ctx || ctx != s_pair_tok) {
        return;
    }
    s_pair_tok = NULL;
    cJSON *j = cJSON_Parse(body ? body : "");
    const char *id = game_jstr(j, "id");
    if (status == 200 && strlen(id) == OS_MESH_ID_LEN) {
        s_pair_error[0] = 0;
        os_cloud_set_beacon(id);
        strlcpy(s_cloud_id, id, sizeof(s_cloud_id));
        set_id(id);
        game_ascii(s_info.code, sizeof(s_info.code), game_jstr(j, "code"));
        s_known = true;
        s_info.rev++;
        update_phase();
        print_status();
        os_launch_id("beacon");
    } else {
        const char *msg = game_jstr(j, "error");
        if (!status) {
            strlcpy(s_pair_error, "can't reach the server", sizeof(s_pair_error));
        } else if (msg[0]) {
            game_ascii(s_pair_error, sizeof(s_pair_error), msg);
        } else {
            snprintf(s_pair_error, sizeof(s_pair_error), "the server answered %d", status);
        }
        printf("beacon: error %s\n", s_pair_error);
    }
    cJSON_Delete(j);
}

// OS task: what the console asked for.
static void run_pair(void *arg)
{
    s_pair_asked = false;
    if (s_pair_tok) {
        return;
    }
    if (!os_api_online()) {
        strlcpy(s_pair_error, "offline: join a Wi-Fi network first", sizeof(s_pair_error));
        printf("beacon: error %s\n", s_pair_error);
        return;
    }
    void *tok = os_api_token();
    s_pair_error[0] = 0;
    if (os_api_call("POST", "/api/device/beacon/pair", "{}", 0, on_pair, tok) == ESP_OK) {
        s_pair_tok = tok;
    } else {
        strlcpy(s_pair_error, "busy, try again", sizeof(s_pair_error));
        printf("beacon: error %s\n", s_pair_error);
    }
}

static void run_sync(void *arg)
{
    beacon_sync();
}

// The console task: changes hop over to the OS task; `status` only reads.
static int cmd_beacon(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "status")) {
        print_status();
    } else if (!strcmp(sub, "pair")) {
        s_pair_asked = true;
        if (!os_post(run_pair, NULL)) {
            s_pair_asked = false;
            printf("beacon: error busy, try again\n");
            return 1;
        }
        printf("beacon: pairing\n");
    } else if (!strcmp(sub, "sync")) {
        os_post(run_sync, NULL);
        printf("beacon: syncing\n");
    } else {
        printf("usage: beacon [status|pair|sync]\n");
        return 1;
    }
    return 0;
}

void beacon_init(void)
{
    os_mesh_subscribe(OS_MESH_RELAY, on_mesh, NULL);
    os_cmd_register("beacon", "beacon [status|pair|sync] - this badge as a quest beacon (QuestMakers pair it)", cmd_beacon);
}
