// GIF -> RGB565, one frame at a time (lua_gif.h). The LZW table is the usual
// prefix/suffix pair; a code's string is written straight to its pixels from
// the end back (its length found by a first walk), so there is no stack.

#include <string.h>
#include "lua_gif.h"

// ---------------------------------------------------------------------------
// The file, a byte at a time through w->in

static int rd_byte(lh_gif_work_t *w)
{
    if (w->in_pos == w->in_len) {
        w->in_len = (uint16_t)w->read(w->ud, w->in, sizeof(w->in));
        w->in_pos = 0;
        if (!w->in_len) {
            return -1;
        }
    }
    w->pos++;
    return w->in[w->in_pos++];
}

static bool rd_bytes(lh_gif_work_t *w, uint8_t *dst, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
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

// Skips data sub-blocks up to and including the empty one that ends them.
static bool skip_blocks(lh_gif_work_t *w)
{
    for (;;) {
        int n = rd_byte(w);
        if (n < 0) {
            return false;
        }
        if (!n) {
            return true;
        }
        if (!rd_bytes(w, NULL, (unsigned)n)) {
            return false;
        }
    }
}

static bool read_table(lh_gif_work_t *w, uint16_t *pal, unsigned n)
{
    uint8_t c[3];
    for (unsigned i = 0; i < n; i++) {
        if (!rd_bytes(w, c, 3)) {
            return false;
        }
        pal[i] = lh_px_rgb(c[0], c[1], c[2]);
    }
    for (unsigned i = n; i < 256; i++) {
        pal[i] = 0;
    }
    return true;
}

static void start(lh_gif_work_t *w, lh_png_read_t read, void *ud, uint32_t pos)
{
    w->read = read;
    w->ud = ud;
    w->in_pos = w->in_len = 0;
    w->pos = pos;
}

const char *lh_gif_open(lh_gif_t *g, lh_gif_work_t *w, lh_png_read_t read, void *ud, uint32_t *first)
{
    uint8_t h[13];
    start(w, read, ud, 0);
    if (!rd_bytes(w, h, 13) || memcmp(h, "GIF8", 4) || (h[4] != '7' && h[4] != '9') || h[5] != 'a') {
        return "not a GIF";
    }
    memset(g, 0, sizeof(*g));
    g->w = (uint16_t)(h[6] | h[7] << 8);
    g->h = (uint16_t)(h[8] | h[9] << 8);
    if (g->w < 1 || g->h < 1 || g->w > LH_PNG_DIM_MAX || g->h > LH_PNG_DIM_MAX) {
        return "too big (1024 x 1024 at most)";
    }
    g->loops = -1;
    if (h[10] & 0x80) {
        g->ngct = (uint16_t)(2u << (h[10] & 7));
        if (!read_table(w, g->gct, g->ngct)) {
            return "the file ends early";
        }
    }
    *first = w->pos;
    return NULL;
}

// ---------------------------------------------------------------------------
// LZW

static int data_byte(lh_gif_work_t *w)
{
    if (w->blocks_done) {
        return -1;
    }
    if (!w->block_left) {
        int n = rd_byte(w);
        if (n <= 0) {
            w->blocks_done = true;
            return -1;
        }
        w->block_left = (uint8_t)n;
    }
    w->block_left--;
    return rd_byte(w);
}

// The next code, or -1 when the data runs out.
static int code_of(lh_gif_work_t *w, int size)
{
    while (w->nbits < size) {
        int b = data_byte(w);
        if (b < 0) {
            return -1;
        }
        w->bits |= (uint32_t)b << w->nbits;
        w->nbits += 8;
    }
    int c = (int)(w->bits & ((1u << size) - 1));
    w->bits >>= size;
    w->nbits -= size;
    return c;
}

typedef struct {
    lh_gif_t *g;
    uint16_t *canvas;
    const uint16_t *pal;
    int transparent;
    uint16_t fx, fy, fw, fh;
    bool interlaced;
    uint32_t n, total;              // pixels written so far, in the frame
    uint32_t pass_rows[4];          // interlaced: rows before each pass
} frame_t;

static uint32_t row_of(const frame_t *f, uint32_t r)
{
    if (!f->interlaced) {
        return r;
    }
    static const uint8_t START[4] = {0, 4, 2, 1}, STEP[4] = {8, 8, 4, 2};
    for (int p = 3; p >= 0; p--) {
        if (r >= f->pass_rows[p]) {
            return START[p] + (r - f->pass_rows[p]) * STEP[p];
        }
    }
    return r;
}

static void plot(const frame_t *f, uint32_t i, unsigned index)
{
    if (i >= f->total || (int)index == f->transparent) {
        return;
    }
    uint32_t x = f->fx + i % f->fw, y = f->fy + row_of(f, i / f->fw);
    if (x < f->g->w && y < f->g->h) {
        f->canvas[y * f->g->w + x] = f->pal[index];
    }
}

// Writes code's string at the frame's next pixels; returns its first index.
static unsigned put(frame_t *f, const lh_gif_work_t *w, unsigned code, unsigned base)
{
    uint32_t len = 1;
    unsigned c = code;
    while (c >= base) {
        c = w->prefix[c];
        len++;
    }
    unsigned first = c;
    c = code;
    for (uint32_t k = len; k-- > 0;) {
        plot(f, f->n + k, c >= base ? w->suffix[c] : c);
        if (c >= base) {
            c = w->prefix[c];
        }
    }
    f->n += len;
    return first;
}

static const char *lzw(frame_t *f, lh_gif_work_t *w, int min)
{
    if (min < 2 || min > 8) {
        return "bad GIF data";
    }
    const unsigned clear = 1u << min, eoi = clear + 1, base = clear + 2;
    unsigned next = base;
    int size = min + 1, prev = -1;
    unsigned prev_first = 0;
    w->block_left = 0;
    w->blocks_done = false;
    w->bits = 0;
    w->nbits = 0;
    for (;;) {
        int code = code_of(w, size);
        if (code < 0 || (unsigned)code == eoi) {
            break;                  // a missing end code is common: what's there counts
        }
        if ((unsigned)code == clear) {
            next = base;
            size = min + 1;
            prev = -1;
            continue;
        }
        if (prev < 0) {
            if ((unsigned)code >= clear) {
                return "bad GIF data";
            }
            prev_first = put(f, w, (unsigned)code, base);
            prev = code;
            continue;
        }
        if ((unsigned)code > next || ((unsigned)code == next && next >= 4096)) {
            return "bad GIF data";
        }
        unsigned first;
        if ((unsigned)code == next) {           // KwKwK: prev's string and its own first index
            w->prefix[next] = (uint16_t)prev;
            w->suffix[next] = (uint8_t)prev_first;
            next++;
            first = put(f, w, (unsigned)code, base);
        } else {
            first = put(f, w, (unsigned)code, base);
            if (next < 4096) {
                w->prefix[next] = (uint16_t)prev;
                w->suffix[next] = (uint8_t)first;
                next++;
            }
        }
        if (next == (1u << size) && size < 12) {
            size++;
        }
        prev = code;
        prev_first = first;
        if (f->n >= f->total) {
            break;                  // a full frame: ignore the rest
        }
    }
    // To the end of the data, wherever the codes stopped.
    if (!w->blocks_done && ((w->block_left && !rd_bytes(w, NULL, w->block_left)) || !skip_blocks(w))) {
        return "the file ends early";
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Frames

const char *lh_gif_frame(lh_gif_t *g, lh_gif_work_t *w, lh_png_read_t read, void *ud, uint16_t *canvas,
                         uint32_t *pos, bool *done)
{
    start(w, read, ud, *pos);
    *done = false;
    if (g->disposal == 2) {                 // the last frame goes back to see-through
        for (uint32_t y = g->fy; y < (uint32_t)g->fy + g->fh && y < g->h; y++) {
            for (uint32_t x = g->fx; x < (uint32_t)g->fx + g->fw && x < g->w; x++) {
                canvas[y * g->w + x] = LH_PX_CLEAR;
            }
        }
    }
    g->disposal = 0;
    unsigned disposal = 0, delay = 0;
    int transparent = -1;
    for (;;) {
        int b = rd_byte(w);
        if (b < 0 || b == 0x3B) {           // end of file or trailer
            *done = true;
            return NULL;
        }
        if (b == 0x21) {
            int label = rd_byte(w);
            uint8_t e[11];
            if (label == 0xF9) {            // graphic control: disposal, transparency, delay
                if (!rd_bytes(w, e, 1) || e[0] != 4 || !rd_bytes(w, e, 4)) {
                    return "bad GIF";
                }
                disposal = (e[0] >> 2) & 7;
                delay = (unsigned)(e[1] | e[2] << 8);
                transparent = e[0] & 1 ? e[3] : -1;
            } else if (label == 0xFF) {     // application: the NETSCAPE loop count
                int n = rd_byte(w);
                if (n != 11 || !rd_bytes(w, e, 11)) {
                    return "bad GIF";
                }
                if (!memcmp(e, "NETSCAPE2.0", 11) || !memcmp(e, "ANIMEXTS1.0", 11)) {
                    uint8_t sub[4];
                    if (!rd_bytes(w, sub, 1)) {
                        return "the file ends early";
                    }
                    if (sub[0] == 3) {
                        if (!rd_bytes(w, sub + 1, 3)) {
                            return "the file ends early";
                        }
                        if (sub[1] == 1) {
                            g->loops = (int16_t)(sub[2] | sub[3] << 8);
                        }
                    } else if (sub[0] && !rd_bytes(w, NULL, sub[0])) {
                        return "the file ends early";
                    }
                    if (sub[0] == 0) {
                        continue;
                    }
                }
            } else if (label < 0) {
                return "the file ends early";
            }
            if (!skip_blocks(w)) {
                return "the file ends early";
            }
            continue;
        }
        if (b != 0x2C) {
            return "bad GIF";
        }
        uint8_t d[9];
        if (!rd_bytes(w, d, 9)) {
            return "the file ends early";
        }
        frame_t f = {
            .g = g, .canvas = canvas, .transparent = transparent, .pal = g->gct,
            .fx = (uint16_t)(d[0] | d[1] << 8), .fy = (uint16_t)(d[2] | d[3] << 8),
            .fw = (uint16_t)(d[4] | d[5] << 8), .fh = (uint16_t)(d[6] | d[7] << 8),
            .interlaced = d[8] & 0x40,
        };
        if (d[8] & 0x80) {
            if (!read_table(w, w->lct, 2u << (d[8] & 7))) {
                return "the file ends early";
            }
            f.pal = w->lct;
        } else if (!g->ngct) {
            return "no colour table";
        }
        f.total = (uint32_t)f.fw * f.fh;
        uint32_t h = f.fh;
        f.pass_rows[0] = 0;
        f.pass_rows[1] = (h + 7) / 8;
        f.pass_rows[2] = f.pass_rows[1] + (h + 3) / 8;
        f.pass_rows[3] = f.pass_rows[2] + (h + 1) / 4;
        int min = rd_byte(w);
        if (min < 0) {
            return "the file ends early";
        }
        const char *err = f.total ? lzw(&f, w, min) : (skip_blocks(w) ? NULL : "the file ends early");
        if (err) {
            return err;
        }
        g->disposal = (uint8_t)(disposal == 2 ? 2 : 0);
        g->fx = f.fx;
        g->fy = f.fy;
        g->fw = f.fw;
        g->fh = f.fh;
        g->delay_ms = (uint16_t)(delay < 2 ? LH_GIF_DELAY_ZERO : delay * 10 < LH_GIF_DELAY_MIN ? LH_GIF_DELAY_MIN
                                 : delay * 10 > 60000 ? 60000 : delay * 10);
        *pos = w->pos;
        return NULL;
    }
}
