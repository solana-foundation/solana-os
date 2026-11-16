#pragma once

// badge.h: the typed C API of Solana OS apps (docs/C_API.md). It wraps the few
// imports of badge_abi.h, which work on the Lua API (docs/LUA_API.md) through a
// checked stack, so every function here is one Lua API call and means what the
// Lua reference says. Include this one file.
//
// Conventions
//   - Indices count from 0 (LEDs 0..7, list rows, results); Lua's count from 1.
//   - Status: 0 for done, BADGE_ERR (-1) for refused or failed, with the
//     reason in badge_error() until the next failure. Constructors return a
//     handle whose .ref is 0 on failure.
//   - Strings come back in buffers you pass: at most cap - 1 bytes and a NUL.
//     Functions return the full length, so a result >= cap says it was cut.
//   - Tables you send are JSON text (json_push() decodes it on the badge), or
//     built on the stack (badge_new_table(), badge_set_*).
//   - Tables you get stay on the stack as a badge_val, valid until the
//     callback returns; read documented fields with the typed helpers, the
//     rest with badge_field_*() or json_encode().
//   - Callbacks are a function pointer and a void *user. Waiting callbacks
//     take a slot each of BADGE_CALLBACKS (64): one-shot ones free theirs when
//     they run, timers on timer_cancel(), lists and links on ui_obj_remove(),
//     sockets and links after their close event. A NULL callback is allowed
//     (the call still happens; nothing runs when it answers).
//   - Every wrapper leaves the stack as it found it, except the *_push()
//     functions, which leave exactly one value for you to pop.
//
// Why a header and not a library: there's nothing to build or link, an app
// pays only for what it calls (everything is static inline, and the linker
// drops the rest), and the little shared state (the error text, the callback
// slots) is declared weak, so all of an app's files share one copy.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "badge_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BADGE_ERR (-1)
#define BADGE_CALLBACKS 64

// ============================================================================
// Colours, keys, sizes
// ============================================================================

// A colour argument is 0xRRGGBB (a fixed colour) or a theme colour, which
// follows the Dark/Light theme. In option structs 0 means "the default": use
// COL_BLACK there for black. COL_NONE means "no colour" where the Lua API
// takes none (ui_canvas_clear: see-through; a palette entry: see-through).
#define COL_TEXT 0x80000000u
#define COL_DIM 0x80000001u
#define COL_BG 0x80000002u
#define COL_PANEL 0x80000003u
#define COL_BORDER 0x80000004u
#define COL_PURPLE 0x80000005u
#define COL_GREEN 0x80000006u
#define COL_PINK 0x80000007u
#define COL_LPURPLE 0x80000008u
#define COL_BLUE 0x80000009u
#define COL_AQUA 0x8000000Au
#define COL_LIME 0x8000000Bu
#define COL_YELLOW 0x8000000Cu
#define COL_ORANGE 0x8000000Du
#define COL_BLACK 0x01000000u
#define COL_NONE 0xFFFFFFFFu

#define RGB(r, g, b) ((((uint32_t)(r) & 0xFF) << 16) | (((uint32_t)(g) & 0xFF) << 8) | ((uint32_t)(b) & 0xFF))
// A pixel for sos_blit()/ui_canvas_blit(): RGB565, as the screen stores it.
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xFF) >> 3)))
#define RGB565_HEX(hex) RGB565(((hex) >> 16) & 0xFF, ((hex) >> 8) & 0xFF, (hex) & 0xFF)
// The same pixel big-endian, the screen's own byte order (ui_frame with UI_FRAME_BE).
#define RGB565_BE(px) ((uint16_t)((((px) & 0xFF) << 8) | (((px) >> 8) & 0xFF)))

// on_key()'s keys and sys_keys()'s bits.
#define KEY_UP SOS_KEY_UP
#define KEY_DOWN SOS_KEY_DOWN
#define KEY_LEFT SOS_KEY_LEFT
#define KEY_RIGHT SOS_KEY_RIGHT
#define KEY_SELECT SOS_KEY_SELECT
#define KEY_BACK SOS_KEY_BACK // CANCEL
#define KEY_HELD(keys, k) (((keys) >> (k)) & 1u)

static inline const char *key_name(int32_t k)
{
    static const char *const names[] = {"up", "down", "left", "right", "select", "back"};
    return k >= 0 && k <= 5 ? names[k] : "?";
}

#define UI_W 320 // the app area below the status bar
#define UI_H 218
#define LED_COUNT 8 // 0..5 across the top, 6..7 on the right edge

// ============================================================================
// Handles and stack values
// ============================================================================

// An object the badge keeps for the app (a label, a canvas, a socket, ...).
// {0} is none.
typedef struct {
    int32_t ref;
} badge_obj;
typedef badge_obj ui_obj;    // label, rect, bar, gradient, list, QR, image, canvas, animation
typedef badge_obj ui_img;    // pixels from ui_load()
typedef badge_obj ble_link;  // ble_connect()
typedef badge_obj ws_socket; // ws_open()

// A value on the current stack frame (an absolute index), valid until the
// function that got it returns: a callback's argument, a field you pushed.
typedef struct {
    int32_t idx;
} badge_val;

// A run of bytes (solana_pda seeds).
typedef struct {
    const void *p;
    uint32_t len;
} badge_bytes;

// ============================================================================
// Internals (badge__*: not part of the API)
// ============================================================================

__attribute__((weak)) char badge__err[160];
__attribute__((weak)) char badge__buf[4100]; // the data of the event being delivered (ws, ble)
__attribute__((weak)) char badge__line[516]; // fs_lines' line

typedef void (*badge__fnptr)(void);
typedef struct {
    badge__fnptr fn;
    void *user;
    int32_t tag;   // timer id, or the ref of the object it belongs to
    uint16_t gen;  // bumped on every reuse, so a stale call is ignored
    uint8_t used;
    uint8_t flags; // BADGE__ONCE, BADGE__TIMER, BADGE__OWNED, BADGE__CLOSE
} badge__slot;
__attribute__((weak)) badge__slot badge__slots[BADGE_CALLBACKS];
__attribute__((weak)) int32_t badge__scan_tok; // the running ble scan's callback (token + 1)

enum { BADGE__ONCE = 1, BADGE__TIMER = 2, BADGE__OWNED = 4, BADGE__CLOSE = 8 };

static inline uint32_t badge__len(const char *s)
{
    return s ? (uint32_t)strlen(s) : 0;
}

// sos_push_str of a NUL-terminated string (nil for NULL).
static inline void sos_push_cstr(const char *s)
{
    if (s) {
        sos_push_str(s, badge__len(s));
    } else {
        sos_push_nil();
    }
}

static inline void badge__seterr(const char *s)
{
    uint32_t n = badge__len(s);
    if (n >= sizeof badge__err) {
        n = sizeof badge__err - 1;
    }
    memcpy(badge__err, s, n);
    badge__err[n] = 0;
}

// Pushes the API function (or value) at a dotted name: "ui.label".
static inline int32_t badge__fn(const char *path)
{
    return sos_global(path, badge__len(path));
}

// Calls the function below its nargs arguments. 0 with nres values pushed, or
// BADGE_ERR with nothing pushed and the error kept for badge_error().
static inline int badge__call(int32_t nargs, int32_t nres)
{
    if (sos_call(nargs, nres) == 0) {
        return 0;
    }
    sos_to_str(-1, badge__err, sizeof badge__err);
    sos_pop(1);
    return BADGE_ERR;
}

// For actions: a function that answers nothing, true, or nil plus a reason.
// 0, or BADGE_ERR with the reason. Pushes nothing.
static inline int badge__call_ok(int32_t nargs)
{
    if (badge__call(nargs, 2)) {
        return BADGE_ERR;
    }
    int failed = !sos_to_bool(-2) && sos_type(-1) == SOS_STR;
    if (failed) {
        sos_to_str(-1, badge__err, sizeof badge__err);
    }
    sos_pop(2);
    return failed ? BADGE_ERR : 0;
}

// For constructors: keeps the one result as an object.
static inline badge_obj badge__call_obj(int32_t nargs)
{
    badge_obj o = {0};
    if (badge__call(nargs, 2)) {
        return o;
    }
    if (sos_type(-2) <= SOS_NIL) {
        if (sos_type(-1) == SOS_STR) {
            sos_to_str(-1, badge__err, sizeof badge__err);
        } else {
            badge__seterr("no object");
        }
        sos_pop(2);
        return o;
    }
    sos_pop(1);
    o.ref = sos_ref();
    return o;
}

// For readers: the one result as an integer (dflt when it isn't a number).
static inline int32_t badge__call_int(int32_t nargs, int32_t dflt)
{
    if (badge__call(nargs, 1)) {
        return dflt;
    }
    int32_t t = sos_type(-1), v = t == SOS_INT || t == SOS_NUM ? sos_to_int(-1) : dflt;
    sos_pop(1);
    return v;
}

static inline int badge__call_bool(int32_t nargs)
{
    if (badge__call(nargs, 1)) {
        return 0;
    }
    int v = sos_to_bool(-1);
    sos_pop(1);
    return v;
}

// The one result as a string: its length, or BADGE_ERR (buf "") when it
// isn't one (with the second result, a reason, as the error).
static inline int32_t badge__call_str(int32_t nargs, char *buf, uint32_t cap)
{
    if (cap) {
        buf[0] = 0;
    }
    if (badge__call(nargs, 2)) {
        return BADGE_ERR;
    }
    int32_t n = BADGE_ERR;
    if (sos_type(-2) == SOS_STR) {
        n = (int32_t)sos_to_str(-2, buf, cap);
    } else if (sos_type(-1) == SOS_STR) {
        sos_to_str(-1, badge__err, sizeof badge__err);
    } else {
        badge__seterr("nil");
    }
    sos_pop(2);
    return n;
}

// Pushes obj's method `name`, then obj (self): add the arguments, then
// badge__call(1 + nargs, ...).
static inline int badge__method(badge_obj o, const char *name)
{
    if (!o.ref) {
        badge__seterr("no object");
        return BADGE_ERR;
    }
    sos_push_ref(o.ref);
    sos_get(-1, name, badge__len(name));
    sos_remove(-2);
    sos_push_ref(o.ref);
    return 0;
}

static inline void badge__push_color(uint32_t c)
{
    static const char *const theme[] = {"text", "dim", "bg", "panel", "border", "purple", "green",
                                        "pink", "lpurple", "blue", "aqua", "lime", "yellow", "orange"};
    if ((c & 0x80000000u) && (c & 0xFF) < 14) {
        sos_push_str(theme[c & 0xFF], badge__len(theme[c & 0xFF]));
    } else {
        sos_push_int((int32_t)(c & 0xFFFFFF));
    }
}

static inline void badge__push_bytes(const void *p, uint32_t n)
{
    sos_push_str((const char *)p, n);
}

// Field setters for the table on top of the stack (it stays there).
static inline void badge__set(const char *k)
{
    sos_set(-2, k, badge__len(k));
}
static inline void badge__seti(const char *k, int32_t v)
{
    sos_push_int(v);
    badge__set(k);
}
static inline void badge__setb(const char *k, int v)
{
    sos_push_bool(v);
    badge__set(k);
}
static inline void badge__sets(const char *k, const char *v)
{
    if (v) {
        sos_push_cstr(v);
        badge__set(k);
    }
}
static inline void badge__setc(const char *k, uint32_t c) // 0: leave the default
{
    if (c) {
        badge__push_color(c);
        badge__set(k);
    }
}

// A table of strings (an array) on top of the stack.
static inline void badge__push_strings(const char *const *s, int n)
{
    sos_new_table();
    for (int i = 0; i < n; i++) {
        sos_push_cstr(s[i]);
        sos_seti(-2, i + 1);
    }
}

// --- callback slots ---------------------------------------------------------

// Takes a slot for fn/user and returns its token (what the badge hands back
// as ud), or 0 when all are taken.
static inline uint32_t badge__slot_new(badge__fnptr fn, void *user, uint8_t flags)
{
    for (uint32_t i = 0; i < BADGE_CALLBACKS; i++) {
        badge__slot *s = &badge__slots[i];
        if (!s->used) {
            s->used = 1;
            s->gen++;
            s->fn = fn;
            s->user = user;
            s->tag = 0;
            s->flags = flags;
            return (uint32_t)s->gen << 8 | (i + 1);
        }
    }
    badge__seterr("badge.h: more than 64 callbacks waiting");
    return 0;
}

static inline badge__slot *badge__slot_at(uint32_t tok)
{
    uint32_t i = (tok & 0xFF) - 1;
    if (i >= BADGE_CALLBACKS || !badge__slots[i].used || badge__slots[i].gen != (uint16_t)(tok >> 8)) {
        return NULL;
    }
    return &badge__slots[i];
}

static inline void badge__slot_free(uint32_t tok)
{
    badge__slot *s = badge__slot_at(tok);
    if (s) {
        s->used = 0;
    }
}

// Frees every slot that belongs to an object (a list's on_select, a link's events).
static inline void badge__slots_free_owned(int32_t ref)
{
    for (int i = 0; i < BADGE_CALLBACKS; i++) {
        if (badge__slots[i].used && (badge__slots[i].flags & BADGE__OWNED) && badge__slots[i].tag == ref) {
            badge__slots[i].used = 0;
        }
    }
}

