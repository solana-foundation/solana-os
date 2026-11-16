// Collaborator, see collab.h.
//
// Everything lives on the OS task: a 250 ms esp_timer posts a tick there,
// which decides when to sync (os_api runs the call on the cloud task and its
// answer comes back here), sends the server's APP messages from the station
// at most 4 a second, filters what the mesh task copied for us, shows or
// closes the takeover screen, and steps the LED pattern. The mesh handler
// only copies APP messages into a small ring while a station is on.
//
// The answer is the server's word, never ours: live minutes, earnings, the
// session. What we send back is what we saw (battery, channels, peers heard,
// messages for the station) and what the owner did (end, off).

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "bsp.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"
#include "core/os_data.h"
#include "core/os_mem.h"
#include "core/os_mesh.h"
#include "core/os_service.h"
#include "core/os_wifi.h"
#include "lua/lua_install.h"
#include "lua/lua_png.h"
#include "collab.h"
#include "game.h"
#include "trade.h"
#include "xdex.h"

static const char *TAG = "collab";

#define NVS_NS          "collab"
#define TICK_MS         250
#define LIVE_EVERY_MS   30000           // until the server says otherwise
#define RETRY_MS        10000           // after a failed sync
#define NEXT_MIN_MS     1000
#define NEXT_MAX_MS     60000
#define SEND_GAP_MS     250             // MESH.md: at most 4 station messages a second
#define SENDS_MAX       8
#define RX_RING         16              // copied by the mesh task, filtered here
#define RX_KEEP         16              // waiting for the next sync (8 go per sync)
#define RX_JSON_MAX     400
#define NEAR_MAX        16
#define LISTEN_MAX      4
#define IDLE_MS         30000           // a screen takeover waits for this much quiet in another app
#define BATT_MIN        15              // HHGTS.md: Collaborator ends under 15 % off USB
#define LED_FRAME_MS    50
#define IMG_PATH        "cache/collab.png"
#define IMG_MAX         (48 * 1024)
#define RESP_CAP        4096

typedef struct {
    uint32_t n;
    char to[OS_MESH_ID_LEN + 1];
    char *json;                         // {"app":..,"to":..,"d":..}, malloc'd
} send_t;

static collab_info_t s_info;
static collab_show_t s_show;
static bool s_on;                       // the service runs
static esp_timer_handle_t s_timer;
static volatile bool s_tick_queued;
static void *s_tok;                     // the sync in flight (os_api ctx), NULL = none
static int64_t s_next_sync;
static bool s_start_flag;               // the first sync since start says so
// The session as the badge runs it.
static long s_sid;                      // 0 = none
static int s_rev = -1;                  // the display revision we have
static uint32_t s_ack;                  // highest send handled
static send_t s_sends[SENDS_MAX];
static int s_nsends;
static int64_t s_send_at;
static char s_listen[LISTEN_MAX][25];
static int s_nlisten;
static char *s_rx[RX_KEEP];             // {"from":..,"to":..,"app":..,"d":..} waiting for a sync
static int s_nrx;
static int s_rx_sent;                   // how many the sync in flight carries
// An end to tell the server at the next sync.
static const char *s_end;               // "owner" | "battery" | "busy" | NULL
static long s_end_sid;
// The takeover screen.
static bool s_closing;                  // we close it (not the owner)
static int64_t s_screen_wait_toast;
// LEDs.
static esp_timer_handle_t s_led_timer;
static char s_led_p[8];                 // "" = the badge's own
static uint32_t s_led_col, s_led_ms, s_led_frame;
// Image.
static char s_img_sha[65];
static lv_image_dsc_t s_img_dsc;
static uint16_t *s_img_px;
static void *s_img_tok;
static volatile bool s_img_busy;
static char s_img_next[65];              // a new image asked for while a decode reads the file: fetched after it

// The mesh task's ring (s_rx_mux).
static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static char *s_ring[RX_RING];
static int s_ring_n;
static volatile bool s_listening;       // a station is on: copy APP messages

static void changed(void)
{
    s_info.rev++;
}

