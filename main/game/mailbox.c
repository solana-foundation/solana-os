// Mailbox, the built-in background service (game/mailbox.h). Collects the
// owner's mail from the server, keeps it on the card, tells the owner.
//
// Everything the server sends is hostile input: every string is cut to its
// field and to printable ASCII before it's kept (game_ascii), `data` must be
// a JSON object of ≤ 128 bytes or it's dropped, and nothing in a message does
// anything on the badge beyond being shown (and handed to the app it names).

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"
#include "core/os_data.h"
#include "core/os_mem.h"
#include "core/os_service.h"
#include "core/theme.h"
#include "game.h"
#include "lua/lua_host.h"
#include "mailbox.h"

static const char *TAG = "mailbox";

#define INDEX_FILE      "mail/index.json"
#define INDEX_MAX       (16 * 1024)
#define MAIL_FILE_MAX   2048
#define PAGES_MAX       16              // per collection: 16 x 8 = every message the server keeps, and more
#define RETRY_MS        30000           // after a failed collection, while hello still says mail waits

typedef struct {
    mailbox_mail_t m;
    bool loaded;                        // text and data are in RAM (else on the card)
} slot_t;

static SemaphoreHandle_t s_mx;
static slot_t *s_slots;                 // newest first
static int s_n, s_cap;
static uint32_t s_after;                // the highest id collected (in RAM)
static uint32_t s_saved;                // the highest id the card's index holds: what we acknowledge
static uint32_t s_epoch;
static uint32_t s_server_gen;           // os_cloud_server_gen() when the list was read
static char s_server[96];               // the server the list and its cursor belong to
static bool s_ready;                    // the list was read from the card for this epoch
static bool s_card;                     // ... and the card was there

// The service, OS task only.
static bool s_on;
static bool s_busy, s_failed;
static void *s_tok;                     // os_api ctx of the collection running
static uint32_t s_tok_gen;              // os_cloud_server_gen() when it began
static int s_pages;
static int64_t s_synced_at, s_failed_at, s_seen_hello;
static int s_new;                       // new messages this collection, for the notice
static char s_new_an[MAILBOX_LABEL_MAX + 1], s_new_title[MAILBOX_TITLE_MAX + 1];

static void lock(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mx);
}

static uint32_t now_s(void)
{
    return (uint32_t)(os_cloud_time_ms() / 1000);   // 0 when the clock was never synced
}

static void mail_path(char *out, size_t n, uint32_t id)
{
    snprintf(out, n, "mail/%" PRIu32 ".json", id);
}

// ---------------------------------------------------------------------------
// The list (lock held)

static bool alloc_slots(void)
{
    if (s_slots) {
        return true;
    }
    // PSRAM when there is some (every 2.x badge): 50 messages are ~23 KB.
    s_cap = os_mem_psram() ? MAILBOX_MAX : MAILBOX_RAM_MAX;
    s_slots = heap_caps_calloc(s_cap, sizeof(slot_t), os_mem_psram() ? OS_MEM_PSRAM : OS_MEM_SRAM);
    if (!s_slots) {
        s_cap = 0;
    }
    return s_slots != NULL;
}

// How many the list holds now: without a card only the last few, in RAM until a restart.
static int cap(void)
{
    return s_card ? s_cap : (s_cap < MAILBOX_RAM_MAX ? s_cap : MAILBOX_RAM_MAX);
}

static int find(uint32_t id)
{
    for (int i = 0; i < s_n; i++) {
        if (s_slots[i].m.h.id == id) {
            return i;
        }
    }
    return -1;
}

static void remove_at(int i, bool file)
{
    if (file && s_card) {
        char p[32];
        mail_path(p, sizeof(p), s_slots[i].m.h.id);
        os_data_remove(p);
    }
    memmove(&s_slots[i], &s_slots[i + 1], (s_n - i - 1) * sizeof(slot_t));
    s_n--;
}

