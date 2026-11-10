// ble for Lua apps (docs/LUA_API.md "ble"): Bluetooth LE through os_ble.
//
// The app owns os_ble from ble.on() until ble.off() or its exit, under a ctx
// that is never a pointer (os_api_token). os_ble's events wait there until
// the Lua task takes them, between callbacks, at most LH_BLE_PER_ROUND a
// round so keys and timers still get their turn; wake() only says there are
// some, once until they are taken. Everything a peer sends reaches the Lua
// heap only as an argument of a callback the app gave, so a device that
// floods the badge costs the app instruction budget, never memory it didn't
// ask to keep.
//
// One link at a time, either way round. A link object carries the id it was
// made with; only the live one's methods work, so a stale object can't touch
// a newer link. Callbacks owed without an os_ble event (a second connect's
// "busy", ble.off's "off" and its lost answers) go out from a short list on
// the next round, never inside the call that caused them.

#include <stdlib.h>
#include <string.h>
#include "core/os_api.h"
#include "core/os_ble.h"
#include "lua_priv.h"

#define BLE_META        "ble.link"
#define UUID_TEXT_MAX   36

typedef struct {
    uint32_t id;
} ble_handle_t;

// ---------------------------------------------------------------------------
// UUIDs and addresses

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10
         : -1;
}

static bool uuid_parse(const char *p, size_t n, os_ble_uuid_t *u)
{
    if (n != 4 && n != UUID_TEXT_MAX) {
        return false;
    }
    u->len = n == 4 ? 2 : 16;
    int o = 0;
    for (size_t i = 0; i < n;) {
        if (n == UUID_TEXT_MAX && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (p[i++] != '-') {
                return false;
            }
            continue;
        }
        int hi = hexval(p[i]), lo = i + 1 < n ? hexval(p[i + 1]) : -1;
        if (hi < 0 || lo < 0 || o >= u->len) {
            return false;
        }
        u->b[o++] = (uint8_t)(hi << 4 | lo);
        i += 2;
    }
    return o == u->len;
}

// Lower case, the way the app may have written it: "ffe1" or the 8-4-4-4-12 form.
static void uuid_fmt(const os_ble_uuid_t *u, char out[UUID_TEXT_MAX + 1])
{
    static const char hex[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < u->len && i < 16; i++) {
        if (u->len == 16 && (i == 4 || i == 6 || i == 8 || i == 10)) {
            out[o++] = '-';
        }
        out[o++] = hex[u->b[i] >> 4];
        out[o++] = hex[u->b[i] & 15];
    }
    out[o] = 0;
}

static bool uuid_eq(const os_ble_uuid_t *a, const os_ble_uuid_t *b)
{
    return a->len == b->len && !memcmp(a->b, b->b, a->len);
}

static void check_uuid(lua_State *L, int idx, os_ble_uuid_t *u)
{
    size_t n = 0;
    const char *p = lua_type(L, idx) == LUA_TSTRING ? lua_tolstring(L, idx, &n) : NULL;
    if (!p || !uuid_parse(p, n, u)) {
        luaL_argerror(L, idx, "a UUID: \"180f\" or \"6e400001-b5a3-f393-e0a9-e50e24dcca9e\"");
    }
}

// The field `name` of the table at idx as a UUID; false when it's nil.
static bool field_uuid(lua_State *L, int idx, const char *name, os_ble_uuid_t *u)
{
    lua_getfield(L, idx, name);
    bool has = !lua_isnil(L, -1);
    if (has) {
        size_t n = 0;
        const char *p = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &n) : NULL;
        if (!p || !uuid_parse(p, n, u)) {
            luaL_error(L, "ble: %s is a UUID: \"180f\" or \"6e400001-b5a3-f393-e0a9-e50e24dcca9e\"", name);
        }
    }
    lua_pop(L, 1);
    return has;
}

static void addr_fmt(const os_ble_addr_t *a, char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a->val[0], a->val[1], a->val[2], a->val[3], a->val[4],
             a->val[5]);
}

