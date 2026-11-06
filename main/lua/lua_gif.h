#pragma once

// A GIF decoder for Lua apps' images and animations (lua_img.c), with no Lua
// or LVGL in it so the host test (tests/png/) builds it as is.
//
// It draws one frame at a time onto a w x h RGB565 canvas (LH_PX_CLEAR is
// see-through), the way a browser composes them: a frame's transparent
// pixels leave what was there, and its disposal (2: back to the background)
// happens once the next frame is found, so the last frame stays. "Background"
// is see-through, as browsers do it, unless the GIF is opaque: a frame with
// no transparent index and a background colour in the global table goes back
// to that colour, so moving parts of an opaque GIF leave no holes. Disposal 3 ("restore previous") would need
// a second canvas, so it's treated as 1 (leave it). The file streams through
// a 256-byte buffer from wherever the caller left it, so an animation is
// played by keeping the offset of the next frame, not the file.
//
// The file is untrusted: every code, length and position is checked; a bad
// file is an error string, never a read or write out of bounds. A frame must
// fit the logical screen (the canvas), so the pixels decoded per frame, the
// work an animation step does under the LVGL lock, are bounded by the canvas
// the app's heap already paid for.

#include <stdbool.h>
#include <stdint.h>
#include "lua_png.h"

#define LH_GIF_DELAY_MIN    20      // ms; shorter delays play at this pace
#define LH_GIF_DELAY_ZERO   100     // ms for a delay of 0 or 1 (what browsers do)

typedef struct {
    uint16_t w, h;                  // the logical screen: the canvas
    uint16_t gct[256];              // global colour table as RGB565
    uint16_t ngct;
    int32_t loops;                  // NETSCAPE loop count (0 to 65535, 0: forever); -1 if the file has none
    uint16_t delay_ms;              // of the frame drawn last
    uint8_t disposal;               // of the frame drawn last, applied when the next is found
    uint8_t bg;                     // background colour index (global table)
    uint16_t dispose_px;            // what disposal 2 fills with: LH_PX_CLEAR or the background colour
    uint16_t fx, fy, fw, fh;        // where the frame drawn last was
} lh_gif_t;

// Scratch while decoding (about 13 KB): the LZW table and the input buffer.
typedef struct {
    uint16_t prefix[4096];
    uint8_t suffix[4096];
    uint16_t lct[256];
    lh_png_read_t read;
    void *ud;
    uint8_t in[256];
    uint16_t in_pos, in_len;
    uint32_t pos;                   // file offset of the next byte the decoder takes
    uint8_t block_left;             // of the current LZW data sub-block
    bool blocks_done;
    uint32_t bits;
    int nbits;
} lh_gif_work_t;

// Reads the header and global colour table from the start of the file.
// *first: offset of what follows (the first frame's blocks). NULL, or why not.
const char *lh_gif_open(lh_gif_t *g, lh_gif_work_t *w, lh_png_read_t read, void *ud, uint32_t *first);

// Draws the next frame onto canvas (g->w * g->h), reading from offset *pos,
// where the caller has put the file; *pos is then the offset after it.
// *done: the file ended before another frame (nothing drawn). NULL, or why not.
const char *lh_gif_frame(lh_gif_t *g, lh_gif_work_t *w, lh_png_read_t read, void *ud, uint16_t *canvas,
                         uint32_t *pos, bool *done);