// Writes the index; true once the card has it (then s_saved = s_after).
// Without a card there's nothing to write: what's in RAM is all there is,
// acknowledged as such (HHGTS.md "Mailbox"), so s_saved follows s_after.
static bool save_index(void)
{
    if (!s_card) {
        s_saved = s_after;
        return true;
    }
    if (!os_data_ready()) {
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "server", s_server);
    cJSON_AddNumberToObject(root, "after", s_after);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (int i = 0; i < s_n; i++) {
        const mailbox_item_t *h = &s_slots[i].m.h;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", h->id);
        cJSON_AddStringToObject(o, "app", h->app);
        cJSON_AddStringToObject(o, "an", h->an);
        cJSON_AddStringToObject(o, "from", h->from);
        cJSON_AddStringToObject(o, "by", h->by);
        cJSON_AddStringToObject(o, "title", h->title);
        cJSON_AddNumberToObject(o, "at", h->at);
        cJSON_AddNumberToObject(o, "exp", h->exp);
        cJSON_AddBoolToObject(o, "r", h->read);
        cJSON_AddItemToArray(items, o);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    bool ok = false;
    if (json) {
        os_data_mkdirs(INDEX_FILE);
        ok = os_data_write(INDEX_FILE, json, strlen(json));
        cJSON_free(json);
    }
    if (ok) {
        s_saved = s_after;
    } else {
        ESP_LOGW(TAG, "couldn't save the index: acknowledging only up to %" PRIu32, s_saved);
    }
    return ok;
}

static uint32_t jnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble < 4294967296.0 ? (uint32_t)v->valuedouble : 0;
}

static void jstr(char *out, size_t n, const cJSON *o, const char *k)
{
    game_ascii(out, n, game_jstr(o, k));
}

// A message's header from the server or the index. false: not a message.
static bool parse_head(const cJSON *o, mailbox_item_t *h)
{
    memset(h, 0, sizeof(*h));
    h->id = jnum(o, "id");
    h->at = jnum(o, "at");
    h->exp = jnum(o, "exp");
    jstr(h->app, sizeof(h->app), o, "app");
    jstr(h->an, sizeof(h->an), o, "an");
    jstr(h->from, sizeof(h->from), o, "from");
    jstr(h->by, sizeof(h->by), o, "by");
    jstr(h->title, sizeof(h->title), o, "title");
    h->read = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "r"));
    if (!h->an[0]) {
        strlcpy(h->an, "Solana", sizeof(h->an));
    }
    return h->id > 0 && h->title[0];
}

// text and data of a message, from the server or its file.
static void parse_body(const cJSON *o, mailbox_mail_t *m)
{
    jstr(m->text, sizeof(m->text), o, "text");
    m->data[0] = 0;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(o, "data");
    if (cJSON_IsObject(d)) {
        char *s = cJSON_PrintUnformatted(d);
        if (s && strlen(s) <= MAILBOX_DATA_MAX) {
            game_ascii(m->data, sizeof(m->data), s);
            if (strlen(m->data) != strlen(s)) {
                m->data[0] = 0;         // it wasn't all printable: no data rather than other data
            }
        }
        cJSON_free(s);
    }
}

static void load(void)
{
    s_n = 0;
    s_after = s_saved = 0;
    s_epoch = os_data_epoch();
    s_server_gen = os_cloud_server_gen();
    os_cloud_get_server(s_server, sizeof(s_server));   // no allocation: it can't fail
    s_card = os_data_ready();
    s_ready = true;
    if (!alloc_slots() || !s_card) {
        return;
    }
    char *raw = os_data_read(INDEX_FILE, INDEX_MAX, NULL);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    s_after = s_saved = jnum(root, "after");
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (s_n >= cap()) {
            break;
        }
        slot_t *sl = &s_slots[s_n];
        memset(sl, 0, sizeof(*sl));
        if (parse_head(it, &sl->m.h) && find(sl->m.h.id) < 0) {
            s_n++;
        }
    }
    // Another server's mailbox (the owner kept, the server changed): its ids and
    // cursor mean nothing here, so it goes, files too, and collecting starts over.
    // An index without "server" (2.3.0) is the current server's. With no
    // server configured nothing is anyone else's: the mail stays and keeps
    // its server, so setting the same one again finds it.
    const char *was = game_jstr(root, "server");
    bool other = root && was && was[0] && s_server[0] && strcmp(was, s_server);
    if (!s_server[0] && was) {
        strlcpy(s_server, was, sizeof(s_server));
    }
    cJSON_Delete(root);
    if (other) {
        ESP_LOGI(TAG, "the server changed: the old server's %d messages go", s_n);
        while (s_n > 0) {
            remove_at(s_n - 1, true);
        }
        s_after = s_saved = 0;
        save_index();
    }
    ESP_LOGI(TAG, "%d on the card, after %" PRIu32, s_n, s_after);
}

