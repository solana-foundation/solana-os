// The C app warning (docs/HHGTS.md "One look", C_API.md "Security": "What
// players are told"): the App Store's details and install confirm, and the
// Apps library's info. The site says the longer form (web cwarning.ts).
//
// A C app from the store runs as native code, kept in its sandbox by the
// store's compiler alone (the chip has no isolation), where a Lua app's every
// step goes through the interpreter. Without a native form for this badge
// (and from Run on badge or the console) it runs as WebAssembly in the
// interpreter instead: C_WARN_WASM says so, without the warning.
#pragma once

#include "core/theme.h"
#include "core/ui.h"

#define C_WARN_TITLE    "C apps can be more dangerous"
#define C_WARN_BODY     "Lua apps run in an interpreter. A C app runs as native code that only the store's " \
                        "compiler keeps in its sandbox: a flaw there could reach your whole badge."
#define C_WARN_WASM     "C app: it runs as WebAssembly in the badge's interpreter, not as native code."

// The title (orange) and the text under it, in a column that is `parent`'s
// full width (a flex column: the store's details body, the library's info).
static inline void c_warning_add(lv_obj_t *parent)
{
    ui_label(parent, &sol_font_14, COL_ORANGE, C_WARN_TITLE);
    lv_obj_t *l = ui_label(parent, &sol_font_14, COL_DIM, C_WARN_BODY);
    lv_obj_set_width(l, lv_pct(100));
}