static bool nvs_flag(const char *key, bool dflt)
{
    nvs_handle_t h;
    uint8_t v = dflt ? 1 : 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void nvs_flag_set(const char *key, bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool collab_consent(void)
{
    return nvs_flag("ok", false);
}

const collab_info_t *collab_info(void)
{
    return &s_info;
}

const collab_show_t *collab_show(void)
{
    return &s_show;
}

// ---------------------------------------------------------------------------
// LEDs (the screen's: only while the takeover screen shows)

static uint8_t led_scale(uint32_t c, int shift, int num, int den)
{
    return (uint8_t)(((c >> shift) & 0xFF) * OS_LED_MAX / 255 * num / den);
}

static void led_frame(void *arg)
{
    uint32_t f = s_led_frame++;
    uint32_t c = s_led_col ? s_led_col : COL_GREEN;
    uint32_t period = s_led_ms ? s_led_ms : 800;
    uint32_t steps = period / LED_FRAME_MS ? period / LED_FRAME_MS : 1;
    uint32_t phase = f % steps;
    if (!strcmp(s_led_p, "solid")) {
        bsp_rgb_fill(led_scale(c, 16, 1, 1), led_scale(c, 8, 1, 1), led_scale(c, 0, 1, 1));
    } else if (!strcmp(s_led_p, "blink")) {
        bool lit = phase < steps / 2;
        bsp_rgb_fill(lit ? led_scale(c, 16, 1, 1) : 0, lit ? led_scale(c, 8, 1, 1) : 0, lit ? led_scale(c, 0, 1, 1) : 0);
    } else if (!strcmp(s_led_p, "pulse")) {
        int half = steps / 2 ? steps / 2 : 1;
        int lvl = phase < (uint32_t)half ? (int)phase : (int)steps - (int)phase;
        bsp_rgb_fill(led_scale(c, 16, lvl, half), led_scale(c, 8, lvl, half), led_scale(c, 0, lvl, half));
    } else if (!strcmp(s_led_p, "chase")) {
        int at = (int)(phase * BOARD_RGB_COUNT / steps);
        for (int i = 0; i < BOARD_RGB_COUNT; i++) {
            bool lit = i == at;
            bsp_rgb_set(i, lit ? led_scale(c, 16, 1, 1) : 0, lit ? led_scale(c, 8, 1, 1) : 0, lit ? led_scale(c, 0, 1, 1) : 0);
        }
    } else if (!strcmp(s_led_p, "rainbow")) {
        for (int i = 0; i < BOARD_RGB_COUNT; i++) {
            int h = (int)((phase * 360 / steps + i * 360 / BOARD_RGB_COUNT) % 360);
            int x = (h % 60) * 255 / 60;        // the rising or falling edge within a sixth
            int r, g, b;
            switch (h / 60) {
            case 0:  r = 255;     g = x;       b = 0;       break;
            case 1:  r = 255 - x; g = 255;     b = 0;       break;
            case 2:  r = 0;       g = 255;     b = x;       break;
            case 3:  r = 0;       g = 255 - x; b = 255;     break;
            case 4:  r = x;       g = 0;       b = 255;     break;
            default: r = 255;     g = 0;       b = 255 - x; break;
            }
            uint32_t rgb = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
            bsp_rgb_set(i, led_scale(rgb, 16, 1, 1), led_scale(rgb, 8, 1, 1), led_scale(rgb, 0, 1, 1));
        }
    } else {
        bsp_rgb_fill(0, 0, 0);
    }
    bsp_rgb_show();
}

// The pattern runs only while the takeover screen shows. on_screen: -1 asks
// the OS (a display update), 1 the screen is entering, 0 it is leaving: in
// both on_enter and on_exit os_current() still names the app before.
static void led_set(int on_screen)
{
    bool here = on_screen < 0 ? os_current() == &app_collab_screen : on_screen > 0;
    bool want = s_led_p[0] && strcmp(s_led_p, "off") && here;
    if (!s_led_timer) {
        return;                             // made at boot (collab_init)
    }
    esp_timer_stop(s_led_timer);
    if (want) {
        s_led_frame = 0;
        esp_timer_start_periodic(s_led_timer, LED_FRAME_MS * 1000);
    } else {
        bsp_rgb_fill(0, 0, 0);
        bsp_rgb_show();
    }
}

static void led_apply(void)
{
    led_set(-1);
}

void collab_screen_entered(void)
{
    led_set(1);                             // its first pattern lights now, not at the next update
}

// ---------------------------------------------------------------------------
// The image: downloaded to the card, decoded by a short-lived task into PSRAM

typedef struct {
    FILE *f;
} png_src_t;

static size_t png_read(void *ud, uint8_t *buf, size_t n)
{
    return fread(buf, 1, n, ((png_src_t *)ud)->f);
}

typedef struct {
    char sha[65];
    uint16_t *px;
    int w, h;
    bool ok;
} decoded_t;

static void img_free(void)
{
    s_show.img = NULL;
    heap_caps_free(s_img_px);
    s_img_px = NULL;
    memset(&s_img_dsc, 0, sizeof(s_img_dsc));
}

static void img_fetch(const char *sha);

static void img_ready(void *arg)
{
    decoded_t *d = arg;
    s_img_busy = false;
    if (d->ok && !strcmp(d->sha, s_img_sha) && s_show.k == COLLAB_SHOW_IMAGE) {
        img_free();
        s_img_px = d->px;
        s_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_img_dsc.header.w = d->w;
        s_img_dsc.header.h = d->h;
        s_img_dsc.header.stride = d->w * 2;
        s_img_dsc.data_size = d->w * d->h * 2;
        s_img_dsc.data = (const uint8_t *)d->px;
        s_show.img = &s_img_dsc;
    } else {
        heap_caps_free(d->px);
        if (!strcmp(d->sha, s_img_sha)) {
            s_show.img_failed = true;
        }
    }
    s_info.show_rev++;
    free(d);
    if (s_img_next[0]) {
        char next[sizeof(s_img_next)];
        strlcpy(next, s_img_next, sizeof(next));
        s_img_next[0] = 0;
        s_img_sha[0] = 0;                   // so img_fetch doesn't take it for the one it has
        if (s_show.k == COLLAB_SHOW_IMAGE) {
            img_fetch(next);
        }
    }
}

static void decode_task(void *arg)
{
    decoded_t *d = arg;
    char path[96];
    os_data_path(path, sizeof(path), IMG_PATH);
    png_src_t src = { .f = fopen(path, "rb") };
    lh_png_info_t info;
    lh_png_work_t *w = malloc(sizeof(*w));
    uint8_t *raw = NULL;
    const char *err = !src.f ? "can't open it" : !w ? "no memory" : lh_png_info(png_read, &src, &info);
    if (!err && (info.w > 320 || info.h > 200)) {
        err = "too big";
    }
    if (!err) {
        raw = heap_caps_malloc(lh_png_raw_size(&info), os_mem_bulk_caps());
        d->px = heap_caps_malloc((size_t)info.w * info.h * 2, os_mem_bulk_caps());
        err = !raw || !d->px ? "no memory" : lh_png_decode(png_read, &src, &info, w, raw, d->px);
        if (!err) {
            d->w = info.w;
            d->h = info.h;
            d->ok = true;
        }
    }
    if (err) {
        ESP_LOGW(TAG, "image %.12s: %s", d->sha, err);
    }
    if (src.f) {
        fclose(src.f);
    }
    heap_caps_free(raw);
    free(w);
    if (!d->ok) {
        heap_caps_free(d->px);
        d->px = NULL;
    }
    if (!os_post(img_ready, d)) {
        heap_caps_free(d->px);
        free(d);
        s_img_busy = false;
    }
    vTaskDelete(NULL);
}

static void on_img(int status, const char *body, void *ctx)
{
    if (ctx != s_img_tok || !ctx) {
        return;
    }
    s_img_tok = NULL;
    ESP_LOGI(TAG, "image %.12s: download answered %d", s_img_sha, status);
    decoded_t *d = calloc(1, sizeof(*d));
    if (status < 200 || status > 299 || !d) {
        free(d);
        s_show.img_failed = true;
        s_info.show_rev++;
        return;
    }
    strlcpy(d->sha, s_img_sha, sizeof(d->sha));
    s_img_busy = true;
    if (xTaskCreate(decode_task, "collab_png", 4096, d, 3, NULL) != pdPASS) {
        free(d);
        s_img_busy = false;
        s_show.img_failed = true;
        s_info.show_rev++;
    }
}

static void img_fetch(const char *sha)
{
    if (!strcmp(sha, s_img_sha) && (s_show.img || s_img_tok || s_img_busy)) {
        s_show.img = s_img_px ? &s_img_dsc : NULL;
        return;
    }
    if (s_img_busy) {
        // decode_task still reads cache/collab.png: a download now would rewrite it
        // under the reader. This one waits; img_ready fetches it (and drops the old).
        strlcpy(s_img_next, sha, sizeof(s_img_next));
        strlcpy(s_img_sha, sha, sizeof(s_img_sha));
        img_free();
        return;
    }
    os_api_cancel(s_img_tok);
    s_img_tok = NULL;
    img_free();
    strlcpy(s_img_sha, sha, sizeof(s_img_sha));
    if (!os_data_ready()) {
        s_show.img_failed = true;
        return;
    }
    char path[96];                          // 25 + 64 hex
    snprintf(path, sizeof(path), "/api/device/collab/image/%s", sha);
    char dest[96];
    os_data_path(dest, sizeof(dest), IMG_PATH);
    void *tok = os_api_token();
    if (os_api_download(path, dest, IMG_MAX, on_img, tok) == ESP_OK) {
        s_img_tok = tok;
    } else {
        s_show.img_failed = true;
    }
}

// ---------------------------------------------------------------------------
// The session

static void sends_clear(void)
{
    for (int i = 0; i < s_nsends; i++) {
        free(s_sends[i].json);
    }
    s_nsends = 0;
}

static void rx_clear(void)
{
    for (int i = 0; i < s_nrx; i++) {
        free(s_rx[i]);
    }
    s_nrx = 0;
    s_rx_sent = 0;
    portENTER_CRITICAL(&s_rx_mux);
    char *ring[RX_RING];
    int n = s_ring_n;
    memcpy(ring, s_ring, sizeof(ring));
    s_ring_n = 0;
    portEXIT_CRITICAL(&s_rx_mux);
    for (int i = 0; i < n; i++) {
        free(ring[i]);
    }
}

static void close_screen(void)
{
    if (os_current() == &app_collab_screen) {
        s_closing = true;
        os_launch(&os_launcher);
    }
}

// The session is over on our side (ended by anyone): radio, screen, LEDs back.
static void session_drop(void)
{
    if (!s_sid && !s_info.station[0]) {
        return;
    }
    ESP_LOGI(TAG, "session %ld over", s_sid);
    s_sid = 0;
    s_rev = -1;
    s_ack = 0;
    s_listening = false;
    os_mesh_station(NULL, NULL);
    sends_clear();
    rx_clear();
    s_nlisten = 0;
    s_info.station[0] = 0;
    s_info.screen = false;
    s_info.minutes = s_info.pending = 0;
    s_info.sent = s_info.heard = 0;
    s_led_p[0] = 0;
    led_apply();
    close_screen();
    os_api_cancel(s_img_tok);
    s_img_tok = NULL;
    img_free();
    s_img_sha[0] = 0;
    s_img_next[0] = 0;
    memset(&s_show, 0, sizeof(s_show));
    s_info.show_rev++;
    changed();
}

static uint32_t hex_col(const cJSON *o, const char *key, bool *has)
{
    const char *v = game_jstr(o, key);
    *has = strlen(v) == 6;
    return *has ? game_parse_hex(v, 0) : 0;
}

static void apply_show(const cJSON *show)
{
    const char *k = game_jstr(show, "k");
    collab_show_t n = {0};
    n.k = !strcmp(k, "card") ? COLLAB_SHOW_CARD : !strcmp(k, "qr") ? COLLAB_SHOW_QR : !strcmp(k, "image") ? COLLAB_SHOW_IMAGE
        : !strcmp(k, "color") ? COLLAB_SHOW_COLOR : !strcmp(k, "count") ? COLLAB_SHOW_COUNT : COLLAB_SHOW_IDLE;
    game_ascii(n.title, sizeof(n.title), game_jstr(show, "title"));
    game_ascii(n.body, sizeof(n.body), game_jstr(show, "body"));
    game_ascii(n.text, sizeof(n.text), game_jstr(show, "text"));
    n.col = hex_col(show, "col", &n.has_col);
    n.bg = hex_col(show, "bg", &n.has_bg);
    const cJSON *to = cJSON_GetObjectItem(show, "to");
    n.to = cJSON_IsNumber(to) ? (int64_t)to->valuedouble : 0;
    if (n.k == COLLAB_SHOW_QR && !n.text[0]) {
        n.k = COLLAB_SHOW_IDLE;
    }
    const lv_image_dsc_t *had = s_show.img;
    s_show = n;
    s_show.img = had;
    const char *img = game_jstr(show, "img");
    if (n.k == COLLAB_SHOW_IMAGE && strlen(img) == 64) {
        img_fetch(img);
    } else {
        os_api_cancel(s_img_tok);
        s_img_tok = NULL;
        img_free();
        s_img_sha[0] = 0;
        s_img_next[0] = 0;
    }
    s_info.show_rev++;
}

static void apply_led(const cJSON *led)
{
    if (!cJSON_IsObject(led)) {
        s_led_p[0] = 0;
    } else {
        game_ascii(s_led_p, sizeof(s_led_p), game_jstr(led, "p"));
        bool has;
        s_led_col = hex_col(led, "col", &has);
        int ms = game_jint(led, "ms", 800);
        s_led_ms = ms < 100 ? 100 : ms > 5000 ? 5000 : ms;
    }
    led_apply();
}

static bool app_id_ok(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 24) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) {
            return false;
        }
    }
    return true;
}

