// Background services (Solana OS 2.3.0): docs/LUA_API.md "service" and its
// "How the badge runs it", docs/HHGTS.md "Background services".
//
// An app with the `background` permission leaves service.lua (a C app: a
// second instance of its module, started at on_service_start) running after
// it closes. Each service is an lh_t like the app's, flagged by s->svc, so the
// bindings it keeps (storage, fs, json, crypto, timers, mesh, http, rpc, the
// reads of lamps/items/profile) are the app's own code; what a service must
// not have is simply never put in its globals (svc_libs).
//
//   slots     up to LUA_SVC_MAX services at once (struct lh_svc, PSRAM). The
//             OS task starts them (service.start after the owner's yes,
//             Services, boot) and asks them to stop; only the svc task boots,
//             runs and closes their states, so a stop never waits on one.
//   svc task  one task (12 KB, priority below the app's "lua" task, made
//             with the first service and gone with the last) runs every
//             service's events, ticks (1 s) and timers round robin, without
//             the LVGL lock: a service has no UI. Each callback has
//             LUA_SVC_STEPS_MAX instructions, all of a service's callbacks
//             LUA_SVC_STEPS_MIN a minute.
//   memory    a service's lh_t, its Lua heap (≤ LUA_SVC_HEAP_MAX), its event
//             ring and its answers' copies live in PSRAM. SRAM: the task's
//             stack while any runs and transient JSON parsing; nothing stays.
//   events    a ring of LH_SVC_EVENTS per service: mesh messages (the mesh
//             task copies and queues, never runs Lua), app messages,
//             lua_host_call, wakes from server answers. Full: dropped.
//   files     the card takes 5 open files: a service closes its fs cache
//             after every round, so the svc task holds at most one.
//   owner     /sdcard/hh/services.json remembers the owner's answers
//             (known_t). Its consent dialog is os_confirm: the OS draws it
//             and answers it from its own keys; no app or script can.
//
// Locks: s_mx guards the slots' state and rings and the known table; lh_mx
// guards call records and the published sessions (lh_svc_session). Order:
// lh_mx before s_mx, never the other way round.

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "bsp.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "core/os_mem.h"
#include "core/os_mesh.h"
#include "core/os_service.h"
#include "lua_install.h"
#include "lua_priv.h"
#include "lua_wasm.h"

static const char *TAG = "svc";

#define SVC_STACK           (12 * 1024)
#define SVC_PRIO            1           // below the app's "lua" task (2), above idle
#define SVC_KNOWN_MAX       16          // apps services.json remembers
#define SVC_FILE            "services.json"
#define SVC_FILE_MAX        4096
#define SVC_BOOT_DELAY_MS   10000       // after lua_svc_init: the launcher, Wi-Fi and the server link first
#define SVC_BOOT_GAP_MS     1000        // one service a second
#define SVC_FAILS_MAX       3           // failures in a row: no more starting at boot
#define SVC_SETTLED_MS      60000       // a start counts as done once the service ran this long (or ended)
#define SVC_SRAM_START      (64 * 1024) // malloc-able SRAM a start needs (os_mem_sram_malloc_free)
#define SVC_SRAM_LOW        (40 * 1024) // ... below this the newest one stops (the app's UI floor is 32 KB)
#define SVC_PSRAM_LOW       (LUA_HOST_PSRAM_RESERVE + 256 * 1024)  // the app on screen keeps its 512 KB first
#define SVC_BATTERY_LOW     15          // %, not on USB
#define SVC_CHECK_MS        1000
#define SVC_EXPIRY_MS       60000       // quest apps' expiry, checked this often
#define SVC_EVENTS_ROUND    4           // events per service per round, then the others get theirs
#define SVC_WAIT_MAX_MS     200
#define SVC_NOTICE_MAX      80
#define SVC_WAIT_GONE_MS    2500        // pause/forget: how long a worker waits for a state to go
#define SVC_GEN_BASE        0x80000000u // service launch numbers never equal the app's

static const char *const s_consent =
    "Let %s run in the background?\nIt keeps running after you close it and after a restart, "
    "until you stop it in Services.";

// One app the owner answered for (services.json) and how its service ended.
typedef struct {
    char id[LUA_PKG_ID_MAX + 1];        // "": free
    char ver[17];
    char name[21];
    const char *symbol;
    uint32_t accent;
    bool allowed, boot;
    uint8_t fails;
    bool failed;                        // its last run ended on an error or a limit
    bool resume;                        // stopped by the system (battery, a C app): start again when it can
    bool starting;                      // a start not yet seen to finish (services.json): set before it runs,
                                        // cleared after SVC_SETTLED_MS or at its end; still set at boot =
                                        // the badge restarted mid-start (a crash): that boot skips it
    char status[OS_SERVICE_STATUS_MAX + 1];  // why it isn't running ("" = never ran)
} known_t;

enum { SV_STARTING, SV_RUNNING };

// How a stop ends (what services.json keeps).
enum {
    STOP_NONE,
    STOP_OWNER,     // Services / svc stop: allowed and boot off
    STOP_APP,       // service.stop() from either side: boot off
    STOP_FORGET,    // the app is going: forgotten
    STOP_PAUSE,     // an update swaps its files: boot unchanged, the installer resumes it
    STOP_SYSTEM,    // battery, memory, OTA, a C app: boot unchanged
    STOP_FAILED,    // an error or a limit (sv->why): fails + 1
};

struct lh_svc {
    uint8_t state;                      // SV_*
    int slot;
    char id[LUA_PKG_ID_MAX + 1];
    lh_t *s;                            // its state (PSRAM), from the boot on
    bool c;                             // a C app's service
    uint32_t req_gen;                   // the app launch whose service.start waits for the boot, 0 = none
    uint8_t stop;                       // STOP_*: asked to stop (the svc task ends it)
    char why[OS_SERVICE_STATUS_MAX + 1];    // the status it ends with
    lh_ev_t ev[LH_SVC_EVENTS];          // ring (under s_mx)
    int ev_head, ev_n;
    int64_t started, next_tick, min_start;
    uint32_t min_steps;
    char status[OS_SERVICE_STATUS_MAX + 1];  // service.status
};

static SemaphoreHandle_t s_mx, s_wake, s_save_mx;
static struct lh_svc *s_slot[LUA_SVC_MAX];     // under s_mx
static lh_t *s_pub[LUA_SVC_MAX];                // under lh_mx: states api_cb may answer
static TaskHandle_t s_task;                     // under s_mx
static known_t *s_known;                        // PSRAM, SVC_KNOWN_MAX
static bool s_loaded;
static uint32_t s_epoch;
static uint32_t s_gen;
static int s_mesh_sub = -1;                     // svc task
static esp_timer_handle_t s_check_timer, s_boot_timer;
static bool s_battery_stopped;                  // OS task
static int s_boot_next = -1;                    // OS task: the next known entry to start at boot
static int64_t s_expiry_at;

static void *ps_calloc(size_t n)
{
    return heap_caps_calloc(1, n, os_mem_psram() ? OS_MEM_PSRAM : MALLOC_CAP_DEFAULT);
}

static void wake_task(void)
{
    if (s_wake) {
        xSemaphoreGive(s_wake);
    }
}

// ---------------------------------------------------------------------------
// The known table and services.json

static known_t *known_find(const char *id)      // s_mx held
{
    for (int i = 0; s_known && i < SVC_KNOWN_MAX; i++) {
        if (s_known[i].id[0] && !strcmp(s_known[i].id, id)) {
            return &s_known[i];
        }
    }
    return NULL;
}

static known_t *known_add(const char *id)       // s_mx held
{
    known_t *k = known_find(id);
    for (int i = 0; !k && s_known && i < SVC_KNOWN_MAX; i++) {
        if (!s_known[i].id[0]) {
            k = &s_known[i];
            *k = (known_t){ 0 };
            strlcpy(k->id, id, sizeof(k->id));
            strlcpy(k->name, id, sizeof(k->name));
            k->symbol = LV_SYMBOL_LOOP;
        }
    }
    return k;
}

static void known_pkg(known_t *k, const lua_pkg_t *p)   // s_mx held
{
    strlcpy(k->name, p->name, sizeof(k->name));
    strlcpy(k->ver, p->version, sizeof(k->ver));
    k->symbol = p->symbol;
    k->accent = p->accent;
}

// Writes the table as it is now. Writers take turns, each writing the
// newest table, so the file never goes back to an older one.
static bool known_save(void)
{
    if (!os_data_ready()) {
        return false;
    }
    xSemaphoreTake(s_save_mx, portMAX_DELAY);
    cJSON *j = cJSON_CreateObject();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; j && s_known && i < SVC_KNOWN_MAX; i++) {
        const known_t *k = &s_known[i];
        cJSON *e = k->id[0] ? cJSON_AddObjectToObject(j, k->id) : NULL;
        if (e) {
            cJSON_AddBoolToObject(e, "allowed", k->allowed);
            cJSON_AddBoolToObject(e, "boot", k->boot);
            cJSON_AddNumberToObject(e, "fails", k->fails);
            if (k->starting) {
                cJSON_AddBoolToObject(e, "starting", true);
            }
            if (!k->boot && !strncmp(k->status, "Off: ", 5)) {
                cJSON_AddStringToObject(e, "why", k->status);   // why starting at boot went off, across restarts
            }
            cJSON_AddStringToObject(e, "ver", k->ver);
        }
    }
    xSemaphoreGive(s_mx);
    char *text = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    bool ok = text && os_data_write(SVC_FILE, text, strlen(text));
    if (!ok) {
        ESP_LOGW(TAG, "can't write %s", SVC_FILE);
    }
    cJSON_free(text);
    xSemaphoreGive(s_save_mx);
    return ok;
}

