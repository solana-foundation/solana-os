// The benchmark's workloads (kernels.h). Written the way a C programmer would
// write them, nothing tuned for one compiler; the Lua version mirrors each
// step so the checksums match.
#include "kernels.h"

#include <string.h>

// --- Mandelbrot ------------------------------------------------------------------

#define FR 12 // fractional bits

uint32_t k_mandel_row(int y)
{
    int32_t ci = -4915 + (y * 9830) / K_MANDEL_H; // -1.2 .. 1.2
    uint32_t sum = 0;
    for (int x = 0; x < K_MANDEL_W; x++) {
        int32_t cr = -8192 + (x * 12288) / K_MANDEL_W; // -2 .. 1
        int32_t zr = 0, zi = 0;
        int n = 0;
        while (n < K_MANDEL_ITER) {
            int32_t zr2 = (zr * zr) >> FR, zi2 = (zi * zi) >> FR;
            if (zr2 + zi2 > (4 << FR)) {
                break;
            }
            zi = ((zr * zi) >> (FR - 1)) + ci;
            zr = zr2 - zi2 + cr;
            n++;
        }
        sum += (uint32_t)n;
    }
    return sum;
}

uint32_t k_mandelf_row(int y)
{
    float ci = -1.2f + (float)y * (2.4f / (float)K_MANDEL_H);
    uint32_t sum = 0;
    for (int x = 0; x < K_MANDEL_W; x++) {
        float cr = -2.0f + (float)x * (3.0f / (float)K_MANDEL_W);
        float zr = 0.0f, zi = 0.0f;
        int n = 0;
        while (n < K_MANDEL_ITER) {
            float zr2 = zr * zr, zi2 = zi * zi;
            if (zr2 + zi2 > 4.0f) {
                break;
            }
            zi = 2.0f * zr * zi + ci;
            zr = zr2 - zi2 + cr;
            n++;
        }
        sum += (uint32_t)n;
    }
    return sum;
}

uint32_t k_mandeld_row(int y)
{
    double ci = -1.2 + (double)y * (2.4 / (double)K_MANDEL_H);
    uint32_t sum = 0;
    for (int x = 0; x < K_MANDEL_W; x++) {
        double cr = -2.0 + (double)x * (3.0 / (double)K_MANDEL_W);
        double zr = 0.0, zi = 0.0;
        int n = 0;
        while (n < K_MANDEL_ITER) {
            double zr2 = zr * zr, zi2 = zi * zi;
            if (zr2 + zi2 > 4.0) {
                break;
            }
            zi = 2.0 * zr * zi + ci;
            zr = zr2 - zi2 + cr;
            n++;
        }
        sum += (uint32_t)n;
    }
    return sum;
}

// --- Sieve -----------------------------------------------------------------------

uint32_t k_sieve(uint8_t *flags)
{
    for (int i = 2; i <= K_SIEVE_N; i++) {
        flags[i] = 1;
    }
    for (int i = 2; i * i <= K_SIEVE_N; i++) {
        if (flags[i]) {
            for (int j = i * i; j <= K_SIEVE_N; j += i) {
                flags[j] = 0;
            }
        }
    }
    uint32_t count = 0;
    for (int i = 2; i <= K_SIEVE_N; i++) {
        count += flags[i];
    }
    return count;
}

// --- SHA-256 ---------------------------------------------------------------------

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

void k_sha_init(uint32_t h[8])
{
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h, h0, sizeof h0);
}

void k_sha_block(uint32_t h[8], const uint32_t m[16])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = m[i];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
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

void k_sha_msg_words(uint32_t *w)
{
    for (int i = 0; i < K_SHA_MSG / 4; i++) {
        uint32_t v = 0;
        for (int j = 0; j < 4; j++) {
            v = (v << 8) | (uint32_t)(((i * 4 + j) * 7 + 3) & 255);
        }
        w[i] = v;
    }
}

uint32_t k_sha_msg(void)
{
    static uint32_t w[K_SHA_MSG / 4];
    uint32_t h[8], pad[16] = {0x80000000u};
    k_sha_msg_words(w);
    k_sha_init(h);
    for (int b = 0; b < K_SHA_MSG / 64; b++) {
        k_sha_block(h, w + b * 16);
    }
    pad[15] = K_SHA_MSG * 8; // the message is a whole number of blocks
    k_sha_block(h, pad);
    return h[0];
}

// --- Memory ----------------------------------------------------------------------

void k_fill(uint32_t *a, uint32_t base, uint32_t n, uint32_t k)
{
    for (uint32_t i = 0; i < n; i++) {
        a[i] = (i + base) ^ k;
    }
}

void k_copy(uint32_t *dst, const uint32_t *src, uint32_t n)
{
    memcpy(dst, src, n * sizeof(uint32_t));
}

uint32_t k_sum(const uint32_t *a, uint32_t n)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) {
        s += a[i];
    }
    return s;
}

// --- Drawing ---------------------------------------------------------------------

uint32_t k_draw_row(uint16_t *row, int w, int y, int t)
{
    uint32_t sum = 0;
    for (int x = 0; x < w; x++) {
        uint32_t r = (uint32_t)(x + t) & 31, g = (uint32_t)((y << 1) + t) & 63, b = ((uint32_t)(x ^ y) >> 3) & 31;
        uint16_t px = (uint16_t)((r << 11) | (g << 5) | b);
        row[x] = px;
        sum += px;
    }
    return sum;
}

// --- Particles -------------------------------------------------------------------

#define XMAX ((K_PART_W - 2) << 8)
#define YMAX ((K_PART_H - 2) << 8)

const uint16_t k_part_colors[8] = {0x9A3F, 0x178A, 0xEAB7, 0xB59F, 0x6D5E, 0x877F, 0xAFF3, 0xEF90};

void k_part_init(k_part *p)
{
    for (int i = 0; i < K_PART_N; i++) {
        p[i].x = ((i * 97) % (K_PART_W - 2)) << 8;
        p[i].y = ((i * 53) % (K_PART_H - 8)) << 8;
        p[i].vx = ((i * 29) % 200) - 100;
        p[i].vy = 0;
    }
}

void k_part_step(k_part *p, int i)
{
    k_part *q = &p[i];
    q->vy += 6; // gravity
    q->x += q->vx;
    q->y += q->vy;
    if (q->x < 0) {
        q->x = -q->x;
        q->vx = -q->vx;
    } else if (q->x > XMAX) {
        q->x = 2 * XMAX - q->x;
        q->vx = -q->vx;
    }
    if (q->y > YMAX) {
        q->y = 2 * YMAX - q->y;
        q->vy = -((q->vy * 15) >> 4); // loses a little at each bounce
        if (q->vy > -96) {
            q->vy = -(400 + ((i * 37) & 127)); // and is kicked up again
        }
    }
}

uint32_t k_part_sum(const k_part *p)
{
    uint32_t s = 0;
    for (int i = 0; i < K_PART_N; i++) {
        s += (uint32_t)p[i].x + (uint32_t)p[i].y * 3u + (uint32_t)p[i].vy;
    }
    return s;
}

void k_part_draw(uint16_t *frame, const k_part *p, int i)
{
    int x = p[i].x >> 8, y = p[i].y >> 8;
    if (y < 0) {
        return; // above the box: not drawn (the Lua canvas clips the same)
    }
    uint16_t c = k_part_colors[i & 7];
    uint16_t *o = frame + y * K_PART_W + x;
    o[0] = c;
    o[1] = c;
    o[K_PART_W] = c;
    o[K_PART_W + 1] = c;
}