static bool id_ok(const char *s)
{
    if (strlen(s) != OS_MESH_ID_LEN) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || *s == 'e')) {
            return false;
        }
    }
    return true;
}

static void queue_send(const cJSON *e)
{
    const cJSON *jn = cJSON_GetObjectItem(e, "n");
    const char *to = game_jstr(e, "to"), *app = game_jstr(e, "app");
    const cJSON *d = cJSON_GetObjectItem(e, "d");
    if (!cJSON_IsNumber(jn) || jn->valuedouble < 1 || jn->valuedouble > 4294967295.0) {
        return;
    }
    uint32_t n = (uint32_t)jn->valuedouble;
    if (n <= s_ack) {
        return;                             // done already
    }
    for (int i = 0; i < s_nsends; i++) {
        if (s_sends[i].n == n) {
            return;                         // waiting already
        }
    }
    bool ok = (!strcmp(to, "*") || id_ok(to)) && app_id_ok(app) && cJSON_IsObject(d) && s_nsends < SENDS_MAX;
    char *json = NULL;
    if (ok) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "app", app);
        cJSON_AddStringToObject(m, "to", to);
        cJSON_AddItemToObject(m, "d", cJSON_Duplicate(d, true));
        json = cJSON_PrintUnformatted(m);
        cJSON_Delete(m);
        char *dj = cJSON_PrintUnformatted(d);
        ok = json && dj && strlen(dj) <= 200;
        cJSON_free(dj);
    }
    if (!ok) {
        cJSON_free(json);
        if (n > s_ack) {
            s_ack = n;                      // dropped: handled
        }
        return;
    }
    send_t *s = &s_sends[s_nsends++];
    s->n = n;
    strlcpy(s->to, to, sizeof(s->to));
    s->json = json;
}

