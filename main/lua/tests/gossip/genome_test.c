// Host test of game/gossip_genome.c against docs/GOSSIP.md §3's three vectors
// and §4's HELLO tag vector (MESH.md "Tags"), plus a record signed with a
// test key. Built and run by genome_test.py; prints "ok" lines and exits 1 on
// the first failure.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/ed25519.h"
#include "game/gossip_genome.h"

static int fails;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (cond) {                              \
            printf("ok   ");                     \
        } else {                                 \
            printf("FAIL ");                     \
            fails++;                             \
        }                                        \
        printf(__VA_ARGS__);                     \
        printf("\n");                            \
    } while (0)

typedef struct {
    const char *kind, *nonce, *parent, *addr, *G;
    int bump;
    int hue, r, g, b, body, eyes, marks, seed;
    int ntraits, traits[3];
    int vir, hard, stab, rarity;
} vec_t;

static const vec_t V[3] = {
    {"root", "0102030405060708", NULL, "59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMruo",
     "03ec29c2b46ec7796a2c660097494fe9158ea94290cf78279c5cbfe6d2239d62", 255,
     3, 255, 77, 64, 4, 5, 1, 5, 2, {2, 3}, 10, 2, 7, 1},
    {"fork", "0000000000000001", "59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMruo",
     "G2QqkHaYPwAg64t6soMZvrDDT48gH3SF8DYsfutQ2JWC",
     "49e674d6bae74e9e521eaed32e56b2b232a24972a6c55ffcf4d9e45d27bbf8e0", 255,
     73, 120, 255, 64, 6, 4, 4, 14, 2, {4, 0}, 9, 9, 3, 1},
    {"root", "ffeeddccbbaa9904", NULL, "31UFvJfpgAT4Paado1LNpEor9LL5DXMFAiCYzE2j5Cdj",
     "d263b55cf6342cdfcb1a034523cdc72c26c672f64ed11e2eb76aa9f4cb1e9bcb", 254,
     210, 240, 64, 255, 3, 4, 5, 22, 3, {2, 4, 0}, 5, 4, 4, 1},
};

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        snprintf(out + i * 2, 3, "%02x", b[i]);
    }
}