static bool addr_parse(const char *p, size_t n, os_ble_addr_t *a)
{
    if (n != 17) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        int hi = hexval(p[i * 3]), lo = hexval(p[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && p[i * 3 + 2] != ':')) {
            return false;
        }
        a->val[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

// A value: a string of at most OS_BLE_DATA_MAX bytes.
static const char *check_data(lua_State *L, int idx, size_t *n)
{
    const char *p = lua_type(L, idx) == LUA_TSTRING ? lua_tolstring(L, idx, n) : NULL;
    luaL_argcheck(L, p, idx, "a string of bytes");
    luaL_argcheck(L, *n <= OS_BLE_DATA_MAX, idx, "at most 244 bytes");
    return p;
}

static bool printable(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] < 0x20 || p[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

static int nil_why(lua_State *L, const char *why)
{
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}

static int ok_true(lua_State *L)
{
    lua_pushboolean(L, 1);
    return 1;
}

// ---------------------------------------------------------------------------
// Callbacks

// os_ble: an event waits (any task).
static void ble_wake(void *ctx)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lh_t *s = lh_cur;
    if (s && s->ble.ctx == ctx && !s->ble.woken) {
        lh_ev_t ev = { .kind = LH_EV_WAKE, .gen = s->gen };
        s->ble.woken = lh_post(&ev);     // a lost wake is picked up within LH_MAX_WAIT_MS anyway
    }
    xSemaphoreGive(lh_mx);
}

// Up to three arguments after the callback's reference.
typedef struct {
    int ref;
    int n;
    struct {
        char type;                      // 's' string, 'b' boolean
        const char *p;
        size_t len;
        bool b;
    } a[3];
} ble_cb_t;

static int call_cb(lua_State *L)
{
    const ble_cb_t *c = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref);
    for (int i = 0; i < c->n; i++) {
        if (c->a[i].type == 's') {
            lua_pushlstring(L, c->a[i].p ? c->a[i].p : "", c->a[i].len);
        } else {
            lua_pushboolean(L, c->a[i].b);
        }
    }
    lua_call(L, c->n, 0);
    return 0;
}

static void arg_s(ble_cb_t *c, const char *p, size_t len)
{
    c->a[c->n].type = 's';
    c->a[c->n].p = p;
    c->a[c->n].len = len;
    c->n++;
}

static void arg_str(ble_cb_t *c, const char *p)
{
    arg_s(c, p, strlen(p));
}

static void arg_b(ble_cb_t *c, bool b)
{
    c->a[c->n].type = 'b';
    c->a[c->n].b = b;
    c->n++;
}

static void run(lh_t *s, ble_cb_t *c)
{
    if (c->ref != LUA_NOREF && c->ref != LUA_REFNIL && s->L && !s->failed) {
        lh_callback(s, call_cb, c);
    }
}

static void unref(lh_t *s, int *ref)
{
    if (s->L && *ref != LUA_NOREF) {
        luaL_unref(s->L, LUA_REGISTRYINDEX, *ref);
    }
    *ref = LUA_NOREF;
}

// A callback for the next round; takes the reference. false: the list is full
// (the reference is dropped).
static bool owe(lh_t *s, int ref, uint8_t kind, const char *reason)
{
    if (ref == LUA_NOREF) {
        return true;
    }
    if (s->ble.nlate >= LH_BLE_LATE) {
        unref(s, &ref);
        return false;
    }
    s->ble.late[s->ble.nlate].ref = ref;
    s->ble.late[s->ble.nlate].kind = kind;
    s->ble.late[s->ble.nlate].reason = reason;
    s->ble.nlate++;
    lh_ev_t ev = { .kind = LH_EV_WAKE, .gen = s->gen };
    lh_post(&ev);
    return true;
}

// The scan report as the dev table.
static int call_scan(lua_State *L)
{
    const os_ble_ev_t *ev = lua_touserdata(L, 1);
    lh_t *s = lh_get(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->ble.scan_ref);
    lua_createtable(L, 0, 8);
    char text[UUID_TEXT_MAX + 1];
    addr_fmt(&ev->addr, text);
    lua_pushstring(L, text);
    lua_setfield(L, -2, "addr");
    lua_pushinteger(L, ev->rssi);
    lua_setfield(L, -2, "rssi");
    char name[OS_BLE_NAME_MAX + 1];
    strlcpy(name, ev->name, sizeof(name));
    for (char *p = name; *p; p++) {
        if (*p < 0x20 || *p > 0x7E) {
            *p = '?';
        }
    }
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    int n = ev->n_services < OS_BLE_SERVICES_MAX ? ev->n_services : OS_BLE_SERVICES_MAX;
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        uuid_fmt(&ev->services[i], text);
        lua_pushstring(L, text);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "services");
    if (ev->company) {
        lua_pushinteger(L, ev->company);
        lua_setfield(L, -2, "company");
    }
    lua_pushlstring(L, ev->data ? (const char *)ev->data : "", ev->data ? ev->len : 0);
    lua_setfield(L, -2, "data");
    lua_pushboolean(L, ev->connectable);
    lua_setfield(L, -2, "connectable");
    lua_pushboolean(L, ev->addr.random);
    lua_setfield(L, -2, "random");
    lua_call(L, 1, 0);
    return 0;
}

