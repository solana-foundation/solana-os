/*******************************************************************************
 * Size: 8 px
 * Bpp: 4
 * Opts: --no-compress --no-prefilter --bpp 4 --size 8 --font node_modules/@fontsource/geist/files/geist-latin-700-normal.woff --range 0x20-0x5F,0xB7 --format lvgl --lv-include lvgl.h --force-fast-kern-format -o main/assets/stream_caps_8.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef STREAM_CAPS_8
#define STREAM_CAPS_8 1
#endif

#if STREAM_CAPS_8

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */

    /* U+0021 "!" */
    0x9a, 0x9a, 0x89, 0x78, 0x67, 0xab,

    /* U+0022 "\"" */
    0xa6, 0xc9, 0x6b, 0x43, 0x50,

    /* U+0023 "#" */
    0xd, 0x4e, 0x29, 0xff, 0xf9, 0x3f, 0x5d, 0xd,
    0xff, 0xf4, 0x8b, 0x9a, 0x8, 0x8a, 0x70,

    /* U+0024 "$" */
    0x0, 0x0, 0x0, 0x9, 0x0, 0x1b, 0xfd, 0x26,
    0xda, 0x9a, 0x3f, 0xe5, 0x0, 0x2b, 0xda, 0x7d,
    0xa7, 0xe0, 0xaf, 0xe5, 0x0, 0x90, 0x0,

    /* U+0025 "%" */
    0x5c, 0x90, 0x73, 0xa, 0x2d, 0x29, 0x0, 0x4b,
    0x7a, 0x10, 0x0, 0x6, 0x6a, 0xa0, 0x1, 0xa4,
    0x89, 0x40, 0x92, 0x1c, 0xc1,

    /* U+0026 "&" */
    0xa, 0xfc, 0x0, 0x1f, 0x5f, 0x40, 0xc, 0xca,
    0x0, 0x5e, 0xd8, 0xb3, 0xab, 0x5f, 0xe1, 0x4e,
    0xfc, 0xf3,

    /* U+0027 "'" */
    0xa4, 0x93, 0x41,

    /* U+0028 "(" */
    0xa, 0x42, 0xf0, 0x7b, 0x9, 0x90, 0xa9, 0x8,
    0xb0, 0x3e, 0x0, 0xd4,

    /* U+0029 ")" */
    0x94, 0x6, 0xc0, 0x2f, 0x0, 0xf3, 0xf, 0x31,
    0xf1, 0x5d, 0xb, 0x60,

    /* U+002A "*" */
    0x0, 0x0, 0x2c, 0x80, 0xaf, 0xe1, 0x2b, 0x80,
    0x0, 0x0,

    /* U+002B "+" */
    0x3, 0xc0, 0x0, 0x3c, 0x0, 0xaf, 0xff, 0x31,
    0x5c, 0x20, 0x1, 0x50, 0x0,

    /* U+002C "," */
    0x43, 0xb9, 0x64,

    /* U+002D "-" */
    0xaf, 0xf0, 0x12, 0x20,

    /* U+002E "." */
    0x43, 0xb9,

    /* U+002F "/" */
    0x0, 0x8a, 0x0, 0xd5, 0x1, 0xf1, 0x6, 0xc0,
    0xb, 0x70, 0xf, 0x20, 0x4e, 0x0, 0x99, 0x0,

    /* U+0030 "0" */
    0x9, 0xfd, 0x30, 0x5f, 0x49, 0xe0, 0x9a, 0x2,
    0xf2, 0x9a, 0x1, 0xf2, 0x5e, 0x48, 0xe0, 0x9,
    0xfd, 0x40,

    /* U+0031 "1" */
    0x5, 0xca, 0xfc, 0x18, 0xc0, 0x7c, 0x7, 0xc0,
    0x7c,

    /* U+0032 "2" */
    0xa, 0xfd, 0x36, 0xe4, 0xaa, 0x1, 0x1c, 0x80,
    0x6e, 0x70, 0x2f, 0x52, 0x27, 0xff, 0xfc,

    /* U+0033 "3" */
    0x1b, 0xfd, 0x36, 0xb3, 0xc9, 0x0, 0xef, 0x20,
    0x2, 0xab, 0x8d, 0x3a, 0xc1, 0xbf, 0xd3,

    /* U+0034 "4" */
    0x0, 0x8f, 0x30, 0x3f, 0xf3, 0xc, 0x7f, 0x37,
    0xc0, 0xf3, 0xcf, 0xff, 0xe2, 0x22, 0xf5,

    /* U+0035 "5" */
    0x2f, 0xff, 0x84, 0xff, 0xf3, 0x5c, 0x3a, 0xb0,
    0x10, 0x5e, 0x6e, 0x3a, 0xb0, 0xaf, 0xd2,

    /* U+0036 "6" */
    0x8, 0xed, 0x24, 0xe3, 0x65, 0x8d, 0xec, 0x29,
    0xc0, 0xaa, 0x7c, 0x2b, 0xa0, 0xbf, 0xc2,

    /* U+0037 "7" */
    0xdf, 0xff, 0x52, 0x24, 0xe2, 0x0, 0xb6, 0x0,
    0x2f, 0x0, 0x7, 0xb0, 0x0, 0xa9, 0x0,

    /* U+0038 "8" */
    0x1c, 0xfd, 0x40, 0x6e, 0x3b, 0xb0, 0x1e, 0xff,
    0x40, 0x9b, 0x6, 0xe0, 0xac, 0x39, 0xf0, 0x2c,
    0xfd, 0x50,

    /* U+0039 "9" */
    0x1c, 0xfb, 0xa, 0xb2, 0xc7, 0xaa, 0xc, 0xa2,
    0xde, 0xda, 0x46, 0x3d, 0x61, 0xcf, 0xa0,

    /* U+003A ":" */
    0x1, 0x6e, 0x14, 0x0, 0x25, 0x6e,

    /* U+003B ";" */
    0x1, 0x6e, 0x14, 0x0, 0x25, 0x6e, 0x19,

    /* U+003C "<" */
    0x0, 0x0, 0x0, 0x6d, 0x5e, 0xc4, 0xbc, 0x10,
    0x29, 0xfa, 0x0, 0x18,

    /* U+003D "=" */
    0xaf, 0xff, 0x11, 0x22, 0x20, 0xaf, 0xff, 0x11,
    0x22, 0x20,

    /* U+003E ">" */
    0x0, 0x0, 0x9, 0x92, 0x0, 0x19, 0xf9, 0x0,
    0x7, 0xf1, 0x5e, 0xc4, 0x6, 0x40, 0x0,

    /* U+003F "?" */
    0x1b, 0xfb, 0x8, 0xb3, 0xe5, 0x0, 0x6e, 0x20,
    0x3f, 0x20, 0x2, 0x60, 0x0, 0x5f, 0x0,

    /* U+0040 "@" */
    0x1, 0x9c, 0xcb, 0x20, 0xb, 0x50, 0x2, 0xc0,
    0x48, 0x4d, 0xc8, 0x84, 0x93, 0xd3, 0x87, 0x65,
    0xa1, 0xe2, 0x97, 0x92, 0x84, 0x7d, 0x8d, 0x80,
    0x2c, 0x10, 0x11, 0x0, 0x4, 0xbc, 0xc3, 0x0,

    /* U+0041 "A" */
    0x0, 0xfc, 0x0, 0x4, 0xce, 0x20, 0xa, 0x68,
    0x70, 0xf, 0xff, 0xd0, 0x5c, 0x22, 0xe2, 0xa8,
    0x0, 0xb8,

    /* U+0042 "B" */
    0x6f, 0xfe, 0x70, 0x6d, 0x27, 0xe0, 0x6f, 0xef,
    0x70, 0x6d, 0x14, 0xf1, 0x6d, 0x25, 0xf2, 0x6f,
    0xff, 0x90,

    /* U+0043 "C" */
    0x8, 0xee, 0x80, 0x5f, 0x54, 0xf5, 0x9a, 0x0,
    0x21, 0x9a, 0x0, 0x32, 0x5f, 0x54, 0xf4, 0x8,
    0xef, 0x80,

    /* U+0044 "D" */
    0x6f, 0xfd, 0x40, 0x6d, 0x37, 0xf1, 0x6d, 0x0,
    0xe5, 0x6d, 0x0, 0xe5, 0x6d, 0x27, 0xf1, 0x6f,
    0xfd, 0x50,

    /* U+0045 "E" */
    0x6f, 0xff, 0x86, 0xd2, 0x21, 0x6f, 0xff, 0x76,
    0xd2, 0x21, 0x6d, 0x22, 0x16, 0xff, 0xf9,

    /* U+0046 "F" */
    0x6f, 0xff, 0x86, 0xd2, 0x21, 0x6f, 0xff, 0x56,
    0xd2, 0x20, 0x6d, 0x0, 0x6, 0xd0, 0x0,

    /* U+0047 "G" */
    0x8, 0xee, 0x90, 0x5f, 0x53, 0xa3, 0x9a, 0xd,
    0xe7, 0x9a, 0x1, 0xc8, 0x5f, 0x55, 0xf8, 0x8,
    0xfe, 0x88,

    /* U+0048 "H" */
    0x6d, 0x0, 0xf2, 0x6d, 0x0, 0xf2, 0x6f, 0xff,
    0xf2, 0x6d, 0x23, 0xf2, 0x6d, 0x0, 0xf2, 0x6d,
    0x0, 0xf2,

    /* U+0049 "I" */
    0x6d, 0x6d, 0x6d, 0x6d, 0x6d, 0x6d,

    /* U+004A "J" */
    0x0, 0xa, 0x80, 0x0, 0xa8, 0x0, 0xa, 0x83,
    0x30, 0xa8, 0x6d, 0x3d, 0x60, 0xbf, 0xb0,

    /* U+004B "K" */
    0x6d, 0x8, 0xd0, 0x6d, 0x4f, 0x20, 0x6e, 0xf6,
    0x0, 0x6f, 0xdc, 0x0, 0x6d, 0xe, 0x60, 0x6d,
    0x4, 0xf2,

    /* U+004C "L" */
    0x6d, 0x0, 0x6, 0xd0, 0x0, 0x6d, 0x0, 0x6,
    0xd0, 0x0, 0x6d, 0x22, 0x16, 0xff, 0xf6,

    /* U+004D "M" */
    0x6f, 0x60, 0x1f, 0xb6, 0xfb, 0x6, 0xfb, 0x6e,
    0xf1, 0xbd, 0xb6, 0xdb, 0x7f, 0x8b, 0x6d, 0x5f,
    0xb7, 0xb6, 0xd0, 0xf5, 0x7b,

    /* U+004E "N" */
    0x6f, 0x30, 0xd6, 0x6f, 0xc0, 0xd6, 0x6d, 0xe4,
    0xd6, 0x6d, 0x5d, 0xd6, 0x6d, 0xc, 0xf6, 0x6d,
    0x3, 0xf6,

    /* U+004F "O" */
    0x8, 0xef, 0x90, 0x5f, 0x54, 0xe8, 0x9a, 0x0,
    0x7d, 0x9a, 0x0, 0x7d, 0x5f, 0x54, 0xe8, 0x8,
    0xef, 0xa0,

    /* U+0050 "P" */
    0x6f, 0xfe, 0x50, 0x6d, 0x28, 0xe0, 0x6d, 0x6,
    0xf0, 0x6f, 0xff, 0x70, 0x6d, 0x20, 0x0, 0x6d,
    0x0, 0x0,

    /* U+0051 "Q" */
    0x7, 0xef, 0x90, 0x5f, 0x54, 0xe7, 0x9a, 0x0,
    0x8c, 0x9a, 0x14, 0x8c, 0x5f, 0x5d, 0xf6, 0x7,
    0xef, 0xe0, 0x0, 0x0, 0x52,

    /* U+0052 "R" */
    0x6f, 0xfe, 0x90, 0x6d, 0x27, 0xf0, 0x6f, 0xff,
    0x90, 0x6d, 0x27, 0xf0, 0x6d, 0x2, 0xf1, 0x6d,
    0x1, 0xf2,

    /* U+0053 "S" */
    0x1b, 0xfd, 0x20, 0x6e, 0x39, 0xc0, 0x3e, 0x84,
    0x0, 0x2, 0x5b, 0xb0, 0x7e, 0x37, 0xf0, 0x9,
    0xfe, 0x60,

    /* U+0054 "T" */
    0xef, 0xff, 0xb2, 0x5f, 0x21, 0x3, 0xf0, 0x0,
    0x3f, 0x0, 0x3, 0xf0, 0x0, 0x3f, 0x0,

    /* U+0055 "U" */
    0x8b, 0x1, 0xf2, 0x8b, 0x1, 0xf2, 0x8b, 0x1,
    0xf2, 0x8b, 0x1, 0xf2, 0x6e, 0x37, 0xf0, 0x9,
    0xfe, 0x50,

    /* U+0056 "V" */
    0xa9, 0x0, 0xb8, 0x5e, 0x1, 0xf2, 0xf, 0x36,
    0xd0, 0xa, 0x9b, 0x70, 0x4, 0xef, 0x20, 0x0,
    0xfc, 0x0,

    /* U+0057 "W" */
    0x9a, 0x9, 0xb0, 0x9b, 0x5e, 0xd, 0xe0, 0xc7,
    0x1f, 0x3f, 0xd3, 0xf3, 0xd, 0xab, 0x9a, 0xe0,
    0x8, 0xf7, 0x6f, 0xa0, 0x4, 0xf3, 0x2f, 0x60,

    /* U+0058 "X" */
    0x9c, 0x5, 0xf1, 0xe, 0x7e, 0x60, 0x4, 0xfc,
    0x0, 0x4, 0xfc, 0x0, 0xe, 0x7e, 0x70, 0x9c,
    0x5, 0xf2,

    /* U+0059 "Y" */
    0xc, 0x80, 0x7d, 0x0, 0x4f, 0x2e, 0x40, 0x0,
    0xbe, 0xc0, 0x0, 0x3, 0xf4, 0x0, 0x0, 0x1f,
    0x20, 0x0, 0x1, 0xf2, 0x0,

    /* U+005A "Z" */
    0xaf, 0xff, 0x71, 0x27, 0xf3, 0x1, 0xe6, 0x0,
    0xca, 0x0, 0x8e, 0x32, 0x1c, 0xff, 0xf8,

    /* U+005B "[" */
    0x6f, 0xf0, 0x6c, 0x20, 0x6c, 0x0, 0x6c, 0x0,
    0x6c, 0x0, 0x6c, 0x0, 0x6c, 0x0, 0x6f, 0xf0,
    0x2, 0x20,

    /* U+005C "\\" */
    0xa8, 0x0, 0x5d, 0x0, 0xf, 0x20, 0xb, 0x70,
    0x7, 0xb0, 0x2, 0xf0, 0x0, 0xd5, 0x0, 0x8a,

    /* U+005D "]" */
    0xdf, 0x71, 0xa8, 0xa, 0x80, 0xa8, 0xa, 0x80,
    0xa8, 0xa, 0x8d, 0xf8, 0x12, 0x10,

    /* U+005E "^" */
    0x6, 0x40, 0x1d, 0xc0, 0x76, 0xb2,

    /* U+005F "_" */
    0xaf, 0xff, 0x21, 0x22, 0x20,

    /* U+00B7 "·" */
    0x11, 0xb9, 0x32
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 29, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 33, .box_w = 2, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 6, .adv_w = 50, .box_w = 3, .box_h = 3, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 11, .adv_w = 75, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 26, .adv_w = 86, .box_w = 5, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 49, .adv_w = 106, .box_w = 7, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 70, .adv_w = 90, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 88, .adv_w = 26, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 91, .adv_w = 41, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 103, .adv_w = 41, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 115, .adv_w = 54, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 125, .adv_w = 73, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 138, .adv_w = 30, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 141, .adv_w = 53, .box_w = 4, .box_h = 2, .ofs_x = 0, .ofs_y = 1},
    {.bitmap_index = 145, .adv_w = 30, .box_w = 2, .box_h = 2, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 147, .adv_w = 67, .box_w = 4, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 163, .adv_w = 89, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 181, .adv_w = 57, .box_w = 3, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 190, .adv_w = 84, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 205, .adv_w = 83, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 220, .adv_w = 84, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 235, .adv_w = 86, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 250, .adv_w = 80, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 265, .adv_w = 70, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 280, .adv_w = 85, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 298, .adv_w = 81, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 313, .adv_w = 40, .box_w = 2, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 319, .adv_w = 40, .box_w = 2, .box_h = 7, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 326, .adv_w = 70, .box_w = 4, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 338, .adv_w = 71, .box_w = 5, .box_h = 4, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 348, .adv_w = 70, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 363, .adv_w = 76, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 378, .adv_w = 123, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 410, .adv_w = 93, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 428, .adv_w = 90, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 446, .adv_w = 94, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 464, .adv_w = 92, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 482, .adv_w = 80, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 497, .adv_w = 77, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 512, .adv_w = 94, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 530, .adv_w = 92, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 548, .adv_w = 38, .box_w = 2, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 554, .adv_w = 80, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 569, .adv_w = 88, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 587, .adv_w = 75, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 602, .adv_w = 117, .box_w = 7, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 623, .adv_w = 96, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 641, .adv_w = 99, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 659, .adv_w = 86, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 677, .adv_w = 98, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 698, .adv_w = 89, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 716, .adv_w = 87, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 734, .adv_w = 77, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 749, .adv_w = 90, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 767, .adv_w = 93, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 785, .adv_w = 130, .box_w = 8, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 809, .adv_w = 88, .box_w = 6, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 827, .adv_w = 81, .box_w = 7, .box_h = 6, .ofs_x = -1, .ofs_y = 0},
    {.bitmap_index = 848, .adv_w = 76, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 863, .adv_w = 50, .box_w = 4, .box_h = 9, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 881, .adv_w = 64, .box_w = 4, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 897, .adv_w = 50, .box_w = 3, .box_h = 9, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 911, .adv_w = 59, .box_w = 4, .box_h = 3, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 917, .adv_w = 72, .box_w = 5, .box_h = 2, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 922, .adv_w = 30, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = 1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 64, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    },
    {
        .range_start = 183, .range_length = 1, .glyph_id_start = 65,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};

