// led, timer, sys, badge, storage and wifi for Lua apps (docs/LUA_API.md).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_keys.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "core/os_wifi.h"
#include "core/os_mem.h"
#include "esp_heap_caps.h"
#include "lua_priv.h"

// ---------------------------------------------------------------------------
// led: a shadow copy, shown (scaled to OS_LED_MAX) after each callback

static int channel(lua_State *L, int idx)
{
    lua_Integer v = luaL_checkinteger(L, idx);
    return v < 0 ? 0 : v > 255 ? 255 : (int)v;
}

static lh_t *led_session(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_LED, "led");
    return lh_get(L);
}

static int led_index(lua_State *L)
{
    lua_Integer i = luaL_checkinteger(L, 1);
    luaL_argcheck(L, i >= 1 && i <= LH_LED_COUNT, 1, "LEDs are 1..8");
    return (int)i - 1;
}

static int l_led_set(lua_State *L)
{
    lh_t *s = led_session(L);
    int i = led_index(L);
    s->led[i] = channel(L, 2) << 16 | channel(L, 3) << 8 | channel(L, 4);
    s->led_dirty = true;
    return 0;
}

static int l_led_hex(lua_State *L)
{
    lh_t *s = led_session(L);
    int i = led_index(L);
    s->led[i] = (uint32_t)luaL_checkinteger(L, 2) & 0xFFFFFF;
    s->led_dirty = true;
    return 0;
}

static void led_fill(lh_t *s, uint32_t rgb)
{
    for (int i = 0; i < LH_LED_COUNT; i++) {
        s->led[i] = rgb;
    }
    s->led_dirty = true;
}

static int l_led_fill(lua_State *L)
{
    lh_t *s = led_session(L);
    led_fill(s, channel(L, 1) << 16 | channel(L, 2) << 8 | channel(L, 3));
    return 0;
}

static int l_led_off(lua_State *L)
{
    led_fill(led_session(L), 0);
    return 0;
}

void lh_led_flush(lh_t *s)
{
    if (!s->led_dirty) {
        return;
    }
    for (int i = 0; i < LH_LED_COUNT; i++) {
        uint32_t c = s->led[i];
        bsp_rgb_set(i, (c >> 16 & 0xFF) * OS_LED_MAX / 255, (c >> 8 & 0xFF) * OS_LED_MAX / 255,
                    (c & 0xFF) * OS_LED_MAX / 255);
    }
    bsp_rgb_show();
    s->led_dirty = false;
    s->led_used = true;
}

void lh_led_off(void)
{
    bsp_rgb_fill(0, 0, 0);
    bsp_rgb_show();
}

// ---------------------------------------------------------------------------
// timer

static int add_timer(lua_State *L, bool every)
{
    lh_t *s = lh_get(L);
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (s->ui_off) {
        return 0;       // closing: nothing will fire
    }
    // A service (2.3.0): 4 timers, none faster than a second.
    int max = s->svc ? LH_SVC_TIMER_MAX : LH_TIMER_MAX;
    lua_Integer min = s->svc ? LH_SVC_TIMER_MIN_MS : LH_TIMER_MIN_MS;
    if (ms < min) {
        ms = min;
    }
    for (int i = 0; i < max; i++) {
        lh_timer_t *t = &s->timers[i];
        if (t->id) {
            continue;
        }
        lua_pushvalue(L, 2);
        t->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        t->id = ++s->timer_seq;
        t->period = every ? (uint32_t)ms : 0;
        t->due = lh_now() + ms;
        lua_pushinteger(L, t->id);
        return 1;
    }
    return luaL_error(L, "too many timers (%d)", max);
}

static int l_after(lua_State *L)
{
    return add_timer(L, false);
}

static int l_every(lua_State *L)
{
    return add_timer(L, true);
}

static int l_cancel(lua_State *L)
{
    lh_t *s = lh_get(L);
    lua_Integer id = luaL_checkinteger(L, 1);
    for (int i = 0; id && i < LH_TIMER_MAX; i++) {
        lh_timer_t *t = &s->timers[i];
        if (t->id == id) {
            luaL_unref(L, LUA_REGISTRYINDEX, t->fn_ref);
            t->id = 0;
        }
    }
    return 0;
}