static void send_one(int64_t now)
{
    if (!s_nsends || now - s_send_at < SEND_GAP_MS) {
        return;
    }
    s_send_at = now;
    send_t s = s_sends[0];
    memmove(&s_sends[0], &s_sends[1], (s_nsends - 1) * sizeof(send_t));
    s_nsends--;
    esp_err_t err = os_mesh_station_send(strcmp(s.to, "*") ? s.to : NULL, s.json);
    if (err == ESP_OK) {
        s_info.sent++;
        changed();
    } else {
        ESP_LOGW(TAG, "send %" PRIu32 " to %s: %s", s.n, s.to, esp_err_to_name(err));
    }
    if (s.n > s_ack) {
        s_ack = s.n;                        // sent or dropped, like the air
    }
    free(s.json);
}

// What the mesh task copied: keep APP messages for the station, or broadcasts
// of an app the server listens for. Hostile input: checked like any APP message.
static void rx_filter(void)
{
    char *ring[RX_RING];
    portENTER_CRITICAL(&s_rx_mux);
    int n = s_ring_n;
    memcpy(ring, s_ring, sizeof(ring));
    s_ring_n = 0;
    portEXIT_CRITICAL(&s_rx_mux);
    for (int i = 0; i < n; i++) {
        const char *from = ring[i], *json = ring[i] + OS_MESH_ID_LEN + 1;
        cJSON *m = s_sid ? cJSON_Parse(json) : NULL;
        const char *to = game_jstr(m, "to"), *app = game_jstr(m, "app");
        const cJSON *d = cJSON_GetObjectItem(m, "d");
        bool keep = cJSON_IsObject(d) && app_id_ok(app) && (!strcmp(to, s_info.station) || (!strcmp(to, "*") && s_info.station[0]));
        if (keep && strcmp(to, s_info.station)) {
            keep = false;
            for (int k = 0; k < s_nlisten; k++) {
                keep = keep || !strcmp(app, s_listen[k]);
            }
        }
        char *out = NULL;
        if (keep) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "from", from);
            cJSON_AddStringToObject(e, "to", to);
            cJSON_AddStringToObject(e, "app", app);
            cJSON_AddItemToObject(e, "d", cJSON_Duplicate(d, true));
            out = cJSON_PrintUnformatted(e);
            cJSON_Delete(e);
        }
        if (out && strlen(out) <= RX_JSON_MAX) {
            if (s_nrx == RX_KEEP) {
                free(s_rx[0]);              // the oldest goes first
                memmove(&s_rx[0], &s_rx[1], (RX_KEEP - 1) * sizeof(char *));
                s_nrx--;
                s_rx_sent = s_rx_sent > 0 ? s_rx_sent - 1 : 0;
            }
            char *copy = strdup(out);
            if (copy) {
                s_rx[s_nrx++] = copy;
                s_info.heard++;
                changed();
            }
        }
        cJSON_free(out);
        cJSON_Delete(m);
        free(ring[i]);
    }
}