// A report queued before ble.scan replaced the scan still has to pass the new one's filters.
static bool scan_wants(const lh_t *s, const os_ble_ev_t *ev)
{
    if (s->ble.scan_by_service) {
        bool found = false;
        for (int i = 0; i < ev->n_services && i < OS_BLE_SERVICES_MAX && !found; i++) {
            found = uuid_eq(&ev->services[i], &s->ble.scan_service);
        }
        if (!found) {
            return false;
        }
    }
    return !s->ble.scan_name[0] || !strncmp(ev->name, s->ble.scan_name, strlen(s->ble.scan_name));
}

static int served_index(const lh_t *s, const os_ble_uuid_t *u)
{
    for (int i = 0; i < s->ble.n_chrs; i++) {
        if (uuid_eq(&s->ble.chrs[i], u)) {
            return i;
        }
    }
    return -1;
}

// Answers still waiting when the link ended: each cb(false, "lost"), now.
static void ops_lost(lh_t *s)
{
    for (int i = 0; i < OS_BLE_OPS_MAX; i++) {
        if (!s->ble.ops[i].op) {
            continue;
        }
        int ref = s->ble.ops[i].ref;
        s->ble.ops[i].op = 0;
        s->ble.ops[i].ref = LUA_NOREF;
        ble_cb_t c = { .ref = ref };
        arg_b(&c, false);
        arg_str(&c, "lost");
        run(s, &c);
        unref(s, &ref);
    }
}

static void on_event(lh_t *s, os_ble_ev_t *ev)
{
    char chr[UUID_TEXT_MAX + 1];
    switch (ev->kind) {
    case OS_BLE_EV_SCAN:
        if (s->ble.scan_ref != LUA_NOREF && scan_wants(s, ev)) {
            lh_callback(s, call_scan, ev);
        }
        break;
    case OS_BLE_EV_OPEN:
        if (ev->central) {
            if (s->ble.link_state == LH_BLE_LINK_CONNECTING) {
                s->ble.link_state = LH_BLE_LINK_OPEN;
                ble_cb_t c = { .ref = s->ble.link_ref };
                arg_str(&c, "open");
                run(s, &c);
            }
        } else {
            s->ble.peer = true;
            memset(s->ble.subscribed, 0, sizeof(s->ble.subscribed));
            char addr[18];
            addr_fmt(&ev->addr, addr);
            ble_cb_t c = { .ref = s->ble.serve_ref };
            arg_str(&c, "open");
            arg_str(&c, addr);
            run(s, &c);
        }
        break;
    case OS_BLE_EV_CLOSE: {
        const char *reason = ev->reason ? ev->reason : "lost";
        if (ev->central) {
            if (s->ble.link_state == LH_BLE_LINK_NONE) {
                break;
            }
            int ref = s->ble.link_ref;
            s->ble.link_ref = LUA_NOREF;
            s->ble.link_id = 0;
            s->ble.link_state = LH_BLE_LINK_NONE;
            ops_lost(s);
            ble_cb_t c = { .ref = ref };
            arg_str(&c, "close");
            arg_str(&c, reason);
            run(s, &c);
            unref(s, &ref);
        } else if (s->ble.peer) {
            s->ble.peer = false;
            memset(s->ble.subscribed, 0, sizeof(s->ble.subscribed));
            ble_cb_t c = { .ref = s->ble.serve_ref };
            arg_str(&c, "close");
            arg_str(&c, reason);
            run(s, &c);
        }
        break;
    }
    case OS_BLE_EV_NOTIFY:
        if (s->ble.link_state == LH_BLE_LINK_OPEN) {
            uuid_fmt(&ev->chr, chr);
            ble_cb_t c = { .ref = s->ble.link_ref };
            arg_str(&c, "notify");
            arg_str(&c, chr);
            arg_s(&c, (const char *)ev->data, ev->data ? ev->len : 0);
            run(s, &c);
        }
        break;
    case OS_BLE_EV_WRITE:
        if (s->ble.peer) {
            uuid_fmt(&ev->chr, chr);
            ble_cb_t c = { .ref = s->ble.serve_ref };
            arg_str(&c, "write");
            arg_str(&c, chr);
            arg_s(&c, (const char *)ev->data, ev->data ? ev->len : 0);
            run(s, &c);
        }
        break;
    case OS_BLE_EV_SUBSCRIBE: {
        int i = served_index(s, &ev->chr);
        if (s->ble.peer && i >= 0) {
            s->ble.subscribed[i] = ev->ok;
            uuid_fmt(&ev->chr, chr);
            ble_cb_t c = { .ref = s->ble.serve_ref };
            arg_str(&c, "subscribe");
            arg_str(&c, chr);
            arg_b(&c, ev->ok);
            run(s, &c);
        }
        break;
    }
    case OS_BLE_EV_DONE:
        for (int i = 0; i < OS_BLE_OPS_MAX; i++) {
            if (!ev->op || s->ble.ops[i].op != ev->op) {
                continue;
            }
            int ref = s->ble.ops[i].ref;
            uint8_t kind = s->ble.ops[i].kind;
            s->ble.ops[i].op = 0;
            s->ble.ops[i].ref = LUA_NOREF;
            ble_cb_t c = { .ref = ref };
            arg_b(&c, ev->ok);
            if (!ev->ok) {
                arg_str(&c, ev->reason ? ev->reason : "lost");
            } else if (kind == LH_BLE_OP_READ) {
                arg_s(&c, (const char *)ev->data, ev->data ? ev->len : 0);
            }
            run(s, &c);
            unref(s, &ref);
            break;
        }
        break;
    }
}