// Reads services.json once (OS task: a small file), and again after the card
// was wiped (a new owner, a factory reset). A missing or broken file means
// nothing is allowed. Entries whose app is gone are dropped.
static void known_load(void)
{
    uint32_t epoch = os_data_epoch();
    if (s_loaded && epoch == s_epoch) {
        return;
    }
    if (!s_known && !(s_known = ps_calloc(SVC_KNOWN_MAX * sizeof(known_t)))) {
        return;
    }
    char *text = os_data_ready() ? os_data_read(SVC_FILE, SVC_FILE_MAX, NULL) : NULL;
    cJSON *j = text ? cJSON_Parse(text) : NULL;
    free(text);
    lua_pkg_t *p = malloc(sizeof(*p));
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memset(s_known, 0, SVC_KNOWN_MAX * sizeof(known_t));
    xSemaphoreGive(s_mx);
    const cJSON *e;
    cJSON *root = cJSON_IsObject(j) ? j : NULL;
    cJSON_ArrayForEach(e, root) {
        if (!e->string || !lua_pkg_valid_id(e->string) || !cJSON_IsObject(e)) {
            continue;
        }
        bool have = p && lua_pkg_load(e->string, p) && (p->perms & LUA_PERM_BACKGROUND);
        if (!have) {
            continue;
        }
        xSemaphoreTake(s_mx, portMAX_DELAY);
        known_t *k = known_add(e->string);
        if (k) {
            k->allowed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "allowed"));
            k->boot = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "boot"));
            const cJSON *f = cJSON_GetObjectItemCaseSensitive(e, "fails");
            k->fails = cJSON_IsNumber(f) && f->valueint > 0 ? (uint8_t)(f->valueint > 9 ? 9 : f->valueint) : 0;
            k->starting = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "starting"));
            const char *why = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "why"));
            if (why && !k->boot) {
                lua_pkg_plain(k->status, sizeof(k->status), why);
            }
            known_pkg(k, p);
        }
        xSemaphoreGive(s_mx);
    }
    cJSON_Delete(j);
    free(p);
    s_epoch = epoch;
    s_loaded = true;
}

// ---------------------------------------------------------------------------
// Slots

static struct lh_svc *slot_find(const char *id)     // s_mx held
{
    for (int i = 0; i < LUA_SVC_MAX; i++) {
        if (s_slot[i] && !strcmp(s_slot[i]->id, id)) {
            return s_slot[i];
        }
    }
    return NULL;
}

static int slot_count(void)                         // s_mx held
{
    int n = 0;
    for (int i = 0; i < LUA_SVC_MAX; i++) {
        n += s_slot[i] != NULL;
    }
    return n;
}

bool lua_svc_active(const char *id)
{
    if (!s_mx || !id) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool on = slot_find(id) != NULL;
    xSemaphoreGive(s_mx);
    return on;
}

static bool running(const char *id)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    struct lh_svc *sv = slot_find(id);
    bool on = sv && sv->state == SV_RUNNING && !sv->stop;
    xSemaphoreGive(s_mx);
    return on;
}

lh_t *lh_svc_session(int i)
{
    return i >= 0 && i < LUA_SVC_MAX ? s_pub[i] : NULL;
}

bool lh_svc_wamr_busy(void)
{
    return lw_busy_svc();
}

// The ring: an event for a service (any task). Its data becomes the ring's.
static bool ring_push(struct lh_svc *sv, const lh_ev_t *ev)    // s_mx held
{
    if (sv->stop || sv->ev_n >= LH_SVC_EVENTS) {
        return false;
    }
    sv->ev[(sv->ev_head + sv->ev_n) % LH_SVC_EVENTS] = *ev;
    sv->ev_n++;
    return true;
}

static bool ring_pop(struct lh_svc *sv, lh_ev_t *ev)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool got = sv->ev_n > 0;
    if (got) {
        *ev = sv->ev[sv->ev_head];
        sv->ev_head = (sv->ev_head + 1) % LH_SVC_EVENTS;
        sv->ev_n--;
    }
    xSemaphoreGive(s_mx);
    return got;
}

