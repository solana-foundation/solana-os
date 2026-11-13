// crypto.* and solana.* (docs/LUA_API.md "crypto and solana"): SHA-256, the
// Ed25519 checks an app can make without a key (is this a point of the curve,
// does this signature verify), base58, and program-derived addresses.
//
// No permission: nothing here leaves the badge or touches anything of the
// owner's. No signing and no key generation, on purpose: a badge holds no
// keys (LUA_API.md "rpc"). The work is done in C, so it charges the callback's
// instruction budget by what it costs in time (lh_charge_steps), before it
// starts.

#include <string.h>
#include "psa/crypto.h"
#include "core/ed25519.h"
#include "lua_priv.h"

#define B58_BYTES_MAX       128                 // solana.encode / decode: bytes a side
#define B58_CHARS_MAX       176                 // 128 bytes as base58, at most 175
#define PDA_SEEDS_MAX       15                  // Solana's 16, less the bump
#define PDA_SEED_MAX        32

// What each call costs of the 200 000 a callback gets. Not yet timed on a
// badge: they come from the instructions an ESP32-C5 build of core/ed25519.c
// runs for each (counted in QEMU: 1.04 million for the on-curve test, 11.5
// million for a signature), at one or two cycles each on a 240 MHz core, so
// that a callback of nothing but these stays within about half a second.
#define STEPS_HASH          50                  // crypto.sha256, plus 1 per 4 bytes
#define STEPS_CURVE         5000                // crypto.on_curve: 4 to 9 ms
#define STEPS_VERIFY        40000               // crypto.verify: 50 to 100 ms, plus 1 per 2 bytes of message
#define STEPS_PDA           10000               // solana.pda, however many bumps it tries (2 on average)
#define STEPS_B58           20                  // solana.encode / decode, plus n * n / 16

static int l_sha256(lua_State *L)
{
    size_t n;
    const char *p = luaL_checklstring(L, 1, &n);
    lh_charge_steps(L, STEPS_HASH + (unsigned)(n / 4));
    uint8_t out[32];
    size_t len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)p, n, out, sizeof(out), &len) != PSA_SUCCESS || len != 32) {
        return luaL_error(L, "crypto.sha256: the badge couldn't hash that");
    }
    lua_pushlstring(L, (const char *)out, sizeof(out));
    return 1;
}

static int l_on_curve(lua_State *L)
{
    size_t n;
    const char *p = luaL_checklstring(L, 1, &n);
    if (n != 32) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lh_charge_steps(L, STEPS_CURVE);
    lua_pushboolean(L, ed25519_on_curve((const uint8_t *)p));
    return 1;
}

static int l_verify(lua_State *L)
{
    size_t kl, ml, sl;
    const char *key = luaL_checklstring(L, 1, &kl);
    const char *msg = luaL_checklstring(L, 2, &ml);
    const char *sig = luaL_checklstring(L, 3, &sl);
    if (kl != 32 || sl != 64) {         // what arrives is untrusted: a wrong size is "no", not an error
        lua_pushboolean(L, 0);
        return 1;
    }
    lh_charge_steps(L, STEPS_VERIFY + (unsigned)(ml / 2));
    lua_pushboolean(L, ed25519_verify((const uint8_t *)key, (const uint8_t *)msg, ml, (const uint8_t *)sig));
    return 1;
}

// ---------------------------------------------------------------------------
// base58 (Bitcoin's alphabet, as Solana writes addresses and signatures)

static const char B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static int l_encode(lua_State *L)
{
    size_t n;
    const uint8_t *in = (const uint8_t *)luaL_checklstring(L, 1, &n);
    luaL_argcheck(L, n <= B58_BYTES_MAX, 1, "at most 128 bytes");
    lh_charge_steps(L, STEPS_B58 + (unsigned)(n * n / 16));
    uint8_t digits[B58_CHARS_MAX];      // base 58, lowest first
    size_t nd = 0, zeros = 0;
    while (zeros < n && in[zeros] == 0) {
        zeros++;
    }
    for (size_t i = zeros; i < n; i++) {
        unsigned carry = in[i];
        for (size_t j = 0; j < nd; j++) {
            carry += (unsigned)digits[j] << 8;
            digits[j] = (uint8_t)(carry % 58);
            carry /= 58;
        }
        while (carry) {
            digits[nd++] = (uint8_t)(carry % 58);
            carry /= 58;
        }
    }
    char out[B58_CHARS_MAX];
    size_t len = 0;
    for (size_t i = 0; i < zeros; i++) {    // each leading zero byte is a "1"
        out[len++] = '1';
    }
    while (nd) {
        out[len++] = B58[digits[--nd]];
    }
    lua_pushlstring(L, out, len);
    return 1;
}