void lh_ble_init(lh_t *s)
{
    s->ble.scan_ref = s->ble.serve_ref = s->ble.link_ref = LUA_NOREF;
    for (int i = 0; i < OS_BLE_OPS_MAX; i++) {
        s->ble.ops[i].ref = LUA_NOREF;
    }
}

void lh_ble_run(lh_t *s)
{
    if (!s->ble.ctx && !s->ble.nlate) {
        return;                         // off, nothing owed
    }
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    s->ble.woken = false;
    xSemaphoreGive(lh_mx);
    while (s->L && !s->failed && s->ble.nlate) {
        int ref = s->ble.late[0].ref;
        uint8_t kind = s->ble.late[0].kind;
        const char *reason = s->ble.late[0].reason;
        memmove(&s->ble.late[0], &s->ble.late[1], --s->ble.nlate * sizeof(s->ble.late[0]));
        ble_cb_t c = { .ref = ref };
        if (kind == LH_BLE_LATE_CLOSE) {
            arg_str(&c, "close");
        } else {
            arg_b(&c, false);
        }
        arg_str(&c, reason);
        run(s, &c);
        unref(s, &ref);
    }
    os_ble_ev_t ev;
    int n = 0;
    while (s->L && !s->failed && s->ble.ctx && n < LH_BLE_PER_ROUND && os_ble_next(s->ble.ctx, &ev)) {
        n++;
        on_event(s, &ev);
        free(ev.data);
    }
    if (n == LH_BLE_PER_ROUND) {
        lh_ev_t w = { .kind = LH_EV_WAKE, .gen = s->gen };
        lh_post(&w);                    // more may wait: come back after the keys
    }
}

void lh_ble_stop(lh_t *s)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    void *ctx = s->ble.ctx;
    s->ble.ctx = NULL;
    xSemaphoreGive(lh_mx);
    if (ctx) {
        os_ble_stop(ctx);
    }
}

// ---------------------------------------------------------------------------
// The module

static lh_t *session(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_BLE, "ble");
    return lh_get(L);
}

// The session, Bluetooth on: anything else is a Lua error.
static lh_t *on_session(lua_State *L, const char *fn)
{
    lh_t *s = session(L);
    if (!s->ble.ctx) {
        luaL_error(L, "ble.%s: Bluetooth is off (call ble.on() first)", fn);
    }
    return s;
}

// ble.on() -> true | nil, why
static int l_on(lua_State *L)
{
    lh_t *s = session(L);
    if (s->ble.ctx) {
        return ok_true(L);
    }
    if (!os_ble_available()) {
        return nil_why(L, "unavailable");
    }
    void *ctx = os_api_token();
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    s->ble.ctx = ctx;                   // before os_ble can wake us for it
    s->ble.woken = false;
    xSemaphoreGive(lh_mx);
    esp_err_t err = os_ble_start(ble_wake, ctx);
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    bool kept = s->ble.ctx == ctx && !s->quit;
    if (err != ESP_OK || !kept) {
        s->ble.ctx = NULL;
    }
    xSemaphoreGive(lh_mx);
    if (err == ESP_OK && !kept) {
        os_ble_stop(ctx);               // the app was stopped meanwhile
    }
    if (err != ESP_OK || !kept) {
        return nil_why(L, err == ESP_ERR_NO_MEM ? "not enough memory" : "unavailable");
    }
    s->ble.served = s->ble.peer = false;
    s->ble.n_chrs = 0;
    memset(s->ble.subscribed, 0, sizeof(s->ble.subscribed));
    return ok_true(L);
}