int main(void)
{
    // GOSSIP_PROGRAM
    uint8_t prog[32];
    gg_sha256("solana-bp26-gossip", 18, prog);
    char ph[65], pa[GOSSIP_ADDR_MAX + 1];
    hex(prog, 32, ph);
    gg_b58_encode32(gg_program, pa);
    CHECK(!memcmp(prog, gg_program, 32) && !strcmp(pa, "FqinVUvLr5N6sGpLMF2jxAUhLVaavRXPgGBpTozmcWjb"),
          "GOSSIP_PROGRAM %s = %s", ph, pa);

    for (int i = 0; i < 3; i++) {
        const vec_t *v = &V[i];
        uint8_t nonce[8], program[32], addr[32], want[32];
        gg_unhex(v->nonce, nonce, 8);
        if (v->parent) {
            CHECK(gg_b58_decode32(v->parent, program), "vector %d: parent decodes", i + 1);
        } else {
            memcpy(program, gg_program, 32);
        }
        int bump = gg_pda_find(v->kind, nonce, program, addr);
        char a[GOSSIP_ADDR_MAX + 1];
        gg_b58_encode32(addr, a);
        CHECK(bump == v->bump && !strcmp(a, v->addr), "vector %d: PDA %s bump %d", i + 1, a, bump);
        CHECK(gg_b58_decode32(v->addr, want) && !memcmp(want, addr, 32), "vector %d: base58 round trip", i + 1);
        CHECK(gg_pda_ok(v->kind, nonce, program, v->bump, want), "vector %d: one-try check with its bump", i + 1);
        CHECK(!gg_pda_ok(v->kind, nonce, program, v->bump == 255 ? 254 : 255, want), "vector %d: another bump fails", i + 1);

        gossip_genome_t g;
        gg_decode(want, &g);
        char gh[65];
        hex(g.g, 32, gh);
        CHECK(!strcmp(gh, v->G), "vector %d: G %s", i + 1, gh);
        CHECK(g.hue == v->hue && g.r == v->r && g.gr == v->g && g.b == v->b, "vector %d: hue %d RGB (%d,%d,%d)",
              i + 1, g.hue, g.r, g.gr, g.b);
        CHECK(g.body == v->body && g.eyes == v->eyes && g.marks == v->marks && g.mark_seed == v->seed,
              "vector %d: body %d eyes %d marks %d seed %d", i + 1, g.body, g.eyes, g.marks, g.mark_seed);
        bool tr = g.ntraits == v->ntraits;
        for (int t = 0; t < v->ntraits && tr; t++) {
            tr = g.traits[t] == v->traits[t];
        }
        CHECK(tr, "vector %d: %d traits %d %d %d", i + 1, g.ntraits, g.traits[0], g.traits[1], g.traits[2]);
        CHECK(g.vir == v->vir && g.hard == v->hard && g.stab == v->stab && g.rarity == v->rarity,
              "vector %d: vir %d hard %d stab %d rarity %d", i + 1, g.vir, g.hard, g.stab, g.rarity);
    }

    // Base58 edge cases: the wrong length, a bad character, a non-canonical spelling.
    uint8_t b[32];
    CHECK(!gg_b58_decode32("", b) && !gg_b58_decode32("59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMru0", b)
              && !gg_b58_decode32("59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMruoo", b)
              && !gg_b58_decode32("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", b)
              && !gg_b58_decode32("159qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMru", b),
          "base58: bad addresses refused");
    uint8_t zero[32] = {0};
    char za[GOSSIP_ADDR_MAX + 1];
    gg_b58_encode32(zero, za);
    CHECK(!strcmp(za, "11111111111111111111111111111111") && gg_b58_decode32(za, b) && !memcmp(b, zero, 32),
          "base58: the zero address");

    // §4: the HELLO tag vector, its g field, and g parsing.
    uint8_t key[16];
    for (int i = 0; i < 16; i++) {
        key[i] = (uint8_t)i;
    }
    char msg[160], tag[17];
    int n = gg_tag_msg(msg, sizeof(msg), "7K2Q9M", V[0].addr, 58000000, 'r', 0);
    gg_hmac_tag(key, msg, tag);
    CHECK(n > 0 && !strcmp(tag, "6034c14d8b593123"), "HELLO tag %s over %s", tag, msg);
    gg_hmac_tag(key, "xdex|7K2Q9M|9XQ3TT|0011223344556677", tag);
    CHECK(!strcmp(tag, "bc6e8523fe5d601c"), "MESH.md's xdex vector %s", tag);
    gg_g_t g;
    CHECK(gg_g_parse("r0.58000000.6034c14d8b593123", &g) && g.c == 'r' && g.b == 0 && g.w == 58000000
              && !strcmp(g.tag, "6034c14d8b593123"),
          "g parses");
    CHECK(!gg_g_parse("x0.58000000.6034c14d8b593123", &g) && !gg_g_parse("r0.058000000.6034c14d8b593123", &g)
              && !gg_g_parse("r0.58000000.6034C14D8B593123", &g) && !gg_g_parse("r0.58000000.6034c14d8b59312", &g)
              && !gg_g_parse("r.58000000.6034c14d8b593123", &g) && !gg_g_parse("", &g)
              && !gg_g_parse("n9.1234567890123.6034c14d8b593123", &g),
          "bad g fields refused");

    // §3 "Record": a record signed with a test key (seed 07 x 32), checked as the badge does.
    gg_record_t r = {.gen = 0, .bump = 255, .fl = 0};
    strcpy(r.a, V[0].addr);
    strcpy(r.seed, "0102030405060708");
    gg_unhex("084e5b2a34091c647ab51bd3eb4e9a16cb472a8bf6f531f0395c49a11012d0f3"
             "7aedf96ebd40ff7e2b9250114cb3558edd7ef6a2a7cf7aa082b677222b6e0e01", r.sig, 64);
    uint8_t pub[32];
    gg_unhex("ea4a6c63e29c520abef5507b132ec5f9954776aebebe7b92421eea691446d22c", pub, 32);
    char m[200];
    int ml = gg_record_msg(&r, m, sizeof(m));
    CHECK(gg_record_shape_ok(&r), "record: shape and PDA");
    CHECK(ml > 0 && ed25519_verify(pub, (const uint8_t *)m, (size_t)ml, r.sig), "record: signature verifies: %s", m);
    r.fl = 1;
    ml = gg_record_msg(&r, m, sizeof(m));
    CHECK(!ed25519_verify(pub, (const uint8_t *)m, (size_t)ml, r.sig), "record: a changed flag fails");
    gg_record_t f = r;
    f.fl = 0;
    f.gen = 1;
    CHECK(!gg_record_shape_ok(&f), "record: a root with gen 1 and no parent fails");
    f = r;
    f.fl = 0;
    f.bump = 254;
    CHECK(!gg_record_shape_ok(&f), "record: the wrong bump fails");
    gg_record_t fk = {.gen = 1, .bump = 255};
    strcpy(fk.a, V[1].addr);
    strcpy(fk.par, V[0].addr);
    strcpy(fk.seed, "0000000000000001");
    CHECK(gg_record_shape_ok(&fk), "record: vector 2 as a fork of vector 1");
    strcpy(fk.par, V[2].addr);
    CHECK(!gg_record_shape_ok(&fk), "record: the wrong parent fails");

    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
