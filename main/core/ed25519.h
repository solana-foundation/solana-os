#pragma once

// Ed25519 for Lua apps (lua/lua_crypto.c): is this a point of the curve, and
// does this signature verify. No signing and no key generation: a badge holds
// no keys (docs/LUA_API.md "rpc"). Everything here works on public data, so
// nothing is constant-time. Portable C: the badge and the emulator build it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The 32 bytes are the encoding of a point of the curve: a y for which an x
// exists. This is the test Solana applies to a program-derived address, which
// must fail it. As there, a y of p or more counts as y - p.
bool ed25519_on_curve(const uint8_t p[32]);

// sig (R then s) is pub's signature of msg. False for an s of the group order
// or more, a pub that isn't on the curve, and an R that isn't the canonical
// encoding of s*B - h*A.
bool ed25519_verify(const uint8_t pub[32], const uint8_t *msg, size_t len, const uint8_t sig[64]);
