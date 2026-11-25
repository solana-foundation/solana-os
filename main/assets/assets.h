#pragma once

// Brand assets converted from web/static/brand.
// Fonts: tools/gen_assets.sh (lv_font_conv). Images: tools/img2lvgl.py.
// Each font: ABC Diatype's ASCII, Latin-1 and Latin Extended-A, then Cyrillic
// from Inter (sol_cyr_*, OFL, tools/fonts/), then Montserrat's LV_SYMBOL_* icons.

#include "lvgl.h"

LV_FONT_DECLARE(sol_mono_12);   // ABC Diatype Semi-Mono 12
LV_FONT_DECLARE(sol_font_14);   // ABC Diatype 14
LV_FONT_DECLARE(sol_font_20);   // ABC Diatype Medium 20
LV_FONT_DECLARE(sol_font_32);   // ABC Diatype Bold 32 (no symbols)
LV_FONT_DECLARE(sol_cyr_12);    // Inter 12, Cyrillic: the fallbacks above
LV_FONT_DECLARE(sol_cyr_14);
LV_FONT_DECLARE(sol_cyr_20);
LV_FONT_DECLARE(sol_cyr_32);

LV_IMAGE_DECLARE(img_logomark_16);
LV_IMAGE_DECLARE(img_logomark_40);
LV_IMAGE_DECLARE(img_logomark_72);
LV_IMAGE_DECLARE(img_foundation);
LV_IMAGE_DECLARE(img_foundation_black);