bool lh_svc_post(lh_t *s, const lh_ev_t *ev)
{
    if (ev->kind == LH_EV_WAKE) {
        wake_task();                    // every round answers finished calls anyway
        return true;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool ok = s->svc && ring_push(s->svc, ev);
    xSemaphoreGive(s_mx);
    if (ok) {
        wake_task();
    }
    return ok;
}

// "<fn>\0<json>" in PSRAM, for a service's ring.
static char *call_data(const char *fn, const char *json, bool psram)
{
    size_t nf = strlen(fn), nj = json ? strlen(json) : 0;
    char *d = psram ? heap_caps_malloc(nf + 1 + nj + 1, os_mem_psram() ? OS_MEM_PSRAM : MALLOC_CAP_DEFAULT)
                    : malloc(nf + 1 + nj + 1);
    if (d) {
        memcpy(d, fn, nf + 1);
        memcpy(d + nf + 1, json ? json : "", nj + 1);
    }
    return d;
}

static bool post_to_service(const char *id, const char *fn, const char *json, uint32_t need_perms)
{
    char *data = call_data(fn, json, true);
    if (!data) {
        return false;
    }
    lh_ev_t ev = { .kind = LH_EV_CALL, .data = data };
    xSemaphoreTake(s_mx, portMAX_DELAY);
    struct lh_svc *sv = slot_find(id);
    bool ok = sv && sv->state == SV_RUNNING && sv->s && (sv->s->pkg.perms & need_perms) == need_perms
              && ring_push(sv, &ev);
    xSemaphoreGive(s_mx);
    if (ok) {
        wake_task();
    } else {
        free(data);
    }
    return ok;
}

// Calls waiting in the app's queue (under lh_mx): at most LH_SVC_EVENTS, so
// a busy service never fills the queue the app's keys come through.
static uint32_t s_app_gen;
static int s_app_waiting;

// id NULL: whichever app is on screen, if it asks for every permission in need_perms.
static bool post_to_app(const char *id, const char *fn, const char *json, uint32_t need_perms)
{
    char *data = call_data(fn, json, false);
    if (!data) {
        return false;
    }
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lh_t *a = lh_cur;
    bool ok = a && !a->failed && (!id || !strcmp(a->pkg.id, id)) && (a->pkg.perms & need_perms) == need_perms;
    if (ok && s_app_gen != a->gen) {
        s_app_gen = a->gen;
        s_app_waiting = 0;
    }
    ok = ok && s_app_waiting < LH_SVC_EVENTS;
    if (ok) {
        lh_ev_t ev = { .kind = LH_EV_CALL, .gen = a->gen, .data = data };
        ok = lh_post(&ev);
        s_app_waiting += ok;
    }
    xSemaphoreGive(lh_mx);
    if (!ok) {
        free(data);
    }
    return ok;
}

static bool fn_name_ok(const char *fn)
{
    size_t n = fn ? strlen(fn) : 0;
    if (n < 4 || n > 27 || strncmp(fn, "on_", 3)) {
        return false;
    }
    for (size_t i = 3; i < n; i++) {
        char c = fn[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

bool lua_host_call(const char *app, const char *fn, const char *json)
{
    if (!s_mx || !app || !lua_pkg_valid_id(app) || !fn_name_ok(fn) || (json && strlen(json) > LUA_SVC_MSG_MAX)) {
        return false;
    }
    bool a = post_to_service(app, fn, json, 0);
    bool b = post_to_app(app, fn, json, 0);
    return a || b;
}

bool lua_host_broadcast(const char *fn, const char *json, uint32_t need_perms)
{
    if (!s_mx || !need_perms || !fn_name_ok(fn) || (json && strlen(json) > LUA_SVC_MSG_MAX)) {
        return false;
    }
    char ids[LUA_SVC_MAX][LUA_PKG_ID_MAX + 1] = { 0 };
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < LUA_SVC_MAX; i++) {
        if (s_slot[i] && s_slot[i]->state == SV_RUNNING && s_slot[i]->s
            && (s_slot[i]->s->pkg.perms & need_perms) == need_perms) {
            strlcpy(ids[i], s_slot[i]->s->pkg.id, sizeof(ids[i]));
        }
    }
    xSemaphoreGive(s_mx);
    bool any = false;
    for (int i = 0; i < LUA_SVC_MAX; i++) {
        if (ids[i][0]) {
            any |= post_to_service(ids[i], fn, json, need_perms);
        }
    }
    any |= post_to_app(NULL, fn, json, need_perms);
    return any;
}

bool lua_svc_call(const char *app, const char *fn, const char *json, uint32_t need_perms)
{
    if (!s_mx || !app || !lua_pkg_valid_id(app) || !fn_name_ok(fn) || (json && strlen(json) > LUA_SVC_MSG_MAX)) {
        return false;
    }
    return post_to_service(app, fn, json, need_perms);
}

// ---------------------------------------------------------------------------
// Answers to the app's service.start

// An LH_EV_SVC_START for launch gen of the app (if it's still that launch).
static void reply(uint32_t gen, const char *err)
{
    if (!gen) {
        return;
    }
    char *data = strdup(err ? err : "");
    if (!data) {
        return;
    }
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    bool ok = lh_cur && lh_cur->gen == gen;
    if (ok) {
        lh_ev_t ev = { .kind = LH_EV_SVC_START, .gen = gen, .data = data };
        ok = lh_post(&ev);
    }
    xSemaphoreGive(lh_mx);
    if (!ok) {
        free(data);
    }
}

// ---------------------------------------------------------------------------
// Starting (OS task)

static void svc_task(void *arg);
static void checks_arm(void);

// Starts app <id>'s service (the owner said yes, or did before). req_gen: the
// app launch to answer when the boot is done (0 = none). NULL: on its way
// (or already running); else why not.
static bool ask_stop(const char *id, uint8_t kind, const char *why);

static const char *start(const char *id, uint32_t req_gen)
{
    // req_gen is handed to the slot below (and zeroed): whether the app on
    // screen asked is kept apart, for the record check after.
    const bool asked = req_gen != 0;
    bool fresh = false;                 // this call made the slot (not already running)
    known_load();
    lua_pkg_t *p = malloc(sizeof(*p));
    const char *why = NULL;
    if (!p) {
        why = "not enough memory";
    } else if (!os_mem_psram()) {
        why = "unavailable";                // a service's heap is PSRAM (every badge so far has it)
    } else if (!os_data_ready()) {
        why = "no card";
    } else if (lua_install_busy(id)) {
        why = "installing";
    } else if (!lua_pkg_load(id, p)) {
        why = "not installed";
    } else if (!(p->perms & LUA_PERM_BACKGROUND)) {
        why = "denied";
    } else if (lua_pkg_expired(p)) {
        why = "expired";
    } else if (os_battery_below(SVC_BATTERY_LOW)) {
        why = "low battery";
    } else if (os_mem_sram_malloc_free() < SVC_SRAM_START
               || heap_caps_get_free_size(OS_MEM_PSRAM) < SVC_PSRAM_LOW + LUA_SVC_HEAP_MAX / 4) {
        why = "low memory";
    } else if (p->lang == LUA_PKG_LANG_C && lh_svc_wamr_busy()) {
        why = "busy";                       // one WebAssembly runtime: another C service has it
    } else if (p->lang == LUA_PKG_LANG_C && lw_busy()) {
        // A C app holds the runtime (its own app asking, often): the service
        // starts as soon as that app closes (lh_svc_wamr_released).
        xSemaphoreTake(s_mx, portMAX_DELAY);
        known_t *k = known_add(id);
        if (k) {
            known_pkg(k, p);
            k->resume = true;
            k->failed = false;
            strlcpy(k->status, "Starts when the C app closes", sizeof(k->status));
        }
        xSemaphoreGive(s_mx);
        free(p);
        reply(req_gen, NULL);
        return NULL;
    }
    if (why) {
        free(p);
        return why;
    }
    bool c = p->lang == LUA_PKG_LANG_C;
    struct lh_svc *nw = ps_calloc(sizeof(*nw));
    xSemaphoreTake(s_mx, portMAX_DELAY);
    known_t *k = known_add(id);
    if (k) {
        known_pkg(k, p);
    }
    struct lh_svc *sv = slot_find(id);
    if (sv) {
        why = sv->stop ? "stopping" : NULL;     // already running (or booting)
        if (!why && sv->state == SV_STARTING && req_gen && !sv->req_gen) {
            sv->req_gen = req_gen;
            req_gen = 0;
        }
    } else if (slot_count() >= LUA_SVC_MAX) {
        why = "full";
    } else if (!nw || !k) {
        why = "not enough memory";
    } else {
        int i = 0;
        while (s_slot[i]) {
            i++;
        }
        sv = nw;
        nw = NULL;
        sv->slot = i;
        sv->state = SV_STARTING;
        sv->c = c;
        sv->req_gen = req_gen;
        req_gen = 0;
        fresh = true;
        strlcpy(sv->id, id, sizeof(sv->id));
        s_slot[i] = sv;
        k->failed = false;
        k->resume = false;
        k->status[0] = 0;
        k->starting = true;                 // on the card before it runs (known_save below)
        if (!s_task && xTaskCreate(svc_task, "svc", SVC_STACK, NULL, SVC_PRIO, &s_task) != pdPASS) {
            s_task = NULL;
            s_slot[i] = NULL;
            nw = sv;
            fresh = false;
            why = "not enough memory";
        }
    }
    xSemaphoreGive(s_mx);
    free(nw);
    free(p);
    if (!why && fresh && !known_save()) {
        // The attempt is on the card before the service's code runs: if the badge
        // restarts in the middle (a crash in its start), the next boot knows. A start
        // nobody on screen asked for (boot, a resume, the owner's Services or `svc
        // start`) that can't be recorded doesn't happen: the slot ends before its code
        // runs (the svc task never boots a stopped SV_STARTING slot), and the caller
        // gets "card error" (a resume keeps trying). The app's own service.start runs
        // it anyway, and answers true: the app is on screen, and a boot start needs
        // the record too, so a service the card couldn't record never starts at boot
        // and can't hold the badge in a restart loop. A start of a service already
        // running writes nothing and stops nothing.
        if (!asked) {
            ask_stop(id, STOP_SYSTEM, "Not started: the card can't record it");
            ESP_LOGW(TAG, "%s: not started: services.json can't be written", id);
            wake_task();
            return "card error";
        }
        ESP_LOGW(TAG, "%s: services.json can't be written: started for the app, never at boot", id);
    }
    if (!why) {
        reply(req_gen, NULL);               // it was running already
        wake_task();
        checks_arm();
        ESP_LOGI(TAG, "%s: starting", id);
    }
    return why;
}

// Asks a service to stop (any task). The svc task ends it after the callback
// it may be in; nothing here waits.
static bool ask_stop(const char *id, uint8_t kind, const char *why)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    struct lh_svc *sv = slot_find(id);
    if (sv && (!sv->stop || kind == STOP_FORGET || (kind == STOP_OWNER && sv->stop != STOP_FORGET))) {
        sv->stop = kind;
        strlcpy(sv->why, why ? why : "", sizeof(sv->why));
    }
    xSemaphoreGive(s_mx);
    if (sv) {
        wake_task();
    }
    return sv != NULL;
}

static void wait_gone(const char *id)
{
    for (int ms = 0; lua_svc_active(id) && ms < SVC_WAIT_GONE_MS; ms += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool lua_svc_pause(const char *id)
{
    if (!lua_svc_active(id)) {
        return false;
    }
    ask_stop(id, STOP_PAUSE, "Stopped: updating");
    wait_gone(id);
    return true;
}

static esp_timer_handle_t s_retry_timer;        // a service an update stopped, refused for a moment
static int s_retries;

static void resume_retry(void *arg);

static void resume_post(void *arg)
{
    char *id = arg;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    known_t *k = known_find(id);
    bool go = k && k->allowed;
    xSemaphoreGive(s_mx);
    const char *why = go ? start(id, 0) : NULL;
    if (why) {
        // Refused for now (busy, low memory, full, a card hiccup...): the request
        // is kept (resume) and tried again every 2 s for a minute, never lost.
        bool gone = !strcmp(why, "not installed") || !strcmp(why, "denied") || !strcmp(why, "expired");
        ESP_LOGW(TAG, "%s: not started after the update: %s%s", id, why, gone ? "" : " (trying again)");
        if (!gone) {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            k = known_find(id);
            if (k) {
                k->resume = true;
                strlcpy(k->status, "Starts again after the update", sizeof(k->status));
            }
            xSemaphoreGive(s_mx);
            s_retries = 0;
            if (s_retry_timer && !esp_timer_is_active(s_retry_timer)) {
                esp_timer_start_once(s_retry_timer, 2000 * 1000);
            }
        }
    }
    free(id);
}

void lua_svc_resume(const char *id, bool was_running)
{
    char *copy = was_running && id ? strdup(id) : NULL;
    if (copy && !os_post(resume_post, copy)) {
        free(copy);
    }
}

void lua_svc_forget(const char *id)
{
    if (!s_mx || !id) {
        return;
    }
    if (ask_stop(id, STOP_FORGET, "")) {
        wait_gone(id);
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    known_t *k = known_find(id);
    if (k) {
        k->id[0] = 0;
    }
    xSemaphoreGive(s_mx);
    if (k) {
        known_save();
    }
}

// ---------------------------------------------------------------------------
// The app's `service` module (main.lua)

typedef struct {
    char id[LUA_PKG_ID_MAX + 1];
    char name[21];
    uint32_t gen;
} ask_t;

static void consent_done(bool yes, void *ctx)
{
    ask_t *a = ctx;
    if (!yes) {
        reply(a->gen, "denied");
    } else {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        known_t *k = known_add(a->id);
        if (k) {
            k->allowed = true;
            k->boot = true;
            k->fails = 0;
        }
        xSemaphoreGive(s_mx);
        known_save();
        const char *why = start(a->id, a->gen);
        if (why) {
            reply(a->gen, why);
        }
    }
    free(a);
}

// OS task: the app asked. Allowed before: start; else the owner decides.
static void ask_post(void *arg)
{
    ask_t *a = arg;
    known_load();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    known_t *k = known_find(a->id);
    bool allowed = k && k->allowed;
    bool full = !slot_find(a->id) && slot_count() >= LUA_SVC_MAX;
    xSemaphoreGive(s_mx);
    if (full) {
        reply(a->gen, "full");              // no question the badge can't act on
        free(a);
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    k = known_find(a->id);
    allowed = k && k->allowed;
    if (allowed) {
        k->boot = true;
        k->fails = 0;
    }
    xSemaphoreGive(s_mx);
    if (allowed) {
        known_save();
        const char *why = start(a->id, a->gen);
        if (why) {
            reply(a->gen, why);
        }
        free(a);
        return;
    }
    if (os_confirm_open()) {
        reply(a->gen, "busy");
        free(a);
        return;
    }
    char q[200];
    snprintf(q, sizeof(q), s_consent, a->name);
    os_confirm(q, consent_done, a);         // consent_done frees it, yes or no (leaving the app is a no)
}

static int l_app_start(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_BACKGROUND, "service");
    lh_t *s = lh_get(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (s->ui_off) {
        return 0;
    }
    if (s->svc_start_ref != LUA_NOREF) {
        return luaL_error(L, "service.start: the last one is still waiting");
    }
    ask_t *a = calloc(1, sizeof(*a));
    if (!a) {
        return luaL_error(L, "not enough memory");
    }
    strlcpy(a->id, s->pkg.id, sizeof(a->id));
    lua_pkg_plain(a->name, sizeof(a->name), s->pkg.name);
    a->gen = s->gen;
    lua_pushvalue(L, 1);
    s->svc_start_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (!os_post(ask_post, a)) {
        free(a);
        luaL_unref(L, LUA_REGISTRYINDEX, s->svc_start_ref);
        s->svc_start_ref = LUA_NOREF;
        return luaL_error(L, "service.start: the badge is busy");
    }
    return 0;
}

static int l_app_stop(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_BACKGROUND, "service");
    lh_t *s = lh_get(L);
    if (ask_stop(s->pkg.id, STOP_APP, "Stopped by the app")) {
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int l_app_running(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_BACKGROUND, "service");
    lua_pushboolean(L, running(lh_get(L)->pkg.id));
    return 1;
}

// JSON of the value at 1 (≤ LUA_SVC_MSG_MAX), or NULL and the reason pushed.
static char *encode_msg(lua_State *L)
{
    luaL_checkany(L, 1);
    lh_jbuf_t b = { .max = LUA_SVC_MSG_MAX };
    if (!lh_json_encode(L, 1, &b)) {
        free(b.p);
        lua_pushboolean(L, 0);
        lua_pushstring(L, strcmp(b.err, "too big") ? b.err : "message over 1 KB");
        return NULL;
    }
    return b.p;
}

static int l_app_send(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_BACKGROUND, "service");
    lh_t *s = lh_get(L);
    char *json = encode_msg(L);
    if (!json) {
        return 2;
    }
    bool ok = post_to_service(s->pkg.id, "on_message", json, 0);
    free(json);
    lua_pushboolean(L, ok);
    return 1;
}

void lh_open_service(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"start", l_app_start}, {"stop", l_app_stop}, {"running", l_app_running}, {"send", l_app_send},
        {NULL, NULL},
    };
    luaL_newlib(L, fns);
    lua_setglobal(L, "service");
}

typedef struct {
    int ref;
    const char *err;
} started_t;

static int started_cb(lua_State *L)
{
    const started_t *a = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, a->ref);
    lua_pushboolean(L, a->err == NULL);
    if (a->err) {
        lua_pushstring(L, a->err);
    }
    lua_call(L, a->err ? 2 : 1, 0);
    return 0;
}

static int call_cb(lua_State *L)
{
    const char *fn = lua_touserdata(L, 1);
    const char *json = fn + strlen(fn) + 1;
    lh_t *s = lh_get(L);
    lua_settop(L, 0);
    if (lua_getglobal(L, fn) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        if (s->wasm) {
            if (lh_json_decode(L, json, strlen(json))) {
                lua_pushnil(L);
            }
            lw_call_named(L, s, fn);        // a C app's or service's export of that name, if any
        }
        return 0;
    }
    if (!json[0]) {
        lua_pushnil(L);
    } else if (lh_json_decode(L, json, strlen(json))) {
        return 0;
    }
    lua_call(L, 1, 0);
    return 0;
}

void lh_call_event(lh_t *s, const char *data)
{
    if (data) {
        lh_callback(s, call_cb, (void *)data);
    }
}

void lh_svc_app_event(lh_t *s, const lh_ev_t *ev)
{
    if (ev->kind == LH_EV_CALL) {
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        if (s_app_gen == s->gen && s_app_waiting > 0) {
            s_app_waiting--;
        }
        xSemaphoreGive(lh_mx);
        lh_call_event(s, ev->data);
        return;
    }
    if (s->svc_start_ref == LUA_NOREF || !s->L) {
        return;
    }
    started_t a = { .ref = s->svc_start_ref, .err = ev->data && ev->data[0] ? ev->data : NULL };
    s->svc_start_ref = LUA_NOREF;
    lh_callback(s, started_cb, &a);
    if (s->L) {
        luaL_unref(s->L, LUA_REGISTRYINDEX, a.ref);
    }
}

void lh_svc_app_closed(lh_t *s)
{
    if (s->L && s->svc_start_ref != LUA_NOREF) {
        luaL_unref(s->L, LUA_REGISTRYINDEX, s->svc_start_ref);
    }
    s->svc_start_ref = LUA_NOREF;
}

// ---------------------------------------------------------------------------
// The service's own `service` module (service.lua)

static struct lh_svc *svc_of(lua_State *L)
{
    return lh_get(L)->svc;
}

static int l_svc_send(lua_State *L)
{
    lh_t *s = lh_get(L);
    char *json = encode_msg(L);
    if (!json) {
        return 2;
    }
    bool ok = !s->ui_off && post_to_app(s->pkg.id, "on_service", json, 0);
    free(json);
    lua_pushboolean(L, ok);
    return 1;
}

// Printable ASCII, cut to n - 1.
static void plain(char *out, size_t n, const char *in, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = c >= 0x20 && c < 0x7f ? (char)c : ' ';
    }
    out[o] = 0;
}

static int l_svc_status(lua_State *L)
{
    size_t len;
    const char *line = luaL_checklstring(L, 1, &len);
    luaL_argcheck(L, len <= OS_SERVICE_STATUS_MAX, 1, "at most 48 characters");
    struct lh_svc *sv = svc_of(L);
    char t[OS_SERVICE_STATUS_MAX + 1];
    plain(t, sizeof(t), line, len);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    strlcpy(sv->status, t, sizeof(sv->status));
    xSemaphoreGive(s_mx);
    return 0;
}

static int l_svc_notify(lua_State *L)
{
    size_t len;
    const char *text = luaL_checklstring(L, 1, &len);
    luaL_argcheck(L, len > 0 && len <= SVC_NOTICE_MAX, 1, "1 to 80 characters");
    uint32_t led = 0;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        if (lua_getfield(L, 2, "led") != LUA_TNIL) {
            led = (uint32_t)luaL_checkinteger(L, -1) & 0xFFFFFF;
        }
        lua_pop(L, 1);
    }
    char t[SVC_NOTICE_MAX + 1], id[OS_SERVICE_ID_MAX + 1];
    plain(t, sizeof(t), text, len);
    snprintf(id, sizeof(id), "app:%s", lh_get(L)->pkg.id);
    lua_pushboolean(L, !lh_get(L)->ui_off && os_service_notify(id, led, "%s", t));
    return 1;
}