// The trampoline's first step: the slot's fn/user, freeing a one-shot slot.
// 0 when the slot is stale (its callback was cancelled) or the callback NULL:
// do nothing then.
static inline int badge__slot_take(void *ud, badge__fnptr *fn, void **user)
{
    uint32_t tok = (uint32_t)(uintptr_t)ud;
    badge__slot *s = badge__slot_at(tok);
    if (!s) {
        return 0;
    }
    *fn = s->fn;
    *user = s->user;
    if (s->flags & BADGE__ONCE) {
        s->used = 0;
    }
    return *fn != NULL; // a NULL callback: nothing to run
}

// Pushes a function value that runs trampoline tr for fn/user. Returns the
// token, or 0 (nothing pushed) when no slot is free.
static inline uint32_t badge__push_cb(sos_fn tr, badge__fnptr fn, void *user, uint8_t flags)
{
    uint32_t tok = badge__slot_new(fn, user, flags);
    if (tok) {
        sos_push_fn(tr, (void *)(uintptr_t)tok);
    }
    return tok;
}

// Copies the string argument at idx into buf (NULL when it isn't a string).
static inline const char *badge__argstr(int32_t idx, char *buf, uint32_t cap, uint32_t *len)
{
    if (sos_type(idx) != SOS_STR) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    uint32_t n = sos_to_str(idx, buf, cap);
    if (len) {
        *len = n < cap ? n : cap - 1;
    }
    return buf;
}

// ============================================================================
// The stack: values, fields, JSON
// ============================================================================

static inline const char *badge_error(void)
{
    return badge__err;
}

static inline badge_val badge_arg(int32_t n) // a callback's argument n (1-based, as the frame)
{
    badge_val v = {n};
    return v;
}
static inline badge_val badge_top(void) // the value on top of the stack
{
    badge_val v = {sos_top()};
    return v;
}
static inline void badge_pop(int32_t n)
{
    sos_pop(n);
}

static inline int32_t badge_type(badge_val v)
{
    return v.idx > 0 ? sos_type(v.idx) : SOS_NONE;
}
static inline int32_t badge_int(badge_val v)
{
    return v.idx > 0 ? sos_to_int(v.idx) : 0;
}
static inline float badge_num(badge_val v)
{
    return v.idx > 0 ? sos_to_num(v.idx) : 0;
}
static inline int badge_bool(badge_val v)
{
    return v.idx > 0 ? sos_to_bool(v.idx) : 0;
}
static inline int32_t badge_str(badge_val v, char *buf, uint32_t cap) // BADGE_ERR if not a string
{
    if (cap) {
        buf[0] = 0;
    }
    return badge_type(v) == SOS_STR ? (int32_t)sos_to_str(v.idx, buf, cap) : BADGE_ERR;
}
static inline uint32_t badge_len(badge_val v) // a string's bytes, a list's length
{
    return v.idx > 0 ? sos_len(v.idx) : 0;
}

// Pushes t[key] / t[i] (i from 0) and returns it as a value: pop it when done.
static inline badge_val badge_push_field(badge_val t, const char *key)
{
    sos_get(t.idx, key, badge__len(key));
    return badge_top();
}
static inline badge_val badge_push_index(badge_val t, int32_t i)
{
    sos_geti(t.idx, i + 1);
    return badge_top();
}

static inline int32_t badge_field_type(badge_val t, const char *key)
{
    int32_t ty = sos_get(t.idx, key, badge__len(key));
    sos_pop(1);
    return ty;
}
static inline int32_t badge_field_int(badge_val t, const char *key, int32_t dflt)
{
    int32_t ty = sos_get(t.idx, key, badge__len(key));
    int32_t v = ty == SOS_INT || ty == SOS_NUM ? sos_to_int(-1) : dflt;
    sos_pop(1);
    return v;
}
static inline float badge_field_num(badge_val t, const char *key, float dflt)
{
    int32_t ty = sos_get(t.idx, key, badge__len(key));
    float v = ty == SOS_INT || ty == SOS_NUM ? sos_to_num(-1) : dflt;
    sos_pop(1);
    return v;
}
static inline int badge_field_bool(badge_val t, const char *key) // nil and false: 0
{
    sos_get(t.idx, key, badge__len(key));
    int v = sos_to_bool(-1);
    sos_pop(1);
    return v;
}
static inline int32_t badge_field_str(badge_val t, const char *key, char *buf, uint32_t cap)
{
    badge_val f = badge_push_field(t, key);
    int32_t n = badge_str(f, buf, cap);
    sos_pop(1);
    return n;
}

// Building tables: badge_new_table() pushes one; badge_set_*() set its fields
// (it stays on top); then pass it (it is the argument) or pop it.
static inline void badge_new_table(void)
{
    sos_new_table();
}
static inline void badge_set_int(const char *key, int32_t v)
{
    badge__seti(key, v);
}
static inline void badge_set_num(const char *key, float v)
{
    sos_push_num(v);
    badge__set(key);
}
static inline void badge_set_bool(const char *key, int v)
{
    badge__setb(key, v);
}
static inline void badge_set_str(const char *key, const char *v)
{
    sos_push_cstr(v);
    badge__set(key);
}
static inline void badge_set_bytes(const char *key, const void *p, uint32_t n)
{
    badge__push_bytes(p, n);
    badge__set(key);
}

// json.decode(text), pushed: 0, or BADGE_ERR with nothing pushed (bad JSON).
static inline int json_push(const char *text)
{
    badge__fn("json.decode");
    sos_push_cstr(text);
    if (badge__call(1, 2)) {
        return BADGE_ERR;
    }
    if (sos_type(-2) == SOS_NIL && sos_type(-1) == SOS_STR) {
        sos_to_str(-1, badge__err, sizeof badge__err);
        sos_pop(2);
        return BADGE_ERR;
    }
    sos_pop(1);
    return 0;
}

// json.encode of the value on top of the stack, which it pops: the text's
// length, or BADGE_ERR.
static inline int32_t badge__encode_top(char *buf, uint32_t cap)
{
    badge__fn("json.encode"); // [v f]
    sos_push_value(-2);       // [v f v]
    int32_t n = badge__call_str(1, buf, cap);
    sos_pop(1);
    return n;
}

// json.encode(v) into buf: its length, or BADGE_ERR.
static inline int32_t json_encode(badge_val v, char *buf, uint32_t cap)
{
    sos_push_value(v.idx);
    return badge__encode_top(buf, cap);
}
static inline int32_t badge_field_json(badge_val t, const char *key, char *buf, uint32_t cap)
{
    sos_get(t.idx, key, badge__len(key));
    return badge__encode_top(buf, cap);
}

// A line on the serial console, as Lua's print (no printf needed).
static inline void badge_print(const char *s)
{
    sos_log(s, badge__len(s));
}
// Stops the app with this message on the error screen.
static inline void badge_fail(const char *msg)
{
    sos_fail(msg, badge__len(msg));
}

// ============================================================================
// Generic callbacks
// ============================================================================

// cb(ok, result) of the Lua API's server calls: err is NULL and result the
// value, or err the reason ("offline", "denied", ...).
typedef void (*badge_result_fn)(void *user, badge_val result, const char *err);

static inline int32_t badge__tr_result(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    if (!badge__slot_take(ud, &fn, &user)) {
        return 0;
    }
    (void)nargs;
    if (sos_to_bool(1)) {
        ((badge_result_fn)fn)(user, badge_arg(2), NULL);
    } else {
        char err[160];
        if (!badge__argstr(2, err, sizeof err, NULL)) {
            badge__seterr("failed");
            memcpy(err, badge__err, sizeof err);
        }
        badge_val none = {0};
        ((badge_result_fn)fn)(user, none, err);
    }
    return 0;
}

// Calls the API function already pushed, with nargs arguments pushed, plus a
// result callback: 0 or BADGE_ERR.
static inline int badge__call_with_cb(int32_t nargs, sos_fn tr, badge__fnptr fn, void *user)
{
    uint32_t tok = badge__push_cb(tr, fn, user, BADGE__ONCE);
    if (!tok) {
        sos_pop(nargs + 1);
        return BADGE_ERR;
    }
    if (badge__call_ok(nargs + 1)) {
        badge__slot_free(tok);
        return BADGE_ERR;
    }
    return 0;
}

// ============================================================================
// ui
// ============================================================================

enum { UI_LEFT = 0, UI_CENTER = 1, UI_RIGHT = 2 };

typedef struct {
    int size;       // 12, 14 (default), 20, 32; 12 is always mono
    uint32_t color; // default COL_TEXT
    int mono;
    int w;          // width, for align
    int align;      // UI_LEFT, UI_CENTER, UI_RIGHT (needs w)
} ui_label_opts;

typedef struct {
    uint32_t color;  // default COL_PANEL
    int radius;
    uint32_t border; // a 1 px border of this colour (0: none)
} ui_rect_opts;

typedef struct {
    const char *title, *sub, *right;
} ui_row;

typedef void (*ui_select_fn)(void *user, int index);
typedef void (*ui_confirm_fn)(void *user, int yes);
typedef void (*ui_input_fn)(void *user, const char *text); // NULL: cancelled

static inline int ui_bg(uint32_t color)
{
    badge__fn("ui.bg");
    badge__push_color(color);
    return badge__call_ok(1);
}

static inline ui_obj ui_label(int x, int y, const char *text, const ui_label_opts *o)
{
    static const char *const align[] = {"left", "center", "right"};
    badge__fn("ui.label");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_cstr(text ? text : "");
    sos_new_table();
    if (o) {
        if (o->size) {
            badge__seti("size", o->size);
        }
        badge__setc("color", o->color);
        if (o->mono) {
            badge__setb("mono", 1);
        }
        if (o->w) {
            badge__seti("w", o->w);
        }
        if (o->align > 0 && o->align <= 2) {
            badge__sets("align", align[o->align]);
        }
    }
    return badge__call_obj(4);
}

static inline int ui_label_text(ui_obj l, const char *text)
{
    if (badge__method(l, "text")) {
        return BADGE_ERR;
    }
    sos_push_cstr(text ? text : "");
    return badge__call_ok(2);
}

static inline int ui_label_color(ui_obj l, uint32_t color)
{
    if (badge__method(l, "color")) {
        return BADGE_ERR;
    }
    badge__push_color(color);
    return badge__call_ok(2);
}

static inline ui_obj ui_rect(int x, int y, int w, int h, const ui_rect_opts *o)
{
    badge__fn("ui.rect");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_int(w);
    sos_push_int(h);
    sos_new_table();
    if (o) {
        badge__setc("color", o->color);
        if (o->radius) {
            badge__seti("radius", o->radius);
        }
        badge__setc("border", o->border);
    }
    return badge__call_obj(5);
}

static inline int ui_rect_size(ui_obj r, int w, int h)
{
    if (badge__method(r, "size")) {
        return BADGE_ERR;
    }
    sos_push_int(w);
    sos_push_int(h);
    return badge__call_ok(3);
}

static inline int ui_rect_color(ui_obj r, uint32_t color)
{
    return ui_label_color(r, color); // the same method
}

static inline ui_obj ui_bar(int x, int y, int w, int h, int value)
{
    badge__fn("ui.bar");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_int(w);
    sos_push_int(h);
    sos_push_int(value);
    return badge__call_obj(5);
}

static inline int ui_bar_set(ui_obj b, int value) // 0..100
{
    if (badge__method(b, "set")) {
        return BADGE_ERR;
    }
    sos_push_int(value);
    return badge__call_ok(2);
}

static inline ui_obj ui_gradient(int x, int y, int w, int h)
{
    badge__fn("ui.gradient");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_int(w);
    sos_push_int(h);
    return badge__call_obj(4);
}

// --- any object --------------------------------------------------------------

static inline int ui_obj_pos(ui_obj o, int x, int y)
{
    if (badge__method(o, "pos")) {
        return BADGE_ERR;
    }
    sos_push_int(x);
    sos_push_int(y);
    return badge__call_ok(3);
}

static inline int ui_obj_show(ui_obj o, int show)
{
    if (badge__method(o, "show")) {
        return BADGE_ERR;
    }
    sos_push_bool(show);
    return badge__call_ok(2);
}

// Forgets a handle without removing anything (an image from ui_load, a link
// or socket you no longer need to reach). Frees its callbacks' slots.
static inline void badge_obj_free(badge_obj *o)
{
    if (o->ref) {
        badge__slots_free_owned(o->ref);
        sos_unref(o->ref);
        o->ref = 0;
    }
}

// Removes the object from the screen and frees the handle (o becomes {0}).
static inline int ui_obj_remove(ui_obj *o)
{
    if (badge__method(*o, "remove")) {
        return BADGE_ERR;
    }
    int r = badge__call_ok(1);
    badge_obj_free(o);
    return r;
}

// --- lists ---------------------------------------------------------------------

static inline int32_t badge__tr_select(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (badge__slot_take(ud, &fn, &user)) {
        ((ui_select_fn)fn)(user, sos_to_int(1) - 1);
    }
    return 0;
}

static inline void badge__push_rows(const ui_row *rows, int n)
{
    sos_new_table();
    for (int i = 0; i < n; i++) {
        sos_new_table();
        badge__sets("title", rows[i].title);
        badge__sets("sub", rows[i].sub);
        badge__sets("right", rows[i].right);
        sos_seti(-2, i + 1);
    }
}