/*-----------------
 *    KERNING
 *----------------*/


/*Map glyph_ids to kern left classes*/
static const uint8_t kern_left_class_mapping[] =
{
    0, 0, 0, 1, 0, 0, 0, 2,
    1, 3, 0, 4, 5, 6, 7, 6,
    8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 9, 18, 18, 5, 5, 5,
    0, 0, 19, 20, 21, 22, 23, 24,
    25, 26, 26, 27, 28, 29, 26, 26,
    22, 30, 22, 31, 22, 32, 27, 33,
    33, 28, 34, 35, 36, 37, 0, 5,
    0, 0
};

/*Map glyph_ids to kern right classes*/
static const uint8_t kern_right_class_mapping[] =
{
    0, 0, 1, 2, 0, 0, 3, 4,
    2, 0, 5, 6, 7, 8, 9, 8,
    10, 11, 12, 13, 14, 15, 16, 11,
    17, 18, 19, 20, 20, 7, 7, 7,
    21, 0, 22, 23, 24, 23, 25, 23,
    24, 23, 23, 26, 27, 27, 23, 28,
    24, 23, 24, 23, 24, 29, 30, 31,
    31, 32, 33, 34, 0, 35, 36, 7,
    0, 0
};

/*Kern values between classes*/
static const int8_t kern_class_values[] =
{
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -6, 0, 0, 0, 0, -4, 0,
    1, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -5, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -4, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -1, -1, -3, 0,
    0, -1, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, -5, 0, -10, 0, 0,
    0, 0, 0, 0, 2, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -3, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -5, 0, 0, 0, 0, 0, -9,
    0, 0, 0, 0, -8, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -9, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, -3, 0, 0, 1, 0,
    -5, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -5, 0, 0, -2,
    0, 0, 0, -9, 0, -15, -3, -3,
    -5, -5, -9, -1, 1, -3, -3, -5,
    -5, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -1, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 1, -1, 0, 0, 0, 0,
    -5, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -1, -3, -2, -5,
    -1, 0, -3, 0, -3, -3, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -4, -1, 0, -1, 0, -3, -4, 0,
    -3, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -1, 0, 0, 0, 0, -5, 0, 0,
    0, 0, 0, 0, -3, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -1, -3, 0, 0, 0, 0, 0,
    -1, 0, -1, -3, 0, -3, 0, 0,
    -4, 0, 0, -3, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -2, 0, 0, 0, 0, -3, -3, 0,
    -1, 0, 0, 0, -6, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -3, 0, -3, -2, 0, 0, 0,
    -4, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 2, 0,
    0, 0, 0, -11, -4, -6, 0, 1,
    -1, 0, -5, 0, 3, -1, 0, -3,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 4,
    0, 0, 0, 0, -1, 0, 0, 0,
    0, 0, 0, -2, 0, 0, 0, 0,
    -3, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -3, 0, 0,
    0, 0, -3, 0, -7, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, -5, 0,
    -1, -6, 0, 0, 0, -10, 0, 0,
    0, 0, 0, 0, 0, -6, 0, 0,
    -13, 0, 0, -3, -7, 0, -3, -5,
    -3, -3, -2, -3, -13, -3, -15, 0,
    -10, -1, -15, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -4, 0, 0,
    0, 0, 0, 0, -4, 0, 0, -3,
    -3, -4, -1, -1, -1, 0, 0, -1,
    -4, 0, -5, 0, -7, 0, -5, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -4, 0, 0, 0, 0, 0, 0,
    -2, 0, 0, 0, 0, -4, -1, 0,
    -1, 0, -1, -1, -2, 0, -3, -3,
    -3, 0, -3, 0, 0, 0, 0, 0,
    -2, -1, 0, -3, 0, -6, 0, 0,
    -2, 0, 0, 0, -5, 0, 0, -1,
    -3, -6, 1, 0, 1, -1, -2, 1,
    -5, 0, -6, -5, -6, -1, -6, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, -1, -3, 0, 0, 0,
    0, 0, 0, -1, 0, 0, 1, 0,
    -3, 0, 1, 0, 0, 0, 0, 0,
    0, 0, 0, -10, 0, -11, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -4,
    -2, -11, -2, 0, -2, -8, -3, -2,
    0, 0, 0, -3, -2, -2, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -3, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -3, 0, -5, -2,
    -3, 0, -5, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -1, 0, 0,
    0, 0, 0, 0, -4, 0, 0, 0,
    -1, -1, -2, -2, -2, -2, 0, -2,
    -4, 2, -3, -1, -3, -1, -3, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -3, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, -3, 2, 0,
    2, -2, 0, 2, 0, 1, 0, -3,
    -2, 0, 0, 0, 0, 0, 0, 0,
    0, -2, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -3,
    -3, 0, -1, -5, -1, -4, -2, -1,
    0, -3, -1, 0, 0, -1, -1, 0,
    -2, -11, 0, 0, 0, -15, 0, 0,
    0, -3, 0, 0, 0, 0, 0, 0,
    -16, 0, 0, -2, -10, -3, -4, -6,
    -4, -4, -3, -4, -16, -5, -17, 0,
    -20, -2, -17, 0, -1, 0, 0, 0,
    0, 0, 0, -13, -3, -11, 0, 0,
    0, 0, 0, 0, 0, 0, 0, -3,
    -2, -11, -3, -2, -3, -10, 0, -3,
    0, 0, -6, -5, -5, -3, -6, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, -1, 0, 0, 0, 0, 0, 0,
    -2, 0, 0, -1, -3, -1, 0, -2,
    0, 0, -2, 0, -2, 0, -6, 0,
    -3, 0, -6, 0, 0, 1, 0, -2,
    0, 2, 0, -11, -4, -13, 0, 0,
    0, 0, -3, 0, 1, 0, 0, -7,
    -3, -13, -2, -5, -2, -15, -4, -2,
    1, 0, -1, 0, 1, 0, -1, 3,
    -5, 0, 0, -2, 0, 0, 0, -9,
    0, -15, -3, -3, -5, -5, -9, -1,
    1, -3, -3, -5, -5, -15, -3, -6,
    -3, -10, -2, -3, 1, 0, -1, -1,
    0, -2, -1, 0, 0, 0, 0, 0,
    0, 0, -3, 0, 0, -10, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 0,
    0, -10, -3, -6, -3, -17, 0, -3,
    1, -2, 0, 0, 3, -1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    -2, -3, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, -3, -3, -1, -3,
    -1, -3, 0, -1, 0, 0, -2, -1,
    -1, 0, -2, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 3, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    3, 0, 0, 0, 0, 0, 0, 0,
    -1, -6, 0, 0, 0, -10, 0, 0,
    0, 0, 0, 0, 0, -6, 0, 0,
    -13, 0, 0, -3, -7, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, -15, 0
};


/*Collect the kern class' data in one place*/
static const lv_font_fmt_txt_kern_classes_t kern_classes =
{
    .class_pair_values   = kern_class_values,
    .left_class_mapping  = kern_left_class_mapping,
    .right_class_mapping = kern_right_class_mapping,
    .left_class_cnt      = 37,
    .right_class_cnt     = 36,
};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = &kern_classes,
    .kern_scale = 16,
    .cmap_num = 2,
    .bpp = 4,
    .kern_classes = 1,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t stream_caps_8 = {
#else
lv_font_t stream_caps_8 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 10,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if STREAM_CAPS_8*/

