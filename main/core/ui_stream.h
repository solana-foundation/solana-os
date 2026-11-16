#pragma once

// Widgets of the Streaming theme (UI_THEME_STREAM), for its home screen and
// for the app screens that draw themselves in its look. Call with the LVGL
// lock held. ui_stream() says whether that theme is the active one: an app
// asks it before building its streaming screen, and builds its usual one
// otherwise.
//
// Faces: Bebas Neue for display lines (assets/stream_disp_*.c: capitals,
// digits and punctuation only, so pass text through ui_stream_caps()),
// Geist for the rest (stream_bold_11, stream_caps_8, pearl_font_11/13).

#include <stdbool.h>
#include <stddef.h>
#include "ui.h"

bool ui_stream(void);

// Upper-case `in` into `out` (ASCII; other bytes become spaces), for the
// display face. Returns out.
char *ui_stream_caps(char *out, size_t n, const char *in);

// "SAVED NETWORK": the small spaced capitals over a headline.
lv_obj_t *ui_stream_caps_label(lv_obj_t *parent, const char *text);
// A display line in Bebas Neue at 14, 24 or 30 px (64: digits only). The
// text is upper-cased here.
lv_obj_t *ui_stream_display(lv_obj_t *parent, int px, const char *text);

// A button: `primary` is white with dark text, the other kind grey glass.
// `symbol` (an LV_SYMBOL_*, or NULL) goes before the text.
lv_obj_t *ui_stream_button(lv_obj_t *parent, const char *symbol, const char *text, bool primary);
// A pill: filled white when `on`, outlined otherwise; `dot` (a COL_*, 0 =
// none) puts a status dot before the text.
lv_obj_t *ui_stream_pill(lv_obj_t *parent, const char *text, bool on, uint32_t dot);
// A 51x64 poster for an app: `symbol` (or `icon`, an image, when it has no
// symbol) over the name on a wash of `accent` (0xRRGGBB). `art` marks a store
// app, for artwork of its own one day; nothing is bundled for any app now.
#define UI_POSTER_W     51
#define UI_POSTER_H     64
lv_obj_t *ui_stream_poster(lv_obj_t *parent, const char *name, const char *symbol, const void *icon, uint32_t accent, bool art);

// The selected thing's white ring (a gap, then the ring), or none.
void ui_stream_ring(lv_obj_t *obj, bool on);

// A soft glow behind a screen's centrepiece: an ellipse of `color` fading
// out, w x h centred at (cx, cy) in the parent. Decoration: create it first.
lv_obj_t *ui_stream_glow(lv_obj_t *parent, int cx, int cy, int w, int h, uint32_t color);

// The artwork for the current app's empty state (Quests' radar, X Dex's
// cards, Trade's orb) as a child of parent, or NULL if it has none.
lv_obj_t *ui_stream_empty_art(lv_obj_t *parent);

// The grey pill with an (i) and one line of advice.
lv_obj_t *ui_stream_hint(lv_obj_t *parent, const char *text);
// The current app's advice for its empty state as such a pill, or NULL.
lv_obj_t *ui_stream_empty_hint(lv_obj_t *parent);

// An empty state, centred: a headline, one line under it and a grey hint pill
// with an (i). Any of the three may be NULL. Returns the column, positioned
// at y; put the screen's artwork above it.
lv_obj_t *ui_stream_empty(lv_obj_t *parent, int y, const char *headline, const char *line, const char *hint);

// A "label ... value" row with a hairline above it (My Badge, About).
// Returns the value label; value_color is a COL_* (0 = text).
lv_obj_t *ui_stream_row(lv_obj_t *parent, int y, const char *label, const char *value, uint32_t value_color);
