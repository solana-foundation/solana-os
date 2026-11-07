#pragma once

// A small PNG decoder for Lua apps' images (lua_img.c), with no Lua or LVGL
// in it so the host test (tests/png/) builds it as is.
//
// It decodes into RGB565 with one value, LH_PX_CLEAR, kept for see-through
// pixels: a pixel less than half opaque is see-through, the rest are solid.
// Every PNG colour type and bit depth works; interlaced PNGs don't (Adam7
// needs the whole image twice). Memory: the caller hands in the inflated
// rows (lh_png_raw_size(), the LZ window) and the result, so a picture
// costs what it says up front; the file streams through a 256-byte buffer.
//
// The file is untrusted: every length, code and back-reference is checked,
// and a bad file is an error string, never a read or write out of bounds.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LH_PX_CLEAR     0x0001u     // see-through; a real colour that lands here becomes 0x0000
#define LH_PNG_DIM_MAX  1024        // pixels per side

// Reads up to n bytes of the file from where the last read stopped; returns
// how many it read (0 at the end or on an error).
typedef size_t (*lh_png_read_t)(void *ud, uint8_t *buf, size_t n);

typedef struct {
    uint16_t w, h;
    uint8_t depth, color;       // IHDR bit depth and colour type
} lh_png_info_t;

// Huffman table: codes per length and the symbols in code order.
typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
} lh_huff_t;

// Working state, about 2.3 KB: kept off the C stack (the "lua" task's is 12 KB).
typedef struct {
    lh_png_read_t read;
    void *ud;
    uint8_t in[256];
    uint16_t in_pos, in_len;
    uint32_t chunk_left;        // bytes of the current IDAT not read yet
    bool idat_done;
    uint32_t bits;
    int nbits;
    const char *err;
    lh_huff_t lit, dist;
    uint8_t lengths[288 + 32];
    uint16_t pal[256];          // PLTE as RGB565, LH_PX_CLEAR where tRNS says see-through
    uint16_t npal;
    bool has_key;               // tRNS for grey or RGB: one see-through colour
    uint16_t key[3];
} lh_png_work_t;

// Reads the signature and IHDR (the first 33 bytes). NULL, or why it can't be decoded.
const char *lh_png_info(lh_png_read_t read, void *ud, lh_png_info_t *info);

// Bytes of inflated rows for this image: h * (1 + bytes per row).
size_t lh_png_raw_size(const lh_png_info_t *info);

// Decodes the rest of the file (read continues right after the 33 bytes
// lh_png_info read) into out (w * h RGB565, row by row). raw: lh_png_raw_size()
// bytes of scratch. NULL, or why it failed.
const char *lh_png_decode(lh_png_read_t read, void *ud, const lh_png_info_t *info, lh_png_work_t *w,
                          uint8_t *raw, uint16_t *out);

// 0xRRGGBB as RGB565, never LH_PX_CLEAR.
static inline uint16_t lh_px_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return v == LH_PX_CLEAR ? 0 : v;
}