// Mesh task: copy, never parse (MESH.md "Collaborator stations").
static void on_mesh(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx)
{
    if (!s_listening || from->bridged || from->kind == OS_PEER_BEACON) {
        return;
    }
    size_t len = strlen(json);
    if (len > RX_JSON_MAX) {
        return;
    }
    char *copy = malloc(OS_MESH_ID_LEN + 1 + len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, from->id, OS_MESH_ID_LEN + 1);
    memcpy(copy + OS_MESH_ID_LEN + 1, json, len + 1);
    portENTER_CRITICAL(&s_rx_mux);
    bool room = s_ring_n < RX_RING;
    if (room) {
        s_ring[s_ring_n++] = copy;
    }
    portEXIT_CRITICAL(&s_rx_mux);
    if (!room) {
        free(copy);
    }
}

// ---------------------------------------------------------------------------
// Syncing

static int battery(bool *usb)
{
    *usb = os_battery_usb();            // the OS's one filtered reading (2.4.0)
    return os_battery_pct();
}

static char *sync_body(bool on)
{
    bool usb;
    int batt = battery(&usb);
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    cJSON *b = cJSON_CreateObject();
    cJSON_AddBoolToObject(b, "on", on);
    if (s_start_flag) {
        cJSON_AddBoolToObject(b, "start", true);
    }
    cJSON_AddBoolToObject(b, "screen", s_info.lend);
    cJSON_AddNumberToObject(b, "batt", batt);
    cJSON_AddBoolToObject(b, "usb", usb);
    cJSON_AddNumberToObject(b, "wifi", w.state == OS_WIFI_CONNECTED ? w.channel : 0);
    cJSON_AddNumberToObject(b, "mesh", os_mesh_channel());
    long sid = s_end ? s_end_sid : s_sid;
    if (sid) {
        cJSON_AddNumberToObject(b, "sid", sid);
    }
    if (s_end) {
        cJSON_AddStringToObject(b, "end", s_end);
    }
    os_mesh_peer_t *p = malloc(NEAR_MAX * sizeof(*p));
    int np = p ? os_mesh_peers(p, NEAR_MAX) : 0;
    if (s_sid) {
        cJSON_AddNumberToObject(b, "rev", s_rev);
        cJSON_AddNumberToObject(b, "ack", s_ack);
        cJSON *rx = cJSON_AddArrayToObject(b, "rx");
        s_rx_sent = s_nrx < 8 ? s_nrx : 8;
        for (int i = 0; i < s_rx_sent; i++) {
            cJSON_AddItemToArray(rx, cJSON_Parse(s_rx[i]));
        }
        cJSON *near = cJSON_AddArrayToObject(b, "near");
        for (int i = 0; i < np; i++) {
            if (p[i].bridged) {
                continue;
            }
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "id", p[i].id);
            cJSON_AddStringToObject(e, "k", p[i].kind == OS_PEER_BEACON ? "c" : p[i].kind == OS_PEER_STATION ? "s" : "b");
            cJSON_AddNumberToObject(e, "r", p[i].rssi);
            if (p[i].name[0]) {
                cJSON_AddStringToObject(e, "n", p[i].name);
            }
            cJSON_AddItemToArray(near, e);
        }
    } else {
        int badges = 0;
        for (int i = 0; i < np; i++) {
            badges += p[i].kind == OS_PEER_BADGE && !p[i].bridged;
        }
        cJSON_AddNumberToObject(b, "nearN", badges);
    }
    free(p);
    char *out = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    return out;
}

static void tell_ended(const cJSON *ended)
{
    if (!cJSON_IsObject(ended)) {
        return;
    }
    const char *by = game_jstr(ended, "by");
    int paid = game_jint(ended, "paid", 0);
    if (!strcmp(by, "admin")) {
        if (paid > 0) {
            os_service_notify("collab", COL_LAMPS, "takeover ended, +%d !LAMPs", paid);
        } else {
            os_service_notify("collab", 0, "takeover ended");
        }
    } else if (!strcmp(by, "drop")) {
        os_service_notify("collab", 0, "takeover ended: the badge dropped out. No !LAMPs");
    }
}