int64_t lh_timers_next(lh_t *s)
{
    int64_t next = INT64_MAX;
    for (int i = 0; i < LH_TIMER_MAX; i++) {
        if (s->timers[i].id && s->timers[i].due < next) {
            next = s->timers[i].due;
        }
    }
    return next;
}

typedef struct {
    int ref;
    bool once;
} fire_t;

static int timer_cb(lua_State *L)
{
    const fire_t *f = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, f->ref);
    if (f->once) {
        luaL_unref(L, LUA_REGISTRYINDEX, f->ref);
    }
    lua_call(L, 0, 0);
    return 0;
}

void lh_timers_run(lh_t *s)
{
    // Bounded, so a timer slower than its period can't loop forever.
    for (int n = 0; n < LH_TIMER_MAX && s->L && !s->failed && !s->exit_req; n++) {
        int64_t now = lh_now();
        lh_timer_t *due = NULL;
        for (int i = 0; i < LH_TIMER_MAX; i++) {
            lh_timer_t *t = &s->timers[i];
            if (t->id && t->due <= now && (!due || t->due < due->due)) {
                due = t;
            }
        }
        if (!due) {
            return;
        }
        fire_t f = { .ref = due->fn_ref, .once = !due->period };
        if (due->period) {
            due->due += due->period;
            if (due->due <= now) {
                due->due = now + due->period;
            }
        } else {
            due->id = 0;
        }
        lh_callback(s, timer_cb, &f);
    }
}

// ---------------------------------------------------------------------------
// sys, badge

static int l_millis(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(lh_now() - lh_get(L)->t0));
    return 1;
}

// sys.tick(ms): on_tick's period from now on, LH_TICK_MIN_MS..1000 (2.3.0);
// returns the one before. Periods off the RTOS's 10 ms tick are woken by a
// timer (lua_host.c), so a game asking 16 ms gets 16, not 20.
static int l_tick(lua_State *L)
{
    lh_t *s = lh_get(L);
    int before = s->tick_ms;
    if (!lua_isnoneornil(L, 1)) {
        lua_Integer ms = luaL_checkinteger(L, 1);
        luaL_argcheck(L, ms >= LH_TICK_MIN_MS && ms <= 1000, 1, "16 to 1000 ms");
        s->tick_ms = (int)ms;
        s->tick_set = true;
        if (s->next_tick > lh_now() + ms) {
            s->next_tick = lh_now() + ms;
        }
    }
    lua_pushinteger(L, before);
    return 1;
}

static int l_time(lua_State *L)
{
    int64_t ms = os_cloud_time_ms();
    if (ms) {
        lua_pushinteger(L, (lua_Integer)(ms / 1000));
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_battery(lua_State *L)
{
    int mv = bsp_battery_mv();
    if (mv < 0 || mv > 4300) {      // unknown, or on USB power
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, bsp_battery_percent(mv));
    }
    return 1;
}

// The app's Lua heap: what it uses and its cap on this badge (2.0.0), so an app
// can size its caches for a badge without PSRAM, or one not updated yet (whose
// sys has no memory: 96 KB).
static int l_memory(lua_State *L)
{
    lh_t *s = lh_get(L);
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, (lua_Integer)s->mem);
    lua_setfield(L, -2, "used");
    lua_pushinteger(L, (lua_Integer)s->heap_max);
    lua_setfield(L, -2, "max");
    return 1;
}

// The keys held right now, as a set: { up = true, left = true }. Raw buttons through
// the calibrated map; empty while a dialog has the keys: the app's own ui.confirm or
// ui.input, or the OS's (a confirm, the bridge panel).
static int l_keys(lua_State *L)
{
    static const char *const names[] = {"up", "down", "left", "right", "select", "back"};
    lh_t *s = lh_get(L);
    uint32_t pressed = bsp_buttons_get();
    lua_createtable(L, 0, 2);
    if (s->modal || s->kb || os_confirm_open() || os_overlay()) {
        return 1;
    }
    for (int k = 0; k < (int)(sizeof(names) / sizeof(names[0])); k++) {
        if (pressed & os_key_button(k)) {
            lua_pushboolean(L, 1);
            lua_setfield(L, -2, names[k]);
        }
    }
    return 1;
}

static int l_exit(lua_State *L)
{
    lh_get(L)->exit_req = true;
    return 0;
}