// Before every use: a wipe (new owner, factory reset), a card put in or
// taken out, or another server since the list was read means it is someone
// else's or elsewhere.
static void fresh(void)
{
    if (!s_ready || s_epoch != os_data_epoch() || s_card != os_data_ready() ||
        s_server_gen != os_cloud_server_gen()) {
        load();
    }
}

static void purge_expired(void)
{
    uint32_t t = now_s();
    bool changed = false;
    for (int i = s_n - 1; t && i >= 0; i--) {
        if (s_slots[i].m.h.exp && s_slots[i].m.h.exp <= t) {
            remove_at(i, true);
            changed = true;
        }
    }
    if (changed) {
        save_index();
    }
}

// A collected message: kept newest first, the oldest read one making room,
// else the oldest. ADD_SKIP when it was there already (a repeated page) or
// has expired; ADD_FAIL when its file couldn't be written to the card: then
// it isn't kept, s_after doesn't pass it, and the page stops there, so the
// acknowledgement never covers mail the card doesn't have.
typedef enum { ADD_NEW, ADD_SKIP, ADD_FAIL } add_t;

static add_t add(const cJSON *o)
{
    slot_t sl = {0};
    if (cap() == 0 || !parse_head(o, &sl.m.h)) {
        return ADD_SKIP;
    }
    sl.m.h.read = false;
    uint32_t id = sl.m.h.id;
    if (find(id) >= 0 || (sl.m.h.exp && now_s() && sl.m.h.exp <= now_s())) {
        if (id > s_after) {
            s_after = id;
        }
        return ADD_SKIP;
    }
    parse_body(o, &sl.m);
    sl.loaded = true;
    if (s_card) {
        // The message as kept: what we parsed, never the server's raw text.
        cJSON *f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "id", id);
        cJSON_AddStringToObject(f, "app", sl.m.h.app);
        cJSON_AddStringToObject(f, "an", sl.m.h.an);
        cJSON_AddStringToObject(f, "from", sl.m.h.from);
        cJSON_AddStringToObject(f, "by", sl.m.h.by);
        cJSON_AddStringToObject(f, "title", sl.m.h.title);
        cJSON_AddStringToObject(f, "text", sl.m.text);
        cJSON_AddItemToObject(f, "data", sl.m.data[0] ? cJSON_Parse(sl.m.data) : cJSON_CreateNull());
        cJSON_AddNumberToObject(f, "at", sl.m.h.at);
        cJSON_AddNumberToObject(f, "exp", sl.m.h.exp);
        char *json = cJSON_PrintUnformatted(f);
        cJSON_Delete(f);
        char p[32];
        mail_path(p, sizeof(p), id);
        bool ok = json && os_data_mkdirs(p) && os_data_write(p, json, strlen(json));
        cJSON_free(json);
        if (!ok) {
            ESP_LOGW(TAG, "mail %" PRIu32 ": couldn't save it to the card; not acknowledged", id);
            return ADD_FAIL;
        }
    }
    if (id > s_after) {
        s_after = id;
    }
    while (s_n > 0 && s_n >= cap()) {
        int drop = s_n - 1;
        for (int i = s_n - 1; i >= 0; i--) {
            if (s_slots[i].m.h.read) {
                drop = i;
                break;
            }
        }
        remove_at(drop, true);
    }
    int at = 0;
    while (at < s_n && s_slots[at].m.h.id > id) {
        at++;
    }
    memmove(&s_slots[at + 1], &s_slots[at], (s_n - at) * sizeof(slot_t));
    s_slots[at] = sl;
    s_n++;
    return ADD_NEW;
}

// ---------------------------------------------------------------------------
// Reading (any task)

int mailbox_count(void)
{
    lock();
    fresh();
    int n = s_n;
    unlock();
    return n;
}

