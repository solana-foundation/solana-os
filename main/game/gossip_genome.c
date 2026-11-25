// Gossip's pure parts (docs/GOSSIP.md §3, §4): see gossip_genome.h.

#include <stdio.h>
#include <string.h>
#include "core/ed25519.h"
#include "gossip_genome.h"

const uint8_t gg_program[32] = {
    0xdc, 0x7e, 0xbd, 0xd2, 0xa6, 0x0b, 0x7a, 0x1b, 0xfe, 0xae, 0xc6, 0xf0, 0x25, 0xc4, 0x8e, 0xa1,
    0xdf, 0xcd, 0x20, 0x92, 0xbe, 0xb4, 0x8f, 0xde, 0x4e, 0xe3, 0xd8, 0x5e, 0xf9, 0x66, 0x41, 0x12,
};

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). Small and slow enough: a genome is one block.

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void gg_sha256_init(gg_sha256_t *c)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->n = 0;
}

void gg_sha256_update(gg_sha256_t *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->len += n;
    while (n) {
        size_t take = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, take);
        c->n += take;
        p += take;
        n -= take;
        if (c->n == 64) {
            sha_block(c->h, c->buf);
            c->n = 0;
        }
    }
}

void gg_sha256_final(gg_sha256_t *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    gg_sha256_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) {
        gg_sha256_update(c, &pad, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    gg_sha256_update(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

void gg_sha256(const void *p, size_t n, uint8_t out[32])
{
    gg_sha256_t c;
    gg_sha256_init(&c);
    gg_sha256_update(&c, p, n);
    gg_sha256_final(&c, out);
}

void gg_hmac_tag(const uint8_t key[16], const char *msg, char out[17])
{
    uint8_t pad[64], inner[32], mac[32];
    gg_sha256_t c;
    memset(pad, 0x36, sizeof(pad));
    for (int i = 0; i < 16; i++) {
        pad[i] ^= key[i];
    }
    gg_sha256_init(&c);
    gg_sha256_update(&c, pad, 64);
    gg_sha256_update(&c, msg, strlen(msg));
    gg_sha256_final(&c, inner);
    memset(pad, 0x5c, sizeof(pad));
    for (int i = 0; i < 16; i++) {
        pad[i] ^= key[i];
    }
    gg_sha256_init(&c);
    gg_sha256_update(&c, pad, 64);
    gg_sha256_update(&c, inner, 32);
    gg_sha256_final(&c, mac);
    for (int i = 0; i < 8; i++) {
        snprintf(out + i * 2, 3, "%02x", mac[i]);
    }
}

// ---------------------------------------------------------------------------
// base58 (Bitcoin's alphabet), 32-byte values only

static const char B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

bool gg_b58_decode32(const char *s, uint8_t out[32])
{
    size_t n = s ? strlen(s) : 0;
    if (n < 32 || n > GOSSIP_ADDR_MAX) {
        return false;
    }
    uint8_t b[33] = {0};        // big-endian accumulator, one spare byte to catch overflow
    for (size_t i = 0; i < n; i++) {
        const char *at = strchr(B58, s[i]);
        if (!s[i] || !at) {
            return false;
        }
        unsigned carry = (unsigned)(at - B58);
        for (int j = 32; j >= 0; j--) {
            carry += (unsigned)b[j] * 58;
            b[j] = (uint8_t)carry;
            carry >>= 8;
        }
        if (carry || b[0]) {
            return false;       // more than 32 bytes
        }
    }
    memcpy(out, b + 1, 32);
    // Leading '1's are leading zero bytes: the count must match exactly, or
    // two texts would name one address.
    char back[GOSSIP_ADDR_MAX + 1];
    gg_b58_encode32(out, back);
    return !strcmp(back, s);
}

void gg_b58_encode32(const uint8_t in[32], char out[GOSSIP_ADDR_MAX + 1])
{
    uint8_t d[48] = {0};        // base 58, lowest first
    size_t nd = 0, zeros = 0;
    while (zeros < 32 && in[zeros] == 0) {
        zeros++;
    }
    for (size_t i = zeros; i < 32; i++) {
        unsigned carry = in[i];
        for (size_t j = 0; j < nd; j++) {
            carry += (unsigned)d[j] << 8;
            d[j] = (uint8_t)(carry % 58);
            carry /= 58;
        }
        while (carry) {
            d[nd++] = (uint8_t)(carry % 58);
            carry /= 58;
        }
    }
    size_t len = 0;
    for (size_t i = 0; i < zeros; i++) {
        out[len++] = '1';
    }
    while (nd) {
        out[len++] = B58[d[--nd]];
    }
    out[len] = 0;
}

bool gg_addr_ok(const char *s)
{
    uint8_t b[32];
    return gg_b58_decode32(s, b);
}

// ---------------------------------------------------------------------------
// The genome (§3)

void gg_decode(const uint8_t addr[32], gossip_genome_t *o)
{
    memset(o, 0, sizeof(*o));
    gg_sha256(addr, 32, o->g);
    const uint8_t *G = o->g;
    o->hue = G[0];
    int h6 = G[0] * 6, region = h6 >> 8, f = h6 & 255;
    int p = 64, q = 255 - ((191 * f) >> 8), t = 64 + ((191 * f) >> 8);
    static const uint8_t pick[6][3] = {
        // which of 255, q, t, p goes to r, g, b: 0 = 255, 1 = t, 2 = p, 3 = q
        {0, 1, 2}, {3, 0, 2}, {2, 0, 1}, {2, 3, 0}, {1, 2, 0}, {0, 2, 3},
    };
    int v[4] = {255, t, p, q};
    o->r = (uint8_t)v[pick[region][0]];
    o->gr = (uint8_t)v[pick[region][1]];
    o->b = (uint8_t)v[pick[region][2]];
    o->body = G[1] & 7;
    o->eyes = (G[1] >> 3) & 7;
    o->marks = G[2] & 7;
    o->mark_seed = G[2] >> 3;
    int t1 = G[3] % 6;
    int t2 = (t1 + 1 + G[4] % 5) % 6;
    o->traits[0] = (uint8_t)t1;
    o->traits[1] = (uint8_t)t2;
    o->ntraits = 2;
    if (G[5] < 64) {
        int k = G[5] % 4;       // the k-th of the four traits left, ascending
        for (int tr = 0; tr < 6; tr++) {
            if (tr != t1 && tr != t2 && k-- == 0) {
                o->traits[2] = (uint8_t)tr;
                o->ntraits = 3;
                break;
            }
        }
    }
    o->vir = 1 + G[6] % 10;
    o->hard = 1 + G[7] % 10;
    o->stab = 1 + G[8] % 10;
    o->rarity = G[9] < 2 ? 3 : G[9] < 14 ? 2 : G[9] < 54 ? 1 : 0;
}

bool gg_has_trait(const gossip_genome_t *g, int trait)
{
    for (int i = 0; i < g->ntraits; i++) {
        if (g->traits[i] == trait) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Addresses and records (§3)

static void pda_hash(const char *kind, const uint8_t nonce8[8], const uint8_t program[32], int bump, uint8_t out[32])
{
    static const char tag[] = "ProgramDerivedAddress";
    uint8_t b = (uint8_t)bump;
    gg_sha256_t c;
    gg_sha256_init(&c);
    gg_sha256_update(&c, kind, strlen(kind));
    gg_sha256_update(&c, nonce8, 8);
    gg_sha256_update(&c, &b, 1);
    gg_sha256_update(&c, program, 32);
    gg_sha256_update(&c, tag, sizeof(tag) - 1);
    gg_sha256_final(&c, out);
}

bool gg_pda_ok(const char *kind, const uint8_t nonce8[8], const uint8_t program[32], int bump, const uint8_t addr[32])
{
    if (bump < 0 || bump > 255) {
        return false;
    }
    uint8_t h[32];
    pda_hash(kind, nonce8, program, bump, h);
    // Only the bump given is tried: a server that signed a later bump than the
    // first off-curve one names an address Solana's search wouldn't, but the
    // address is still a PDA nobody can sign for, which is all the game needs.
    return !memcmp(h, addr, 32) && !ed25519_on_curve(h);
}

int gg_pda_find(const char *kind, const uint8_t nonce8[8], const uint8_t program[32], uint8_t addr[32])
{
    for (int bump = 255; bump >= 0; bump--) {
        pda_hash(kind, nonce8, program, bump, addr);
        if (!ed25519_on_curve(addr)) {
            return bump;
        }
    }
    return -1;
}

int gg_record_msg(const gg_record_t *r, char *out, size_t n)
{
    int len = snprintf(out, n, "sos-gossip1|%s|%s|%d|%s|%d|%d", r->a, r->par, r->gen, r->seed, r->bump, r->fl);
    return len < 0 || (size_t)len >= n ? -1 : len;
}

bool gg_unhex(const char *hex, uint8_t *out, size_t n)
{
    if (!hex || strlen(hex) != n * 2) {
        return false;
    }
    for (size_t i = 0; i < n * 2; i++) {
        char c = hex[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) {
            return false;
        }
        out[i / 2] = (uint8_t)(i % 2 ? (out[i / 2] | v) : v << 4);
    }
    return true;
}

bool gg_record_shape_ok(const gg_record_t *r)
{
    uint8_t a[32], par[32], nonce[8];
    if (!gg_b58_decode32(r->a, a) || r->gen < 0 || r->gen > 1000000 || r->bump < 0 || r->bump > 255 || r->fl < 0
        || r->fl > 15 || strlen(r->seed) != 16 || !gg_unhex(r->seed, nonce, 8)) {
        return false;
    }
    for (const char *c = r->seed; *c; c++) {
        if (*c >= 'A' && *c <= 'F') {
            return false;       // the signed message has it as the server wrote it: lowercase
        }
    }
    if (r->gen == 0) {
        return !r->par[0] && gg_pda_ok("root", nonce, gg_program, r->bump, a);
    }
    return gg_b58_decode32(r->par, par) && gg_pda_ok("fork", nonce, par, r->bump, a);
}

// ---------------------------------------------------------------------------
// The HELLO's g (§4)

bool gg_g_parse(const char *g, gg_g_t *o)
{
    if (!g || !g[0] || !strchr("rvibn", g[0]) || g[1] < '0' || g[1] > '9' || g[2] != '.') {
        return false;
    }
    o->c = g[0];
    o->b = g[1] - '0';
    const char *p = g + 3;
    int digits = 0;
    int64_t w = 0;
    while (*p >= '0' && *p <= '9') {
        if (digits == 0 && *p == '0' && p[1] >= '0' && p[1] <= '9') {
            return false;       // one way to write each number: the tag covers the text
        }
        w = w * 10 + (*p - '0');
        p++;
        if (++digits > 12) {
            return false;
        }
    }
    if (!digits || *p != '.') {
        return false;
    }
    p++;
    if (strlen(p) != 16) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) {
            return false;
        }
    }
    o->w = w;
    memcpy(o->tag, p, 17);
    return true;
}

int gg_tag_msg(char *out, size_t n, const char *id, const char *st, int64_t w, char c, int b)
{
    int len = snprintf(out, n, "gossip|%s|%s|%lld|%c%d", id, st, (long long)w, c, b);
    return len < 0 || (size_t)len >= n ? -1 : len;
}