// ble.off(): everything ends; the callbacks owed go out on the next round.
static int l_off(lua_State *L)
{
    lh_t *s = session(L);
    if (!s->ble.ctx) {
        return 0;
    }
    lh_ble_stop(s);
    unref(s, &s->ble.scan_ref);
    s->ble.scan_by_service = false;
    s->ble.scan_name[0] = 0;
    for (int i = 0; i < OS_BLE_OPS_MAX; i++) {
        if (s->ble.ops[i].op) {
            owe(s, s->ble.ops[i].ref, LH_BLE_LATE_LOST, "lost");
            s->ble.ops[i].op = 0;
            s->ble.ops[i].ref = LUA_NOREF;
        }
    }
    if (s->ble.link_state != LH_BLE_LINK_NONE) {
        owe(s, s->ble.link_ref, LH_BLE_LATE_CLOSE, "off");
        s->ble.link_ref = LUA_NOREF;
        s->ble.link_id = 0;
        s->ble.link_state = LH_BLE_LINK_NONE;
    }
    if (s->ble.peer) {
        owe(s, s->ble.serve_ref, LH_BLE_LATE_CLOSE, "off");
        s->ble.serve_ref = LUA_NOREF;
    }
    unref(s, &s->ble.serve_ref);
    s->ble.served = s->ble.peer = false;
    s->ble.n_chrs = 0;
    return 0;
}

static int l_state(lua_State *L)
{
    lh_t *s = session(L);
    lua_pushstring(L, s->ble.ctx ? "on" : "off");
    return 1;
}

static int l_address(lua_State *L)
{
    lh_t *s = session(L);
    os_ble_addr_t a;
    if (!s->ble.ctx || !os_ble_address(&a)) {
        lua_pushnil(L);
        return 1;
    }
    char text[18];
    addr_fmt(&a, text);
    lua_pushstring(L, text);
    return 1;
}

static lua_Integer opt_int_field(lua_State *L, int idx, const char *name, lua_Integer lo, lua_Integer hi,
                                 lua_Integer def)
{
    lua_getfield(L, idx, name);
    lua_Integer v = def;
    if (!lua_isnil(L, -1)) {
        int isnum = 0;
        v = lua_tointegerx(L, -1, &isnum);
        if (!isnum || v < lo || v > hi) {
            luaL_error(L, "ble: %s is a whole number from %d to %d", name, (int)lo, (int)hi);
        }
    }
    lua_pop(L, 1);
    return v;
}

static bool bool_field(lua_State *L, int idx, const char *name)
{
    lua_getfield(L, idx, name);
    bool b = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return b;
}

// ble.advertise(opts | false) -> true | nil, "too big"
static int l_advertise(lua_State *L)
{
    lh_t *s = on_session(L, "advertise");
    if (lua_isnoneornil(L, 1) || (lua_isboolean(L, 1) && !lua_toboolean(L, 1))) {
        os_ble_advertise(s->ble.ctx, NULL);
        return ok_true(L);
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    os_ble_adv_t adv = { 0 };
    os_ble_uuid_t service;
    size_t nlen = 0;
    lua_getfield(L, 1, "name");
    if (!lua_isnil(L, -1)) {
        adv.name = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &nlen) : NULL;
        if (!adv.name || !printable(adv.name, nlen)) {
            luaL_error(L, "ble.advertise: name is printable ASCII");
        }
    }
    if (field_uuid(L, 1, "service", &service)) {
        adv.service = &service;
    }
    lua_getfield(L, 1, "data");
    if (!lua_isnil(L, -1)) {
        adv.data = lua_type(L, -1) == LUA_TSTRING ? (const uint8_t *)lua_tolstring(L, -1, &adv.len) : NULL;
        if (!adv.data) {
            luaL_error(L, "ble.advertise: data is a string of bytes");
        }
    }
    adv.connectable = bool_field(L, 1, "connectable");
    adv.interval_ms = (uint16_t)opt_int_field(L, 1, "interval", 20, 10000, 0);
    size_t max = !adv.service ? OS_BLE_ADV_MAX : adv.service->len == 2 ? OS_BLE_ADV_MAX - 4 : OS_BLE_ADV_MAX - 18;
    if (nlen > OS_BLE_NAME_MAX || adv.len > max) {
        return nil_why(L, "too big");
    }
    esp_err_t err = os_ble_advertise(s->ble.ctx, &adv);   // the strings stay on the stack meanwhile
    if (err == ESP_ERR_INVALID_SIZE) {
        return nil_why(L, "too big");
    }
    return err == ESP_OK ? ok_true(L) : nil_why(L, "busy");
}