// ui.list with the items table already pushed above the function, x, y, w, h.
static inline ui_obj badge__list(ui_select_fn on_select, void *user)
{
    ui_obj l = {0};
    uint32_t tok = 0;
    sos_new_table();
    if (on_select) {
        tok = badge__push_cb(badge__tr_select, (badge__fnptr)on_select, user, BADGE__OWNED);
        if (!tok) {
            sos_pop(7);
            return l;
        }
        badge__set("on_select");
    }
    l = badge__call_obj(6);
    badge__slot *s = tok ? badge__slot_at(tok) : NULL;
    if (s && l.ref) {
        s->tag = l.ref;
    } else if (tok) {
        badge__slot_free(tok);
    }
    return l;
}

// A list of one-line items; on_select(user, index) on SELECT (may be NULL).
// The list takes UP/DOWN. Rows are 36 px.
static inline ui_obj ui_list(int x, int y, int w, int h, const char *const *items, int n, ui_select_fn on_select,
                             void *user)
{
    badge__fn("ui.list");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_int(w);
    sos_push_int(h);
    badge__push_strings(items, n);
    return badge__list(on_select, user);
}

// A list of rows with a title, a second line and a right-hand text.
static inline ui_obj ui_list_rows(int x, int y, int w, int h, const ui_row *rows, int n, ui_select_fn on_select,
                                  void *user)
{
    badge__fn("ui.list");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_int(w);
    sos_push_int(h);
    badge__push_rows(rows, n);
    return badge__list(on_select, user);
}

static inline int ui_list_items(ui_obj list, const char *const *items, int n)
{
    if (badge__method(list, "items")) {
        return BADGE_ERR;
    }
    badge__push_strings(items, n);
    return badge__call_ok(2);
}

static inline int ui_list_set_rows(ui_obj list, const ui_row *rows, int n)
{
    if (badge__method(list, "items")) {
        return BADGE_ERR;
    }
    badge__push_rows(rows, n);
    return badge__call_ok(2);
}

static inline int ui_list_selected(ui_obj list) // the row, or -1
{
    if (badge__method(list, "selected")) {
        return BADGE_ERR;
    }
    return badge__call_int(1, 0) - 1;
}

static inline int ui_list_select(ui_obj list, int index)
{
    if (badge__method(list, "select")) {
        return BADGE_ERR;
    }
    sos_push_int(index + 1);
    return badge__call_ok(2);
}

// --- QR codes, hints, dialogs ---------------------------------------------------

// Black on white, size x size px (25..218), text <= 256 bytes. One at a time.
static inline ui_obj ui_qr(int x, int y, int size, const char *text)
{
    badge__fn("ui.qr");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_int(size);
    sos_push_cstr(text);
    return badge__call_obj(4);
}

static inline int ui_qr_text(ui_obj q, const char *text)
{
    return ui_label_text(q, text); // the same method name
}

static inline int ui_hint(const char *text) // pinned to the bottom (16 px)
{
    badge__fn("ui.hint");
    sos_push_cstr(text);
    return badge__call_ok(1);
}

static inline int ui_toast(const char *text)
{
    badge__fn("ui.toast");
    sos_push_cstr(text);
    return badge__call_ok(1);
}

static inline int32_t badge__tr_confirm(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (badge__slot_take(ud, &fn, &user)) {
        ((ui_confirm_fn)fn)(user, sos_to_bool(1));
    }
    return 0;
}

// A modal question: SELECT = yes, CANCEL = no.
static inline int ui_confirm(const char *text, ui_confirm_fn cb, void *user)
{
    badge__fn("ui.confirm");
    sos_push_cstr(text);
    return badge__call_with_cb(1, badge__tr_confirm, (badge__fnptr)cb, user);
}

static inline int32_t badge__tr_input(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (badge__slot_take(ud, &fn, &user)) {
        char text[260];
        ((ui_input_fn)fn)(user, badge__argstr(1, text, sizeof text, NULL));
    }
    return 0;
}

// The d-pad keyboard: cb(user, text), text NULL when cancelled.
static inline int ui_input(const char *prompt, int maxlen, ui_input_fn cb, void *user)
{
    badge__fn("ui.input");
    sos_push_cstr(prompt);
    sos_push_int(maxlen);
    return badge__call_with_cb(2, badge__tr_input, (badge__fnptr)cb, user);
}

// Removes every object. Handles you still hold point at nothing: free them
// with badge_obj_free().
static inline int ui_clear(void)
{
    badge__fn("ui.clear");
    return badge__call_ok(0);
}

// --- images ----------------------------------------------------------------------

// A pixel-art palette entry: character ch is colour (COL_NONE: see-through).
typedef struct {
    char ch;
    uint32_t color;
} ui_pal;

static inline void badge__push_palette(const ui_pal *pal, int n)
{
    sos_new_table();
    for (int i = 0; i < n; i++) {
        if (pal[i].color == COL_NONE) {
            sos_push_bool(0);
        } else {
            badge__push_color(pal[i].color);
        }
        sos_set(-2, &pal[i].ch, 1);
    }
}

// An image's options: {scale = s, palette = ...}.
static inline void badge__push_img_opts(int scale, const ui_pal *pal, int npal)
{
    sos_new_table();
    if (scale) {
        badge__seti("scale", scale);
    }
    if (pal && npal) {
        badge__push_palette(pal, npal);
        badge__set("palette");
    }
}

// Loads a bundled PNG or GIF (its first frame) once, to show or draw often.
static inline ui_img ui_load(const char *path)
{
    badge__fn("ui.load");
    sos_push_cstr(path);
    return badge__call_obj(1);
}

// Loads pixel art: one character per pixel ("..pp..", LUA_API.md "Images").
static inline ui_img ui_load_rows(const char *const *rows, int n, const ui_pal *pal, int npal)
{
    badge__fn("ui.load");
    badge__push_strings(rows, n);
    badge__push_img_opts(0, pal, npal);
    return badge__call_obj(2);
}

static inline int ui_img_size(ui_img img, int *w, int *h)
{
    if (badge__method(img, "size") || badge__call(1, 2)) {
        return BADGE_ERR;
    }
    if (w) {
        *w = sos_to_int(-2);
    }
    if (h) {
        *h = sos_to_int(-1);
    }
    sos_pop(2);
    return 0;
}

// ui.image(x, y, <source>, {scale}) with the source pushed above x, y.
static inline ui_obj badge__image(int scale, const ui_pal *pal, int npal)
{
    badge__push_img_opts(scale, pal, npal);
    return badge__call_obj(4);
}

static inline ui_obj ui_image(int x, int y, const char *path, int scale) // scale 0: 1
{
    badge__fn("ui.image");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_cstr(path);
    return badge__image(scale, NULL, 0);
}

static inline ui_obj ui_image_of(int x, int y, ui_img img, int scale)
{
    badge__fn("ui.image");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_ref(img.ref);
    return badge__image(scale, NULL, 0);
}

static inline ui_obj ui_image_rows(int x, int y, const char *const *rows, int n, const ui_pal *pal, int npal,
                                   int scale)
{
    badge__fn("ui.image");
    sos_push_int(x);
    sos_push_int(y);
    badge__push_strings(rows, n);
    return badge__image(scale, pal, npal);
}

static inline int ui_image_set(ui_obj o, const char *path)
{
    if (badge__method(o, "set")) {
        return BADGE_ERR;
    }
    sos_push_cstr(path);
    return badge__call_ok(2);
}

static inline int ui_image_set_img(ui_obj o, ui_img img)
{
    if (badge__method(o, "set")) {
        return BADGE_ERR;
    }
    sos_push_ref(img.ref);
    return badge__call_ok(2);
}

static inline int ui_image_scale(ui_obj o, int scale) // 1..16; animations too
{
    if (badge__method(o, "scale")) {
        return BADGE_ERR;
    }
    sos_push_int(scale);
    return badge__call_ok(2);
}

// --- canvas ------------------------------------------------------------------------

// w x h pixels shown at scale (0: 1), filled with color (0: see-through).
static inline ui_obj ui_canvas(int x, int y, int w, int h, int scale, uint32_t color)
{
    badge__fn("ui.canvas");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_int(w);
    sos_push_int(h);
    sos_new_table();
    if (scale) {
        badge__seti("scale", scale);
    }
    badge__setc("color", color);
    return badge__call_obj(5);
}

static inline int ui_canvas_clear(ui_obj c, uint32_t color) // COL_NONE: see-through
{
    if (badge__method(c, "clear")) {
        return BADGE_ERR;
    }
    if (color == COL_NONE) {
        return badge__call_ok(1);
    }
    badge__push_color(color);
    return badge__call_ok(2);
}

static inline int ui_canvas_pixel(ui_obj c, int x, int y, uint32_t color)
{
    if (badge__method(c, "pixel")) {
        return BADGE_ERR;
    }
    sos_push_int(x);
    sos_push_int(y);
    badge__push_color(color);
    return badge__call_ok(4);
}

static inline int ui_canvas_rect(ui_obj c, int x, int y, int w, int h, uint32_t color) // filled
{
    if (badge__method(c, "rect")) {
        return BADGE_ERR;
    }
    sos_push_int(x);
    sos_push_int(y);
    sos_push_int(w);
    sos_push_int(h);
    badge__push_color(color);
    return badge__call_ok(6);
}

static inline int ui_canvas_line(ui_obj c, int x1, int y1, int x2, int y2, uint32_t color)
{
    if (badge__method(c, "line")) {
        return BADGE_ERR;
    }
    sos_push_int(x1);
    sos_push_int(y1);
    sos_push_int(x2);
    sos_push_int(y2);
    badge__push_color(color);
    return badge__call_ok(6);
}

// Stamps an image; its see-through pixels leave the canvas as it was.
static inline int ui_canvas_image(ui_obj c, ui_img img, int x, int y)
{
    if (badge__method(c, "image")) {
        return BADGE_ERR;
    }
    sos_push_ref(img.ref);
    sos_push_int(x);
    sos_push_int(y);
    return badge__call_ok(4);
}

static inline int ui_canvas_image_path(ui_obj c, const char *path, int x, int y)
{
    if (badge__method(c, "image")) {
        return BADGE_ERR;
    }
    sos_push_cstr(path);
    sos_push_int(x);
    sos_push_int(y);
    return badge__call_ok(4);
}

// 0xRRGGBB, or -1 where it's see-through or outside.
static inline int32_t ui_canvas_get(ui_obj c, int x, int y)
{
    if (badge__method(c, "get")) {
        return -1;
    }
    sos_push_int(x);
    sos_push_int(y);
    return badge__call_int(3, -1);
}

// A whole frame straight to the screen, past the UI (sos_frame): w x h RGB565
// pixels scaled to dw x dh at (x, y) of the app area. UI_FRAME_BE: the pixels
// are big-endian (RGB565_BE()), which lets an unscaled frame in a 64-byte-aligned
// buffer go to the screen without a copy. 1 shown, 0 hidden (a dialog or the
// badge's panel is over the app), BADGE_ERR outside the app area.
#define UI_FRAME_BE SOS_FRAME_BE
static inline int ui_frame(const uint16_t *px, int w, int h, int x, int y, int dw, int dh, int flags)
{
    int32_t r = sos_frame(px, w, h, x, y, dw, dh, (uint32_t)flags);
    if (r < 0) {
        badge__seterr("the frame is outside the app area");
        return BADGE_ERR;
    }
    return r;
}

// w x h RGB565 pixels from your memory onto the canvas at (x, y), clipped:
// the fast way to draw a whole frame (sos_blit). 0, or BADGE_ERR.
static inline int ui_canvas_blit(ui_obj c, const uint16_t *px, int x, int y, int w, int h)
{
    if (!c.ref) {
        badge__seterr("no object");
        return BADGE_ERR;
    }
    sos_push_ref(c.ref);
    int32_t r = sos_blit(-1, px, x, y, w, h);
    sos_pop(1);
    if (r) {
        badge__seterr("not a canvas");
        return BADGE_ERR;
    }
    return 0;
}

// --- animations ----------------------------------------------------------------------

enum { UI_LOOP_FILE = 0, UI_LOOP_FOREVER = -1, UI_LOOP_ONCE = -2 }; // or a number of times

typedef struct {
    int scale; // 0: 1
    int loop;  // UI_LOOP_FILE (what the GIF says), UI_LOOP_FOREVER, UI_LOOP_ONCE, or n > 0
} ui_anim_opts;

// A bundled GIF, playing.
static inline ui_obj ui_anim(int x, int y, const char *path, const ui_anim_opts *o)
{
    badge__fn("ui.anim");
    sos_push_int(x);
    sos_push_int(y);
    sos_push_cstr(path);
    sos_new_table();
    if (o && o->scale) {
        badge__seti("scale", o->scale);
    }
    if (o && o->loop == UI_LOOP_FOREVER) {
        badge__setb("loop", 1);
    } else if (o && o->loop == UI_LOOP_ONCE) {
        badge__setb("loop", 0);
    } else if (o && o->loop > 0) {
        badge__seti("loop", o->loop);
    }
    return badge__call_obj(4);
}