static int l_svc_stop(lua_State *L)
{
    ask_stop(lh_get(L)->pkg.id, STOP_APP, "Stopped by the app");
    return 0;
}

// The globals a service gets: LUA_API.md "What a service may use". Library
// tables are lazy (vendor/lauxlib.c: a function set to nil comes back), so a
// trimmed library is a new plain table of the functions it keeps.
static void keep(lua_State *L, const char *lib, const char *const *names)
{
    lua_getglobal(L, lib);
    int t = lua_gettop(L);
    lua_newtable(L);
    for (int i = 0; names[i]; i++) {
        lua_getfield(L, t, names[i]);
        lua_setfield(L, -2, names[i]);
    }
    lua_setglobal(L, lib);
    lua_pop(L, 1);
}

static void svc_libs(lua_State *L)
{
    lh_open_sys(L);                         // led timer sys badge storage wifi
    lh_open_json(L);
    lh_open_net(L);                         // lamps items profile quest mesh http rpc
    lh_open_fs(L);
    lh_open_crypto(L);
    lh_open_mail(L);                        // mail: the reads only (LUA_API.md "service" and "mail")
    lh_open_files(L);                       // files: the reads only (LUA_API.md "service" and "files")
    lh_open_gossip(L);                      // gossip: the reads only (LUA_API.md "service" and "gossip", 2.4.0)
    static const char *const sys[] = {"millis", "time", "battery", "memory", NULL};
    static const char *const lamps[] = {"balance", NULL};
    static const char *const items[] = {"list", "all", NULL};
    static const char *const profile[] = {"me", "get", "items", "pages", NULL};
    static const char *const mesh[] = {"send", "nearby", "meet", "meets", NULL};
    keep(L, "sys", sys);
    keep(L, "lamps", lamps);
    keep(L, "items", items);
    keep(L, "profile", profile);
    keep(L, "mesh", mesh);
    static const char *const mail[] = {"list", "get", NULL};
    keep(L, "mail", mail);
    static const char *const files[] = {"dir", "list", "size", "read", NULL};
    keep(L, "files", files);
    // gossip (2.4.0): the reading functions of docs/GOSSIP.md §10. Each
    // builder adds its own names on its own line, so the lists merge.
    static const char *const gossip[] = {
        // the node side (me, programs, program, genome, nearby, hearing)
        "me", "programs", "program", "genome", "nearby", "hearing",

        // The role side's reads:
        // the role side (role, boss)
        "role", "boss",
        NULL,
    };
    keep(L, "gossip", gossip);
    static const char *const none[] = {"ui", "led", "wifi", "ble", "ws", "quest", NULL};
    for (int i = 0; none[i]; i++) {
        lua_pushnil(L);
        lua_setglobal(L, none[i]);
    }
    static const luaL_Reg svc[] = {
        {"send", l_svc_send}, {"status", l_svc_status}, {"notify", l_svc_notify}, {"stop", l_svc_stop},
        {NULL, NULL},
    };
    luaL_newlib(L, svc);
    lua_setglobal(L, "service");
}

