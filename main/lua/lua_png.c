// PNG -> RGB565 for Lua apps' images (lua_png.h). Inflate follows RFC 1951
// the way tinf does (canonical Huffman decoding one bit at a time): small and
// plenty fast for pictures that fit an app's 96 KB.

#include <stdlib.h>
#include <string.h>
#include "lua_png.h"

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static unsigned channels(uint8_t color)
{
    switch (color) {
    case 2: return 3;
    case 4: return 2;
    case 6: return 4;
    default: return 1;      // 0 grey, 3 palette
    }
}

const char *lh_png_info(lh_png_read_t read, void *ud, lh_png_info_t *info)
{
    static const uint8_t sig[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    uint8_t b[33];
    size_t got = 0;
    while (got < sizeof(b)) {
        size_t n = read(ud, b + got, sizeof(b) - got);
        if (!n) {
            break;
        }
        got += n;
    }
    if (got < sizeof(b) || memcmp(b, sig, 8) || be32(b + 8) != 13 || memcmp(b + 12, "IHDR", 4)) {
        return "not a PNG";
    }
    uint32_t w = be32(b + 16), h = be32(b + 20);
    uint8_t depth = b[24], color = b[25];
    if (w < 1 || h < 1 || w > LH_PNG_DIM_MAX || h > LH_PNG_DIM_MAX) {
        return "too big (1024 x 1024 at most)";
    }
    bool ok;
    switch (color) {
    case 0: ok = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16; break;
    case 3: ok = depth == 1 || depth == 2 || depth == 4 || depth == 8; break;
    case 2: case 4: case 6: ok = depth == 8 || depth == 16; break;
    default: ok = false;
    }
    if (!ok || b[26] || b[27]) {
        return "not a valid PNG";
    }
    if (b[28]) {
        return "interlaced (save it without interlacing)";
    }
    *info = (lh_png_info_t){ .w = (uint16_t)w, .h = (uint16_t)h, .depth = depth, .color = color };
    return NULL;
}

static size_t row_bytes(const lh_png_info_t *info)
{
    return ((size_t)info->w * channels(info->color) * info->depth + 7) / 8;
}

size_t lh_png_raw_size(const lh_png_info_t *info)
{
    return (size_t)info->h * (1 + row_bytes(info));
}

// ---------------------------------------------------------------------------
// The file, a byte at a time through w->in

static int rd_byte(lh_png_work_t *w)
{
    if (w->in_pos == w->in_len) {
        w->in_len = (uint16_t)w->read(w->ud, w->in, sizeof(w->in));
        w->in_pos = 0;
        if (!w->in_len) {
            return -1;
        }
    }
    return w->in[w->in_pos++];
}

static bool rd_bytes(lh_png_work_t *w, uint8_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        int b = rd_byte(w);
        if (b < 0) {
            return false;
        }
        if (dst) {
            dst[i] = (uint8_t)b;
        }
    }
    return true;
}

// The next byte of the zlib stream, across IDAT chunks; -1 after the last one.
static int idat_byte(lh_png_work_t *w)
{
    while (!w->chunk_left) {
        uint8_t hdr[8];
        if (w->idat_done || !rd_bytes(w, NULL, 4) || !rd_bytes(w, hdr, 8) || memcmp(hdr + 4, "IDAT", 4)) {
            w->idat_done = true;
            return -1;
        }
        w->chunk_left = be32(hdr);
    }
    int b = rd_byte(w);
    if (b < 0) {
        w->idat_done = true;
        w->chunk_left = 0;
        return -1;
    }
    w->chunk_left--;
    return b;
}

static int getbit(lh_png_work_t *w)
{
    if (!w->nbits) {
        int b = idat_byte(w);
        if (b < 0) {
            if (!w->err) {
                w->err = "the image data ends early";
            }
            return 0;
        }
        w->bits = (uint32_t)b;
        w->nbits = 8;
    }
    int bit = w->bits & 1;
    w->bits >>= 1;
    w->nbits--;
    return bit;
}

static unsigned getbits(lh_png_work_t *w, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) {
        v |= (unsigned)getbit(w) << i;
    }
    return v;
}

// ---------------------------------------------------------------------------
// Inflate

static void build(lh_huff_t *t, const uint8_t *len, int n)
{
    uint16_t offs[16];
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < n; i++) {
        t->counts[len[i]]++;
    }
    t->counts[0] = 0;
    for (int i = 0, sum = 0; i < 16; i++) {
        offs[i] = (uint16_t)sum;
        sum += t->counts[i];
    }
    for (int i = 0; i < n; i++) {
        if (len[i]) {
            t->symbols[offs[len[i]]++] = (uint16_t)i;
        }
    }
}

// sum + cur always lands among the symbols of length len, so a broken code
// (over- or under-full) reads a wrong symbol or fails, never out of bounds.
static int decode(lh_png_work_t *w, const lh_huff_t *t)
{
    int cur = 0, sum = 0;
    for (int len = 1; len < 16; len++) {
        cur = 2 * cur + getbit(w);
        sum += t->counts[len];
        cur -= t->counts[len];
        if (cur < 0) {
            return t->symbols[sum + cur];
        }
    }
    w->err = "bad image data";
    return -1;
}