// ble.scan(opts | false, cb) -> true | nil, why
static int l_scan(lua_State *L)
{
    lh_t *s = on_session(L, "scan");
    if (lua_isboolean(L, 1) && !lua_toboolean(L, 1)) {
        os_ble_scan(s->ble.ctx, NULL);
        unref(s, &s->ble.scan_ref);
        return ok_true(L);
    }
    bool has_opts = !lua_isnoneornil(L, 1);
    if (has_opts) {
        luaL_checktype(L, 1, LUA_TTABLE);
    }
    luaL_checktype(L, 2, LUA_TFUNCTION);
    os_ble_scan_t sc = { 0 };
    os_ble_uuid_t service;
    char name[OS_BLE_NAME_MAX + 1] = "";
    if (has_opts) {
        if (field_uuid(L, 1, "service", &service)) {
            sc.service = &service;
        }
        lua_getfield(L, 1, "name");
        if (!lua_isnil(L, -1)) {
            size_t n = 0;
            const char *p = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &n) : NULL;
            if (!p || n > OS_BLE_NAME_MAX || !printable(p, n)) {
                luaL_error(L, "ble.scan: name is at most 29 bytes of printable ASCII");
            }
            memcpy(name, p, n);
            name[n] = 0;
        }
        lua_pop(L, 1);
        sc.active = bool_field(L, 1, "active");
        sc.seconds = (uint32_t)opt_int_field(L, 1, "seconds", 0, 86400, 0);
        sc.duty = (uint8_t)opt_int_field(L, 1, "duty", 10, 100, 0);
    }
    sc.name_prefix = name[0] ? name : NULL;
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    // No Lua errors from here on.
    esp_err_t err = os_ble_scan(s->ble.ctx, &sc);
    if (err != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return nil_why(L, "busy");
    }
    unref(s, &s->ble.scan_ref);
    s->ble.scan_ref = ref;
    s->ble.scan_by_service = sc.service != NULL;
    if (sc.service) {
        s->ble.scan_service = service;
    }
    strlcpy(s->ble.scan_name, name, sizeof(s->ble.scan_name));
    return ok_true(L);
}

// ble.connect(dev, cb) -> link
static int l_connect(lua_State *L)
{
    lh_t *s = on_session(L, "connect");
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    os_ble_addr_t addr = { 0 };
    lua_getfield(L, 1, "addr");
    size_t n = 0;
    const char *p = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &n) : NULL;
    if (!p || !addr_parse(p, n, &addr)) {
        luaL_error(L, "ble.connect: addr is an address, \"7c:df:a1:00:12:34\"");
    }
    lua_pop(L, 1);
    addr.random = bool_field(L, 1, "random");
    if (s->ble.nlate >= LH_BLE_LATE) {
        luaL_error(L, "ble.connect: one link at a time");
    }

    ble_handle_t *h = lua_newuserdatauv(L, sizeof(*h), 0);
    h->id = ++s->ble.link_seq;
    luaL_setmetatable(L, BLE_META);
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    // No Lua errors from here on.
    esp_err_t err = s->ble.link_state != LH_BLE_LINK_NONE || s->ble.peer ? ESP_ERR_INVALID_STATE
                  : os_ble_connect(s->ble.ctx, &addr);
    if (err != ESP_OK) {
        owe(s, ref, LH_BLE_LATE_CLOSE, "busy");
        return 1;
    }
    s->ble.link_id = h->id;
    s->ble.link_ref = ref;
    s->ble.link_state = LH_BLE_LINK_CONNECTING;
    unref(s, &s->ble.scan_ref);         // os_ble ends the scan while it connects
    return 1;
}