int mailbox_unread(void)
{
    lock();
    fresh();
    int n = 0;
    for (int i = 0; i < s_n; i++) {
        n += !s_slots[i].m.h.read;
    }
    unlock();
    return n;
}

bool mailbox_item(int i, mailbox_item_t *out)
{
    lock();
    fresh();
    bool ok = i >= 0 && i < s_n;
    if (ok) {
        *out = s_slots[i].m.h;
    }
    unlock();
    return ok;
}

int mailbox_for_app(const char *app, mailbox_item_t *out, int max)
{
    int k = 0;
    lock();
    fresh();
    for (int i = 0; i < s_n && k < max; i++) {
        if (app && app[0] && !strcmp(s_slots[i].m.h.app, app)) {
            out[k++] = s_slots[i].m.h;
        }
    }
    unlock();
    return k;
}

bool mailbox_get(uint32_t id, mailbox_mail_t *out)
{
    lock();
    fresh();
    int i = find(id);
    if (i >= 0 && !s_slots[i].loaded) {
        char p[32];
        mail_path(p, sizeof(p), id);
        char *raw = s_card ? os_data_read(p, MAIL_FILE_MAX, NULL) : NULL;
        cJSON *o = raw ? cJSON_Parse(raw) : NULL;
        free(raw);
        parse_body(o, &s_slots[i].m);   // a missing file reads as no text
        s_slots[i].loaded = true;
        cJSON_Delete(o);
    }
    if (i >= 0) {
        *out = s_slots[i].m;
    }
    unlock();
    return i >= 0;
}

void mailbox_mark_read(uint32_t id)
{
    lock();
    fresh();
    int i = find(id);
    if (i >= 0 && !s_slots[i].m.h.read) {
        s_slots[i].m.h.read = true;
        save_index();
    }
    unlock();
}

void mailbox_delete(uint32_t id)
{
    lock();
    fresh();
    int i = find(id);
    if (i >= 0) {
        remove_at(i, true);
        save_index();
    }
    unlock();
}

int mailbox_delete_read(void)
{
    int k = 0;
    lock();
    fresh();
    for (int i = s_n - 1; i >= 0; i--) {
        if (s_slots[i].m.h.read) {
            remove_at(i, true);
            k++;
        }
    }
    if (k) {
        save_index();
    }
    unlock();
    return k;
}

// ---------------------------------------------------------------------------
// Collecting (OS task)

static void get_page(void);

static void finish(bool ok)
{
    s_busy = false;
    s_tok = NULL;
    s_failed = !ok;
    if (ok) {
        s_synced_at = os_cloud_time_ms();
    } else {
        s_failed_at = game_now_ms();
    }
    lock();
    purge_expired();
    unlock();
    if (s_new == 1) {
        os_service_notify("mailbox", COL_YELLOW, "%s: %s", s_new_an, s_new_title);
    } else if (s_new > 1) {
        os_service_notify("mailbox", COL_YELLOW, "%d new messages", s_new);
    }
    if (s_new) {
        ESP_LOGI(TAG, "%d new", s_new);
    }
    s_new = 0;
}

static void ack_cb(int status, const char *body, void *ctx)
{
    if (ctx != s_tok) {
        return;
    }
    if (s_tok_gen != os_cloud_server_gen()) {
        finish(false);                      // the old server's answer
        return;
    }
    // A lost ack costs nothing: the next collection asks after our own last id.
    finish(status == 200);
}

