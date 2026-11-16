#pragma once

// The C app ABI of Solana OS (docs/C_API.md "The raw ABI"): what a C app compiled
// to WebAssembly imports from the badge and what the badge calls in it.
// badge.h builds the typed API (ui_label(), http_get(), ...) on top of this;
// include that one. This file changes only with SOS_ABI.
//
// A C app sees the same API as a Lua app (docs/LUA_API.md): the host keeps a
// Lua state for it and these calls work on that state's stack, like a small,
// checked Lua C API. So every module, permission and limit a Lua app has is
// the C app's too, and a new Lua API is reachable from C the day it ships
// (sos_global("mesh.scan") ...), before badge.h has a typed wrapper for it.
//
// The stack. Values live on a stack that belongs to the running call (a
// "frame"): on_start() begins with an empty one, a callback begins with its
// arguments at 1..nargs, and a frame ends when the function returns. An index
// is 1-based from the bottom of the frame, or negative from the top (-1 is the
// top). An index outside the frame is a bug in the app: the app stops with an
// error screen (only sos_type() answers SOS_NONE for one). At most 64 values
// a frame.
//
// Errors. A call that can't work because of the app's own mistake (a bad
// index, a pointer outside its memory) stops the app with an error screen.
// What an API function refuses comes back from sos_call() as an error the app
// can handle. Out of memory and a callback that runs too long always stop the
// app.

#include <stdint.h>

#define SOS_ABI 1

#define SOS_IMPORT(name) __attribute__((import_module("badge"), import_name(#name)))
#define SOS_EXPORT(name) __attribute__((export_name(#name), used))