static inline int ui_anim_play(ui_obj a) // after the end it starts over
{
    return badge__method(a, "play") ? BADGE_ERR : badge__call_ok(1);
}
static inline int ui_anim_stop(ui_obj a)
{
    return badge__method(a, "stop") ? BADGE_ERR : badge__call_ok(1);
}
static inline int ui_anim_playing(ui_obj a)
{
    return badge__method(a, "playing") ? 0 : badge__call_bool(1);
}

// ============================================================================
// led (permission "led")
// ============================================================================

static inline int led_set(int i, int r, int g, int b) // i 0..7; 0..255 each, scaled to the badge's cap
{
    badge__fn("led.set");
    sos_push_int(i + 1);
    sos_push_int(r);
    sos_push_int(g);
    sos_push_int(b);
    return badge__call_ok(4);
}

static inline int led_hex(int i, uint32_t hex)
{
    badge__fn("led.hex");
    sos_push_int(i + 1);
    sos_push_int((int32_t)(hex & 0xFFFFFF));
    return badge__call_ok(2);
}

static inline int led_fill(int r, int g, int b)
{
    badge__fn("led.fill");
    sos_push_int(r);
    sos_push_int(g);
    sos_push_int(b);
    return badge__call_ok(3);
}

static inline int led_off(void)
{
    badge__fn("led.off");
    return badge__call_ok(0);
}

// ============================================================================
// timer
// ============================================================================

typedef void (*timer_fn)(void *user);

static inline int32_t badge__tr_timer(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (badge__slot_take(ud, &fn, &user)) {
        ((timer_fn)fn)(user);
    }
    return 0;
}

static inline int32_t badge__timer(const char *which, int32_t ms, timer_fn fn, void *user, uint8_t flags)
{
    badge__fn(which);
    sos_push_int(ms);
    uint32_t tok = badge__push_cb(badge__tr_timer, (badge__fnptr)fn, user, flags | BADGE__TIMER);
    if (!tok) {
        sos_pop(2);
        return BADGE_ERR;
    }
    int32_t id = badge__call_int(2, 0);
    badge__slot *s = badge__slot_at(tok);
    if (id <= 0) {
        badge__slot_free(tok);
        return BADGE_ERR;
    }
    if (s) {
        s->tag = id;
    }
    return id;
}

// Runs fn(user) once after ms (less than 20 is taken as 20). The timer's id
// (> 0), or BADGE_ERR (8 at a time).
static inline int32_t timer_after(int32_t ms, timer_fn fn, void *user)
{
    return badge__timer("timer.after", ms, fn, user, BADGE__ONCE);
}

// Runs fn(user) every ms until timer_cancel().
static inline int32_t timer_every(int32_t ms, timer_fn fn, void *user)
{
    return badge__timer("timer.every", ms, fn, user, 0);
}

static inline void timer_cancel(int32_t id)
{
    if (id <= 0) {
        return;
    }
    badge__fn("timer.cancel");
    sos_push_int(id);
    badge__call_ok(1);
    for (int i = 0; i < BADGE_CALLBACKS; i++) {
        badge__slot *s = &badge__slots[i];
        if (s->used && (s->flags & BADGE__TIMER) && s->tag == id) {
            s->used = 0;
        }
    }
}

// ============================================================================
// sys, badge
// ============================================================================

static inline uint32_t sys_millis(void) // ms since the app started
{
    return sos_millis();
}

static inline uint32_t sys_keys(void) // the keys held now: KEY_HELD(sys_keys(), KEY_LEFT)
{
    return sos_keys();
}

static inline int32_t sys_time(void) // unix seconds from the server clock, 0 if never synced
{
    badge__fn("sys.time");
    return badge__call_int(0, 0);
}

static inline int32_t sys_battery(void) // percent, or -1 on USB / unknown
{
    badge__fn("sys.battery");
    return badge__call_int(0, -1);
}

// The app's heap and its cap (6 MB with PSRAM): the Lua state behind the API
// (labels' text, loaded images, ...) and WAMR's share (the module, its
// instance, your linear memory with its stack and malloc, WAMR's own stack)
// together, as one cap covers them (docs/C_API.md "Limits").
static inline int sys_memory(int32_t *used, int32_t *max)
{
    badge__fn("sys.memory");
    if (badge__call(0, 1)) {
        return BADGE_ERR;
    }
    badge_val t = badge_top();
    if (used) {
        *used = badge_field_int(t, "used", 0);
    }
    if (max) {
        *max = badge_field_int(t, "max", 0);
    }
    sos_pop(1);
    return 0;
}

// on_tick's period from now on, 16..1000 ms (the default is 30): a game that
// wants every 16.7 ms frame asks 16 or 17. Returns the period before, or
// BADGE_ERR. (sys.tick, Solana OS 2.3.0)
static inline int32_t sys_tick(int32_t ms)
{
    badge__fn("sys.tick");
    sos_push_int(ms);
    return badge__call_int(1, BADGE_ERR);
}

static inline void sys_exit(void) // closes the app after this callback
{
    badge__fn("sys.exit");
    badge__call_ok(0);
}

static inline int32_t badge_id(char *buf, uint32_t cap) // "7K2Q9M"; BADGE_ERR before the first hello
{
    badge__fn("badge.id");
    return badge__call_str(0, buf, cap);
}

static inline int32_t badge_owner(char *buf, uint32_t cap) // "@toly"; BADGE_ERR while unclaimed
{
    badge__fn("badge.owner");
    return badge__call_str(0, buf, cap);
}

static inline int badge_online(void)
{
    badge__fn("badge.online");
    return badge__call_bool(0);
}

// ============================================================================
// storage (always allowed): 16 KB of JSON-encodable values per app
// ============================================================================

// Pushes storage.get(key) (nil when unset): pop it when done.
static inline badge_val storage_push(const char *key)
{
    badge__fn("storage.get");
    sos_push_cstr(key);
    if (badge__call(1, 1)) {
        sos_push_nil();
    }
    return badge_top();
}

static inline int32_t storage_get_int(const char *key, int32_t dflt)
{
    badge_val v = storage_push(key);
    int32_t t = badge_type(v), r = t == SOS_INT || t == SOS_NUM ? badge_int(v) : dflt;
    sos_pop(1);
    return r;
}

static inline int32_t storage_get_str(const char *key, char *buf, uint32_t cap) // BADGE_ERR: unset
{
    badge_val v = storage_push(key);
    int32_t n = badge_str(v, buf, cap);
    sos_pop(1);
    return n;
}

static inline int32_t storage_get_json(const char *key, char *buf, uint32_t cap) // "null" when unset
{
    storage_push(key);
    return badge__encode_top(buf, cap);
}

// storage.set(key, <the value on top of the stack>), which it pops.
static inline int badge__storage_set_top(const char *key)
{
    badge__fn("storage.set"); // [v f]
    sos_push_cstr(key);       // [v f k]
    sos_push_value(-3);       // [v f k v]
    sos_remove(-4);           // [f k v]
    return badge__call_ok(2);
}

static inline int storage_set_int(const char *key, int32_t v)
{
    sos_push_int(v);
    return badge__storage_set_top(key);
}
static inline int storage_set_bool(const char *key, int v)
{
    sos_push_bool(v);
    return badge__storage_set_top(key);
}
static inline int storage_set_str(const char *key, const char *v)
{
    sos_push_cstr(v);
    return badge__storage_set_top(key);
}
static inline int storage_set_json(const char *key, const char *json)
{
    if (json_push(json)) {
        return BADGE_ERR;
    }
    return badge__storage_set_top(key);
}
static inline int storage_remove(const char *key)
{
    badge__fn("storage.remove");
    sos_push_cstr(key);
    return badge__call_ok(1);
}

// ============================================================================
// files (permission "files", Solana OS 2.3.0): the app's own folder on the
// card, hh/files/<app id>/: files the badge's owner copied there (read-only)
// and the app's saves in save/ ("save/<name>", read and write)
// ============================================================================

// The folder as the owner sees it on the card: "hh/files/<id>/".
static inline int32_t files_dir(char *buf, uint32_t cap)
{
    badge__fn("files.dir");
    return badge__call_str(0, buf, cap);
}

static inline int32_t files_size(const char *name) // bytes, or -1
{
    badge__fn("files.size");
    sos_push_cstr(name);
    return badge__call_int(1, -1);
}

// Up to len (<= 256 KB) bytes from offset into buf, straight into your memory
// (no copy on the way): how many (0 at the end), or BADGE_ERR (no such file,
// or the card failed).
static inline int32_t files_read(const char *name, uint32_t offset, void *buf, uint32_t len)
{
    int32_t n = sos_file_read(name, badge__len(name), offset, buf, len);
    if (n < 0) {
        badge__seterr("can't read it (no such file?)");
        return BADGE_ERR;
    }
    return n;
}

// Writes len (<= 256 KB) bytes at offset of "save/<name>" (offset at most its
// size; a new file from 0): 0, or BADGE_ERR ("a save file is at most 1 MB",
// "at most 32 files in save/", ...).
static inline int files_write(const char *name, uint32_t offset, const void *data, uint32_t len)
{
    badge__fn("files.write");
    sos_push_cstr(name);
    sos_push_int((int32_t)offset);
    badge__push_bytes(data, len);
    return badge__call_ok(3);
}

static inline int files_remove(const char *name) // a save only; 1 removed
{
    badge__fn("files.remove");
    sos_push_cstr(name);
    return badge__call_bool(1);
}

// Calls fn(user, name, size) for each file, the owner's first, then the saves
// ("save/<name>"), at most 128. Returns how many, or BADGE_ERR.
static inline int32_t files_list(void (*fn)(void *user, const char *name, int32_t size), void *user)
{
    badge__fn("files.list");
    if (badge__call(0, 1)) {
        return BADGE_ERR;
    }
    badge_val t = badge_top();
    int32_t n = (int32_t)badge_len(t);
    for (int32_t i = 0; i < n; i++) {
        char name[80];
        badge_val v = badge_push_index(t, i);
        badge_field_str(v, "name", name, sizeof name);
        int32_t size = badge_field_int(v, "size", 0);
        sos_pop(1);
        fn(user, name, size);
    }
    sos_pop(1);
    return n;
}

// ============================================================================
// fs (always allowed): the app's bundled files, read-only
// ============================================================================

static inline int fs_exists(const char *path) // a file or a folder
{
    badge__fn("fs.exists");
    sos_push_cstr(path);
    return badge__call_bool(1);
}

static inline int32_t fs_size(const char *path) // bytes, or -1
{
    badge__fn("fs.size");
    sos_push_cstr(path);
    return badge__call_int(1, -1);
}

// The whole file (<= 256 KB) into buf: its length (cap must be one more for
// all of it), or BADGE_ERR ("not found"). Binary data is fine.
static inline int32_t fs_read(const char *path, void *buf, uint32_t cap)
{
    badge__fn("fs.read");
    sos_push_cstr(path);
    return badge__call_str(1, (char *)buf, cap);
}

// len bytes from offset (counting from 0); "" past the end.
static inline int32_t fs_read_at(const char *path, int32_t offset, int32_t len, void *buf, uint32_t cap)
{
    badge__fn("fs.read");
    sos_push_cstr(path);
    sos_push_int(offset);
    sos_push_int(len);
    return badge__call_str(3, (char *)buf, cap);
}

// Calls fn(user, name) for each entry, sorted, folders ending in "/"
// (path NULL: the top). Returns how many, or BADGE_ERR.
static inline int32_t fs_list(const char *path, void (*fn)(void *user, const char *name), void *user)
{
    badge__fn("fs.list");
    if (path) {
        sos_push_cstr(path);
    }
    if (badge__call(path ? 1 : 0, 1)) {
        return BADGE_ERR;
    }
    badge_val t = badge_top();
    int32_t n = (int32_t)badge_len(t);
    for (int32_t i = 0; i < n; i++) {
        char name[72];
        badge_val v = badge_push_index(t, i);
        badge_str(v, name, sizeof name);
        sos_pop(1);
        fn(user, name);
    }
    sos_pop(1);
    return n;
}

// Calls fn(user, line, len) for each line, without its "\n" or "\r\n"
// (<= 512 bytes; longer lines come in pieces). Returns how many, or BADGE_ERR.
static inline int32_t fs_lines(const char *path, void (*fn)(void *user, const char *line, uint32_t len), void *user)
{
    badge__fn("fs.lines");
    sos_push_cstr(path);
    if (badge__call(1, 1)) {
        return BADGE_ERR;
    }
    int32_t n = 0;
    for (;;) {
        sos_push_value(-1); // [it it]
        if (badge__call(0, 1)) {
            n = BADGE_ERR;
            break;
        }
        if (sos_type(-1) != SOS_STR) {
            sos_pop(1);
            break;
        }
        uint32_t len = sos_to_str(-1, badge__line, 513);
        sos_pop(1);
        fn(user, badge__line, len < 513 ? len : 512);
        n++;
    }
    sos_pop(1);
    return n;
}

// Pushes fs.json(path), the decoded file: 0, or BADGE_ERR with nothing pushed.
static inline int fs_json_push(const char *path)
{
    badge__fn("fs.json");
    sos_push_cstr(path);
    if (badge__call(1, 2)) {
        return BADGE_ERR;
    }
    if (sos_type(-2) == SOS_NIL && sos_type(-1) == SOS_STR) {
        sos_to_str(-1, badge__err, sizeof badge__err);
        sos_pop(2);
        return BADGE_ERR;
    }
    sos_pop(1);
    return 0;
}