static const uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                      67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                      5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                       513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                       11, 11, 12, 12, 13, 13};

static void fixed_trees(lh_png_work_t *w)
{
    uint8_t *l = w->lengths;
    memset(l, 8, 144);
    memset(l + 144, 9, 112);
    memset(l + 256, 7, 24);
    memset(l + 280, 8, 8);
    build(&w->lit, l, 288);
    memset(l, 5, 30);
    build(&w->dist, l, 30);
}

static void dynamic_trees(lh_png_work_t *w)
{
    static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    unsigned hlit = getbits(w, 5) + 257, hdist = getbits(w, 5) + 1, hclen = getbits(w, 4) + 4;
    if (hlit > 286 || hdist > 30) {
        w->err = "bad image data";
        return;
    }
    uint8_t *l = w->lengths;
    memset(l, 0, 19);
    for (unsigned i = 0; i < hclen; i++) {
        l[ORDER[i]] = (uint8_t)getbits(w, 3);
    }
    build(&w->lit, l, 19);          // the code-length code, for now
    unsigned total = hlit + hdist;
    for (unsigned n = 0; n < total && !w->err;) {
        int sym = decode(w, &w->lit);
        if (sym < 0) {
            return;
        }
        if (sym < 16) {
            l[n++] = (uint8_t)sym;
            continue;
        }
        unsigned rep;
        uint8_t v = 0;
        if (sym == 16) {
            if (!n) {
                w->err = "bad image data";
                return;
            }
            v = l[n - 1];
            rep = 3 + getbits(w, 2);
        } else if (sym == 17) {
            rep = 3 + getbits(w, 3);
        } else {
            rep = 11 + getbits(w, 7);
        }
        if (rep > total - n) {
            w->err = "bad image data";
            return;
        }
        memset(l + n, v, rep);
        n += rep;
    }
    if (w->err) {
        return;
    }
    if (!l[256]) {
        w->err = "bad image data";
        return;
    }
    build(&w->lit, l, (int)hlit);
    build(&w->dist, l + hlit, (int)hdist);
}