static int l_badge_id(lua_State *L)
{
    os_cloud_info_t in;
    os_cloud_get_info(&in);
    if (in.id[0]) {
        lua_pushstring(L, in.id);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_badge_owner(lua_State *L)
{
    os_cloud_info_t in;
    os_cloud_get_info(&in);
    if (in.state == OS_CLOUD_CLAIMED && in.owner[0]) {
        lua_pushstring(L, in.owner);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_badge_online(lua_State *L)
{
    lua_pushboolean(L, os_api_online());
    return 1;
}

// ---------------------------------------------------------------------------
// storage: data.json, kept as a table per state; set() re-encodes it (≤ 16 KB)
// into the app's shared copy, which data.json gets at most once a second and
// when the state closes. The app and its background service (2.3.0) share
// that copy: a state whose table is older than the copy (the other one set
// something) builds its table again from it at its next get or set, so each
// sees the other's writes at once, and data.json always gets the newest.
// The copy lives in PSRAM when there is some: a service keeps nothing in SRAM.

#define STORE_SHARED    (1 + LUA_SVC_MAX)

typedef struct {
    char id[LUA_PKG_ID_MAX + 1];        // "": free
    char *json;                         // NULL: empty (no data.json)
    size_t len;
    uint32_t ver;                       // + 1 per committed set()
    uint32_t saved;                     // the version data.json has
    int users;                          // states holding it (lh_t.store_held)
    bool loaded;
    bool writing;                       // a flush is writing data.json (one at a time)
} store_t;

// THE RULES of the shared copy (every function below follows them):
//  - The entry (store_t) exists while at least one state holds it. A state
//    takes its one reference in lh_storage_load() (store_held), before
//    anything there can fail, and gives back exactly that one in
//    lh_storage_close(); a state that never took one (an error before the
//    load) releases nothing. close_state() (lua_host.c, lua_svc.c) runs
//    lh_storage_flush(force) and then lh_storage_close() whatever way the
//    state ends.
//  - Every field is read and changed only under lh_mx, which is never held
//    across a card access, a delay or a Lua call.
//  - data.json is read only while the entry isn't loaded yet (no state has a
//    copy, so nobody writes it then); of two first holders loading at once,
//    the first to finish installs its copy and the other drops its own.
//  - set() replaces json only if ver still equals the version its table was
//    built from (else it rebuilds and retries), and bumps ver: a commit.
//  - The card gets data.json from lh_storage_flush() only, one writer at a
//    time (writing: set and cleared by the same flush, both under lh_mx); the
//    copy written is the newest at that moment, whoever committed it, and
//    saved moves to its version only when the write succeeded.
//  - A periodic flush that finds the other state writing leaves the change
//    pending (store_dirty) for its next one. A state's last flush (force)
//    does not return before every version committed when it began (all of
//    them once it is the only holder) is on the card: it waits for a write
//    in flight, then writes the newest copy. Versions the other state
//    commits later are that state's to save (its own last flush waits the
//    same way), so a last flush writes at most once more and never chases.
//  - Lost, only when the card fails: a write that fails is not retried by a
//    last flush, and a wait longer than STORE_FINAL_MS (a stalled card) stops
//    waiting; whatever is unsaved when the last holder closes is then logged
//    and freed with the entry.

static store_t s_store[STORE_SHARED];   // under lh_mx

static char *store_dup(const char *p, size_t len)
{
    char *d = heap_caps_malloc(len + 1, os_mem_psram() ? OS_MEM_PSRAM : MALLOC_CAP_DEFAULT);
    if (d) {
        memcpy(d, p, len);
        d[len] = 0;
    }
    return d;
}

static store_t *store_find(const char *id)  // lh_mx held
{
    for (int i = 0; i < STORE_SHARED; i++) {
        if (s_store[i].id[0] && !strcmp(s_store[i].id, id)) {
            return &s_store[i];
        }
    }
    return NULL;
}

// The shared copy, duplicated into *out (NULL: empty), and its version.
// false when the duplicate couldn't be made: never read that as empty, or
// the next set() would write an empty table over the app's data.
static bool store_copy(const char *id, char **out, size_t *len, uint32_t *ver)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    store_t *st = store_find(id);
    bool have = st && st->json;
    char *d = have ? store_dup(st->json, st->len) : NULL;
    *len = d ? st->len : 0;
    *ver = st ? st->ver : 0;
    xSemaphoreGive(lh_mx);
    *out = d;
    return !have || d;
}

// Pushes the table json decodes to, or a new one (missing, broken, not a table).
static void store_push(lua_State *L, const char *json, size_t len)
{
    int top = lua_gettop(L);
    if (!json || lh_json_decode(L, json, len) || !lua_istable(L, -1)) {
        lua_settop(L, top);
        lua_newtable(L);
    }
}

static int store_push_p(lua_State *L)
{
    const char *json = lua_touserdata(L, 1);
    size_t len = (size_t)lua_tointeger(L, 2);
    lua_settop(L, 0);
    store_push(L, json, len);
    return 1;
}

// store_push, then frees json (not on the Lua heap) whatever happens: building
// the table can raise "not enough memory", which is raised again after the free.
static void store_push_free(lua_State *L, char *json, size_t len)
{
    if (!lua_checkstack(L, 3)) {
        free(json);
        luaL_error(L, "storage: not enough memory");
    }
    lua_pushcfunction(L, store_push_p);     // a light C function, a pointer and a number: no allocation
    lua_pushlightuserdata(L, json);
    lua_pushinteger(L, (lua_Integer)len);
    int r = lua_pcall(L, 2, 1, 0);
    free(json);
    if (r != LUA_OK) {
        lua_error(L);
    }
}

// The other side set something since this table was built: build it again.
static void store_refresh(lua_State *L, lh_t *s)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    store_t *st = store_find(s->pkg.id);
    bool stale = st && st->ver != s->store_ver;
    xSemaphoreGive(lh_mx);
    if (!stale) {
        return;
    }
    size_t len;
    uint32_t ver;
    char *json;
    if (!store_copy(s->pkg.id, &json, &len, &ver)) {
        // The old table and its version stay: nothing is lost, the call fails.
        luaL_error(L, "storage: not enough memory");
    }
    store_push_free(L, json, len);
    luaL_unref(L, LUA_REGISTRYINDEX, s->store_ref);
    s->store_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s->store_ver = ver;
}

static int store_table(lua_State *L, lh_t *s)
{
    store_refresh(L, s);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->store_ref);
    return lua_gettop(L);
}

static int l_get(lua_State *L)
{
    lh_t *s = lh_get(L);
    luaL_checkstring(L, 1);
    int t = store_table(L, s);
    lua_pushvalue(L, 1);
    lua_rawget(L, t);
    return 1;
}

static int l_set(lua_State *L)
{
    lh_t *s = lh_get(L);
    luaL_checkstring(L, 1);
    int type = lua_type(L, 2);
    luaL_argcheck(L, type == LUA_TNIL || type == LUA_TNONE || type == LUA_TNUMBER || type == LUA_TSTRING
                  || type == LUA_TBOOLEAN || type == LUA_TTABLE, 2, "storage takes numbers, strings, booleans and tables");
    lua_settop(L, 2);
    // The app and its service each change their own table and replace the
    // shared copy with it. The replacement happens only if nobody replaced
    // the copy since this table was built from it (the version still
    // matches); else the table is built again from the newer copy, this key
    // set on it, and the whole is tried again. So two writers of different
    // keys both land, and of one key the last one to commit wins.
    for (int tries = 0;; tries++) {
        int t = store_table(L, s);
        lua_pushvalue(L, 1);
        lua_rawget(L, t);               // old value, kept to undo
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 2);
        lua_rawset(L, t);
        lh_jbuf_t b = { .max = LH_STORAGE_MAX };
        char *copy = NULL;
        bool ok = lh_json_encode(L, t, &b);
        if (ok && !(copy = store_dup(b.p, b.len))) {
            b.err = "not enough memory";
            ok = false;
        }
        free(b.p);
        if (!ok) {
            lua_pushvalue(L, 1);
            lua_pushvalue(L, -2);
            lua_rawset(L, t);
            return luaL_error(L, "storage: %s", !strcmp(b.err, "too big") ? "full (16 KB per app)" : b.err);
        }
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        store_t *st = s->store_held ? store_find(s->pkg.id) : NULL;
        bool current = !st || st->ver == s->store_ver;
        if (st && current) {
            free(st->json);
            st->json = copy;
            st->len = b.len;
            s->store_ver = ++st->ver;
            copy = NULL;
        }
        xSemaphoreGive(lh_mx);
        free(copy);
        if (!st) {
            // No shared copy to keep it in (never the case while this state
            // holds one): an error, never a set() that silently isn't kept.
            lua_settop(L, 2);
            return luaL_error(L, "storage: unavailable");
        }
        if (current) {
            break;
        }
        // The other side committed first: our table is stale (its next
        // store_table() rebuilds it from the newer copy, without this key).
        lua_settop(L, 2);
        if (tries >= 16) {
            return luaL_error(L, "storage: busy, try again");
        }
    }
    s->store_dirty = true;
    return 0;
}