// ============================================================================
// wifi (permission "wifi")
// ============================================================================

enum { WIFI_OFF, WIFI_CONNECTING, WIFI_ON, WIFI_FAILED };

static inline int wifi_status(void) // WIFI_*, or BADGE_ERR
{
    static const char *const names[] = {"off", "connecting", "on", "failed"};
    char s[16];
    badge__fn("wifi.status");
    if (badge__call_str(0, s, sizeof s) < 0) {
        return BADGE_ERR;
    }
    for (int i = 0; i < 4; i++) {
        if (!strcmp(s, names[i])) {
            return i;
        }
    }
    return BADGE_ERR;
}

static inline int32_t wifi_ssid(char *buf, uint32_t cap) // the saved network, BADGE_ERR if none
{
    badge__fn("wifi.ssid");
    return badge__call_str(0, buf, cap);
}

static inline int wifi_on(void) // joins the saved network
{
    badge__fn("wifi.on");
    return badge__call_ok(0);
}

static inline int wifi_off(void)
{
    badge__fn("wifi.off");
    return badge__call_ok(0);
}

// ============================================================================
// mesh (permission "mesh")
// ============================================================================

typedef struct {
    char id[8];     // "7K2Q9M"
    char name[32];  // "@toly", "" when none
    char kind[8];   // "badge" or "beacon"
    int32_t level;
    int32_t rssi;   // 0 for a bridged badge
    int bridged;
    int app;        // mesh_scan_peer: heard sending this app's messages
} mesh_peer;

static inline void badge__peer(badge_val p, mesh_peer *out)
{
    badge_field_str(p, "id", out->id, sizeof out->id);
    badge_field_str(p, "name", out->name, sizeof out->name);
    badge_field_str(p, "kind", out->kind, sizeof out->kind);
    out->level = badge_field_int(p, "level", 0);
    out->rssi = badge_field_int(p, "rssi", 0);
    out->bridged = badge_field_bool(p, "bridged");
    out->app = badge_field_bool(p, "app");
}

// Badges and beacons in range or bridged, strongest first, into out[0..max):
// returns how many there are (may be more than max), or BADGE_ERR.
static inline int mesh_nearby(mesh_peer *out, int max)
{
    badge__fn("mesh.nearby");
    if (badge__call(0, 1)) {
        return BADGE_ERR;
    }
    badge_val t = badge_top();
    int n = (int)badge_len(t);
    for (int i = 0; i < n && i < max; i++) {
        badge__peer(badge_push_index(t, i), &out[i]);
        sos_pop(1);
    }
    sos_pop(1);
    return n;
}

// Sends a message (a JSON object, <= 200 bytes as JSON) to a badge id, or to
// every badge running this app ("*" or NULL). on_mesh() gets it there.
static inline int mesh_send(const char *to, const char *json)
{
    badge__fn("mesh.send");
    sos_push_cstr(to ? to : "*");
    if (json_push(json)) {
        sos_pop(2);
        return BADGE_ERR;
    }
    return badge__call_ok(2);
}

// The same, with a table you built on top of the stack (popped).
static inline int mesh_send_top(const char *to)
{
    badge__fn("mesh.send");   // [t f]
    sos_push_cstr(to ? to : "*"); // [t f to]
    sos_push_value(-3);       // [t f to t]
    sos_remove(-4);           // [f to t]
    return badge__call_ok(2);
}

static inline int mesh_bridge(void) // shows this badge's bridge code over the app
{
    badge__fn("mesh.bridge");
    return badge__call_ok(0);
}

static inline int mesh_pause(void) // gives the radio back to Wi-Fi
{
    badge__fn("mesh.pause");
    return badge__call_ok(0);
}

static inline int mesh_resume(void)
{
    badge__fn("mesh.resume");
    return badge__call_ok(0);
}

typedef struct {
    int32_t channel; // the channel the app's mesh uses now
    int32_t event;   // the event's channel, outside the app
    char band[4];    // "2.4" or "5"
    int online;      // Wi-Fi joined on the mesh channel
    int paused;
    int scanning;
} mesh_info_t;

static inline int mesh_info(mesh_info_t *out)
{
    badge__fn("mesh.info");
    if (badge__call(0, 1)) {
        return BADGE_ERR;
    }
    badge_val t = badge_top();
    out->channel = badge_field_int(t, "channel", 0);
    out->event = badge_field_int(t, "event", 0);
    badge_field_str(t, "band", out->band, sizeof out->band);
    out->online = badge_field_bool(t, "online");
    out->paused = badge_field_bool(t, "paused");
    out->scanning = badge_field_bool(t, "scanning");
    sos_pop(1);
    return 0;
}

// Moves this badge's mesh to channel n while the app runs; 0 goes back.
static inline int mesh_channel(int n)
{
    badge__fn("mesh.channel");
    sos_push_int(n);
    return badge__call_ok(1);
}

typedef struct {
    const int *channels; // the channels to visit, in order (up to 22), or NULL
    int nchannels;
    const char *band;    // instead: "2.4", "5" or "all" (NULL: the badge's band)
    int dwell;           // ms per channel, 500..5000 (0: 2600)
} mesh_scan_opts;

// cb(user, channels, count): one entry per channel visited, read with
// mesh_scan_channel() and mesh_scan_peer() during the callback.
typedef void (*mesh_scan_fn)(void *user, badge_val channels, int count);

typedef struct {
    int32_t channel, badges, beacons, app, npeers;
    char band[4];
} mesh_channel_t;

static inline int32_t badge__tr_scan(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (badge__slot_take(ud, &fn, &user)) {
        ((mesh_scan_fn)fn)(user, badge_arg(1), (int)sos_len(1));
    }
    return 0;
}

static inline int mesh_scan(const mesh_scan_opts *o, mesh_scan_fn cb, void *user)
{
    badge__fn("mesh.scan");
    sos_new_table();
    if (o && o->channels && o->nchannels) {
        sos_new_table();
        for (int i = 0; i < o->nchannels; i++) {
            sos_push_int(o->channels[i]);
            sos_seti(-2, i + 1);
        }
        badge__set("channels");
    }
    if (o) {
        badge__sets("band", o->band);
        if (o->dwell) {
            badge__seti("dwell", o->dwell);
        }
    }
    return badge__call_with_cb(1, badge__tr_scan, (badge__fnptr)cb, user);
}

static inline int mesh_scan_channel(badge_val channels, int i, mesh_channel_t *out)
{
    badge_val c = badge_push_index(channels, i);
    if (badge_type(c) != SOS_TABLE) {
        sos_pop(1);
        badge__seterr("no such channel");
        return BADGE_ERR;
    }
    out->channel = badge_field_int(c, "channel", 0);
    out->badges = badge_field_int(c, "badges", 0);
    out->beacons = badge_field_int(c, "beacons", 0);
    out->app = badge_field_int(c, "app", 0);
    badge_field_str(c, "band", out->band, sizeof out->band);
    badge_val p = badge_push_field(c, "peers");
    out->npeers = (int32_t)badge_len(p);
    sos_pop(2);
    return 0;
}

static inline int mesh_scan_peer(badge_val channels, int i, int j, mesh_peer *out)
{
    badge_val c = badge_push_index(channels, i);
    badge_val p = badge_push_field(c, "peers");
    badge_val e = badge_push_index(p, j);
    int ok = badge_type(e) == SOS_TABLE;
    if (ok) {
        badge__peer(e, out);
    } else {
        badge__seterr("no such peer");
    }
    sos_pop(3);
    return ok ? 0 : BADGE_ERR;
}

// --- meets (MESH.md "Meets"): a statement both badges sign ---------------------

// err NULL: both signed (peer, their data table); else "refused", "no answer", ...
typedef void (*mesh_meet_fn)(void *user, const char *peer, badge_val data, const char *err);

static inline int32_t badge__tr_meet(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (!badge__slot_take(ud, &fn, &user)) {
        return 0;
    }
    badge_val none = {0};
    char s[160];
    if (!sos_to_bool(1)) {
        if (!badge__argstr(2, s, sizeof s, NULL)) {
            memcpy(s, "failed", 7);
        }
        ((mesh_meet_fn)fn)(user, NULL, none, s);
        return 0;
    }
    badge_val r = badge_arg(2);
    badge_field_str(r, "peer", s, sizeof s);
    badge_val d = badge_push_field(r, "data");
    ((mesh_meet_fn)fn)(user, s, d, NULL);
    sos_pop(1);
    return 0;
}

// Asks badge `id` for a meet; data: a JSON object, <= 100 bytes, ASCII.
static inline int mesh_meet(const char *id, const char *data_json, mesh_meet_fn cb, void *user)
{
    badge__fn("mesh.meet");
    sos_push_cstr(id);
    if (json_push(data_json ? data_json : "{}")) {
        sos_pop(2);
        return BADGE_ERR;
    }
    return badge__call_with_cb(2, badge__tr_meet, (badge__fnptr)cb, user);
}

// In on_meet(): accept, signing your side's data (a JSON object). Returns
// what on_meet() returns: `return mesh_meet_accept("{\"caught\":1}");`.
// Bad JSON refuses (0).
static inline int32_t mesh_meet_accept(const char *data_json)
{
    return json_push(data_json ? data_json : "{}") ? 0 : 1;
}

// The meets the server holds for this badge's owner, oldest first, after the
// id `after` (0: from the start): result {items = {...}, next = id or nil}.
static inline int mesh_meets(int32_t after, badge_result_fn cb, void *user)
{
    badge__fn("mesh.meets");
    if (after > 0) {
        sos_push_int(after);
    }
    return badge__call_with_cb(after > 0 ? 1 : 0, badge__tr_result, (badge__fnptr)cb, user);
}

// ============================================================================
// mail (permission "mail", Solana OS 2.3.0)
// ============================================================================

typedef struct {
    int32_t id, at, exp, read;  // at, exp: unix seconds
    char from[8];       // a badge id, "server" or "solana"
    char by[25];        // the sender's owner label for badge mail, "" otherwise
    char title[33];
    char text[201];     // mail_get only ("" from mail_item)
    char data[129];     // mail_get only: the data as compact JSON, "" when none
} mail_t;

static inline void badge__mail(badge_val m, mail_t *out, int full)
{
    out->id = badge_field_int(m, "id", 0);
    out->at = badge_field_int(m, "at", 0);
    out->exp = badge_field_int(m, "exp", 0);
    out->read = badge_field_bool(m, "read");
    badge_field_str(m, "from", out->from, sizeof out->from);
    badge_field_str(m, "by", out->by, sizeof out->by);
    badge_field_str(m, "title", out->title, sizeof out->title);
    out->text[0] = out->data[0] = 0;
    if (full) {
        badge_field_str(m, "text", out->text, sizeof out->text);
        if (badge_field_type(m, "data") == SOS_TABLE && badge_field_json(m, "data", out->data, sizeof out->data) < 0) {
            out->data[0] = 0;
        }
    }
}

// Queues a notification for the owner of badge `to`, who must have this app
// too (the server checks): 0, or BADGE_ERR and badge_error() ("bad id",
// "title: 1-32 printable ASCII", "not linked", "full", "busy" ...). data_json:
// NULL or a JSON object of <= 128 bytes.
static inline int mail_send(const char *to, const char *title, const char *text, const char *data_json)
{
    badge__fn("mail.send");
    sos_push_cstr(to ? to : "");
    sos_push_cstr(title ? title : "");
    sos_push_cstr(text ? text : "");
    if (data_json && json_push(data_json)) {
        sos_pop(4);
        return BADGE_ERR;
    }
    return badge__call_ok(data_json ? 4 : 3);
}

// This app's mail on the badge, newest first: how many (<= 50), or BADGE_ERR.
static inline int mail_count(void)
{
    badge__fn("mail.list");
    if (badge__call(0, 1)) {
        return BADGE_ERR;
    }
    int n = (int)badge_len(badge_top());
    sos_pop(1);
    return n;
}

// The i-th (0-based) of mail_count()'s: 0, or BADGE_ERR past the end. No text or data.
static inline int mail_item(int i, mail_t *out)
{
    badge__fn("mail.list");
    if (badge__call(0, 1)) {
        return BADGE_ERR;
    }
    badge_val t = badge_top();
    int ok = i >= 0 && i < (int)badge_len(t);
    if (ok) {
        badge__mail(badge_push_index(t, i), out, 0);
        sos_pop(1);
    }
    sos_pop(1);
    if (!ok) {
        badge__seterr("no such mail");
    }
    return ok ? 0 : BADGE_ERR;
}

// One of this app's messages in full, by id: 0, or BADGE_ERR when it's gone.
static inline int mail_get(int32_t id, mail_t *out)
{
    badge__fn("mail.get");
    sos_push_int(id);
    if (badge__call(1, 1)) {
        return BADGE_ERR;
    }
    int ok = sos_type(-1) == SOS_TABLE;
    if (ok) {
        badge__mail(badge_top(), out, 1);
    } else {
        badge__seterr("no such mail");
    }
    sos_pop(1);
    return ok ? 0 : BADGE_ERR;
}