static void inflate_block(lh_png_work_t *w, uint8_t *out, size_t *pos, size_t size)
{
    while (!w->err) {
        int sym = decode(w, &w->lit);
        if (sym < 0) {
            return;
        }
        if (sym < 256) {
            if (*pos >= size) {
                w->err = "more image data than pixels";
                return;
            }
            out[(*pos)++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) {
            return;
        }
        sym -= 257;
        if (sym >= 29) {
            w->err = "bad image data";
            return;
        }
        size_t len = LEN_BASE[sym] + getbits(w, LEN_EXTRA[sym]);
        int d = decode(w, &w->dist);
        if (d < 0) {
            return;
        }
        if (d >= 30) {
            w->err = "bad image data";
            return;
        }
        size_t dist = DIST_BASE[d] + getbits(w, DIST_EXTRA[d]);
        if (w->err) {
            return;
        }
        if (dist > *pos || len > size - *pos) {
            w->err = "bad image data";
            return;
        }
        for (size_t i = 0; i < len; i++, (*pos)++) {
            out[*pos] = out[*pos - dist];
        }
    }
}

static const char *inflate(lh_png_work_t *w, uint8_t *out, size_t size)
{
    unsigned cmf = getbits(w, 8), flg = getbits(w, 8);
    if (w->err || (cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 || (flg & 0x20)) {
        return w->err ? w->err : "bad image data";
    }
    size_t pos = 0;
    int last;
    do {
        last = getbit(w);
        unsigned type = getbits(w, 2);
        if (w->err) {
            break;
        }
        if (type == 0) {
            w->nbits = 0;           // stored: to the byte boundary
            unsigned len = getbits(w, 16), nlen = getbits(w, 16);
            if (!w->err && ((len ^ 0xFFFF) != nlen || len > size - pos)) {
                w->err = "bad image data";
            }
            for (unsigned i = 0; i < len && !w->err; i++) {
                out[pos++] = (uint8_t)getbits(w, 8);
            }
        } else if (type == 1) {
            fixed_trees(w);
            inflate_block(w, out, &pos, size);
        } else if (type == 2) {
            dynamic_trees(w);
            if (!w->err) {
                inflate_block(w, out, &pos, size);
            }
        } else {
            w->err = "bad image data";
        }
    } while (!last && !w->err);
    if (w->err) {
        return w->err;
    }
    return pos == size ? NULL : "the image data ends early";
}

// ---------------------------------------------------------------------------
// Chunks, filters, pixels

// Chunks up to the first IDAT: PLTE and tRNS kept, the rest skipped.
static const char *read_chunks(lh_png_work_t *w, const lh_png_info_t *info)
{
    for (;;) {
        uint8_t hdr[8];
        if (!rd_bytes(w, hdr, 8)) {
            return "the file ends early";
        }
        uint32_t len = be32(hdr);
        const uint8_t *type = hdr + 4;
        if (len > 0x7FFFFFFF) {
            return "not a valid PNG";
        }
        if (!memcmp(type, "IDAT", 4)) {
            w->chunk_left = len;
            return NULL;
        }
        if (!memcmp(type, "IEND", 4)) {
            return "no image data";
        }
        uint8_t b[6];
        if (!memcmp(type, "PLTE", 4) && info->color == 3) {
            if (!len || len % 3 || len > 768) {
                return "not a valid PNG";
            }
            w->npal = (uint16_t)(len / 3);
            for (unsigned i = 0; i < w->npal; i++) {
                if (!rd_bytes(w, b, 3)) {
                    return "the file ends early";
                }
                w->pal[i] = lh_px_rgb(b[0], b[1], b[2]);
            }
        } else if (!memcmp(type, "tRNS", 4) && info->color == 3 && len <= 256) {
            for (unsigned i = 0; i < len; i++) {
                if (!rd_bytes(w, b, 1)) {
                    return "the file ends early";
                }
                if (i < w->npal && b[0] < 128) {
                    w->pal[i] = LH_PX_CLEAR;
                }
            }
        } else if (!memcmp(type, "tRNS", 4) && (info->color == 0 || info->color == 2)
                   && len == (info->color ? 6u : 2u)) {
            if (!rd_bytes(w, b, len)) {
                return "the file ends early";
            }
            for (unsigned i = 0; i < len / 2; i++) {
                w->key[i] = (uint16_t)(b[2 * i] << 8 | b[2 * i + 1]);
            }
            w->has_key = true;
        } else if (!rd_bytes(w, NULL, len)) {
            return "the file ends early";
        }
        if (!rd_bytes(w, NULL, 4)) {            // CRC
            return "the file ends early";
        }
    }
}

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static bool unfilter(uint8_t *cur, const uint8_t *prev, size_t n, size_t bpp, uint8_t f)
{
    for (size_t i = 0; i < n; i++) {
        int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
        switch (f) {
        case 0: return true;
        case 1: cur[i] = (uint8_t)(cur[i] + a); break;
        case 2: cur[i] = (uint8_t)(cur[i] + b); break;
        case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
        case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
        default: return false;
        }
    }
    return true;
}

static unsigned sample(const uint8_t *row, unsigned idx, unsigned depth)
{
    switch (depth) {
    case 8: return row[idx];
    case 16: return (unsigned)row[idx * 2] << 8 | row[idx * 2 + 1];
    default: {
        unsigned bit = idx * depth;
        return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
    }
    }
}

static uint8_t to8(unsigned v, unsigned depth)
{
    return (uint8_t)(depth == 16 ? v >> 8 : depth == 8 ? v : v * 255 / ((1u << depth) - 1));
}

static void convert(const lh_png_work_t *w, const lh_png_info_t *info, const uint8_t *row, uint16_t *out)
{
    unsigned d = info->depth;
    for (unsigned x = 0; x < info->w; x++) {
        unsigned r, g, b, a;
        switch (info->color) {
        case 3: {
            unsigned i = sample(row, x, d);
            out[x] = i < w->npal ? w->pal[i] : 0;
            continue;
        }
        case 0:
            r = g = b = sample(row, x, d);
            a = w->has_key && r == w->key[0] ? 0 : 255;
            break;
        case 2:
            r = sample(row, 3 * x, d), g = sample(row, 3 * x + 1, d), b = sample(row, 3 * x + 2, d);
            a = w->has_key && r == w->key[0] && g == w->key[1] && b == w->key[2] ? 0 : 255;
            break;
        case 4:
            r = g = b = sample(row, 2 * x, d);
            a = to8(sample(row, 2 * x + 1, d), d);
            break;
        default:
            r = sample(row, 4 * x, d), g = sample(row, 4 * x + 1, d), b = sample(row, 4 * x + 2, d);
            a = to8(sample(row, 4 * x + 3, d), d);
        }
        out[x] = a < 128 ? LH_PX_CLEAR : lh_px_rgb(to8(r, d), to8(g, d), to8(b, d));
    }
}

const char *lh_png_decode(lh_png_read_t read, void *ud, const lh_png_info_t *info, lh_png_work_t *w,
                          uint8_t *raw, uint16_t *out)
{
    memset(w, 0, sizeof(*w));
    w->read = read;
    w->ud = ud;
    const char *err = read_chunks(w, info);
    if (!err && info->color == 3 && !w->npal) {
        err = "no palette";
    }
    size_t bpr = row_bytes(info), size = lh_png_raw_size(info);
    if (!err) {
        err = inflate(w, raw, size);
    }
    if (err) {
        return err;
    }
    size_t bpp = channels(info->color) * info->depth / 8;
    bpp = bpp ? bpp : 1;
    for (unsigned y = 0; y < info->h; y++) {
        uint8_t *row = raw + y * (bpr + 1);
        if (!unfilter(row + 1, y ? row + 1 - (bpr + 1) : NULL, bpr, bpp, row[0])) {
            return "bad image data";
        }
        convert(w, info, row + 1, out + (size_t)y * info->w);
    }
    return NULL;
}