static int l_remove(lua_State *L)
{
    lua_settop(L, 1);
    lua_pushnil(L);
    return l_set(L);
}

bool lh_storage_load(lh_t *s)
{
    lua_State *L = s->L;
    const char *id = s->pkg.id;
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    store_t *st = s->store_held ? NULL : store_find(id);
    for (int i = 0; !s->store_held && !st && i < STORE_SHARED; i++) {
        if (!s_store[i].id[0]) {
            st = &s_store[i];
            *st = (store_t){ 0 };
            strlcpy(st->id, id, sizeof(st->id));
        }
    }
    if (st) {
        st->users++;                    // released once, by lh_storage_close()
        s->store_held = true;
    }
    bool need = st && !st->loaded;
    xSemaphoreGive(lh_mx);
    if (!st) {
        return luaL_error(L, "storage: unavailable");
    }
    if (need) {
        // The first state of this app reads the card (outside the lock: the
        // mesh task takes it too); a second one finds the copy.
        char rel[64];
        lua_pkg_rel(rel, sizeof(rel), id, "data.json");
        size_t len = 0;
        char *raw = os_data_read(rel, LH_STORAGE_MAX, &len);
        char *json = raw ? store_dup(raw, len) : NULL;
        free(raw);
        if (!json && os_data_exists(rel)) {
            // data.json is there but couldn't be read (memory, the card): the app
            // doesn't start rather than start empty and write that over its data.
            // The reference stays held: close_state() releases it, once.
            return luaL_error(L, "storage: can't read data.json (not enough memory or a card error)");
        }
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        if (!st->loaded) {
            st->json = json;
            st->len = json ? len : 0;
            st->loaded = true;
            json = NULL;
        }
        xSemaphoreGive(lh_mx);
        free(json);
    }
    size_t len;
    uint32_t ver;
    char *json;
    if (!store_copy(id, &json, &len, &ver)) {
        return luaL_error(L, "storage: not enough memory");
    }
    store_push_free(L, json, len);
    s->store_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s->store_ver = ver;
    return true;
}