// In on_mail(): the mail that arrived (badge_arg(1)), as mail_get gives it.
static inline void mail_arg(mail_t *out)
{
    badge__mail(badge_arg(1), out, 1);
}

// ============================================================================
// ble (permission "ble", Solana OS 1.0.0)
// ============================================================================

enum { BLE_EV_OPEN, BLE_EV_NOTIFY, BLE_EV_CLOSE, BLE_EV_WRITE, BLE_EV_SUBSCRIBE };
enum { BLE_CONNECTING, BLE_OPEN, BLE_CLOSED };

typedef struct {
    char addr[18];        // "7c:df:a1:00:12:34"
    int32_t rssi;
    char name[30];        // "" when none
    char services[4][37]; // lower case, 4 hex digits or the 128-bit form
    int nservices;
    int32_t company;      // -1 if none (0xffff: a badge app)
    uint8_t data[32];     // manufacturer data after the company id
    int data_len;
    int connectable;
    int random;           // the address kind (ble_connect needs it)
} ble_dev;

typedef struct {
    int type;            // BLE_EV_*
    const char *chr;     // NOTIFY, WRITE, SUBSCRIBE: the characteristic
    const char *addr;    // OPEN of a served service: who connected
    const uint8_t *data; // NOTIFY, WRITE
    uint32_t len;
    int on;              // SUBSCRIBE
    const char *reason;  // CLOSE: "closed", "peer closed", "lost", "unreachable", "busy", "off"
} ble_event;

typedef void (*ble_scan_fn)(void *user, const ble_dev *dev);
typedef void (*ble_event_fn)(void *user, const ble_event *ev);
typedef void (*ble_read_fn)(void *user, const uint8_t *data, uint32_t len, const char *err);
typedef void (*ble_done_fn)(void *user, const char *err);

static inline int ble_on(void) // 0, or BADGE_ERR: "not enough memory", "unavailable"
{
    badge__fn("ble.on");
    return badge__call_ok(0);
}

static inline int ble_off(void)
{
    badge__fn("ble.off");
    return badge__call_ok(0);
}

static inline int ble_state(void) // 1 on, 0 off
{
    char s[8];
    badge__fn("ble.state");
    return badge__call_str(0, s, sizeof s) > 0 && !strcmp(s, "on");
}

static inline int32_t ble_address(char *buf, uint32_t cap)
{
    badge__fn("ble.address");
    return badge__call_str(0, buf, cap);
}

typedef struct {
    const char *name;    // <= 29 printable ASCII (NULL: "Badge <id>")
    const char *service; // one service UUID
    const void *data;    // <= 24 bytes (20 with a 16-bit service, 6 with a 128-bit one)
    uint32_t data_len;
    int connectable;
    int interval;        // ms, 20..10000 (0: 100)
} ble_adv;

// Starts or replaces the advertisement; NULL stops it.
static inline int ble_advertise(const ble_adv *a)
{
    badge__fn("ble.advertise");
    if (!a) {
        sos_push_bool(0);
        return badge__call_ok(1);
    }
    sos_new_table();
    badge__sets("name", a->name);
    badge__sets("service", a->service);
    if (a->data) {
        badge__push_bytes(a->data, a->data_len);
        badge__set("data");
    }
    if (a->connectable) {
        badge__setb("connectable", 1);
    }
    if (a->interval) {
        badge__seti("interval", a->interval);
    }
    return badge__call_ok(1);
}

typedef struct {
    const char *service; // only devices advertising it
    const char *name;    // only names starting with it (needs active)
    int active;
    int seconds;         // 0: until ble_scan_stop()
    int duty;            // 10..100 (0: 30)
} ble_scan_opts;

static inline int32_t badge__tr_blescan(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (!badge__slot_take(ud, &fn, &user)) {
        return 0;
    }
    ble_dev d;
    memset(&d, 0, sizeof d);
    badge_val t = badge_arg(1);
    badge_field_str(t, "addr", d.addr, sizeof d.addr);
    d.rssi = badge_field_int(t, "rssi", 0);
    badge_field_str(t, "name", d.name, sizeof d.name);
    d.company = badge_field_int(t, "company", -1);
    char data[33];
    int32_t n = badge_field_str(t, "data", data, sizeof data);
    d.data_len = n < 0 ? 0 : n > 32 ? 32 : n;
    memcpy(d.data, data, (size_t)d.data_len);
    d.connectable = badge_field_bool(t, "connectable");
    d.random = badge_field_bool(t, "random");
    badge_val s = badge_push_field(t, "services");
    for (int i = 0; i < 4 && i < (int)badge_len(s); i++) {
        badge_str(badge_push_index(s, i), d.services[i], sizeof d.services[i]);
        sos_pop(1);
        d.nservices++;
    }
    sos_pop(1);
    ((ble_scan_fn)fn)(user, &d);
    return 0;
}

// Calls cb(user, dev) for each device heard (at most once a second each).
// One scan at a time: a new one replaces it.
static inline int ble_scan(const ble_scan_opts *o, ble_scan_fn cb, void *user)
{
    if (badge__scan_tok) {
        badge__slot_free((uint32_t)badge__scan_tok);
        badge__scan_tok = 0;
    }
    badge__fn("ble.scan");
    sos_new_table();
    if (o) {
        badge__sets("service", o->service);
        badge__sets("name", o->name);
        if (o->active) {
            badge__setb("active", 1);
        }
        if (o->seconds) {
            badge__seti("seconds", o->seconds);
        }
        if (o->duty) {
            badge__seti("duty", o->duty);
        }
    }
    uint32_t tok = badge__push_cb(badge__tr_blescan, (badge__fnptr)cb, user, 0);
    if (!tok) {
        sos_pop(2);
        return BADGE_ERR;
    }
    if (badge__call_ok(2)) {
        badge__slot_free(tok);
        return BADGE_ERR;
    }
    badge__scan_tok = (int32_t)tok;
    return 0;
}

static inline int ble_scan_stop(void)
{
    badge__fn("ble.scan");
    sos_push_bool(0);
    int r = badge__call_ok(1);
    if (badge__scan_tok) {
        badge__slot_free((uint32_t)badge__scan_tok);
        badge__scan_tok = 0;
    }
    return r;
}

// One trampoline for a link's and a served service's events.
static inline int32_t badge__tr_bleev(void *ud, int32_t nargs)
{
    uint32_t tok = (uint32_t)(uintptr_t)ud;
    badge__slot *s = badge__slot_at(tok);
    if (!s) {
        return 0;
    }
    ble_event_fn fn = (ble_event_fn)s->fn;
    void *user = s->user;
    char what[12], a[40];
    ble_event ev;
    memset(&ev, 0, sizeof ev);
    badge__argstr(1, what, sizeof what, NULL);
    (void)nargs;
    if (!strcmp(what, "open")) {
        ev.type = BLE_EV_OPEN;
        ev.addr = badge__argstr(2, a, sizeof a, NULL);
    } else if (!strcmp(what, "notify") || !strcmp(what, "write")) {
        ev.type = what[0] == 'n' ? BLE_EV_NOTIFY : BLE_EV_WRITE;
        ev.chr = badge__argstr(2, a, sizeof a, NULL);
        ev.data = (const uint8_t *)badge__argstr(3, badge__buf, 245, &ev.len);
    } else if (!strcmp(what, "subscribe")) {
        ev.type = BLE_EV_SUBSCRIBE;
        ev.chr = badge__argstr(2, a, sizeof a, NULL);
        ev.on = sos_to_bool(3);
    } else if (!strcmp(what, "close")) {
        ev.type = BLE_EV_CLOSE;
        ev.reason = badge__argstr(2, a, sizeof a, NULL);
        if (s->flags & BADGE__CLOSE) { // a link's close is its last event
            s->used = 0;
        }
    } else {
        return 0;
    }
    if (fn) {
        fn(user, &ev);
    }
    return 0;
}

// Connects to a device from a scan; events open, notify, close (the last).
static inline ble_link ble_connect(const ble_dev *dev, ble_event_fn cb, void *user)
{
    ble_link l = {0};
    badge__fn("ble.connect");
    sos_new_table();
    badge__sets("addr", dev->addr);
    badge__setb("random", dev->random);
    uint32_t tok = badge__push_cb(badge__tr_bleev, (badge__fnptr)cb, user, BADGE__OWNED | BADGE__CLOSE);
    if (!tok) {
        sos_pop(2);
        return l;
    }
    l = badge__call_obj(2);
    badge__slot *s = badge__slot_at(tok);
    if (l.ref && s) {
        s->tag = l.ref;
    } else if (!l.ref) {
        badge__slot_free(tok);
    }
    return l;
}

static inline int32_t badge__tr_bleread(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (!badge__slot_take(ud, &fn, &user)) {
        return 0;
    }
    uint32_t len = 0;
    if (sos_to_bool(1)) {
        const char *d = badge__argstr(2, badge__buf, 245, &len);
        ((ble_read_fn)fn)(user, (const uint8_t *)d, len, NULL);
    } else {
        char err[64];
        if (!badge__argstr(2, err, sizeof err, NULL)) {
            memcpy(err, "failed", 7);
        }
        ((ble_read_fn)fn)(user, NULL, 0, err);
    }
    return 0;
}

static inline int32_t badge__tr_bledone(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (!badge__slot_take(ud, &fn, &user)) {
        return 0;
    }
    char err[64];
    if (sos_to_bool(1)) {
        ((ble_done_fn)fn)(user, NULL);
    } else {
        if (!badge__argstr(2, err, sizeof err, NULL)) {
            memcpy(err, "failed", 7);
        }
        ((ble_done_fn)fn)(user, err);
    }
    return 0;
}

static inline int ble_link_read(ble_link l, const char *service, const char *chr, ble_read_fn cb, void *user)
{
    if (badge__method(l, "read")) {
        return BADGE_ERR;
    }
    sos_push_cstr(service);
    sos_push_cstr(chr);
    return badge__call_with_cb(3, badge__tr_bleread, (badge__fnptr)cb, user);
}

// Without an answer (fast, may be lost).
static inline int ble_link_write(ble_link l, const char *service, const char *chr, const void *data, uint32_t len)
{
    if (badge__method(l, "write")) {
        return BADGE_ERR;
    }
    sos_push_cstr(service);
    sos_push_cstr(chr);
    badge__push_bytes(data, len);
    return badge__call_ok(4);
}

// Waiting for the device to confirm: cb(user, err), err NULL when it did.
static inline int ble_link_write_cb(ble_link l, const char *service, const char *chr, const void *data,
                                    uint32_t len, ble_done_fn cb, void *user)
{
    if (badge__method(l, "write")) {
        return BADGE_ERR;
    }
    sos_push_cstr(service);
    sos_push_cstr(chr);
    badge__push_bytes(data, len);
    return badge__call_with_cb(4, badge__tr_bledone, (badge__fnptr)cb, user);
}

static inline int ble_link_subscribe(ble_link l, const char *service, const char *chr, int on)
{
    if (badge__method(l, "subscribe")) {
        return BADGE_ERR;
    }
    sos_push_cstr(service);
    sos_push_cstr(chr);
    sos_push_bool(on);
    return badge__call_ok(4);
}

static inline int ble_link_close(ble_link l) // the close event follows
{
    return badge__method(l, "close") ? BADGE_ERR : badge__call_ok(1);
}

static inline int ble_link_state(ble_link l) // BLE_CONNECTING, BLE_OPEN, BLE_CLOSED
{
    char s[16];
    if (badge__method(l, "state") || badge__call_str(1, s, sizeof s) < 0) {
        return BADGE_ERR;
    }
    return !strcmp(s, "open") ? BLE_OPEN : !strcmp(s, "connecting") ? BLE_CONNECTING : BLE_CLOSED;
}

typedef struct {
    const char *uuid;
    int read, write, notify;
    const void *value; // the first value a read gets
    uint32_t value_len;
} ble_char;

typedef struct {
    const char *service;
    const ble_char *chars; // 1 to 4
    int nchars;
} ble_service;

// Serves one service; events open (addr), write, subscribe, close.
static inline int ble_serve(const ble_service *svc, ble_event_fn cb, void *user)
{
    badge__fn("ble.serve");
    sos_new_table();
    badge__sets("service", svc->service);
    sos_new_table();
    for (int i = 0; i < svc->nchars; i++) {
        const ble_char *c = &svc->chars[i];
        sos_new_table();
        badge__sets("uuid", c->uuid);
        if (c->read) {
            badge__setb("read", 1);
        }
        if (c->write) {
            badge__setb("write", 1);
        }
        if (c->notify) {
            badge__setb("notify", 1);
        }
        if (c->value) {
            badge__push_bytes(c->value, c->value_len);
            badge__set("value");
        }
        sos_seti(-2, i + 1);
    }
    badge__set("chars");
    uint32_t tok = badge__push_cb(badge__tr_bleev, (badge__fnptr)cb, user, 0);
    if (!tok) {
        sos_pop(2);
        return BADGE_ERR;
    }
    if (badge__call_ok(2)) {
        badge__slot_free(tok);
        return BADGE_ERR;
    }
    return 0;
}

static inline int ble_set(const char *chr, const void *data, uint32_t len) // the value a device reads
{
    badge__fn("ble.set");
    sos_push_cstr(chr);
    badge__push_bytes(data, len);
    return badge__call_ok(2);
}

