// gossip for Lua apps (docs/LUA_API.md "gossip", Solana OS 2.4.0; the parent
// repo's docs/GOSSIP.md §10): the node side, reading game/gossip.h's state.
// Builder: fw-node. lua_gossip_role.c (fw-roles) adds the role side to the
// same table (lh_gossip_role_open).
//
// Every function checks the permission. The reads copy the node service's
// state out as JSON under its lock (game/gossip.c builds it) and decode it
// here, charged to the callback's budget by size; nothing blocks and nothing
// reaches the server. The acting functions are the Lab's (app id gossip-lab)
// alone: each queues one Action (§7) for the next sync, which the service
// asks for soon, and returns true and the action's place in the queue, or
// nil and why.

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "game/gossip.h"
#include "lua_host.h"
#include "lua_priv.h"

#define LAB_ID          "gossip-lab"
#define STEPS_READ      300             // plus 1 per 4 bytes of JSON decoded
#define STEPS_GENOME    400             // one SHA-256 and a base58 decode
#define STEPS_ACT       200

static int fail(lua_State *L, const char *why)
{
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}

// Push the JSON (freed here), or nil when there is none.
static int push_json(lua_State *L, char *json)
{
    if (!json) {
        lua_pushnil(L);
        return 1;
    }
    size_t n = strlen(json);
    lh_charge_steps(L, STEPS_READ + (unsigned)(n / 4));
    // Decoding may raise (out of memory): copy to the Lua heap first so the C
    // string is freed whatever happens.
    lua_pushlstring(L, json, n);
    cJSON_free(json);
    const char *s = lua_tostring(L, -1);
    const char *err = lh_json_decode(L, s, n);
    if (err) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    lua_remove(L, -2);
    return 1;
}

static void need(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_GOSSIP, "gossip");
}

static const char *addr_arg(lua_State *L, int i)
{
    size_t n;
    const char *a = luaL_checklstring(L, i, &n);
    return n >= 32 && n <= GOSSIP_ADDR_MAX ? a : NULL;
}

// ---------------------------------------------------------------------------
// Reading (any app with `gossip`, app services too)

static int l_me(lua_State *L)
{
    need(L);
    return push_json(L, gossip_me_json());
}

static int l_programs(lua_State *L)
{
    need(L);
    return push_json(L, gossip_programs_json());
}

static int l_program(lua_State *L)
{
    need(L);
    const char *a = addr_arg(L, 1);
    lh_charge_steps(L, STEPS_GENOME);
    return push_json(L, a ? gossip_program_json(a) : NULL);
}

static int l_genome(lua_State *L)
{
    need(L);
    const char *a = addr_arg(L, 1);
    lh_charge_steps(L, STEPS_GENOME);
    return push_json(L, a ? gossip_genome_json(a) : NULL);
}

static int l_nearby(lua_State *L)
{
    need(L);
    char *s = gossip_nearby_json();
    return push_json(L, s ? s : NULL);
}

static int l_hearing(lua_State *L)
{
    need(L);
    return push_json(L, gossip_hearing_json());
}

// ---------------------------------------------------------------------------
// Acting (the Lab only)

static bool lab(lua_State *L)
{
    need(L);
    return !strcmp(lh_get(L)->pkg.id, LAB_ID);
}