static void on_sync(int status, const char *body, void *ctx)
{
    if (!ctx || ctx != s_tok) {
        return;
    }
    s_tok = NULL;
    int64_t now = game_now_ms();
    cJSON *j = status == 200 ? cJSON_Parse(body) : NULL;
    const char *state = game_jstr(j, "state");
    if (!cJSON_IsObject(j) || !state[0]) {
        cJSON *e = status ? cJSON_Parse(body ? body : "") : NULL;
        const char *msg = game_jstr(e, "error");
        if (!status) {
            strlcpy(s_info.why, "Can't reach the server", sizeof(s_info.why));
        } else if (status == 403) {
            strlcpy(s_info.why, "Claim this badge first", sizeof(s_info.why));
        } else {
            game_ascii(s_info.why, sizeof(s_info.why), msg[0] ? msg : "The server didn't answer properly");
        }
        cJSON_Delete(e);
        cJSON_Delete(j);
        // A takeover goes on through a Wi-Fi blip (the server's grace decides), so
        // the owner keeps seeing it: only a badge without one is "waiting".
        if (!s_sid && s_info.phase != COLLAB_WAITING) {
            s_info.phase = COLLAB_WAITING;
            s_info.live_since = 0;
        }
        s_next_sync = now + RETRY_MS;
        s_rx_sent = 0;
        changed();
        return;
    }
    s_start_flag = false;
    if (s_end) {
        s_end = NULL;                       // told
        s_end_sid = 0;
    }
    // What it carried is the server's now.
    for (int i = 0; i < s_rx_sent && i < s_nrx; i++) {
        free(s_rx[i]);
    }
    if (s_rx_sent > 0) {
        int left = s_nrx - s_rx_sent;
        memmove(&s_rx[0], &s_rx[s_rx_sent], (left > 0 ? left : 0) * sizeof(char *));
        s_nrx = left > 0 ? left : 0;
    }
    s_rx_sent = 0;

    s_info.synced_ms = now;
    s_info.earns = cJSON_IsTrue(cJSON_GetObjectItem(j, "earns"));
    game_ascii(s_info.why, sizeof(s_info.why), game_jstr(j, "why"));
    const cJSON *t = cJSON_GetObjectItem(j, "today");
    s_info.live_min = game_jint(t, "live", 0);
    s_info.earned = game_jint(t, "earned", 0);
    s_info.cap = game_jint(t, "cap", 0);
    const cJSON *r = cJSON_GetObjectItem(j, "rates");
    s_info.rate_live = game_jint(r, "liveLamps", s_info.rate_live);
    s_info.rate_minutes = game_jint(r, "liveMinutes", s_info.rate_minutes);
    s_info.rate_take = game_jint(r, "takeoverLamps", s_info.rate_take);
    tell_ended(cJSON_GetObjectItem(j, "ended"));

    int next = game_jint(j, "next", LIVE_EVERY_MS);
    s_next_sync = now + (next < NEXT_MIN_MS ? NEXT_MIN_MS : next > NEXT_MAX_MS ? NEXT_MAX_MS : next);

    const cJSON *sess = cJSON_GetObjectItem(j, "session");
    if (!strcmp(state, "off") || !s_on) {
        session_drop();
        s_info.phase = s_on ? COLLAB_WAITING : COLLAB_OFF;
        s_info.live_since = 0;
    } else if (!cJSON_IsObject(sess)) {
        session_drop();
        if (s_info.phase != COLLAB_LIVE) {
            s_info.live_since = now;
        }
        s_info.phase = COLLAB_LIVE;
    } else {
        const cJSON *jid = cJSON_GetObjectItem(sess, "id");
        long sid = cJSON_IsNumber(jid) ? (long)jid->valuedouble : 0;
        const cJSON *st = cJSON_GetObjectItem(sess, "station");
        char station[OS_MESH_ID_LEN + 1];
        strlcpy(station, game_jstr(st, "id"), sizeof(station));
        if (sid != s_sid) {
            session_drop();                 // another session: start clean
            if (sid > 0 && id_ok(station)) {
                s_sid = sid;
                strlcpy(s_info.station, station, sizeof(s_info.station));
                char name[25];
                game_ascii(name, sizeof(name), game_jstr(st, "n"));
                os_mesh_station(station, name);
                s_listening = true;
                os_service_notify("collab", COL_GREEN, "the server is using this badge");
                ESP_LOGI(TAG, "session %ld: station %s", sid, station);
            }
        }
        if (s_sid) {
            if (s_info.phase != COLLAB_LIVE && s_info.phase != COLLAB_TAKEN) {
                s_info.live_since = now;
            }
            s_info.phase = COLLAB_TAKEN;
            s_info.screen = cJSON_IsTrue(cJSON_GetObjectItem(sess, "screen")) && s_info.lend;
            s_info.minutes = game_jint(sess, "minutes", 0);
            s_info.pending = game_jint(sess, "pending", 0);
            int rev = game_jint(sess, "rev", 0);
            if (cJSON_GetObjectItem(sess, "show")) {
                apply_show(cJSON_GetObjectItem(sess, "show"));
                apply_led(cJSON_GetObjectItem(sess, "led"));
                const cJSON *ls = cJSON_GetObjectItem(sess, "listen");
                s_nlisten = 0;
                const cJSON *a;
                cJSON_ArrayForEach(a, ls) {
                    if (cJSON_IsString(a) && app_id_ok(a->valuestring) && s_nlisten < LISTEN_MAX) {
                        strlcpy(s_listen[s_nlisten++], a->valuestring, sizeof(s_listen[0]));
                    }
                }
                s_rev = rev;
            }
            const cJSON *e;
            cJSON_ArrayForEach(e, cJSON_GetObjectItem(sess, "send")) {
                queue_send(e);
            }
        }
    }
    cJSON_Delete(j);
    changed();
}

static void sync_now(void)
{
    if (s_tok) {
        return;
    }
    char *body = sync_body(s_on);
    void *tok = os_api_token();
    if (body && os_api_call("POST", "/api/device/collab/sync", body, RESP_CAP, on_sync, tok) == ESP_OK) {
        s_tok = tok;
    } else {
        s_next_sync = game_now_ms() + (os_api_online() ? 2000 : RETRY_MS);
        if (!os_api_online() && !s_sid && s_info.phase != COLLAB_WAITING) {
            s_info.phase = COLLAB_WAITING;
            s_info.live_since = 0;
            strlcpy(s_info.why, "Waiting for Wi-Fi", sizeof(s_info.why));
            changed();
        }
    }
    cJSON_free(body);
}

// The goodbye: Collaborator was turned off (on: false). Best effort; a badge
// that can't say it simply goes quiet, which the server reads the same way.
static void on_bye(int status, const char *body, void *ctx)
{
    (void)status;
    (void)body;
    (void)ctx;
}

// ---------------------------------------------------------------------------
// The takeover screen

static bool busy_elsewhere(void)
{
    if (os_held() || os_confirm_open() || os_overlay() || lua_install_busy(NULL)) {
        return true;
    }
    const trade_view_t *t = trade_current();
    if (t && t->phase != TRADE_IDLE && t->phase != TRADE_DONE && t->phase != TRADE_CANCELLED) {
        return true;
    }
    const xdex_swap_t *x = xdex_last();
    if (x && (x->phase == XDEX_ASKING || x->phase == XDEX_FINISHING)) {
        return true;
    }
    const os_app_t *cur = os_current();
    const char *id = cur && cur->id ? cur->id : "";
    return !strcmp(id, "beacon") || !strcmp(id, "calib") || !strcmp(id, "update");
}

