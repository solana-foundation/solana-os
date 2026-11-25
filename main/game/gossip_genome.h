#pragma once

// Gossip's pure parts (the parent repo's docs/GOSSIP.md §3 and §4): base58
// addresses, the genome decoded from SHA-256 of an address, the PDA check of a
// program record in one try with its bump, the record's signed message, the
// HELLO's `g` field and its tag's message. Builder: fw-node.
//
// Portable C with no LVGL, Lua, FreeRTOS or ESP-IDF in it (its own SHA-256
// and HMAC; core/ed25519.c for the curve), so a host test builds it as is:
// main/lua/tests/gossip/genome_test.py. Every function is pure and may run
// on any task.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gossip.h"

// GOSSIP_PROGRAM: SHA-256("solana-bp26-gossip"), FqinVUvLr5N6sGpLMF2jxAUhLVaavRXPgGBpTozmcWjb.
extern const uint8_t gg_program[32];

// SHA-256, streaming and in one go.
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} gg_sha256_t;
void gg_sha256_init(gg_sha256_t *c);
void gg_sha256_update(gg_sha256_t *c, const void *p, size_t n);
void gg_sha256_final(gg_sha256_t *c, uint8_t out[32]);
void gg_sha256(const void *p, size_t n, uint8_t out[32]);

// MESH.md "Tags": the first 8 bytes of HMAC-SHA256(key16, msg) as 16
// lowercase hex. The badge signs with os_mesh_sign; this is for tests.
void gg_hmac_tag(const uint8_t key[16], const char *msg, char out[17]);

// A base58 address of exactly 32 bytes (32 to 44 characters): false otherwise.
bool gg_b58_decode32(const char *s, uint8_t out[32]);
void gg_b58_encode32(const uint8_t in[32], char out[GOSSIP_ADDR_MAX + 1]);
// The text is a valid address (decodes to 32 bytes and back to itself).
bool gg_addr_ok(const char *s);

// §3 "Genome": everything from the address's 32 bytes.
void gg_decode(const uint8_t addr[32], gossip_genome_t *out);
bool gg_has_trait(const gossip_genome_t *g, int trait);

// §3 "Address": sha256(kind ‖ nonce8 ‖ bump ‖ program ‖ "ProgramDerivedAddress")
// is `addr` and is not on the curve: a record's address checked in one try.
// kind is "root" (program = GOSSIP_PROGRAM) or "fork" (program = the parent).
bool gg_pda_ok(const char *kind, const uint8_t nonce8[8], const uint8_t program[32], int bump,
               const uint8_t addr[32]);
// findProgramAddress: bump 255 down, the first off the curve; -1 if none. For tests.
int gg_pda_find(const char *kind, const uint8_t nonce8[8], const uint8_t program[32], uint8_t addr[32]);

// A ProgramRecord's fields as the server signs them (§3 "Record").
typedef struct {
    char a[GOSSIP_ADDR_MAX + 1];
    char par[GOSSIP_ADDR_MAX + 1];     // "" for a root
    int gen;
    char seed[17];                      // nonce8 as 16 hex
    int bump;
    int fl;
    uint8_t sig[64];
} gg_record_t;
// "sos-gossip1|a|par|gen|seed|bump|fl"; the length, or -1 when it doesn't fit.
int gg_record_msg(const gg_record_t *r, char *out, size_t n);
// The record's shape and address: a and par valid (par "" exactly when gen is
// 0), the seed 16 lowercase hex, bump and flags in range, and the PDA check.
// No signature check (that's ed25519_verify over gg_record_msg, ~57 ms).
bool gg_record_shape_ok(const gg_record_t *r);
// Parses lowercase or uppercase hex of exactly n bytes.
bool gg_unhex(const char *hex, uint8_t *out, size_t n);

// §4: a HELLO's g, "<c><b>.<w>.<tag>": c one of r v i b n, b 0-9, w decimal
// (no leading zeros, ≤ 12 digits), tag 16 lowercase hex.
typedef struct {
    char c;
    int b;
    int64_t w;
    char tag[17];
} gg_g_t;
bool gg_g_parse(const char *g, gg_g_t *out);
// The tag's message "gossip|<id>|<st>|<w>|<c><b>"; the length, or -1.
int gg_tag_msg(char *out, size_t n, const char *id, const char *st, int64_t w, char c, int b);
