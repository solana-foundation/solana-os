#pragma once

// Brand assets converted from web/static/brand.
// Fonts: tools/gen_assets.sh (lv_font_conv). Images: tools/img2lvgl.py.

#include "lvgl.h"

LV_FONT_DECLARE(sol_mono_12);   // ABC Diatype Semi-Mono 12, symbols from Montserrat 12
LV_FONT_DECLARE(sol_font_14);   // ABC Diatype 14, symbols from Montserrat 14
LV_FONT_DECLARE(sol_font_20);   // ABC Diatype Medium 20, symbols from Montserrat 20
LV_FONT_DECLARE(sol_font_32);   // ABC Diatype Bold 32, ASCII only

// The pearl themes' home screen (core/home/home_pearl.c) in the design's own
// face, Geist (SIL OFL 1.1: GEIST-LICENSE.txt). lv_font_conv, as gen_assets.sh.
LV_FONT_DECLARE(pearl_num_46);  // Geist SemiBold 46: digits , . - + only (the !LAMPs figure)
LV_FONT_DECLARE(pearl_font_13); // Geist SemiBold 13, symbols from Montserrat 12
LV_FONT_DECLARE(pearl_font_11); // Geist Medium 11, symbols from Montserrat 12
LV_FONT_DECLARE(pearl_mono_9);  // Geist Mono Medium 9, ASCII only

LV_IMAGE_DECLARE(img_logomark_16);
LV_IMAGE_DECLARE(img_logomark_40);
LV_IMAGE_DECLARE(img_logomark_72);
LV_IMAGE_DECLARE(img_foundation);
LV_IMAGE_DECLARE(img_foundation_black);

// Theme wallpapers, 320x240 RGB565: tools/gen_wallpaper.py.
LV_IMAGE_DECLARE(img_wall_pearl);
LV_IMAGE_DECLARE(img_wall_black_pearl);
LV_IMAGE_DECLARE(img_wall_aurora);

// Artwork on the pearl themes' home cards (core/home/home_pearl.c), cut to the
// cards' rounded corners: rendered from the design and converted with
// tools/img2lvgl.py.
LV_IMAGE_DECLARE(img_pearl_hero);
LV_IMAGE_DECLARE(img_pearl_wifi);
LV_IMAGE_DECLARE(img_pearl_quest);
LV_IMAGE_DECLARE(img_black_pearl_hero);
LV_IMAGE_DECLARE(img_black_pearl_wifi);
LV_IMAGE_DECLARE(img_black_pearl_quest);

// The Streaming theme (core/ui_stream.h, core/home/home_stream.c): Bebas Neue
// for display lines (SIL OFL 1.1: BEBAS-NEUE-LICENSE.txt; capitals only) and
// Geist Bold for labels. lv_font_conv, as gen_assets.sh.
LV_FONT_DECLARE(stream_disp_14);    // Bebas Neue 14, 0x20-0x5F
LV_FONT_DECLARE(stream_disp_24);    // Bebas Neue 24
LV_FONT_DECLARE(stream_disp_30);    // Bebas Neue 30
LV_FONT_DECLARE(stream_disp_64);    // Bebas Neue 64: digits only
LV_FONT_DECLARE(stream_caps_8);     // Geist Bold 8, capitals: the small spaced labels
LV_FONT_DECLARE(stream_bold_11);    // Geist Bold 11, symbols from Montserrat 12

// The home banner (320x156, opaque: tools/img2lvgl565.py), rendered from the design.
LV_IMAGE_DECLARE(img_stream_hero);
// Empty-state artwork: Quests' radar, X Dex's cards, Trade's orb (core/ui_stream.c).
LV_IMAGE_DECLARE(img_stream_radar);
LV_IMAGE_DECLARE(img_stream_cards);
LV_IMAGE_DECLARE(img_stream_orb);

// The design-matched themes' own faces (all SIL OFL 1.1: LICENSE-*.txt) and the
// boards' line icons (24x24 A8: recolour with image_recolor), from the canvas.
LV_FONT_DECLARE(apple_font_10);
LV_FONT_DECLARE(apple_font_12);
LV_FONT_DECLARE(apple_font_14);
LV_FONT_DECLARE(apple_num_26);
LV_FONT_DECLARE(apple_num_48);
LV_FONT_DECLARE(blocks_font_13);
LV_FONT_DECLARE(blocks_font_15);
LV_FONT_DECLARE(blocks_mono_9);
LV_FONT_DECLARE(blocks_num_41);
LV_FONT_DECLARE(blocks_text_12);
LV_FONT_DECLARE(paper_font_16);
LV_FONT_DECLARE(paper_italic_17);
LV_FONT_DECLARE(paper_mono_11);
LV_FONT_DECLARE(paper_mono_9);
LV_FONT_DECLARE(paper_num_46);
LV_FONT_DECLARE(rpg_bold_10);
LV_FONT_DECLARE(rpg_font_9);
LV_FONT_DECLARE(rpg_num_26);
LV_FONT_DECLARE(term_mono_11);
LV_FONT_DECLARE(term_mono_9);
LV_FONT_DECLARE(term_mono_bold_11);
LV_IMAGE_DECLARE(img_line_app);
LV_IMAGE_DECLARE(img_line_apps);
LV_IMAGE_DECLARE(img_line_badge);
LV_IMAGE_DECLARE(img_line_debug);
LV_IMAGE_DECLARE(img_line_espnow);
LV_IMAGE_DECLARE(img_line_guide);
LV_IMAGE_DECLARE(img_line_quests);
LV_IMAGE_DECLARE(img_line_settings);
LV_IMAGE_DECLARE(img_line_store);
LV_IMAGE_DECLARE(img_line_trade);
LV_IMAGE_DECLARE(img_line_wifi);
LV_IMAGE_DECLARE(img_line_xdex);
LV_IMAGE_DECLARE(img_list_orb);
LV_IMAGE_DECLARE(img_rpg_gem);

// Lumen's home cards (core/home/home_lumen.c): each card's glass, glow and
// artwork as one image rendered from the design (tools/img2lvgl.py).
LV_IMAGE_DECLARE(img_lumen_hero);
LV_IMAGE_DECLARE(img_lumen_wifi);
LV_IMAGE_DECLARE(img_lumen_quest);