static void page_cb(int status, const char *body, void *ctx)
{
    if (ctx != s_tok) {
        return;
    }
    if (s_tok_gen != os_cloud_server_gen()) {
        // The server changed mid-collection: this page is the old server's, and
        // its ids would move the new server's cursor. The retry starts afresh.
        finish(false);
        return;
    }
    cJSON *root = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "collect failed: %d", status);
        finish(false);
        return;
    }
    // Who gets told: apps on screen now (their on_mail), after the lock.
    struct {
        char app[MAILBOX_APP_MAX + 1];
        uint32_t id;
    } told[8];
    int ntold = 0;
    uint32_t before = s_after;
    lock();
    fresh();
    const cJSON *it;
    bool failed = false;
    cJSON_ArrayForEach(it, items) {
        add_t r = add(it);
        if (r == ADD_FAIL) {
            failed = true;                  // oldest first: nothing after it may be acknowledged
            break;
        }
        if (r != ADD_NEW) {
            continue;
        }
        const mailbox_item_t *h = &s_slots[find(jnum(it, "id"))].m.h;
        if (s_new++ == 0) {
            strlcpy(s_new_an, h->an, sizeof(s_new_an));
            if (h->by[0] && h->from[0] && strcmp(h->from, "server") && strcmp(h->from, "solana")) {
                snprintf(s_new_an, sizeof(s_new_an), "%.12s: %.10s", h->an, h->by);
            }
            strlcpy(s_new_title, h->title, sizeof(s_new_title));
        }
        if (h->app[0] && ntold < 8) {
            strlcpy(told[ntold].app, h->app, sizeof(told[ntold].app));
            told[ntold++].id = h->id;
        }
    }
    failed = !save_index() || failed;
    uint32_t after = s_after, saved = s_saved;
    unlock();
    bool more = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "more"));
    cJSON_Delete(root);
    for (int i = 0; i < ntold; i++) {
        lua_host_mail(told[i].app, told[i].id);     // the app on screen, if it's that one
        // SERVICE-SIDE on_mail: the app's running service, through the services'
        // call queue (lua_svc_call), never the app on screen a second time.
        lua_host_mail_service(told[i].app, told[i].id);
    }
    if (!failed && more && after > before && ++s_pages < PAGES_MAX) {
        get_page();
        return;
    }
    // Only what the card holds (s_saved): mail it couldn't keep stays on the
    // server and comes again at the next collection.
    char req[40];
    snprintf(req, sizeof(req), "{\"upto\":%" PRIu32 "}", saved);
    if (os_api_call("POST", "/api/device/mail/ack", req, 256, ack_cb, s_tok) != ESP_OK) {
        finish(false);
    }
}

static void get_page(void)
{
    char path[48];
    snprintf(path, sizeof(path), "/api/device/mail?after=%" PRIu32, s_after);
    if (os_api_call("GET", path, NULL, 0, page_cb, s_tok) != ESP_OK) {
        finish(false);
    }
}

bool mailbox_sync(void)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return false;
    }
    os_cloud_get_info(in);
    bool claimed = in->claimed;
    free(in);
    if (!s_on || s_busy || !claimed || !os_api_online()) {
        return false;
    }
    lock();
    fresh();
    bool ok = s_cap > 0;
    unlock();
    if (!ok) {
        return false;
    }
    s_busy = true;
    s_pages = 0;
    s_new = 0;
    s_tok = os_api_token();
    s_tok_gen = os_cloud_server_gen();
    get_page();
    return s_busy || !s_failed;
}

bool mailbox_syncing(void)
{
    return s_busy;
}

int64_t mailbox_synced_at(void)
{
    return s_synced_at;
}

bool mailbox_failed(void)
{
    return s_failed;
}

bool mailbox_running(void)
{
    return s_on;
}

void mailbox_tick(int64_t now)
{
    if (!s_on || s_busy) {
        return;
    }
    static os_cloud_info_t in;          // ~800 B: not on the OS task's stack
    os_cloud_get_info(&in);
    if (!in.claimed || !in.mail) {
        return;
    }
    bool hello = in.synced_ms && in.synced_ms != s_seen_hello;
    if (hello || (s_failed && now - s_failed_at >= RETRY_MS)) {
        s_seen_hello = in.synced_ms;
        mailbox_sync();
    }
}

// ---------------------------------------------------------------------------
// The service (core/os_service.h)

static os_service_state_t svc_state(void *ctx)
{
    return s_on ? OS_SVC_RUNNING : OS_SVC_STOPPED;
}

static void svc_status(void *ctx, char *out, size_t n)
{
    if (!s_on) {
        strlcpy(out, "Off", n);
        return;
    }
    if (!game_claimed()) {
        strlcpy(out, "Not linked yet", n);
        return;
    }
    if (s_busy) {
        strlcpy(out, "Collecting mail", n);
        return;
    }
    int u = mailbox_unread();
    if (u > 0) {
        snprintf(out, n, "%d unread", u);
    } else {
        strlcpy(out, "No new mail", n);
    }
}