// ble.serve(def, cb) -> true | nil, why
static int l_serve(lua_State *L)
{
    lh_t *s = on_session(L, "serve");
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    os_ble_uuid_t service;
    if (!field_uuid(L, 1, "service", &service)) {
        luaL_error(L, "ble.serve: service is missing");
    }
    lua_getfield(L, 1, "chars");
    luaL_argcheck(L, lua_istable(L, -1), 1, "chars is a list of characteristics");
    int count = (int)lua_rawlen(L, -1);
    luaL_argcheck(L, count >= 1 && count <= OS_BLE_CHARS_MAX, 1, "1 to 4 characteristics");
    os_ble_chr_t chrs[OS_BLE_CHARS_MAX];
    int chars = lua_gettop(L);
    for (int i = 0; i < count; i++) {
        lua_rawgeti(L, chars, i + 1);
        luaL_argcheck(L, lua_istable(L, -1), 1, "each characteristic is a table");
        int c = lua_gettop(L);
        if (!field_uuid(L, c, "uuid", &chrs[i].uuid)) {
            luaL_error(L, "ble.serve: a characteristic has no uuid");
        }
        for (int k = 0; k < i; k++) {
            if (uuid_eq(&chrs[k].uuid, &chrs[i].uuid)) {
                luaL_error(L, "ble.serve: two characteristics have the same uuid");
            }
        }
        chrs[i].read = bool_field(L, c, "read");
        chrs[i].write = bool_field(L, c, "write");
        chrs[i].notify = bool_field(L, c, "notify");
        lua_getfield(L, c, "value");
        if (!lua_isnil(L, -1)) {
            size_t n;
            check_data(L, lua_gettop(L), &n);
        }
        lua_pop(L, 2);
    }
    if (s->ble.served) {
        return nil_why(L, "already serving");
    }
    if (s->ble.link_state != LH_BLE_LINK_NONE || s->ble.peer) {
        return nil_why(L, "busy");
    }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    // No Lua errors from here on.
    esp_err_t err = os_ble_serve(s->ble.ctx, &service, chrs, count);
    if (err != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return nil_why(L, err == ESP_ERR_NO_MEM ? "not enough memory" : "busy");
    }
    for (int i = 0; i < count; i++) {
        lua_rawgeti(L, chars, i + 1);
        lua_getfield(L, -1, "value");
        size_t n = 0;
        const char *v = lua_tolstring(L, -1, &n);
        if (v) {
            os_ble_set(s->ble.ctx, &chrs[i].uuid, (const uint8_t *)v, n, false);
        }
        lua_pop(L, 2);
        s->ble.chrs[i] = chrs[i].uuid;
    }
    s->ble.n_chrs = (uint8_t)count;
    memset(s->ble.subscribed, 0, sizeof(s->ble.subscribed));
    s->ble.served = true;
    unref(s, &s->ble.serve_ref);
    s->ble.serve_ref = ref;
    return ok_true(L);
}

// The served characteristic in argument 1 and the value in 2.
static int served_arg(lua_State *L, lh_t *s, os_ble_uuid_t *u, const char **data, size_t *n, const char *fn)
{
    check_uuid(L, 1, u);
    *data = check_data(L, 2, n);
    int i = served_index(s, u);
    if (i < 0) {
        luaL_error(L, "ble.%s: %s is not a served characteristic", fn, lua_tostring(L, 1));
    }
    return i;
}

// ble.set(chr, data) -> true
static int l_set(lua_State *L)
{
    lh_t *s = on_session(L, "set");
    os_ble_uuid_t u;
    const char *data;
    size_t n;
    served_arg(L, s, &u, &data, &n, "set");
    os_ble_set(s->ble.ctx, &u, (const uint8_t *)data, n, false);
    return ok_true(L);
}

// ble.notify(chr, data) -> true | nil, why
static int l_notify(lua_State *L)
{
    lh_t *s = on_session(L, "notify");
    os_ble_uuid_t u;
    const char *data;
    size_t n;
    int i = served_arg(L, s, &u, &data, &n, "notify");
    const char *why = !s->ble.peer ? "not open" : !s->ble.subscribed[i] ? "not subscribed" : NULL;
    if (why) {
        os_ble_set(s->ble.ctx, &u, (const uint8_t *)data, n, false);
        return nil_why(L, why);
    }
    esp_err_t err = os_ble_set(s->ble.ctx, &u, (const uint8_t *)data, n, true);
    if (err == ESP_OK) {
        return ok_true(L);
    }
    return nil_why(L, err == ESP_ERR_NO_MEM ? "busy" : s->ble.peer ? "not subscribed" : "not open");
}

static int l_drop(lua_State *L)
{
    lh_t *s = on_session(L, "drop");
    if (s->ble.peer) {
        os_ble_disconnect(s->ble.ctx);
    }
    return 0;
}

// The session whose live link is argument 1, or NULL.
static lh_t *live(lua_State *L)
{
    ble_handle_t *h = luaL_checkudata(L, 1, BLE_META);
    lh_t *s = lh_get(L);
    return s->ble.ctx && h->id && h->id == s->ble.link_id && s->ble.link_state != LH_BLE_LINK_NONE ? s : NULL;
}