static void screen_tick(int64_t now)
{
    const os_app_t *cur = os_current();
    if (!s_info.screen) {
        close_screen();
        return;
    }
    if (cur == &app_collab_screen || busy_elsewhere()) {
        return;
    }
    bool idle = cur == &os_launcher || now - os_last_key_ms() >= IDLE_MS;
    if (idle) {
        ESP_LOGI(TAG, "showing the takeover screen (was %s)", cur && cur->id ? cur->id : "?");
        os_launch(&app_collab_screen);
    } else if (now - s_screen_wait_toast > 60000) {
        s_screen_wait_toast = now;
        os_service_notify("collab", 0, "the server wants the screen when you're done");
    }
}

void collab_screen_left(void)
{
    led_set(0);                             // stopped and dark: the badge's own LEDs again
    if (s_closing) {
        s_closing = false;
        return;
    }
    if (s_sid) {
        collab_end_takeover();              // HOME or CANCEL: the owner's end
    }
}

// ---------------------------------------------------------------------------
// The tick

static void tick(void *arg)
{
    s_tick_queued = false;
    if (!s_on) {
        return;
    }
    int64_t now = game_now_ms();
    static int64_t batt_at;
    bool usb = true;
    int batt = -1;
    if (now - batt_at >= 5000) {                // the ADC every 5 s is plenty
        batt_at = now;
        batt = battery(&usb);
    }
    if (!usb && batt >= 0 && batt < BATT_MIN) {
        ESP_LOGW(TAG, "battery %d %%: Collaborator ends", batt);
        if (s_sid) {
            s_end = "battery";
            s_end_sid = s_sid;
        }
        os_service_notify("collab", 0, "turned off: battery low");
        os_service_post_stop("collab");
        return;
    }
    if (s_sid) {
        rx_filter();
        send_one(now);
        if (s_nrx > 0 && s_next_sync > now + NEXT_MIN_MS) {
            s_next_sync = now + NEXT_MIN_MS;
        }
    }
    if (!s_tok && now >= s_next_sync) {
        sync_now();
    }
    screen_tick(now);
}

static void tick_timer(void *arg)
{
    if (!s_tick_queued) {
        s_tick_queued = true;
        if (!os_post(tick, NULL)) {
            s_tick_queued = false;
        }
    }
}

// ---------------------------------------------------------------------------
// The service

static os_service_state_t svc_state(void *ctx)
{
    return s_on ? OS_SVC_RUNNING : OS_SVC_STOPPED;
}

static void svc_status(void *ctx, char *out, size_t n)
{
    char ago[24];
    int64_t now = game_now_ms();
    switch (s_info.phase) {
    case COLLAB_OFF:
        strlcpy(out, "Off", n);
        break;
    case COLLAB_WAITING:
        strlcpy(out, s_info.why[0] ? s_info.why : "Waiting for Wi-Fi", n);
        break;
    case COLLAB_LIVE:
        snprintf(out, n, "Live, %s", ui_fmt_ago(ago, sizeof(ago), now - s_info.live_since));
        if (!strncmp(out, "Live, just now", 14)) {
            strlcpy(out, "Live", n);
        } else {
            // "5 min ago" -> "5 min"
            char *p = strstr(out, " ago");
            if (p) {
                *p = 0;
            }
        }
        break;
    case COLLAB_TAKEN:
        snprintf(out, n, "Taken over (%s), %d min", s_info.screen ? "screen" : "radio", s_info.minutes);
        break;
    }
}

static int32_t svc_memory(void *ctx)
{
    if (!s_on) {
        return 0;
    }
    int32_t m = (int32_t)(sizeof(s_info) + sizeof(s_show) + sizeof(s_sends));
    if (s_img_px) {
        m += s_img_dsc.data_size;
    }
    return m;
}

static bool svc_start(void *ctx, const char **why)
{
    if (!collab_consent()) {
        *why = "Turn it on in the Collaborator app (it asks you first)";
        return false;
    }
    if (s_on) {
        return true;
    }
    s_on = true;
    s_start_flag = true;
    s_info.lend = nvs_flag("screen", true);
    s_info.phase = COLLAB_WAITING;
    s_info.why[0] = 0;
    s_next_sync = 0;
    // Live needs the network: join the saved one if Wi-Fi is off (as the Beacon
    // app does). Turned off later, it stays off: the owner's choice.
    char ssid[33];
    if (!os_wifi_enabled() && os_wifi_saved_ssid(ssid, sizeof(ssid))) {
        os_wifi_set_enabled(true);
    }
    os_api_prewarm();
    if (s_timer) {
        esp_timer_start_periodic(s_timer, TICK_MS * 1000);
    }
    ESP_LOGI(TAG, "on");
    changed();
    return true;
}

static void svc_stop(void *ctx)
{
    if (!s_on) {
        return;
    }
    s_on = false;
    if (s_timer) {
        esp_timer_stop(s_timer);
    }
    os_api_cancel(s_tok);
    s_tok = NULL;
    long sid = s_end ? s_end_sid : s_sid;
    const char *end = s_end ? s_end : sid ? "owner" : NULL;
    s_end = end;
    s_end_sid = sid;
    char *body = sync_body(false);          // on: false, with the end of a session in progress
    s_end = NULL;
    s_end_sid = 0;
    if (body) {
        os_api_call("POST", "/api/device/collab/sync", body, 0, on_bye, os_api_token());
        cJSON_free(body);
    }
    session_drop();
    nvs_flag_set("ok", false);              // turning it on again asks again
    s_info.phase = COLLAB_OFF;
    s_info.live_since = 0;
    ESP_LOGI(TAG, "off");
    changed();
}