static bool printable(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

// An item id as the server gives one: 1-40 printable ASCII.
static const char *item_arg(lua_State *L, int i)
{
    size_t n;
    const char *s = luaL_checklstring(L, i, &n);
    return n >= 1 && n <= 40 && printable(s, n) ? s : NULL;
}

static int queue(lua_State *L, cJSON *a)
{
    lh_charge_steps(L, STEPS_ACT);
    char *s = cJSON_PrintUnformatted(a);
    cJSON_Delete(a);
    if (!s) {
        return fail(L, "out of memory");
    }
    bool ok = gossip_action(s);
    cJSON_free(s);
    if (!ok) {
        return fail(L, gossip_on() ? "busy" : "gossip is off");
    }
    lua_pushboolean(L, 1);
    return 1;
}

static cJSON *action(const char *t)
{
    cJSON *a = cJSON_CreateObject();
    if (a) {
        cJSON_AddStringToObject(a, "t", t);
    }
    return a;
}

// Slots are 1-based in Lua, 0-based on the wire (§7 Action `slot`).
static int slot_arg(lua_State *L, int i)
{
    lua_Integer s = luaL_checkinteger(L, i);
    return s >= 1 && s <= 5 ? (int)s - 1 : -1;
}

static int l_set_class(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    size_t n;
    const char *c = luaL_checklstring(L, 1, &n);
    if (n != 1 || !strchr("rvib", c[0])) {
        return fail(L, "class: r, v, i or b");
    }
    cJSON *a = action("class");
    cJSON_AddStringToObject(a, "c", c);
    return queue(L, a);
}

static int l_activate(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    const char *p = addr_arg(L, 1);
    char *g = p ? gossip_genome_json(p) : NULL;
    if (!g) {
        return fail(L, "bad address");
    }
    cJSON_free(g);
    cJSON *a = action("active");
    cJSON_AddStringToObject(a, "p", p);
    return queue(L, a);
}

static int l_place(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    const char *p = addr_arg(L, 1);
    int slot = slot_arg(L, 2);
    char *g = p ? gossip_genome_json(p) : NULL;
    if (!g) {
        return fail(L, "bad address");
    }
    cJSON_free(g);
    if (slot < 0) {
        return fail(L, "slot: 1 to 5");
    }
    cJSON *a = action("place");
    cJSON_AddStringToObject(a, "p", p);
    cJSON_AddNumberToObject(a, "slot", slot);
    return queue(L, a);
}

static int l_drop(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    int slot = slot_arg(L, 1);
    if (slot < 0) {
        return fail(L, "slot: 1 to 5");
    }
    cJSON *a = action("drop");
    cJSON_AddNumberToObject(a, "slot", slot);
    return queue(L, a);
}

static int l_deploy(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    const char *item = lua_isnoneornil(L, 1) ? NULL : item_arg(L, 1);
    if (!lua_isnoneornil(L, 1) && !item) {
        return fail(L, "bad item");
    }
    cJSON *a = action("deploy");
    if (item) {
        cJSON_AddStringToObject(a, "item", item);
    }
    return queue(L, a);
}

static int l_use(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    const char *item = item_arg(L, 1);
    if (!item) {
        return fail(L, "bad item");
    }
    cJSON *a = action("use");
    cJSON_AddStringToObject(a, "item", item);
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        static const char *const keys[] = {"p", "q"};
        for (int i = 0; i < 2; i++) {
            lua_getfield(L, 2, keys[i]);
            if (!lua_isnil(L, -1)) {
                size_t n;
                const char *v = lua_tolstring(L, -1, &n);
                char *g = v && n <= GOSSIP_ADDR_MAX ? gossip_genome_json(v) : NULL;
                if (!g) {
                    lua_pop(L, 1);
                    cJSON_Delete(a);
                    return fail(L, "bad address");
                }
                cJSON_free(g);
                cJSON_AddStringToObject(a, keys[i], v);
            }
            lua_pop(L, 1);
        }
        lua_getfield(L, 2, "trait");
        if (!lua_isnil(L, -1)) {
            lua_Integer t = lua_tointeger(L, -1);
            if (!lua_isinteger(L, -1) || t < 0 || t > 5) {
                lua_pop(L, 1);
                cJSON_Delete(a);
                return fail(L, "trait: 0 to 5");
            }
            cJSON_AddNumberToObject(a, "trait", (double)t);
        }
        lua_pop(L, 1);
    }
    return queue(L, a);
}

static int l_name(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    const char *p = addr_arg(L, 1);
    size_t n;
    const char *text = luaL_checklstring(L, 2, &n);
    char *g = p ? gossip_genome_json(p) : NULL;
    if (!g) {
        return fail(L, "bad address");
    }
    cJSON_free(g);
    if (n < 1 || n > 16 || !printable(text, n)) {
        return fail(L, "name: 1-16 printable ASCII");
    }
    cJSON *a = action("name");
    cJSON_AddStringToObject(a, "p", p);
    cJSON_AddStringToObject(a, "name", text);
    return queue(L, a);
}

static int l_public(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    cJSON *a = action("public");
    cJSON_AddBoolToObject(a, "on", lua_toboolean(L, 1));
    return queue(L, a);
}

static int l_lead(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    gossip_set_lead(lua_toboolean(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

void lh_open_gossip(lua_State *L)
{
    static const luaL_Reg gossip[] = {
        {"me", l_me},
        {"programs", l_programs},
        {"program", l_program},
        {"genome", l_genome},
        {"nearby", l_nearby},
        {"hearing", l_hearing},
        {"set_class", l_set_class},
        {"activate", l_activate},
        {"place", l_place},
        {"drop", l_drop},
        {"deploy", l_deploy},
        {"use", l_use},
        {"name", l_name},
        {"public", l_public},
        {"lead", l_lead},
        {NULL, NULL},
    };
    luaL_newlib(L, gossip);
    lh_gossip_role_open(L);
    lua_setglobal(L, "gossip");
}

bool lua_host_gossip(const char *ev_json)
{
    return ev_json && lua_host_broadcast("on_gossip", ev_json, LUA_PERM_GOSSIP);
}