// ---------------------------------------------------------------------------
// The svc task: booting, callbacks, failures, closing

typedef struct {
    FILE *f;
    char buf[LH_READ_CHUNK];
} reader_t;

static const char *read_chunk(lua_State *L, void *ud, size_t *size)
{
    reader_t *r = ud;
    *size = fread(r->buf, 1, sizeof(r->buf), r->f);
    return *size ? r->buf : NULL;
}

// Protected: the code (service.lua compiled from the card, or a C app's
// module), then the libraries and storage. service.lua always compiles
// here: store code runs in place from one flash slot, which the app's own
// launch would take away.
static int svc_boot_lua(lua_State *L)
{
    lh_t *s = lh_get(L);
    if (s->pkg.lang == LUA_PKG_LANG_C) {
        lw_boot(L, s);                      // pushes the chunk that instantiates it (on_service_start)
    } else {
        long size;
        reader_t *rd = lua_newuserdatauv(L, sizeof(reader_t), 0);
        rd->f = lh_fs_fopen(s, "service.lua", &size);
        if (!rd->f) {
            return luaL_error(L, "service.lua is missing");
        }
        int rc = lua_load(L, read_chunk, rd, "=service.lua", "t");
        fclose(rd->f);
        rd->f = NULL;
        if (rc != LUA_OK) {
            return lua_error(L);
        }
        lua_remove(L, -2);
    }
    lua_setfield(L, LUA_REGISTRYINDEX, "solana.main");
    lua_gc(L, LUA_GCCOLLECT);
    lh_open_base(L);
    svc_libs(L);
    lh_storage_load(s);
    lua_gc(L, LUA_GCCOLLECT);
    lua_gc(L, LUA_GCGEN);
    lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMUL, 10);
    return 0;
}

static int run_main(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "solana.main");
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "solana.main");
    lua_call(L, 0, 0);
    return 0;
}

static int run_on_stop(lua_State *L)
{
    lua_getglobal(L, "on_stop");
    if (lua_isfunction(L, -1)) {
        lua_call(L, 0, 0);
    } else if (lh_get(L)->wasm) {
        lua_settop(L, 0);
        lw_call_named(L, lh_get(L), "on_stop");
    }
    return 0;
}

static int tick_cb(lua_State *L)
{
    lua_getglobal(L, "on_tick");
    if (lua_isfunction(L, -1)) {
        lua_pushinteger(L, (lua_Integer)(intptr_t)lua_touserdata(L, 1));
        lua_call(L, 1, 0);
    }
    return 0;
}

static void one_line(char *t)
{
    for (; *t; t++) {
        if ((unsigned char)*t < 0x20 || *t == 0x7f) {
            *t = ' ';
        }
    }
}

// Ends the state: on_stop (its own budget, nothing new may start), storage,
// lua_close. The slot stays until svc_end.
static void close_state(lh_t *s, bool on_stop)
{
    if (s->L) {
        s->ui_off = true;
        if (on_stop) {
            lh_callback(s, run_on_stop, NULL);
        }
        lh_storage_flush(s, true);
        lh_storage_close(s);
        s->steps = 0;
        lua_close(s->L);
        s->L = NULL;
    }
    lh_fs_close(s);
    lh_files_close(s);
    lw_close(s);
}

// Stops the service where it is: kind STOP_FAILED or STOP_SYSTEM, status why.
static void end_with(lh_t *s, uint8_t kind, const char *why, bool on_stop)
{
    struct lh_svc *sv = s->svc;
    char line[160];
    strlcpy(line, why, sizeof(line));
    one_line(line);
    printf("[%s] %s\n", s->tag, line);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (sv->stop != STOP_FORGET && sv->stop != STOP_OWNER) {
        sv->stop = kind;
        strlcpy(sv->why, why, sizeof(sv->why));
    }
    xSemaphoreGive(s_mx);
    s->failed = true;
    strlcpy(s->err, why, sizeof(s->err));
    if (s->L) {
        lua_settop(s->L, 0);
    }
    close_state(s, on_stop);
}

// The callback failed with rc and its error on the stack (or msg_in).
static void fail(lh_t *s, int rc, const char *msg_in)
{
    lua_State *L = s->L;
    char msg[OS_SERVICE_STATUS_MAX + 1 + 100];
    if (s->mem_fatal) {
        rc = LUA_ERRMEM;
    }
    if (rc != LUA_ERRMEM && s->mem_hit && lua_type(L, -1) == LUA_TSTRING
            && !strcmp(lua_tostring(L, -1), "not enough memory")) {
        rc = LUA_ERRMEM;
    }
    if (rc == LUA_ERRMEM && !s->mem_hit) {
        // Not its cap: the badge's PSRAM reserve. Not its fault either.
        end_with(s, STOP_SYSTEM, "Stopped: the badge was low on memory", false);
        return;
    }
    if (rc == LUA_ERRMEM) {
        snprintf(msg, sizeof(msg), "Failed: The service needs more than %d MB",
                 (int)(s->heap_max / (1024 * 1024)));
    } else {
        if (s->killed && s->kill_msg[0]) {
            msg_in = s->kill_msg;
        }
        snprintf(msg, sizeof(msg), "Failed: %s", msg_in ? msg_in : lua_type(L, -1) == LUA_TSTRING
                 ? lua_tostring(L, -1) : "(error is not a string)");
    }
    bool budget = s->killed || rc == LUA_ERRMEM;
    s->killed = false;
    s->mem_fatal = false;
    end_with(s, STOP_FAILED, msg, !budget);
}

bool lh_svc_callback(lh_t *s, lua_CFunction fn, void *arg)
{
    lua_State *L = s->L;
    struct lh_svc *sv = s->svc;
    if (!L) {
        return false;
    }
    s->steps = 0;
    s->wasm_cpu = 0;
    s->cb_t0 = lh_now();
    s->kill_msg[0] = 0;
    s->cb_seq++;
    lua_pushcfunction(L, fn);
    lua_pushlightuserdata(L, arg);
    int rc = lua_pcall(L, 1, 0, 0);
    if (s->steps > s->steps_peak) {
        s->steps_peak = s->steps;
    }
    if (!s->ui_off) {
        int64_t now = lh_now();
        if (now - sv->min_start >= 60000) {
            sv->min_start = now;
            sv->min_steps = 0;
        }
        sv->min_steps += s->steps + s->wasm_cpu;
    }
    if (rc == LUA_OK && s->killed) {
        fail(s, LUA_ERRRUN, s->kill_msg);
        return false;
    }
    if (rc != LUA_OK) {
        if (s->ui_off) {
            // on_stop or a finalizer failed: just log it.
            char line[160];
            strlcpy(line, lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "error", sizeof(line));
            one_line(line);
            printf("[%s] %s\n", s->tag, line);
            lua_settop(L, 0);
            s->mem_fatal = false;
            s->killed = false;
            return false;
        }
        fail(s, rc, NULL);
        return false;
    }
    if (!s->ui_off && sv->min_steps > LUA_SVC_STEPS_MIN) {
        end_with(s, STOP_FAILED, "Failed: Stopped: used too much of the processor", false);
        return false;
    }
    return true;
}

static void mesh_sync(void);

