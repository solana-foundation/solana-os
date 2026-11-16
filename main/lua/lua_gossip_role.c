// gossip for Lua apps, the role side (docs/LUA_API.md "gossip", Solana OS
// 2.4.0; the parent repo's docs/GOSSIP.md §10): role, boss, and the Lab's
// calls that speak to roles. Builder: fw-roles. Reads game/gossip_role.h.
//
// Nothing here reaches the server or blocks: reads copy the role service's
// state (never the role key), acting calls queue a message or an Action that
// the OS service sends at its own pace. Every acting call is the Lab's alone
// (app id `gossip-lab`); others get nil, "lab only".

#include <stdlib.h>
#include <string.h>
#include "game/gossip_role.h"
#include "lua_priv.h"

#define LAB_ID      "gossip-lab"
#define STEPS_READ  200             // a read: a copy and a small decode

static int fail(lua_State *L, const char *why)
{
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}

static int done(lua_State *L, const char *err)
{
    if (err) {
        return fail(L, err);
    }
    lua_pushboolean(L, 1);
    return 1;
}

// The permission for everyone; the Lab's id for acting.
static bool lab(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_GOSSIP, "gossip");
    lh_t *s = lh_get(L);
    return !strcmp(s->pkg.id, LAB_ID);
}

// A JSON text from the service as a Lua value (nil for NULL).
static int push_json(lua_State *L, char *j)
{
    lh_charge_steps(L, STEPS_READ);
    if (!j) {
        lua_pushnil(L);
        return 1;
    }
    const char *err = lh_json_decode(L, j, strlen(j));
    free(j);
    if (err) {
        lua_pushnil(L);
    }
    return 1;
}

// gossip.role() -> RoleState | nil
static int l_role(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_GOSSIP, "gossip");
    return push_json(L, gossip_role_state_json());
}

// gossip.boss() -> {id, sid, beat, ms, bpm, fork, p, phase, rssi, real} | nil
static int l_boss(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_GOSSIP, "gossip");
    return push_json(L, gossip_role_boss_json());
}

// gossip.bids() -> {{from, name, p, fee, rssi, age}, ...} | nil (Leader)
static int l_bids(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    return push_json(L, gossip_role_bids_json());
}

// gossip.voters() -> {voters, conf, fin, p, all, beat, beats, fork, phase} | nil (boss)
static int l_voters(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    return push_json(L, gossip_role_voters_json());
}

// gossip.bid(role, a, fee) -> true | nil, err
static int l_bid(lua_State *L)
{
    const char *role = luaL_checkstring(L, 1), *a = luaL_checkstring(L, 2);
    bool fee = lua_toboolean(L, 3);
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    return done(L, gossip_role_bid(role, a, fee));
}

// gossip.claim(role, c) -> true | nil, err
static int l_claim(lua_State *L)
{
    const char *role = luaL_checkstring(L, 1);
    lua_Integer c = luaL_checkinteger(L, 2);
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    return done(L, c < 0 || c > 9999 ? "count: 0-9999" : gossip_role_claim(role, (int)c));
}

// gossip.cure(id) -> true | nil, err
static int l_cure(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    return done(L, gossip_role_cure(id));
}

// gossip.vote(votes, lockout) -> true | nil, err
static int l_vote(lua_State *L)
{
    lua_Integer v = luaL_checkinteger(L, 1), l = luaL_checkinteger(L, 2);
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    if (v < 0 || v > 9999 || l < 0 || l > 32) {
        return fail(L, "votes 0-9999, lockout 0-32");
    }
    return done(L, gossip_role_vote((int)v, (int)l));
}

// gossip.role_answer(yes) -> true | nil, err
static int l_role_answer(lua_State *L)
{
    luaL_checkany(L, 1);
    bool yes = lua_toboolean(L, 1);
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    return done(L, gossip_role_answer(yes));
}

// gossip.role_end() -> true | nil, err
static int l_role_end(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    return done(L, gossip_role_end());
}

// gossip.block({{p = a, by = id}, ...}) -> true | nil, err (Leader, <= 4)
static int l_block(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    gossip_role_pick_t picks[4];
    lua_Integer n = luaL_len(L, 1);
    if (n < 0 || n > 4) {
        return fail(L, "at most 4 programs");
    }
    memset(picks, 0, sizeof(picks));
    for (int i = 0; i < n; i++) {
        lua_geti(L, 1, i + 1);
        if (!lua_istable(L, -1)) {
            return fail(L, "each entry: {p = address, by = badge id}");
        }
        lua_getfield(L, -1, "p");
        lua_getfield(L, -2, "by");
        const char *p = lua_tostring(L, -2), *by = lua_tostring(L, -1);
        if (!p || strlen(p) > GOSSIP_ADDR_MAX || (by && strlen(by) > 6)) {
            return fail(L, "bad program or id");
        }
        strlcpy(picks[i].p, p, sizeof(picks[i].p));
        strlcpy(picks[i].by, by ? by : "", sizeof(picks[i].by));
        lua_pop(L, 3);
    }
    return done(L, gossip_role_block(picks, (int)n));
}

// gossip.fork() -> true | nil, err (the boss's owner)
static int l_fork(lua_State *L)
{
    if (!lab(L)) {
        return fail(L, "lab only");
    }
    return done(L, gossip_role_fork());
}

// The `gossip` table is on top of the stack: add functions with
// lua_setfield(L, -2, name) and leave the stack as it was.
void lh_gossip_role_open(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"role", l_role},
        {"boss", l_boss},
        {"bids", l_bids},
        {"voters", l_voters},
        {"bid", l_bid},
        {"claim", l_claim},
        {"cure", l_cure},
        {"vote", l_vote},
        {"role_answer", l_role_answer},
        {"role_end", l_role_end},
        {"block", l_block},
        {"fork", l_fork},
        {NULL, NULL},
    };
    for (const luaL_Reg *f = fns; f->name; f++) {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
}