// A last flush's wait for the other state's write in flight is bounded: the
// app's error path runs it with the LVGL lock held, and the OS task waits
// LH_EXIT_WAIT_MS (2 s) for an app to close. A data.json write takes tens of
// ms (16 KB at most), so only a stalled card reaches it.
#define STORE_FINAL_MS      2000
#define STORE_FINAL_POLL_MS 20

void lh_storage_flush(lh_t *s, bool force)
{
    if (!s->store_held || (!force && (!s->store_dirty || lh_now() - s->store_saved < LH_STORAGE_SAVE_MS))) {
        return;
    }
    char rel[64];
    lua_pkg_rel(rel, sizeof(rel), s->pkg.id, "data.json");
    int64_t t0 = lh_now();
    bool pending = false;
    bool have_target = false;
    uint32_t target = 0;                // a last flush: the version that must reach the card
    for (;;) {
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        store_t *st = store_find(s->pkg.id);
        if (st && !have_target) {
            target = st->ver;           // what was committed when this flush began
            have_target = true;
        }
        if (st && st->users <= 1) {
            target = st->ver;           // the only holder: nobody else can commit any more
        }
        pending = st && st->loaded && st->ver != st->saved;
        bool needed = pending && (!force || (int32_t)(target - st->saved) > 0);
        bool busy = needed && st->writing;
        char *json = NULL;
        size_t len = 0;
        uint32_t ver = 0;
        bool ok = true;
        if (needed && !busy) {
            ver = st->ver;
            len = st->len;
            json = st->json ? store_dup(st->json, st->len) : NULL;
            ok = !st->json || json;     // a copy that can't be made stays pending
            st->writing = ok;
        }
        xSemaphoreGive(lh_mx);
        if (!needed || !ok) {
            break;                      // saved (or not needed by this one), or no memory for the copy
        }
        if (busy) {
            // The other state is writing. A periodic flush leaves it pending (its
            // next one tries again); a last one waits for that write to end.
            if (!force || lh_now() - t0 >= STORE_FINAL_MS) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(STORE_FINAL_POLL_MS));
            continue;
        }
        // An empty copy (no data.json and nothing set since) has nothing to write.
        if (json && !os_data_write(rel, json, len)) {
            printf("[lua:%s] storage: write failed\n", s->tag[0] ? s->tag : s->pkg.id);
            ok = false;
        }
        free(json);
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        st = store_find(s->pkg.id);     // still ours: this state holds a reference
        if (st) {
            st->writing = false;
            if (ok && (int32_t)(ver - st->saved) > 0) {
                st->saved = ver;
            }
            pending = st->ver != st->saved;
        }
        xSemaphoreGive(lh_mx);
        // The copy written was the newest, so it covers the target: done, unless
        // the write failed (not retried here: the card is failing).
        break;
    }
    s->store_dirty = pending;
    s->store_saved = lh_now();
}