// The bytes, or nil and why not: text from outside is untrusted.
static int l_decode(lua_State *L)
{
    size_t n;
    const char *in = luaL_checklstring(L, 1, &n);
    if (n > B58_CHARS_MAX) {
        lua_pushnil(L);
        lua_pushliteral(L, "too long");
        return 2;
    }
    lh_charge_steps(L, STEPS_B58 + (unsigned)(n * n / 16));
    uint8_t bytes[B58_BYTES_MAX];       // lowest first
    size_t nb = 0, zeros = 0;
    while (zeros < n && in[zeros] == '1') {
        zeros++;
    }
    for (size_t i = zeros; i < n; i++) {
        const char *at = in[i] ? strchr(B58, in[i]) : NULL;
        if (!at) {
            lua_pushnil(L);
            lua_pushliteral(L, "not base58");
            return 2;
        }
        unsigned carry = (unsigned)(at - B58);
        for (size_t j = 0; j < nb; j++) {
            carry += (unsigned)bytes[j] * 58;
            bytes[j] = (uint8_t)carry;
            carry >>= 8;
        }
        while (carry) {
            if (nb == sizeof(bytes)) {
                lua_pushnil(L);
                lua_pushliteral(L, "too long");
                return 2;
            }
            bytes[nb++] = (uint8_t)carry;
            carry >>= 8;
        }
    }
    if (zeros + nb > B58_BYTES_MAX) {
        lua_pushnil(L);
        lua_pushliteral(L, "too long");
        return 2;
    }
    uint8_t out[B58_BYTES_MAX];
    memset(out, 0, zeros);
    for (size_t i = 0; i < nb; i++) {
        out[zeros + i] = bytes[nb - 1 - i];
    }
    lua_pushlstring(L, (const char *)out, zeros + nb);
    return 1;
}

// ---------------------------------------------------------------------------
// Program-derived addresses

// solana.pda(seeds, program): the address and its bump, as Solana's
// find_program_address gives them: SHA-256 of the seeds, a bump from 255
// down, the program and "ProgramDerivedAddress", for the first bump whose
// hash is not a point of the curve. nil if no bump gives one (never seen).
static int l_pda(lua_State *L)
{
    static const char tag[] = "ProgramDerivedAddress";
    luaL_checktype(L, 1, LUA_TTABLE);
    size_t plen;
    const char *program = luaL_checklstring(L, 2, &plen);
    luaL_argcheck(L, plen == 32, 2, "a program address is 32 bytes: solana.decode its base58");
    size_t count = (size_t)lua_rawlen(L, 1);
    luaL_argcheck(L, count <= PDA_SEEDS_MAX, 1, "at most 15 seeds");
    uint8_t seeds[PDA_SEEDS_MAX * PDA_SEED_MAX];
    size_t total = 0;
    for (size_t i = 1; i <= count; i++) {
        lua_rawgeti(L, 1, (lua_Integer)i);
        size_t sl = 0;
        const char *sp = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &sl) : NULL;
        luaL_argcheck(L, sp, 1, "each seed is a string of bytes");
        luaL_argcheck(L, sl <= PDA_SEED_MAX, 1, "a seed is at most 32 bytes");
        memcpy(seeds + total, sp, sl);
        total += sl;
        lua_pop(L, 1);
    }
    lh_charge_steps(L, STEPS_PDA);
    for (int bump = 255; bump >= 0; bump--) {
        uint8_t b = (uint8_t)bump, out[32];
        size_t len = 0;
        psa_hash_operation_t c = PSA_HASH_OPERATION_INIT;
        bool ok = psa_hash_setup(&c, PSA_ALG_SHA_256) == PSA_SUCCESS &&
                  psa_hash_update(&c, seeds, total) == PSA_SUCCESS && psa_hash_update(&c, &b, 1) == PSA_SUCCESS &&
                  psa_hash_update(&c, (const uint8_t *)program, 32) == PSA_SUCCESS &&
                  psa_hash_update(&c, (const uint8_t *)tag, sizeof(tag) - 1) == PSA_SUCCESS &&
                  psa_hash_finish(&c, out, sizeof(out), &len) == PSA_SUCCESS && len == 32;
        psa_hash_abort(&c);
        if (!ok) {
            return luaL_error(L, "solana.pda: the badge couldn't hash that");
        }
        if (!ed25519_on_curve(out)) {
            lua_pushlstring(L, (const char *)out, sizeof(out));
            lua_pushinteger(L, bump);
            return 2;
        }
    }
    lua_pushnil(L);
    return 1;
}

void lh_open_crypto(lua_State *L)
{
    static const luaL_Reg crypto[] = {
        {"sha256", l_sha256}, {"on_curve", l_on_curve}, {"verify", l_verify}, {NULL, NULL},
    };
    static const luaL_Reg solana[] = {
        {"encode", l_encode}, {"decode", l_decode}, {"pda", l_pda}, {NULL, NULL},
    };
    luaL_newlib(L, crypto);
    lua_setglobal(L, "crypto");
    luaL_newlib(L, solana);
    lua_setglobal(L, "solana");
}