static const char *svc_mark(void *ctx)
{
    return s_sid ? LV_SYMBOL_EYE_OPEN : NULL;     // whenever a takeover runs, whatever else
}

static const os_service_def_t s_def = {
    .id = "collab",
    .name = "Collaborator",
    .symbol = LV_SYMBOL_EYE_OPEN,
    .accent = COL_GREEN,
    .flags = OS_SVC_STOPPABLE | OS_SVC_BOOT | OS_SVC_SCREEN,
    .state = svc_state,
    .status = svc_status,
    .memory = svc_memory,
    .start = svc_start,
    .stop = svc_stop,
    .mark = svc_mark,
};

// ---------------------------------------------------------------------------
// The owner's actions

void collab_turn_on(void)
{
    nvs_flag_set("ok", true);
    const char *why = "";
    if (!os_service_start("collab", &why)) {
        ESP_LOGW(TAG, "didn't start: %s", why);
    }
}

void collab_turn_off(void)
{
    os_service_stop("collab", NULL);
}

void collab_set_lend(bool lend)
{
    nvs_flag_set("screen", lend);
    s_info.lend = lend;
    if (!lend) {
        s_info.screen = false;
        close_screen();
    }
    s_next_sync = 0;                        // tell the server now
    changed();
}

void collab_end_takeover(void)
{
    if (!s_sid) {
        return;
    }
    ESP_LOGI(TAG, "the owner ended session %ld", s_sid);
    s_end = "owner";
    s_end_sid = s_sid;
    session_drop();
    s_info.phase = COLLAB_LIVE;
    os_service_notify("collab", 0, "takeover ended. No !LAMPs");
    s_next_sync = 0;
    os_api_cancel(s_tok);                   // its answer would bring the session back
    s_tok = NULL;
    changed();
}

// ---------------------------------------------------------------------------
// Console: `collab` (the badge in hand: `collab on yes` gives the consent the
// app asks for, as `reset yes` does)

static void cmd_run(void *arg)
{
    char *line = arg;
    const char *why = "";
    if (!strcmp(line, "on yes")) {
        collab_turn_on();
    } else if (!strcmp(line, "off")) {
        collab_turn_off();
    } else if (!strcmp(line, "end")) {
        collab_end_takeover();
    } else if (!strcmp(line, "sync")) {
        s_next_sync = 0;
    } else if (!strcmp(line, "lend on") || !strcmp(line, "lend off")) {
        collab_set_lend(!strcmp(line, "lend on"));
    } else if (line[0] && strcmp(line, "status")) {
        printf("usage: collab [status|on yes|off|end|sync|lend on|off]\n");
        free(line);
        return;
    }
    (void)why;
    static const char *const phases[] = { "off", "waiting", "live", "taken" };
    char st[OS_SERVICE_STATUS_MAX + 1];
    svc_status(NULL, st, sizeof(st));
    printf("collab: %s (%s)%s\n", phases[s_info.phase], st, collab_consent() ? "" : ", no consent");
    // What the LEDs do (nobody at the console sees them): the pattern and whether it runs.
    printf("  leds: %s, %s\n", s_led_p[0] ? s_led_p : "none", s_led_timer && esp_timer_is_active(s_led_timer) ? "running" : "off");
    printf("  lend screen %s, earns %s%s%s; today %d min live, %d of %d !LAMPs\n", s_info.lend ? "yes" : "no",
           s_info.earns ? "yes" : "no", s_info.why[0] ? ": " : "", s_info.why, s_info.live_min, s_info.earned, s_info.cap);
    if (s_sid) {
        printf("  session %ld, station %s, screen %s, %d min, pending %d, rev %d, ack %" PRIu32
               ", %d sends waiting, %d heard waiting, sent %d, heard %d, led %s\n",
               s_sid, s_info.station, s_info.screen ? "yes" : "no", s_info.minutes, s_info.pending, s_rev, s_ack,
               s_nsends, s_nrx, s_info.sent, s_info.heard, s_led_p[0] ? s_led_p : "-");
    }
    free(line);
}

static int cmd_collab(int argc, char **argv)
{
    char *line = calloc(1, 32);
    if (!line) {
        return 1;
    }
    for (int i = 1; i < argc && i < 3; i++) {
        strlcat(line, argv[i], 32);
        if (i + 1 < argc && i < 2) {
            strlcat(line, " ", 32);
        }
    }
    if (!os_post(cmd_run, line)) {
        free(line);
        printf("collab: busy, try again\n");
        return 1;
    }
    return 0;
}

void collab_init(void)
{
    s_info.lend = nvs_flag("screen", true);
    s_info.rate_live = 1;
    s_info.rate_minutes = 10;
    s_info.rate_take = 2;
    // Both timers made at boot, not when Collaborator is first turned on or the
    // screen first lights the LEDs: long-lived things made mid-run split the heap.
    const esp_timer_create_args_t tick = { .callback = tick_timer, .name = "collab" };
    if (esp_timer_create(&tick, &s_timer) != ESP_OK) {
        s_timer = NULL;
    }
    const esp_timer_create_args_t led = { .callback = led_frame, .name = "collab_led" };
    if (esp_timer_create(&led, &s_led_timer) != ESP_OK) {
        s_led_timer = NULL;
    }
    os_service_register(&s_def);
    os_mesh_subscribe(OS_MESH_APP, on_mesh, NULL);
    os_cmd_register("collab", "collab [status|on yes|off|end|sync|lend on|off] - Collaborator (2.3.0)", cmd_collab);
}