static int32_t svc_memory(void *ctx)
{
    return (int32_t)(s_cap * sizeof(slot_t));
}

static bool svc_start(void *ctx, const char **why)
{
    s_on = true;
    s_seen_hello = 0;                   // the last hello's count is news to us
    return true;
}

static void svc_stop(void *ctx)
{
    s_on = false;
    if (s_busy) {
        os_api_cancel(s_tok);           // no callback of this collection runs after this
        s_busy = false;
        s_tok = NULL;
        s_new = 0;
    }
}

static int svc_unread(void *ctx)
{
    return s_on ? mailbox_unread() : 0;
}

static const os_service_def_t s_def = {
    .id = "mailbox",
    .name = "Mailbox",
    .symbol = LV_SYMBOL_ENVELOPE,
    .accent = COL_YELLOW,
    .flags = OS_SVC_STOPPABLE | OS_SVC_BOOT,
    .state = svc_state,
    .status = svc_status,
    .memory = svc_memory,
    .start = svc_start,
    .stop = svc_stop,
    .unread = svc_unread,
};

// ---------------------------------------------------------------------------
// Console: mail [list|show <id>|sync|hello|read <id>|rm <id>|clear]

static void sync_post(void *arg)
{
    printf("%s\n", mailbox_sync() ? "collecting" : "can't collect now (Mailbox off, offline, unclaimed or busy)");
}

static int cmd_mail(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    uint32_t id = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
    if (!strcmp(sub, "status")) {
        os_cloud_info_t *in = malloc(sizeof(*in));
        if (in) {
            os_cloud_get_info(in);
        }
        lock();
        fresh();
        int unread = 0;
        for (int i = 0; i < s_n; i++) {
            unread += !s_slots[i].m.h.read;
        }
        printf("mailbox: %s, %d of %d, %d unread, after %" PRIu32 ", %s%s, hello mail %d\n", s_on ? "on" : "off", s_n,
               s_cap, unread, s_after, s_card ? "card" : "RAM only", s_busy ? ", collecting" : s_failed ? ", last failed" : "",
               in ? in->mail : -1);
        unlock();
        free(in);
        return 0;
    }
    if (!strcmp(sub, "list")) {
        mailbox_item_t h;
        for (int i = 0; mailbox_item(i, &h); i++) {
            printf("%" PRIu32 " %s %s | %s | %s | %s\n", h.id, h.read ? " " : "*", h.an, h.from, h.by, h.title);
        }
        return 0;
    }
    if (!strcmp(sub, "show") && id) {
        mailbox_mail_t *m = malloc(sizeof(*m));
        if (m && mailbox_get(id, m)) {
            printf("%s (%s %s) at %" PRIu32 " exp %" PRIu32 " app %s\n%s\n%s\ndata %s\n", m->h.an, m->h.from, m->h.by,
                   m->h.at, m->h.exp, m->h.app[0] ? m->h.app : "-", m->h.title, m->text, m->data[0] ? m->data : "-");
        } else {
            printf("no mail %" PRIu32 "\n", id);
        }
        free(m);
        return 0;
    }
    if (!strcmp(sub, "sync")) {
        os_post(sync_post, NULL);
        return 0;
    }
    if (!strcmp(sub, "hello")) {
        os_cloud_refresh();             // collects the usual way when hello says mail waits
        return 0;
    }
    if (!strcmp(sub, "read") && id) {
        mailbox_mark_read(id);
        return 0;
    }
    if (!strcmp(sub, "rm") && id) {
        mailbox_delete(id);
        return 0;
    }
    if (!strcmp(sub, "clear")) {
        printf("deleted %d read\n", mailbox_delete_read());
        return 0;
    }
    printf("mail [status|list|show <id>|sync|hello|read <id>|rm <id>|clear]\n");
    return 1;
}

void mailbox_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    os_service_register(&s_def);
    os_cmd_register("mail", "mail [status|list|show <id>|sync|hello|read <id>|rm <id>|clear] - the Mailbox", cmd_mail);
}