static void boot(struct lh_svc *sv)
{
    lh_t *s = heap_caps_calloc(1, sizeof(*s), os_mem_psram() ? OS_MEM_PSRAM : MALLOC_CAP_DEFAULT);
    const char *why = NULL;
    if (!s) {
        why = "Stopped: the badge was low on memory";
    } else if (!lua_pkg_load(sv->id, &s->pkg) || !(s->pkg.perms & LUA_PERM_BACKGROUND)) {
        why = "Failed: the app isn't installed";
    }
    if (why) {
        free(s);
        xSemaphoreTake(s_mx, portMAX_DELAY);
        sv->stop = STOP_FAILED;
        strlcpy(sv->why, why, sizeof(sv->why));
        xSemaphoreGive(s_mx);
        return;
    }
    os_cloud_info_t in;
    os_cloud_get_info(&in);
    s->svc = sv;
    s->gen = SVC_GEN_BASE | ++s_gen;
    s->t0 = lh_now();
    s->psram = true;
    s->heap_max = LUA_SVC_HEAP_MAX;
    s->steps_max = LUA_SVC_STEPS_MAX;
    s->heap_before = heap_caps_get_free_size(OS_MEM_SRAM);
    s->modal_ref = s->store_ref = s->scan_ref = s->svc_start_ref = LUA_NOREF;
    lh_ble_init(s);
    snprintf(s->tag, sizeof(s->tag), "%s:svc", sv->id);
    strlcpy(s->mesh_me, in.id, sizeof(s->mesh_me));
    s->L = lh_new_state(s);
    sv->s = s;
    sv->started = sv->min_start = lh_now();
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    s_pub[sv->slot] = s;                    // from here on api_cb finds its calls
    xSemaphoreGive(lh_mx);
    if (!s->L) {
        s->failed = true;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        sv->stop = STOP_SYSTEM;
        strlcpy(sv->why, "Stopped: the badge was low on memory", sizeof(sv->why));
        xSemaphoreGive(s_mx);
        return;
    }
    lua_pushcfunction(s->L, svc_boot_lua);
    int rc = lua_pcall(s->L, 0, 0, 0);
    ESP_LOGI(TAG, "%s: %s (%u bytes of heap)", s->tag, rc == LUA_OK ? "booted" : "didn't boot", (unsigned)s->mem);
    if (rc != LUA_OK) {
        fail(s, rc, NULL);
        return;
    }
    s->next_tick = lh_now() + LH_SVC_TICK_MS;
    if (lh_callback(s, run_main, NULL)) {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        sv->state = SV_RUNNING;
        uint32_t gen = sv->req_gen;
        sv->req_gen = 0;
        known_t *k = known_find(sv->id);
        if (k) {
            k->ver[0] = 0;
            strlcpy(k->ver, s->pkg.version, sizeof(k->ver));
        }
        xSemaphoreGive(s_mx);
        reply(gen, NULL);
        mesh_sync();
    }
}

// One round of a running service: up to SVC_EVENTS_ROUND events, answers,
// meets, the tick and timers. Its fs cache closes after: the card's five
// open files are shared with the app and the installer.
static void round_of(struct lh_svc *sv)
{
    lh_t *s = sv->s;
    lh_ev_t ev;
    for (int n = 0; n < SVC_EVENTS_ROUND && s->L && !s->failed && ring_pop(sv, &ev); n++) {
        if (ev.kind == LH_EV_MESH) {
            lh_mesh_event(s, ev.data);
        } else if (ev.kind == LH_EV_MEET) {
            lh_meet_event(s, ev.data);
        } else if (ev.kind == LH_EV_CALL) {
            lh_call_event(s, ev.data);
        }
        free(ev.data);
    }
    if (s->L && !s->failed) {
        lh_calls_run(s);
    }
    if (s->L && !s->failed) {
        lh_meet_run(s);
    }
    int64_t now = lh_now();
    if (s->L && !s->failed && now >= s->next_tick) {
        int64_t ms = now - s->next_tick + LH_SVC_TICK_MS;
        s->next_tick = now + LH_SVC_TICK_MS;
        lh_callback(s, tick_cb, (void *)(intptr_t)ms);
    }
    if (s->L && !s->failed) {
        lh_timers_run(s);
    }
    if (s->L) {
        lh_storage_flush(s, false);
    }
    lh_fs_close(s);
    lh_files_close(s);                      // the files module's cached one too: the card takes 5
}

static int64_t next_due(struct lh_svc *sv)
{
    lh_t *s = sv->s;
    int64_t wake = s->next_tick, t = lh_timers_next(s);
    if (t < wake) {
        wake = t;
    }
    if (s->store_dirty && s->store_saved + LH_STORAGE_SAVE_MS < wake) {
        wake = s->store_saved + LH_STORAGE_SAVE_MS;
    }
    if (s->meet) {
        int64_t m = lh_now() + 500;         // its repeats (MEET_RESEND_MS) and timeouts
        wake = m < wake ? m : wake;
    }
    return wake;
}

// The state is closed or never booted: free it, record the outcome, free the slot.
static void svc_end(struct lh_svc *sv)
{
    lh_t *s = sv->s;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint8_t kind = sv->stop ? sv->stop : STOP_FAILED;
    char why[sizeof(sv->why)];
    strlcpy(why, sv->why, sizeof(why));
    xSemaphoreGive(s_mx);
    if (s) {
        if (s->L) {
            close_state(s, true);
        }
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        s_pub[sv->slot] = NULL;             // api_cb can't find its calls any more
        for (lh_call_t *c = s->calls; c; c = c->next) {
            if (!c->done && c->state == LH_CALL_SENT) {
                os_api_cancel(c->ctx);      // its own os_api_token(): no answer reaches anyone
            }
        }
        xSemaphoreGive(lh_mx);
        lh_calls_free(s);
    }
    bool c = sv->c;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    uint32_t gen = sv->req_gen;
    known_t *k = known_find(sv->id);
    if (k) {
        k->starting = false;                // it ended: the badge didn't restart under it
        switch (kind) {
        case STOP_OWNER:
            k->allowed = false;
            k->boot = false;
            strlcpy(k->status, "Stopped", sizeof(k->status));
            break;
        case STOP_APP:
            k->boot = false;
            strlcpy(k->status, why[0] ? why : "Stopped", sizeof(k->status));
            break;
        case STOP_FAILED:
            k->failed = true;
            if (++k->fails >= SVC_FAILS_MAX) {
                k->boot = false;
            }
            strlcpy(k->status, why, sizeof(k->status));
            break;
        case STOP_SYSTEM:
        case STOP_PAUSE:
            strlcpy(k->status, why, sizeof(k->status));
            break;
        default:
            break;
        }
        if (kind == STOP_OWNER || kind == STOP_APP) {
            k->fails = 0;
        }
    }
    s_slot[sv->slot] = NULL;
    xSemaphoreGive(s_mx);
    if (gen) {
        // service.start still waits: it didn't come up.
        reply(gen, kind == STOP_FAILED && why[0] ? why + (strncmp(why, "Failed: ", 8) ? 0 : 8) : "stopped");
    }
    if (kind != STOP_FORGET && kind != STOP_PAUSE) {
        known_save();
    }
    ESP_LOGI(TAG, "%s: ended (%s)%s", sv->id, why[0] ? why : "stopped", c ? ", C" : "");
    if (s) {
        free(s);
    }
    free(sv);
    mesh_sync();
}

// ---------------------------------------------------------------------------
// The mesh: one subscription for every service that has the permission

static void svc_mesh(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx)
{
    cJSON *m = cJSON_Parse(json);
    const cJSON *app = cJSON_GetObjectItemCaseSensitive(m, "app");
    const cJSON *to = cJSON_GetObjectItemCaseSensitive(m, "to");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(m, "d");
    const cJSON *meet = cJSON_GetObjectItemCaseSensitive(m, "m");
    if (!cJSON_IsString(app) || !cJSON_IsString(to) || (!cJSON_IsObject(d) && !cJSON_IsObject(meet))) {
        cJSON_Delete(m);
        return;
    }
    // Copied: the parsed message goes before the event is queued.
    char id[LUA_PKG_ID_MAX + 1];
    strlcpy(id, app->valuestring, sizeof(id));
    // A meet goes to one side: the app when it's on screen with the mesh,
    // else its service (two answers would sign two statements).
    bool app_has_it = lh_mesh_app_is(id);
    char *text = NULL;
    lh_ev_t ev = { 0 };
    char me[sizeof(((lh_t *)0)->mesh_me)] = "";
    xSemaphoreTake(s_mx, portMAX_DELAY);
    struct lh_svc *sv = slot_find(id);
    lh_t *s = sv && sv->state == SV_RUNNING && !sv->stop ? sv->s : NULL;
    bool ok = s && (s->pkg.perms & LUA_PERM_MESH) && sv->ev_n < LH_SVC_EVENTS;
    if (ok) {
        // Copied under the lock: svc_end may free the state once it's given back. `s`
        // below is only compared with the slot's state again, never read.
        strlcpy(me, s->mesh_me, sizeof(me));
    }
    xSemaphoreGive(s_mx);
    if (ok) {
        bool to_me = me[0] && !strcmp(to->valuestring, me);
        bool mine = to_me || !strcmp(to->valuestring, "*") || !me[0];
        bool is_meet = !d && to_me && cJSON_IsObject(meet) && !app_has_it;
        if ((mine && cJSON_IsObject(d)) || is_meet) {
            text = cJSON_PrintUnformatted(is_meet ? meet : d);
            ev.kind = is_meet ? LH_EV_MEET : LH_EV_MESH;
        }
    }
    cJSON_Delete(m);
    if (!text) {
        return;
    }
    size_t idn = strlen(from->id), n = strlen(text);
    char *data = heap_caps_malloc(idn + 1 + n + 1, os_mem_psram() ? OS_MEM_PSRAM : MALLOC_CAP_DEFAULT);
    if (data) {
        memcpy(data, from->id, idn + 1);
        memcpy(data + idn + 1, text, n + 1);
        ev.data = data;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        sv = slot_find(id);
        // The slot may have changed meanwhile: only the same state's ring.
        bool pushed = sv && sv->s == s && ring_push(sv, &ev);
        xSemaphoreGive(s_mx);
        if (pushed) {
            wake_task();
        } else {
            free(data);
        }
    }
    cJSON_free(text);
}