static inline int ble_notify(const char *chr, const void *data, uint32_t len) // set and push
{
    badge__fn("ble.notify");
    sos_push_cstr(chr);
    badge__push_bytes(data, len);
    return badge__call_ok(2);
}

static inline int ble_drop(void) // disconnects the device that connected to us
{
    badge__fn("ble.drop");
    return badge__call_ok(0);
}

// ============================================================================
// lamps (permission "lamps")
// ============================================================================

typedef struct {
    int32_t balance;
    int32_t awarded;  // lamps_award
    int needs_x;      // lamps_award: the owner must link X to earn
    char id[24];      // lamps_deposit: the ledger entry
} lamps_result;

typedef void (*lamps_fn)(void *user, const lamps_result *r, const char *err);

static inline int32_t badge__tr_lamps(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (!badge__slot_take(ud, &fn, &user)) {
        return 0;
    }
    lamps_result r;
    memset(&r, 0, sizeof r);
    char err[160];
    if (!sos_to_bool(1)) {
        if (!badge__argstr(2, err, sizeof err, NULL)) {
            memcpy(err, "failed", 7);
        }
        ((lamps_fn)fn)(user, NULL, err);
        return 0;
    }
    badge_val v = badge_arg(2);
    if (badge_type(v) == SOS_TABLE) {
        r.balance = badge_field_int(v, "balance", 0);
        r.awarded = badge_field_int(v, "awarded", 0);
        r.needs_x = badge_field_bool(v, "needsX");
        if (badge_field_str(v, "id", r.id, sizeof r.id) < 0 && badge_field_type(v, "id") == SOS_INT) {
            int32_t id = badge_field_int(v, "id", 0), k = 0;
            char t[12];
            do {
                t[k++] = (char)('0' + id % 10);
                id /= 10;
            } while (id && k < 11);
            for (int i = 0; i < k; i++) {
                r.id[i] = t[k - 1 - i];
            }
        }
    } else {
        r.balance = badge_int(v);
    }
    ((lamps_fn)fn)(user, &r, NULL);
    return 0;
}

static inline int lamps_balance(lamps_fn cb, void *user)
{
    badge__fn("lamps.balance");
    return badge__call_with_cb(0, badge__tr_lamps, (badge__fnptr)cb, user);
}

// An in-app purchase paid to the app's author; the badge asks the owner first.
static inline int lamps_spend(int32_t amount, const char *sku, lamps_fn cb, void *user)
{
    badge__fn("lamps.spend");
    sos_push_int(amount);
    sos_push_cstr(sku);
    return badge__call_with_cb(2, badge__tr_lamps, (badge__fnptr)cb, user);
}

static inline int lamps_transfer(const char *badge_id, int32_t amount, const char *memo, lamps_fn cb, void *user)
{
    badge__fn("lamps.transfer");
    sos_push_cstr(badge_id);
    sos_push_int(amount);
    sos_push_cstr(memo);
    return badge__call_with_cb(3, badge__tr_lamps, (badge__fnptr)cb, user);
}

static inline int lamps_deposit(const char *address, int32_t amount, const char *memo, lamps_fn cb, void *user)
{
    badge__fn("lamps.deposit");
    sos_push_cstr(address);
    sos_push_int(amount);
    sos_push_cstr(memo);
    return badge__call_with_cb(3, badge__tr_lamps, (badge__fnptr)cb, user);
}

static inline int lamps_award(int32_t amount, const char *reason, lamps_fn cb, void *user)
{
    badge__fn("lamps.award");
    sos_push_int(amount);
    sos_push_cstr(reason);
    return badge__call_with_cb(2, badge__tr_lamps, (badge__fnptr)cb, user);
}

// ============================================================================
// items (permission "items")
// ============================================================================

typedef struct {
    char id[40];
    char kind[32];
    char name[48];
    int tradable;
} item_t;

// The documented fields of an item table (its data: badge_field_json(item, "data", ...)).
static inline void item_info(badge_val item, item_t *out)
{
    badge_field_str(item, "id", out->id, sizeof out->id);
    badge_field_str(item, "kind", out->kind, sizeof out->kind);
    badge_field_str(item, "name", out->name, sizeof out->name);
    out->tradable = badge_field_bool(item, "tradable");
}

// result: a list of items (badge_len, badge_push_index, item_info).
static inline int items_list(badge_result_fn cb, void *user)
{
    badge__fn("items.list");
    return badge__call_with_cb(0, badge__tr_result, (badge__fnptr)cb, user);
}

static inline int items_all(badge_result_fn cb, void *user)
{
    badge__fn("items.all");
    return badge__call_with_cb(0, badge__tr_result, (badge__fnptr)cb, user);
}

// data: JSON, <= 1 KB. result: the new item.
static inline int items_grant(const char *kind, const char *name, const char *data_json, badge_result_fn cb,
                              void *user)
{
    badge__fn("items.grant");
    sos_push_cstr(kind);
    sos_push_cstr(name);
    if (json_push(data_json ? data_json : "{}")) {
        sos_pop(3);
        return BADGE_ERR;
    }
    return badge__call_with_cb(3, badge__tr_result, (badge__fnptr)cb, user);
}

static inline int items_update(const char *id, const char *data_json, badge_result_fn cb, void *user)
{
    badge__fn("items.update");
    sos_push_cstr(id);
    if (json_push(data_json ? data_json : "{}")) {
        sos_pop(2);
        return BADGE_ERR;
    }
    return badge__call_with_cb(2, badge__tr_result, (badge__fnptr)cb, user);
}

static inline int items_remove(const char *id, badge_result_fn cb, void *user)
{
    badge__fn("items.remove");
    sos_push_cstr(id);
    return badge__call_with_cb(1, badge__tr_result, (badge__fnptr)cb, user);
}

// ============================================================================
// profile (permission "profile")
// ============================================================================

typedef struct {
    char name[48];   // the name people see, "" if none
    char x[32];      // the X handle without "@", "" without X
    char x_name[64];
    char solana[48]; // the linked Solana address (base58), "" if none or private
    int32_t level;
    int32_t lamps;   // profile_me only
    int32_t pages;   // profile_me only
    char badge[8];   // profile_me only
} profile_t;

typedef void (*profile_fn)(void *user, const profile_t *p, const char *err);

static inline int32_t badge__tr_profile(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (!badge__slot_take(ud, &fn, &user)) {
        return 0;
    }
    profile_t p;
    memset(&p, 0, sizeof p);
    char err[160];
    if (!sos_to_bool(1)) {
        if (!badge__argstr(2, err, sizeof err, NULL)) {
            memcpy(err, "failed", 7);
        }
        ((profile_fn)fn)(user, NULL, err);
        return 0;
    }
    badge_val t = badge_arg(2);
    badge_field_str(t, "name", p.name, sizeof p.name);
    badge_field_str(t, "x", p.x, sizeof p.x);
    badge_field_str(t, "xName", p.x_name, sizeof p.x_name);
    badge_field_str(t, "solana", p.solana, sizeof p.solana);
    badge_field_str(t, "badge", p.badge, sizeof p.badge);
    p.level = badge_field_int(t, "level", 0);
    p.lamps = badge_field_int(t, "lamps", 0);
    p.pages = badge_field_int(t, "pages", 0);
    ((profile_fn)fn)(user, &p, NULL);
    return 0;
}

static inline int profile_me(profile_fn cb, void *user)
{
    badge__fn("profile.me");
    return badge__call_with_cb(0, badge__tr_profile, (badge__fnptr)cb, user);
}

static inline int profile_get(const char *badge_id, profile_fn cb, void *user)
{
    badge__fn("profile.get");
    sos_push_cstr(badge_id);
    return badge__call_with_cb(1, badge__tr_profile, (badge__fnptr)cb, user);
}

// ============================================================================
// the player's inventory: profile_items, profile_pages (permission "inventory", Solana OS 2.3.0)
// ============================================================================

// One page of what the badge's owner holds. cursor: NULL or "" for the first page,
// else the previous result's "next" (badge_field_str(result, "next", ...); nil at the
// end). result: {items = {...}, next = "...", total = n} / {pages = {...}, next =
// "...", owned = n, total = n}; walk its list with badge_push_field, badge_len and
// badge_push_index, and read each entry with profile_item_info / profile_page_info.
typedef struct {
    char id[24];
    char kind[33];
    char name[41];
    char app[25];    // the store app that made it, "" if none
    char quest[49];  // the quest that gave it, "" if none
    int tradable;
} profile_item_t;

typedef struct {
    char id[49];
    char title[49];
    char category[16];
    char rarity[16];
    int32_t number;
    int32_t count;   // copies held
} profile_page_t;

static inline void profile_item_info(badge_val item, profile_item_t *out)
{
    memset(out, 0, sizeof *out);
    badge_field_str(item, "id", out->id, sizeof out->id);
    badge_field_str(item, "kind", out->kind, sizeof out->kind);
    badge_field_str(item, "name", out->name, sizeof out->name);
    badge_field_str(item, "app", out->app, sizeof out->app);
    badge_field_str(item, "quest", out->quest, sizeof out->quest);
    out->tradable = badge_field_bool(item, "tradable");
}

static inline void profile_page_info(badge_val page, profile_page_t *out)
{
    memset(out, 0, sizeof *out);
    badge_field_str(page, "id", out->id, sizeof out->id);
    badge_field_str(page, "title", out->title, sizeof out->title);
    badge_field_str(page, "category", out->category, sizeof out->category);
    badge_field_str(page, "rarity", out->rarity, sizeof out->rarity);
    out->number = badge_field_int(page, "number", 0);
    out->count = badge_field_int(page, "count", 0);
}

// Each wrapper names its function in a literal of its own: the store's code check credits
// a name to badge.h only where a wrapper spells it out (server/src/capi.ts).
static inline int profile_items(const char *cursor, badge_result_fn cb, void *user)
{
    badge__fn("profile.items");
    int n = cursor && cursor[0] ? 1 : 0;
    if (n) {
        sos_push_cstr(cursor);
    }
    return badge__call_with_cb(n, badge__tr_result, (badge__fnptr)cb, user);
}

static inline int profile_pages(const char *cursor, badge_result_fn cb, void *user)
{
    badge__fn("profile.pages");
    int n = cursor && cursor[0] ? 1 : 0;
    if (n) {
        sos_push_cstr(cursor);
    }
    return badge__call_with_cb(n, badge__tr_result, (badge__fnptr)cb, user);
}

// ============================================================================
// quest (permission "quest")
// ============================================================================

typedef struct {
    char id[40];
    char title[64];
    char step[64]; // the current step's title
} quest_t;

static inline void quest_info(badge_val q, quest_t *out)
{
    badge_field_str(q, "id", out->id, sizeof out->id);
    badge_field_str(q, "title", out->title, sizeof out->title);
    badge_field_str(q, "step", out->step, sizeof out->step);
}

static inline int quest_report(const char *event, const char *data_json, badge_result_fn cb, void *user)
{
    badge__fn("quest.report");
    sos_push_cstr(event);
    if (json_push(data_json ? data_json : "{}")) {
        sos_pop(2);
        return BADGE_ERR;
    }
    if (!cb) {
        return badge__call_ok(2);
    }
    return badge__call_with_cb(2, badge__tr_result, (badge__fnptr)cb, user);
}

// result: a list of quests (badge_len, badge_push_index, quest_info).
static inline int quest_active(badge_result_fn cb, void *user)
{
    badge__fn("quest.active");
    return badge__call_with_cb(0, badge__tr_result, (badge__fnptr)cb, user);
}

// ============================================================================
// http (permission "http", the app's hosts)
// ============================================================================

typedef struct {
    int32_t status;   // check it: a 404 is an answer too
    char type[64];    // Content-Type
    int truncated;    // the body was cut at about 3.5 KB
    badge_val body;   // the text: badge_str(res->body, buf, cap), or http_body()
    badge_val location; // a 3xx's Location (not followed), else nil
} http_response;

typedef void (*http_fn)(void *user, const http_response *res, const char *err);

typedef struct {
    const char *name, *value;
} http_header;

typedef struct {
    const char *method;  // NULL: GET, or POST with a body
    const char *url;     // https:// only
    const http_header *headers; // up to 8
    int nheaders;
    const char *body;    // NULL: none
    uint32_t body_len;   // 0: strlen(body)
    int json;            // body is JSON text: send it as a table (JSON, with its content type)
} http_req;

static inline int32_t http_body(const http_response *res, char *buf, uint32_t cap)
{
    return badge_str(res->body, buf, cap);
}

static inline int32_t badge__tr_http(void *ud, int32_t nargs)
{
    badge__fnptr fn;
    void *user;
    (void)nargs;
    if (!badge__slot_take(ud, &fn, &user)) {
        return 0;
    }
    char err[160];
    if (!sos_to_bool(1)) {
        if (!badge__argstr(2, err, sizeof err, NULL)) {
            memcpy(err, "failed", 7);
        }
        ((http_fn)fn)(user, NULL, err);
        return 0;
    }
    http_response r;
    memset(&r, 0, sizeof r);
    badge_val t = badge_arg(2);
    r.status = badge_field_int(t, "status", 0);
    badge_field_str(t, "type", r.type, sizeof r.type);
    r.truncated = badge_field_bool(t, "truncated");
    r.body = badge_push_field(t, "body");
    r.location = badge_push_field(t, "location");
    ((http_fn)fn)(user, &r, NULL);
    sos_pop(2);
    return 0;
}