void lh_storage_close(lh_t *s)
{
    if (!s->store_held) {
        return;                         // never took a reference: nothing to release
    }
    s->store_held = false;
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    store_t *st = store_find(s->pkg.id);
    bool lost = false;
    if (st && --st->users <= 0) {
        lost = st->loaded && st->ver != st->saved;  // the last flush failed
        free(st->json);
        *st = (store_t){ 0 };
    }
    xSemaphoreGive(lh_mx);
    if (lost) {
        printf("[lua:%s] storage: data.json couldn't be written; the last changes are lost\n",
               s->tag[0] ? s->tag : s->pkg.id);
    }
}

// ---------------------------------------------------------------------------
// wifi

static int l_wifi_status(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_WIFI, "wifi");
    const char *st = "off";
    switch (os_wifi_state()) {
    case OS_WIFI_CONNECTED:  st = "on"; break;
    case OS_WIFI_CONNECTING: st = "connecting"; break;
    case OS_WIFI_FAILED:     st = "failed"; break;
    default: break;
    }
    lua_pushstring(L, st);
    return 1;
}

static int l_wifi_ssid(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_WIFI, "wifi");
    char ssid[33];
    if (os_wifi_saved_ssid(ssid, sizeof(ssid)) && ssid[0]) {
        lua_pushstring(L, ssid);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_wifi_on(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_WIFI, "wifi");
    os_wifi_set_enabled(true);
    return 0;
}

static int l_wifi_off(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_WIFI, "wifi");
    os_wifi_set_enabled(false);
    return 0;
}

// ---------------------------------------------------------------------------

void lh_open_sys(lua_State *L)
{
    static const luaL_Reg led[] = {
        {"set", l_led_set}, {"hex", l_led_hex}, {"fill", l_led_fill}, {"off", l_led_off}, {NULL, NULL},
    };
    static const luaL_Reg timer[] = {{"after", l_after}, {"every", l_every}, {"cancel", l_cancel}, {NULL, NULL}};
    static const luaL_Reg sys[] = {
        {"millis", l_millis}, {"time", l_time}, {"battery", l_battery}, {"keys", l_keys}, {"memory", l_memory},
        {"exit", l_exit}, {"tick", l_tick}, {NULL, NULL},
    };
    static const luaL_Reg badge[] = {
        {"id", l_badge_id}, {"owner", l_badge_owner}, {"online", l_badge_online}, {NULL, NULL},
    };
    static const luaL_Reg storage[] = {{"get", l_get}, {"set", l_set}, {"remove", l_remove}, {NULL, NULL}};
    static const luaL_Reg wifi[] = {
        {"status", l_wifi_status}, {"ssid", l_wifi_ssid}, {"on", l_wifi_on}, {"off", l_wifi_off}, {NULL, NULL},
    };
    luaL_newlib(L, led);
    lua_pushinteger(L, LH_LED_COUNT);
    lua_setfield(L, -2, "count");
    lua_setglobal(L, "led");
    luaL_newlib(L, timer);
    lua_setglobal(L, "timer");
    luaL_newlib(L, sys);
    lua_setglobal(L, "sys");
    luaL_newlib(L, badge);
    lua_setglobal(L, "badge");
    luaL_newlib(L, storage);
    lua_setglobal(L, "storage");
    luaL_newlib(L, wifi);
    lua_setglobal(L, "wifi");
}