// svc task: subscribed while any running service has the mesh permission.
static void mesh_sync(void)
{
    bool want = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < LUA_SVC_MAX; i++) {
        want = want || (s_slot[i] && s_slot[i]->s && (s_slot[i]->s->pkg.perms & LUA_PERM_MESH));
    }
    xSemaphoreGive(s_mx);
    if (want && s_mesh_sub < 0) {
        s_mesh_sub = os_mesh_subscribe(OS_MESH_APP, svc_mesh, NULL);
    } else if (!want && s_mesh_sub >= 0) {
        os_mesh_unsubscribe(s_mesh_sub);    // waits for a handler running now (it takes s_mx: not held here)
        s_mesh_sub = -1;
    }
}

// ---------------------------------------------------------------------------
// The task

static void svc_task(void *arg)
{
    volatile char top;
    lh_stack_enter(1, (uintptr_t)&top - SVC_STACK);
    int64_t blocked = lh_now();
    for (;;) {
        int64_t wake = lh_now() + SVC_WAIT_MAX_MS;
        for (int i = 0; i < LUA_SVC_MAX; i++) {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            struct lh_svc *sv = s_slot[i];
            uint8_t state = sv ? sv->state : 0;
            bool stop = sv && sv->stop;
            xSemaphoreGive(s_mx);
            if (!sv) {
                continue;
            }
            if (state == SV_STARTING && !stop) {
                boot(sv);
            } else if (state == SV_RUNNING && !stop) {
                round_of(sv);
            }
            xSemaphoreTake(s_mx, portMAX_DELAY);
            stop = sv->stop || !sv->s || !sv->s->L;
            xSemaphoreGive(s_mx);
            if (stop) {
                svc_end(sv);
            } else if (sv->state == SV_RUNNING) {
                int64_t t = next_due(sv);
                wake = t < wake ? t : wake;
            }
        }
        xSemaphoreTake(s_mx, portMAX_DELAY);
        bool any = slot_count() > 0;
        if (!any) {
            s_task = NULL;                  // a start from now on makes a new task
        }
        xSemaphoreGive(s_mx);
        if (!any) {
            break;
        }
        // Never busy for long without letting the idle task in (the task
        // watchdog), however much the services have to do.
        int64_t now = lh_now(), ms = wake - now;
        if (ms <= 0 && now - blocked < 1000) {
            continue;
        }
        TickType_t ticks = ms <= 0 ? 1 : (ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS;
        blocked = now;
        xSemaphoreTake(s_wake, ticks);
    }
    mesh_sync();
    lh_stack_leave(1);
    ESP_LOGI(TAG, "no services: the svc task ends");
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Checks (OS task, every second while services run or wait for USB):
// battery, memory, a wiped card, quest apps' expiry

static void stop_all(uint8_t kind, const char *why, bool c_only)
{
    char ids[LUA_SVC_MAX][LUA_PKG_ID_MAX + 1] = { 0 };
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < LUA_SVC_MAX; i++) {
        if (s_slot[i] && (!c_only || s_slot[i]->c)) {
            strlcpy(ids[i], s_slot[i]->id, sizeof(ids[i]));
            known_t *k = known_find(s_slot[i]->id);
            if (k && kind == STOP_SYSTEM) {
                k->resume = true;
            }
        }
    }
    xSemaphoreGive(s_mx);
    for (int i = 0; i < LUA_SVC_MAX; i++) {
        if (ids[i][0]) {
            ask_stop(ids[i], kind, why);
        }
    }
}

// Starts the services the system stopped (battery back on USB, a C app gone).
static void resume_all(bool c_only);

static void resume_retry_post(void *arg)
{
    resume_all(false);
    bool left = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; s_known && i < SVC_KNOWN_MAX; i++) {
        left = left || (s_known[i].id[0] && s_known[i].resume && s_known[i].allowed && s_known[i].boot);
    }
    xSemaphoreGive(s_mx);
    if (left && ++s_retries < 30 && s_retry_timer) {
        esp_timer_start_once(s_retry_timer, 2000 * 1000);
    }
}

static void resume_retry(void *arg)
{
    if (!os_post(resume_retry_post, NULL) && s_retry_timer) {
        esp_timer_start_once(s_retry_timer, 500 * 1000);
    }
}

static void resume_all(bool c_only)
{
    char ids[SVC_KNOWN_MAX][LUA_PKG_ID_MAX + 1];
    int n = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; s_known && i < SVC_KNOWN_MAX; i++) {
        known_t *k = &s_known[i];
        if (k->id[0] && k->resume && k->allowed && k->boot) {
            strlcpy(ids[n++], k->id, sizeof(ids[0]));
        }
    }
    xSemaphoreGive(s_mx);
    for (int i = 0; i < n; i++) {
        lua_pkg_t *p = malloc(sizeof(*p));
        bool c = p && lua_pkg_load(ids[i], p) && p->lang == LUA_PKG_LANG_C;
        free(p);
        if (c_only && !c) {
            continue;
        }
        const char *why = start(ids[i], 0);
        ESP_LOGI(TAG, "%s: %s%s", ids[i], why ? "not resumed: " : "resumed", why ? why : "");
        if (!why) {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            known_t *k = known_find(ids[i]);
            if (k) {
                k->resume = false;
            }
            xSemaphoreGive(s_mx);
        }
    }
}

static void checks(void *arg)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int n = slot_count();
    // The newest running one, for a low-memory stop.
    struct lh_svc *newest = NULL;
    for (int i = 0; i < LUA_SVC_MAX; i++) {
        struct lh_svc *sv = s_slot[i];
        if (sv && !sv->stop && (!newest || sv->started > newest->started)) {
            newest = sv;
        }
    }
    char newest_id[LUA_PKG_ID_MAX + 1] = "";
    if (newest) {
        strlcpy(newest_id, newest->id, sizeof(newest_id));
    }
    // A service that has run SVC_SETTLED_MS got through its start.
    bool settled = false;
    for (int i = 0; i < LUA_SVC_MAX; i++) {
        struct lh_svc *sv = s_slot[i];
        known_t *k = sv && sv->state == SV_RUNNING && lh_now() - sv->started >= SVC_SETTLED_MS ? known_find(sv->id) : NULL;
        if (k && k->starting) {
            k->starting = false;
            settled = true;
        }
    }
    xSemaphoreGive(s_mx);
    if (settled) {
        known_save();
    }

    bool usb = os_battery_usb();
    if (n && os_battery_below(SVC_BATTERY_LOW)) {
        stop_all(STOP_SYSTEM, "Stopped: battery low", false);
        s_battery_stopped = true;
    } else if (s_battery_stopped && usb) {
        s_battery_stopped = false;
        resume_all(false);
    }
    if (s_loaded && os_data_epoch() != s_epoch) {
        // The card was wiped (a new owner): its services and answers are gone.
        stop_all(STOP_FORGET, "", false);
        s_loaded = false;
        known_load();
    }
    if (newest_id[0] && (heap_caps_get_free_size(OS_MEM_PSRAM) < SVC_PSRAM_LOW
                         || os_mem_sram_malloc_free() < SVC_SRAM_LOW)) {
        ask_stop(newest_id, STOP_SYSTEM, "Stopped: the badge was low on memory");
        ESP_LOGW(TAG, "%s: stopped, low memory (PSRAM %u, SRAM %u)", newest_id,
                 (unsigned)heap_caps_get_free_size(OS_MEM_PSRAM),
                 (unsigned)os_mem_sram_malloc_free());
    }
    int64_t now = lh_now();
    if (n && now - s_expiry_at >= SVC_EXPIRY_MS) {
        s_expiry_at = now;
        char ids[LUA_SVC_MAX][LUA_PKG_ID_MAX + 1] = { 0 };
        xSemaphoreTake(s_mx, portMAX_DELAY);
        for (int i = 0; i < LUA_SVC_MAX; i++) {
            if (s_slot[i] && s_slot[i]->s && s_slot[i]->s->pkg.temp) {
                strlcpy(ids[i], s_slot[i]->id, sizeof(ids[i]));
            }
        }
        xSemaphoreGive(s_mx);
        lua_pkg_t *p = malloc(sizeof(*p));
        for (int i = 0; p && i < LUA_SVC_MAX; i++) {
            // app.json again: the library moves a quest app's expiry forward.
            if (ids[i][0] && lua_pkg_load(ids[i], p) && lua_pkg_expired(p)) {
                ask_stop(ids[i], STOP_SYSTEM, "Stopped: the quest app expired");
                xSemaphoreTake(s_mx, portMAX_DELAY);
                known_t *k = known_find(ids[i]);
                if (k) {
                    k->allowed = k->boot = false;
                }
                xSemaphoreGive(s_mx);
                known_save();
            }
        }
        free(p);
    }
    if (!n && !s_battery_stopped && s_boot_next < 0) {
        esp_timer_stop(s_check_timer);
    }
}