static int free_op(const lh_t *s)
{
    for (int i = 0; i < OS_BLE_OPS_MAX; i++) {
        if (!s->ble.ops[i].op) {
            return i;
        }
    }
    return -1;
}

static const char *op_error(esp_err_t err)
{
    return err == ESP_ERR_NO_MEM ? "busy" : err == ESP_ERR_INVALID_SIZE ? "too big" : "not open";
}

// One operation: kind, and argument cb_idx as its callback (0: none).
// Arguments 2 and 3 are the service and characteristic.
static int start_op(lua_State *L, uint8_t kind, int cb_idx, const char *data, size_t len, bool on)
{
    lh_t *s = live(L);
    os_ble_uuid_t service, chr;
    check_uuid(L, 2, &service);
    check_uuid(L, 3, &chr);
    if (cb_idx) {
        luaL_checktype(L, cb_idx, LUA_TFUNCTION);
    }
    if (!s || s->ble.link_state != LH_BLE_LINK_OPEN) {
        return nil_why(L, "not open");
    }
    bool answered = kind != LH_BLE_OP_WRITE || cb_idx;
    int slot = answered ? free_op(s) : 0;
    if (slot < 0) {
        return nil_why(L, "busy");
    }
    int ref = LUA_NOREF;
    if (cb_idx) {
        lua_pushvalue(L, cb_idx);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    // No Lua errors from here on.
    uint32_t op = 0;
    esp_err_t err = kind == LH_BLE_OP_READ ? os_ble_read(s->ble.ctx, &service, &chr, &op)
                  : kind == LH_BLE_OP_SUB ? os_ble_subscribe(s->ble.ctx, &service, &chr, on, &op)
                  : os_ble_write(s->ble.ctx, &service, &chr, (const uint8_t *)data, len, cb_idx != 0, &op);
    if (err != ESP_OK) {
        unref(s, &ref);
        return nil_why(L, op_error(err));
    }
    if (answered && op) {
        s->ble.ops[slot].op = op;
        s->ble.ops[slot].ref = ref;
        s->ble.ops[slot].kind = kind;
    } else {
        unref(s, &ref);
    }
    return ok_true(L);
}

// link:read(service, chr, cb)
static int m_read(lua_State *L)
{
    return start_op(L, LH_BLE_OP_READ, 4, NULL, 0, false);
}

// link:write(service, chr, data, [cb])
static int m_write(lua_State *L)
{
    size_t n;
    const char *data = check_data(L, 4, &n);
    return start_op(L, LH_BLE_OP_WRITE, lua_isnoneornil(L, 5) ? 0 : 5, data, n, false);
}

// link:subscribe(service, chr, [on])
static int m_subscribe(lua_State *L)
{
    return start_op(L, LH_BLE_OP_SUB, 0, NULL, 0, lua_isnoneornil(L, 4) || lua_toboolean(L, 4));
}

// link:close(): cb("close", "closed") follows, from os_ble
static int m_close(lua_State *L)
{
    lh_t *s = live(L);
    if (s && s->ble.link_state != LH_BLE_LINK_CLOSING) {
        s->ble.link_state = LH_BLE_LINK_CLOSING;
        os_ble_disconnect(s->ble.ctx);
    }
    return 0;
}

static int m_state(lua_State *L)
{
    lh_t *s = live(L);
    lua_pushstring(L, !s ? "closed" : s->ble.link_state == LH_BLE_LINK_OPEN ? "open"
                   : s->ble.link_state == LH_BLE_LINK_CONNECTING ? "connecting" : "closed");
    return 1;
}

void lh_open_ble(lua_State *L)
{
    static const luaL_Reg methods[] = {{"read", m_read}, {"write", m_write}, {"subscribe", m_subscribe},
                                       {"close", m_close}, {"state", m_state}, {NULL, NULL}};
    static const luaL_Reg fns[] = {{"on", l_on}, {"off", l_off}, {"state", l_state}, {"address", l_address},
                                   {"advertise", l_advertise}, {"scan", l_scan}, {"connect", l_connect},
                                   {"serve", l_serve}, {"set", l_set}, {"notify", l_notify}, {"drop", l_drop},
                                   {NULL, NULL}};
    luaL_newmetatable(L, BLE_META);
    luaL_newlib(L, methods);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "link");
    lua_setfield(L, -2, "__metatable");     // getmetatable() can't reach the methods
    lua_pop(L, 1);
    luaL_newlib(L, fns);
    lua_setglobal(L, "ble");
}