static inline void badge__push_headers(const http_header *h, int n)
{
    sos_new_table();
    for (int i = 0; i < n; i++) {
        sos_push_cstr(h[i].value);
        sos_set(-2, h[i].name, badge__len(h[i].name));
    }
}

static inline int http_get(const char *url, http_fn cb, void *user)
{
    badge__fn("http.get");
    sos_push_cstr(url);
    return badge__call_with_cb(1, badge__tr_http, (badge__fnptr)cb, user);
}

static inline int http_get_headers(const char *url, const http_header *h, int n, http_fn cb, void *user)
{
    badge__fn("http.get");
    sos_push_cstr(url);
    badge__push_headers(h, n);
    return badge__call_with_cb(2, badge__tr_http, (badge__fnptr)cb, user);
}

// body: text, sent as it is (text/plain; charset=utf-8).
static inline int http_post(const char *url, const char *body, http_fn cb, void *user)
{
    badge__fn("http.post");
    sos_push_cstr(url);
    sos_push_cstr(body ? body : "");
    return badge__call_with_cb(2, badge__tr_http, (badge__fnptr)cb, user);
}

// json: JSON text, sent as JSON (application/json).
static inline int http_post_json(const char *url, const char *json, http_fn cb, void *user)
{
    badge__fn("http.post");
    sos_push_cstr(url);
    if (json_push(json)) {
        sos_pop(2);
        return BADGE_ERR;
    }
    return badge__call_with_cb(2, badge__tr_http, (badge__fnptr)cb, user);
}

static inline int http_request(const http_req *q, http_fn cb, void *user)
{
    badge__fn("http.request");
    sos_new_table();
    badge__sets("method", q->method);
    badge__sets("url", q->url);
    if (q->headers && q->nheaders) {
        badge__push_headers(q->headers, q->nheaders);
        badge__set("headers");
    }
    if (q->body) {
        if (q->json) {
            if (json_push(q->body)) {
                sos_pop(2);
                return BADGE_ERR;
            }
        } else {
            sos_push_str(q->body, q->body_len ? q->body_len : badge__len(q->body));
        }
        badge__set("body");
    }
    return badge__call_with_cb(1, badge__tr_http, (badge__fnptr)cb, user);
}

// ============================================================================
// ws (permission "http", the app's hosts; Solana OS 0.5.1)
// ============================================================================

enum { WS_EV_OPEN, WS_EV_MESSAGE, WS_EV_CLOSE };
enum { WS_CONNECTING, WS_OPEN, WS_CLOSED };

typedef struct {
    int type;           // WS_EV_*
    const char *data;   // MESSAGE: the bytes (<= 4 KB), NUL-terminated
    uint32_t len;
    const char *reason; // CLOSE: "closed", "host closed", "lost", "offline", ...
    int32_t code;       // CLOSE: the host's close code, or -1
} ws_event;

typedef void (*ws_fn)(void *user, const ws_event *ev);

static inline int32_t badge__tr_ws(void *ud, int32_t nargs)
{
    uint32_t tok = (uint32_t)(uintptr_t)ud;
    badge__slot *s = badge__slot_at(tok);
    (void)nargs;
    if (!s) {
        return 0;
    }
    ws_fn fn = (ws_fn)s->fn;
    void *user = s->user;
    char what[12], reason[48];
    ws_event ev;
    memset(&ev, 0, sizeof ev);
    ev.code = -1;
    badge__argstr(1, what, sizeof what, NULL);
    if (!strcmp(what, "open")) {
        ev.type = WS_EV_OPEN;
    } else if (!strcmp(what, "message")) {
        ev.type = WS_EV_MESSAGE;
        ev.data = badge__argstr(2, badge__buf, sizeof badge__buf, &ev.len);
    } else if (!strcmp(what, "close")) {
        ev.type = WS_EV_CLOSE;
        ev.reason = badge__argstr(2, reason, sizeof reason, NULL);
        if (sos_type(3) == SOS_INT) {
            ev.code = sos_to_int(3);
        }
        s->used = 0; // always the last event
    } else {
        return 0;
    }
    if (fn) {
        fn(user, &ev);
    }
    return 0;
}

// Opens a WebSocket (wss://, to one of the app's hosts); protocols may be NULL.
static inline ws_socket ws_open(const char *url, const char *const *protocols, int nprotocols, ws_fn cb, void *user)
{
    ws_socket w = {0};
    badge__fn("ws.open");
    sos_push_cstr(url);
    sos_new_table();
    if (protocols && nprotocols) {
        badge__push_strings(protocols, nprotocols);
        badge__set("protocols");
    }
    uint32_t tok = badge__push_cb(badge__tr_ws, (badge__fnptr)cb, user, BADGE__OWNED);
    if (!tok) {
        sos_pop(3);
        return w;
    }
    w = badge__call_obj(3);
    badge__slot *s = badge__slot_at(tok);
    if (w.ref && s) {
        s->tag = w.ref;
    } else if (!w.ref) {
        badge__slot_free(tok);
    }
    return w;
}

static inline int ws_send_bytes(ws_socket w, const void *data, uint32_t len)
{
    if (badge__method(w, "send")) {
        return BADGE_ERR;
    }
    badge__push_bytes(data, len);
    return badge__call_ok(2);
}

static inline int ws_send(ws_socket w, const char *text)
{
    return ws_send_bytes(w, text, badge__len(text));
}

static inline int ws_send_json(ws_socket w, const char *json) // sent as a table, re-encoded
{
    if (badge__method(w, "send")) {
        return BADGE_ERR;
    }
    if (json_push(json)) {
        sos_pop(2);
        return BADGE_ERR;
    }
    return badge__call_ok(2);
}

static inline int ws_close(ws_socket w) // the close event follows
{
    return badge__method(w, "close") ? BADGE_ERR : badge__call_ok(1);
}

static inline int ws_state(ws_socket w) // WS_CONNECTING, WS_OPEN, WS_CLOSED
{
    char s[16];
    if (badge__method(w, "state") || badge__call_str(1, s, sizeof s) < 0) {
        return BADGE_ERR;
    }
    return !strcmp(s, "open") ? WS_OPEN : !strcmp(s, "connecting") ? WS_CONNECTING : WS_CLOSED;
}

// ============================================================================
// rpc (permission "rpc", Solana OS 0.5.2)
// ============================================================================

// Solana JSON-RPC: params is the params array as JSON (NULL: none), cluster
// "mainnet" (NULL) or "devnet". result: the call's result (integers beyond
// 32 bits come as decimal strings).
static inline int rpc_call(const char *method, const char *params_json, const char *cluster, badge_result_fn cb,
                           void *user)
{
    badge__fn("rpc.call");
    sos_push_cstr(method);
    if (!params_json) {
        sos_push_nil();
    } else if (json_push(params_json)) {
        sos_pop(2);
        return BADGE_ERR;
    }
    int n = 2;
    if (cluster) {
        sos_new_table();
        badge__sets("cluster", cluster);
        n = 3;
    }
    return badge__call_with_cb(n, badge__tr_result, (badge__fnptr)cb, user);
}

// ============================================================================
// crypto and solana (always allowed, Solana OS 2.1.0)
// ============================================================================

// Reads the one result, a string of exactly n bytes, into out.
static inline int badge__call_bytes(int32_t nargs, uint8_t *out, uint32_t n)
{
    char tmp[129];
    if (n > 128) {
        badge__seterr("too long");
        return BADGE_ERR;
    }
    int32_t got = badge__call_str(nargs, tmp, n + 1);
    if (got != (int32_t)n) {
        if (got >= 0) {
            badge__seterr("unexpected length");
        }
        return BADGE_ERR;
    }
    memcpy(out, tmp, n);
    return 0;
}

static inline int crypto_sha256(const void *data, uint32_t len, uint8_t out[32])
{
    badge__fn("crypto.sha256");
    badge__push_bytes(data, len);
    return badge__call_bytes(1, out, 32);
}

static inline int crypto_on_curve(const uint8_t bytes[32]) // 1: a point of the Ed25519 curve
{
    badge__fn("crypto.on_curve");
    badge__push_bytes(bytes, 32);
    return badge__call_bool(1);
}

// 1 when sig (64 bytes) is key's (32 bytes) over msg (verify_strict).
static inline int crypto_verify(const uint8_t key[32], const void *msg, uint32_t len, const uint8_t sig[64])
{
    badge__fn("crypto.verify");
    badge__push_bytes(key, 32);
    badge__push_bytes(msg, len);
    badge__push_bytes(sig, 64);
    return badge__call_bool(3);
}

// Base58 of up to 128 bytes into out: its length (an address: 32..44).
static inline int32_t solana_encode(const void *bytes, uint32_t len, char *out, uint32_t cap)
{
    badge__fn("solana.encode");
    badge__push_bytes(bytes, len);
    return badge__call_str(1, out, cap);
}

// The bytes of base58 text: how many (check: an address is 32), or BADGE_ERR
// ("not base58", "too long").
static inline int32_t solana_decode(const char *text, uint8_t *out, uint32_t cap)
{
    char tmp[129];
    badge__fn("solana.decode");
    sos_push_cstr(text);
    int32_t n = badge__call_str(1, tmp, sizeof tmp);
    if (n < 0) {
        return BADGE_ERR;
    }
    if ((uint32_t)n > cap) {
        badge__seterr("buffer too small");
        return BADGE_ERR;
    }
    memcpy(out, tmp, (size_t)n);
    return n;
}

// A program-derived address (32 bytes) and its bump, as findProgramAddress.
static inline int solana_pda(const badge_bytes *seeds, int nseeds, const uint8_t program[32], uint8_t out[32],
                             int *bump)
{
    char tmp[33];
    badge__fn("solana.pda");
    sos_new_table();
    for (int i = 0; i < nseeds; i++) {
        badge__push_bytes(seeds[i].p, seeds[i].len);
        sos_seti(-2, i + 1);
    }
    badge__push_bytes(program, 32);
    if (badge__call(2, 2)) {
        return BADGE_ERR;
    }
    int ok = sos_type(-2) == SOS_STR && sos_to_str(-2, tmp, sizeof tmp) == 32;
    if (ok) {
        memcpy(out, tmp, 32);
        if (bump) {
            *bump = sos_to_int(-1);
        }
    } else {
        badge__seterr("no bump works");
    }
    sos_pop(2);
    return ok ? 0 : BADGE_ERR;
}

// ============================================================================
// service (permission "background", Solana OS 2.3.0, C_API.md "service")
// ============================================================================

// 1 in a service instance (started at on_service_start), 0 in the app.
static inline int sys_is_service(void)
{
    badge__fn("service.status");        // only a service has it
    int is = sos_type(-1) == SOS_FN;
    sos_pop(1);
    return is;
}

// service.send's answer: 0, or BADGE_ERR ("not running", the queue full, too big).
static inline int badge__call_sent(int32_t nargs)
{
    if (badge__call(nargs, 2)) {
        return BADGE_ERR;
    }
    int ok = sos_to_bool(-2);
    if (!ok) {
        if (sos_type(-1) == SOS_STR) {
            sos_to_str(-1, badge__err, sizeof badge__err);
        } else {
            badge__seterr("not running");
        }
    }
    sos_pop(2);
    return ok ? 0 : BADGE_ERR;
}

// In the app: start its service. The first time the badge asks the owner;
// cb(user, result, err): err NULL when it runs, else "denied", "full",
// "low battery", "busy", ... (LUA_API.md "service").
static inline int service_start(badge_result_fn cb, void *user)
{
    badge__fn("service.start");
    return badge__call_with_cb(0, badge__tr_result, (badge__fnptr)cb, user);
}

// Stops it (from either side); it won't start at boot until started again.
static inline int service_stop(void)
{
    badge__fn("service.stop");
    if (badge__call(0, 0)) {
        return BADGE_ERR;
    }
    return 0;
}

// In the app: 1 while its service runs.
static inline int service_running(void)
{
    badge__fn("service.running");
    return badge__call_bool(0);
}

// A message (JSON, <= 1 KB): from the app to the service's on_message(),
// from the service to the app's on_service() (only while the app is open).
static inline int service_send(const char *json)
{
    badge__fn("service.send");
    if (json_push(json)) {
        sos_pop(1);
        return BADGE_ERR;
    }
    return badge__call_sent(1);
}

// In the service: the line Services shows (<= 48 printable ASCII; "" = "Running").
static inline int service_status(const char *line)
{
    badge__fn("service.status");
    sos_push_cstr(line ? line : "");
    if (badge__call(1, 0)) {
        return BADGE_ERR;
    }
    return 0;
}

// In the service: a toast "<App>: text" (<= 80 characters), led 0xRRGGBB or
// 0; one every 10 s. 0, or BADGE_ERR when it was dropped.
static inline int service_notify(const char *text, uint32_t led)
{
    badge__fn("service.notify");
    sos_push_cstr(text ? text : "");
    sos_new_table();
    if (led) {
        sos_push_int((int32_t)(led & 0xFFFFFF));
        sos_set(-2, "led", 3);
    }
    if (!badge__call_bool(2)) {
        badge__seterr("dropped: one notice every 10 s");
        return BADGE_ERR;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