static void check_timer(void *arg)
{
    os_post(checks, NULL);
}

static void checks_arm(void)
{
    if (s_check_timer && !esp_timer_is_active(s_check_timer)) {
        esp_timer_start_periodic(s_check_timer, SVC_CHECK_MS * 1000);
    }
}

// ---------------------------------------------------------------------------
// Boot: the services that were running start ~10 s in, one a second

static void boot_next(void *arg)
{
    known_load();
    while (s_boot_next >= 0 && s_boot_next < SVC_KNOWN_MAX) {
        int i = s_boot_next++;
        char id[LUA_PKG_ID_MAX + 1] = "";
        xSemaphoreTake(s_mx, portMAX_DELAY);
        known_t *k = s_known ? &s_known[i] : NULL;
        bool crashed = false;
        if (k && k->starting && esp_reset_reason() == ESP_RST_SW) {
            // An orderly restart (reboot, an update, a factory reset: esp_restart)
            // within its first minute isn't one it caused.
            k->starting = false;
        }
        if (k && k->id[0] && k->allowed && k->boot) {
            if (k->starting) {
                // Its last start never finished: the badge restarted under it (a crash in
                // its start, or the power). Not now; N in a row turn starting at boot off,
                // so a service that crashes the badge can't keep it in a restart loop.
                crashed = true;
                k->starting = false;
                if (++k->fails >= SVC_FAILS_MAX) {
                    k->boot = false;
                    strlcpy(k->status, "Off: it kept restarting the badge", sizeof(k->status));
                } else {
                    strlcpy(k->status, "Skipped: the badge restarted as it started", sizeof(k->status));
                }
            } else {
                strlcpy(id, k->id, sizeof(id));
            }
        }
        xSemaphoreGive(s_mx);
        if (crashed) {
            known_save();
            ESP_LOGW(TAG, "boot: %s skipped: the badge restarted during its last start", k->id);
            continue;
        }
        if (!id[0]) {
            continue;
        }
        const char *why = start(id, 0);
        ESP_LOGI(TAG, "boot: %s %s%s", id, why ? "didn't start: " : "started", why ? why : "");
        if (why && (!strcmp(why, "low battery") || !strcmp(why, "busy"))) {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            k = known_find(id);
            if (k) {
                k->resume = true;           // on USB, or when the C app closes
                s_battery_stopped = s_battery_stopped || !strcmp(why, "low battery");
            }
            xSemaphoreGive(s_mx);
        }
        esp_timer_start_once(s_boot_timer, SVC_BOOT_GAP_MS * 1000);
        return;
    }
    s_boot_next = -1;
    checks_arm();
}

static void boot_timer(void *arg)
{
    if (s_boot_next < 0) {
        s_boot_next = 0;
    }
    if (!os_post(boot_next, NULL)) {
        esp_timer_start_once(s_boot_timer, 500 * 1000);
    }
}

// ---------------------------------------------------------------------------
// C services and the one WebAssembly runtime

static void yield_post(void *arg)
{
    stop_all(STOP_SYSTEM, "Paused: a C app is open", true);
}

void lh_svc_wamr_yield(void)
{
    if (lh_svc_wamr_busy()) {
        os_post(yield_post, NULL);
    }
}

static void released_post(void *arg)
{
    resume_all(true);
}

void lh_svc_wamr_released(void)
{
    os_post(released_post, NULL);
}

// ---------------------------------------------------------------------------
// The provider: app services in Services and `svc` (core/os_service.h)

static int p_count(void)
{
    known_load();
    int n = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; s_known && i < SVC_KNOWN_MAX; i++) {
        n += s_known[i].id[0] != 0;
    }
    xSemaphoreGive(s_mx);
    return n;
}

static bool p_get(int idx, os_service_view_t *out)
{
    memset(out, 0, sizeof(*out));
    bool found = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0, n = 0; s_known && i < SVC_KNOWN_MAX; i++) {
        known_t *k = &s_known[i];
        if (!k->id[0] || n++ != idx) {
            continue;
        }
        found = true;
        snprintf(out->id, sizeof(out->id), "app:%s", k->id);
        strlcpy(out->app, k->id, sizeof(out->app));
        strlcpy(out->name, k->name, sizeof(out->name));
        out->symbol = k->symbol ? k->symbol : LV_SYMBOL_LOOP;
        out->accent = k->accent ? k->accent : COL_PURPLE;
        out->kind = OS_SVC_APP;
        out->stoppable = true;
        out->boot = k->allowed && k->boot;
        out->memory = -1;
        struct lh_svc *sv = slot_find(k->id);
        if (sv) {
            out->state = sv->stop ? OS_SVC_STOPPING : OS_SVC_RUNNING;
            strlcpy(out->status, sv->stop ? "Stopping" : sv->status[0] ? sv->status
                    : sv->state == SV_STARTING ? "Starting" : "Running", sizeof(out->status));
            out->memory = sv->s ? (int32_t)sv->s->mem : -1;
        } else {
            out->state = k->failed ? OS_SVC_FAILED : OS_SVC_STOPPED;
            strlcpy(out->status, k->status[0] ? k->status : k->allowed ? "Not running" : "Off: the app asks again",
                    sizeof(out->status));
        }
        break;
    }
    xSemaphoreGive(s_mx);
    return found;
}

static const char *app_of(const char *id)
{
    return id && !strncmp(id, "app:", 4) ? id + 4 : NULL;
}

static bool p_start(const char *sid, const char **why)
{
    const char *id = app_of(sid);
    if (!id) {
        *why = "Not found";
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    known_t *k = known_find(id);
    if (k) {
        k->allowed = true;                  // the owner pressed it: that is the yes
        k->boot = true;
        k->fails = 0;
    }
    xSemaphoreGive(s_mx);
    if (!k) {
        *why = "Not found";
        return false;
    }
    known_save();
    const char *err = start(id, 0);
    static const struct { const char *code, *text; } texts[] = {
        {"full", "3 services are running: stop one first"}, {"low battery", "The battery is low"},
        {"low memory", "The badge is low on memory"}, {"busy", "A C app is using the runtime: close it first"},
        {"not installed", "The app isn't installed"}, {"installing", "The app is installing"},
        {"expired", "The app has expired"}, {"no card", "Insert a microSD card"},
        {"card error", "The card can't record it"},
    };
    for (size_t i = 0; err && i < sizeof(texts) / sizeof(texts[0]); i++) {
        if (!strcmp(err, texts[i].code)) {
            err = texts[i].text;
        }
    }
    *why = err ? err : "";
    return err == NULL;
}

static void p_stop(const char *sid)
{
    const char *id = app_of(sid);
    if (!id) {
        return;
    }
    if (!ask_stop(id, STOP_OWNER, "Stopped")) {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        known_t *k = known_find(id);
        if (k) {
            k->allowed = k->boot = false;
            k->failed = false;
            strlcpy(k->status, "Stopped", sizeof(k->status));
        }
        xSemaphoreGive(s_mx);
        known_save();
    }
}

static bool p_set_boot(const char *sid, bool on)
{
    const char *id = app_of(sid);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    known_t *k = id ? known_find(id) : NULL;
    bool ok = k && (k->allowed || !on);
    if (ok) {
        k->boot = on;
        if (on) {
            k->fails = 0;
        }
    }
    xSemaphoreGive(s_mx);
    if (ok) {
        known_save();
    }
    return ok;
}

static void p_stop_all(const char *why)
{
    stop_all(STOP_SYSTEM, why ? why : "Stopped", false);
}

static const os_service_provider_t s_provider = {
    .count = p_count, .get = p_get, .start = p_start, .stop = p_stop, .set_boot = p_set_boot,
    .stop_all = p_stop_all,
};

void lua_svc_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_save_mx = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    os_service_set_provider(&s_provider);
    const esp_timer_create_args_t ca = { .callback = check_timer, .name = "svc_check" };
    esp_timer_create(&ca, &s_check_timer);
    const esp_timer_create_args_t ra = { .callback = resume_retry, .name = "svc_retry" };
    if (esp_timer_create(&ra, &s_retry_timer) != ESP_OK) {
        s_retry_timer = NULL;
    }
    const esp_timer_create_args_t ba = { .callback = boot_timer, .name = "svc_appboot" };
    if (esp_timer_create(&ba, &s_boot_timer) == ESP_OK) {
        esp_timer_start_once(s_boot_timer, SVC_BOOT_DELAY_MS * 1000);
    }
}