#ifdef __cplusplus
extern "C" {
#endif

// What sos_type() answers.
enum {
    SOS_NONE = -1,  // no value at that index
    SOS_NIL = 0,
    SOS_BOOL = 1,
    SOS_INT = 2,    // a 32-bit integer
    SOS_NUM = 3,    // a 32-bit float (the badge's Lua has no doubles)
    SOS_STR = 4,    // bytes, any content
    SOS_TABLE = 5,
    SOS_FN = 6,
    SOS_OBJ = 7,    // an API object: a label, a canvas, a socket, ...
};

// on_key()'s keys, and the bits of sos_keys().
enum { SOS_KEY_UP, SOS_KEY_DOWN, SOS_KEY_LEFT, SOS_KEY_RIGHT, SOS_KEY_SELECT, SOS_KEY_BACK };

#define SOS_MULTRET (-1)

// A C function the badge can call (sos_push_fn): its arguments are the frame's
// values 1..nargs; it returns how many results it left on top of the stack.
typedef int32_t (*sos_fn)(void *ud, int32_t nargs);

// --- the frame -------------------------------------------------------------

SOS_IMPORT(top) int32_t sos_top(void);                  // values in the frame
SOS_IMPORT(pop) void sos_pop(int32_t n);                // drop the top n
SOS_IMPORT(push_value) void sos_push_value(int32_t idx);    // a copy of the value at idx
SOS_IMPORT(remove) void sos_remove(int32_t idx);        // take the value at idx out; those above move down

// --- push ------------------------------------------------------------------

SOS_IMPORT(push_nil) void sos_push_nil(void);
SOS_IMPORT(push_bool) void sos_push_bool(int32_t b);
SOS_IMPORT(push_int) void sos_push_int(int32_t v);
SOS_IMPORT(push_num) void sos_push_num(float v);
SOS_IMPORT(push_str) void sos_push_str(const char *p, uint32_t len);    // the bytes are copied
SOS_IMPORT(new_table) void sos_new_table(void);
// A function value that calls fn(ud, nargs). It lives as long as the badge
// holds it (a timer, a list's on-select, an http callback): nothing to free.
SOS_IMPORT(push_fn) void sos_push_fn(sos_fn fn, void *ud);

// --- read ------------------------------------------------------------------

SOS_IMPORT(type) int32_t sos_type(int32_t idx);
SOS_IMPORT(to_bool) int32_t sos_to_bool(int32_t idx);   // nil and false are 0, anything else 1
SOS_IMPORT(to_int) int32_t sos_to_int(int32_t idx);     // a number (floats truncate), else 0
SOS_IMPORT(to_num) float sos_to_num(int32_t idx);       // a number, else 0
// A string's bytes into buf: at most cap - 1 of them, then a NUL (nothing is
// written when cap is 0). Returns the string's full length, so a result
// >= cap says it was cut. 0 and "" for a value that isn't a string.
SOS_IMPORT(to_str) uint32_t sos_to_str(int32_t idx, char *buf, uint32_t cap);
SOS_IMPORT(len) uint32_t sos_len(int32_t idx);          // a string's bytes, a table's array length, else 0

// --- tables and objects ----------------------------------------------------

// Pushes the field `key` of the table or object at idx (nil when there is
// none, or the value has no fields) and returns its type. An object's fields
// are its methods: sos_get(obj, "text", 4).
SOS_IMPORT(get) int32_t sos_get(int32_t idx, const char *key, uint32_t klen);
SOS_IMPORT(set) void sos_set(int32_t idx, const char *key, uint32_t klen);     // table[key] = the top value, popped
SOS_IMPORT(geti) int32_t sos_geti(int32_t idx, int32_t i);                     // pushes table[i] (1-based), returns its type
SOS_IMPORT(seti) void sos_seti(int32_t idx, int32_t i);                        // table[i] = the top value, popped
// Walks a table: with the previous key on top (nil to start) it pops the key
// and pushes the next key and its value, returning 1; at the end it pushes
// nothing and returns 0.
SOS_IMPORT(next) int32_t sos_next(int32_t idx);

// --- the API ---------------------------------------------------------------

// Pushes what a dotted name of the Lua API holds ("ui.label", "ui.W",
// "mesh.send") and returns its type; nil when there is no such name.
SOS_IMPORT(global) int32_t sos_global(const char *path, uint32_t len);
// Calls the function below its nargs arguments (all popped). 0: it worked and
// left nresults values (SOS_MULTRET: all of them; count with sos_top()).
// Anything else: it failed and left one value, the error's text.
SOS_IMPORT(call) int32_t sos_call(int32_t nargs, int32_t nresults);

// --- values kept across calls ----------------------------------------------

// Pops the top value and keeps it; the number names it until sos_unref().
// 0 for nil (nothing kept). At most 1024 at a time.
SOS_IMPORT(ref) int32_t sos_ref(void);
SOS_IMPORT(unref) void sos_unref(int32_t ref);
SOS_IMPORT(push_ref) int32_t sos_push_ref(int32_t ref);     // pushes it (nil for 0), returns its type

// --- straight to the badge (no stack) --------------------------------------

// w * h RGB565 pixels (row by row, little-endian) from the app's memory onto
// the ui.canvas at idx, with its top-left at (x, y), clipped to the canvas.
// 0, or -1 when idx isn't a canvas. A whole frame in one call: draw into your
// own buffer, then blit. The pixel value 0x0001 is drawn as 0x0000 (black):
// 0x0001 is the canvas's see-through value, which a blit never writes.
SOS_IMPORT(blit) int32_t sos_blit(int32_t canvas, const uint16_t *px, int32_t x, int32_t y, int32_t w, int32_t h);
// w * h RGB565 pixels from the app's memory straight to the screen, past the
// UI's own drawing, scaled (nearest) to dw * dh at (x, y) of the app area
// (UI_W x UI_H); the whole frame must fit in it. flags SOS_FRAME_BE: the
// pixels are big-endian, the screen's own order. 1 shown, 0 not shown (a
// dialog, the keyboard or the badge's own panel is over the app), -1 outside
// the app area. Returns while the screen is still taking the frame: an
// unscaled big-endian frame in a 64-byte-aligned buffer, h * w * 2 a multiple
// of 64, is read where it is, so draw the next one into another buffer
// (docs/C_API.md "Frames straight to the screen"). 1 to 1024 pixels a side.
#define SOS_FRAME_BE 1
SOS_IMPORT(frame) int32_t sos_frame(const uint16_t *px, int32_t w, int32_t h, int32_t x, int32_t y,
                                    int32_t dw, int32_t dh, uint32_t flags);
// Up to len (<= 256 KB) bytes of the file `name` of the app's folder on the
// card (permission "files") from offset off, into buf: how many (0 at the
// end), or -1 when there's no such file or the card failed.
SOS_IMPORT(file_read) int32_t sos_file_read(const char *name, uint32_t nlen, uint32_t off, void *buf, uint32_t len);
SOS_IMPORT(keys) uint32_t sos_keys(void);       // the keys held now: bit (1 << SOS_KEY_UP) ...
SOS_IMPORT(millis) uint32_t sos_millis(void);   // ms since the app started
SOS_IMPORT(log) void sos_log(const char *p, uint32_t len);      // a line on the serial console, as Lua's print
SOS_IMPORT(fail) void sos_fail(const char *msg, uint32_t len);  // stop the app with this on the error screen; doesn't return
SOS_IMPORT(host_abi) int32_t sos_host_abi(void);    // the badge's SOS_ABI

// --- what the badge calls --------------------------------------------------
//
// Define the ones you need; only on_start is required. Each runs as one
// callback (docs/C_API.md "Limits"), never two at once.

SOS_EXPORT(on_start) void on_start(void);                           // once, at launch: build the UI here
SOS_EXPORT(on_key) int32_t on_key(int32_t key, int32_t repeat);     // SOS_KEY_*; return 1 if handled (0 for BACK exits the app)
SOS_EXPORT(on_tick) void on_tick(int32_t ms);                       // every 30 ms; ms since the last tick
SOS_EXPORT(on_exit) void on_exit(void);                             // the app is closing
SOS_EXPORT(on_mesh) void on_mesh(void);                             // frame: 1 = the sender's badge id, 2 = the message (a table)
SOS_EXPORT(on_meet) int32_t on_meet(void);                          // frame: 1 = who asks, 2 = their data; push a table and return 1 to sign
SOS_EXPORT(on_mail) void on_mail(void);                             // frame: 1 = the mail (a table, as mail.get gives it); "mail", 2.3.0

// The ABI this app was built for; the badge refuses a newer one than its own.
__attribute__((export_name("badge_abi"), weak)) int32_t badge_abi(void)
{
    return SOS_ABI;
}

#ifdef __cplusplus
}
#endif
