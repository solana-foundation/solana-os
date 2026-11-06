// Images for Lua apps (docs/LUA_API.md "Images"): ui.load, ui.image, ui.canvas,
// ui.anim.
//
// Pixels are RGB565, two bytes each, with LH_PX_CLEAR for see-through, and
// live in Lua userdata ("ui.img"), so they count against the app's 48 KB like
// anything else it keeps; PNG decoding's scratch goes through the same
// allocator and is freed before ui.load returns. Nothing is ever drawn at the
// size it is shown: a view (an lv_image_dsc_t of our own, flagged
// LV_IMAGE_FLAGS_USER1) points LVGL's lv_image at the pixels and a scale, and
// our image decoder hands LVGL one scaled line at a time (get_area), so 8x
// pixel art costs what its few pixels cost and a line of ~1 KB while drawing.
//
// Lifetimes: the object's pool slot holds the view (registry ref in
// items_ref), the view holds its pixels (user value 1), so pixels outlive
// every lv_image showing them. LVGL reads them only while drawing, under the
// LVGL lock, which the script holds too whenever it runs; when the app
// closes, the OS task holds the lock until it has deleted the objects.
//
// Animations (ui.anim) keep one frame: the GIF's canvas, drawn on by each
// frame in turn (lua_gif.c), and the file offset of the next frame. The Lua
// task's loop plays them between callbacks (lh_anims_run, under the LVGL
// lock), reopening the file for each frame, so an animation holds no file
// open and a long one costs no more than a short one. Decoding needs a 13 KB
// table, reserved once per app (lh_t.gif_work) by its first ui.anim.

#include <string.h>
#include "lvgl_private.h"         // lv_image_decoder_dsc_t
#include "lua_gif.h"
#include "lua_png.h"
#include "lua_priv.h"

#define IMG_META    "ui.img"
#define SCALE_MAX   16
#define SHOWN_MAX   4096            // px per side once scaled (lv_image_header_t's 16-bit stride)
#define PNG_SKIP    33              // signature + IHDR, read by lh_png_info
#define PX_PER_STEP 16              // instructions charged per pixel written

typedef struct {
    uint16_t w, h;
    uint16_t px[];
} lh_pix_t;

typedef struct {
    lv_image_dsc_t dsc;             // first: what lv_image_set_src() gets
    lh_pix_t *pix;                  // user value 1 keeps it alive
    uint8_t scale;
} lh_view_t;

typedef struct {
    lh_view_t view;                 // first: shown like any view
    lh_gif_t gif;
    char rel[LUA_PKG_PATH_MAX + 1];
    uint32_t first, next;           // file offsets: the first frame's blocks, the next frame's
    int64_t due;                    // when the next frame shows
    int32_t loops, loops_set;       // plays left after this one (-1: forever), and as set
    bool playing, ended;
} lh_anim_t;

static const uint8_t s_tag;         // dsc.data of our views: how the decoder knows them
static lv_image_decoder_t *s_dec;

static inline uint16_t px_of(lv_color_t c)
{
    uint16_t v = lv_color_to_u16(c);
    return v == LH_PX_CLEAR ? 0 : v;
}

// ---------------------------------------------------------------------------
// LVGL decoder: one scaled line at a time

static lv_result_t dec_info(lv_image_decoder_t *d, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
    (void)d;
    const lv_image_dsc_t *i = dsc->src;
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE || i->data != &s_tag) {
        return LV_RESULT_INVALID;
    }
    *header = i->header;
    return LV_RESULT_OK;
}

static lv_result_t dec_open(lv_image_decoder_t *d, lv_image_decoder_dsc_t *dsc)
{
    (void)d;
    dsc->decoded = NULL;            // drawn through get_area
    dsc->user_data = NULL;          // the line buffer, made on the first line
    return LV_RESULT_OK;
}

static lv_result_t dec_area(lv_image_decoder_t *d, lv_image_decoder_dsc_t *dsc, const lv_area_t *full,
                            lv_area_t *done)
{
    (void)d;
    const lh_view_t *v = dsc->src;
    const lh_pix_t *p = v->pix;
    int32_t w = lv_area_get_width(full);
    lv_draw_buf_t *line = dsc->user_data;
    if (done->y1 == LV_COORD_MIN) {
        lv_draw_buf_t *r = line ? lv_draw_buf_reshape(line, LV_COLOR_FORMAT_ARGB8888, w, 1, LV_STRIDE_AUTO) : NULL;
        if (!r) {
            if (line) {
                lv_draw_buf_destroy(line);
            }
            line = r = lv_draw_buf_create(w, 1, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
            dsc->user_data = line;
            if (!line) {
                return LV_RESULT_INVALID;
            }
        }
        *done = *full;
        done->y2 = done->y1;
    } else {
        done->y1++;
        done->y2++;
    }
    if (done->y1 > full->y2 || full->x1 < 0 || full->x2 >= p->w * v->scale || done->y1 >= p->h * v->scale) {
        return LV_RESULT_INVALID;
    }
    const uint16_t *row = p->px + (size_t)(done->y1 / v->scale) * p->w;
    lv_color32_t *out = (lv_color32_t *)line->data;
    for (int32_t x = full->x1; x <= full->x2; x++, out++) {
        uint16_t c = row[x / v->scale];
        if (c == LH_PX_CLEAR) {
            *out = (lv_color32_t){0};
            continue;
        }
        uint8_t r = c >> 11, g = (c >> 5) & 63, b = c & 31;
        *out = (lv_color32_t){ .red = (uint8_t)(r << 3 | r >> 2), .green = (uint8_t)(g << 2 | g >> 4),
                               .blue = (uint8_t)(b << 3 | b >> 2), .alpha = 255 };
    }
    dsc->decoded = line;
    return LV_RESULT_OK;
}

static void dec_close(lv_image_decoder_t *d, lv_image_decoder_dsc_t *dsc)
{
    (void)d;
    if (dsc->user_data) {
        lv_draw_buf_destroy(dsc->user_data);
        dsc->user_data = NULL;
    }
}

// Created once at boot (lua_host_init), not by the first app that shows an
// image: made mid-app, its few bytes would stay in the middle of the heap the
// app had used and split the largest free block. Newer decoders are asked first.
void lh_img_init(void)
{
    if (s_dec) {
        return;
    }
    s_dec = lv_image_decoder_create();
    lv_image_decoder_set_info_cb(s_dec, dec_info);
    lv_image_decoder_set_open_cb(s_dec, dec_open);
    lv_image_decoder_set_get_area_cb(s_dec, dec_area);
    lv_image_decoder_set_close_cb(s_dec, dec_close);
}

// ---------------------------------------------------------------------------
// Pixels

static lh_pix_t *new_pix(lua_State *L, const char *what, int w, int h)
{
    size_t bytes = (size_t)w * h * 2;
    if (bytes > LUA_HOST_HEAP_MAX) {
        luaL_error(L, "%s: %d x %d pixels take %d KB, more than an app's %d KB", what, w, h,
                   (int)(bytes / 1024), LUA_HOST_HEAP_MAX / 1024);
    }
    lh_pix_t *p = lua_newuserdatauv(L, sizeof(lh_pix_t) + bytes, 0);
    p->w = (uint16_t)w;
    p->h = (uint16_t)h;
    luaL_setmetatable(L, IMG_META);
    return p;
}

// Scratch on the script's own allowance, freed before anything can raise.
static void *scratch(lua_State *L, size_t n)
{
    void *ud;
    lua_Alloc fa = lua_getallocf(L, &ud);
    void *p = fa(ud, NULL, LUA_TUSERDATA, n);
    if (!p) {
        lua_gc(L, LUA_GCCOLLECT);
        p = fa(ud, NULL, LUA_TUSERDATA, n);
    }
    return p;
}

static void scratch_free(lua_State *L, void *p, size_t n)
{
    void *ud;
    lua_Alloc fa = lua_getallocf(L, &ud);
    if (p) {
        fa(ud, p, n, 0);
    }
}

static size_t file_read(void *ud, uint8_t *buf, size_t n)
{
    return fread(buf, 1, n, (FILE *)ud);
}

// A GIF's header (with w for scratch); NULL, or why not. The file is closed again.
static const char *gif_head(lh_t *s, const char *rel, lh_gif_t *g, lh_gif_work_t *w, uint32_t *first)
{
    long size;
    FILE *f = lh_fs_fopen(s, rel, &size);
    if (!f) {
        return "couldn't read it from the microSD card";
    }
    const char *err = lh_gif_open(g, w, file_read, f, first);
    fclose(f);
    return err;
}

// Draws the frame at *pos onto p (the file is opened for it); NULL, or why not.
static const char *gif_step(lh_t *s, const char *rel, lh_gif_t *g, lh_gif_work_t *w, lh_pix_t *p, uint32_t *pos,
                            bool *done)
{
    long size;
    FILE *f = lh_fs_fopen(s, rel, &size);
    const char *err = !f || fseek(f, (long)*pos, SEEK_SET) ? "couldn't read it from the microSD card" : NULL;
    if (!err) {
        err = lh_gif_frame(g, w, file_read, f, p->px, pos, done);
    }
    if (f) {
        fclose(f);
    }
    return err;
}

static lh_pix_t *new_clear_pix(lua_State *L, const char *what, int w, int h)
{
    lh_pix_t *p = new_pix(L, what, w, h);
    for (int i = 0; i < w * h; i++) {
        p->px[i] = LH_PX_CLEAR;
    }
    return p;
}

// A GIF's first frame, pushed as pixels. The decoding table is the one
// ui.anim reserved, if the app has it, else scratch, never held across a call
// that can raise: the header is read once to size the pixels, then again.
static lh_pix_t *load_gif(lua_State *L, lh_t *s, const char *rel, long size, const char *what)
{
    lh_gif_t g;
    uint32_t first;
    lh_gif_work_t *shared = s->gif_work;
    lh_gif_work_t *w = shared ? shared : scratch(L, sizeof(*w));
    const char *err = w ? gif_head(s, rel, &g, w, &first) : "not enough memory to load it";
    if (!shared) {
        scratch_free(L, w, sizeof(*w));
    }
    if (err) {
        luaL_error(L, "%s: %s: %s", what, rel, err);
    }
    size_t need = (shared ? 0 : sizeof(*w)) + (size_t)g.w * g.h * 2;
    if (need > LUA_HOST_HEAP_MAX) {
        luaL_error(L, "%s: %s (%d x %d) takes %d KB to load, more than an app's %d KB", what, rel, g.w, g.h,
                   (int)(need / 1024), LUA_HOST_HEAP_MAX / 1024);
    }
    lh_charge_steps(L, (unsigned)(size / LH_FS_BYTES_PER_STEP + (size_t)g.w * g.h / PX_PER_STEP));
    lh_pix_t *p = new_clear_pix(L, what, g.w, g.h);
    w = shared ? shared : scratch(L, sizeof(*w));
    bool done = false;
    err = w ? gif_head(s, rel, &g, w, &first) : "not enough memory to load it";
    if (!err) {
        err = gif_step(s, rel, &g, w, p, &first, &done);
    }
    if (!shared) {
        scratch_free(L, w, sizeof(*w));
    }
    if (err) {
        luaL_error(L, "%s: %s: %s", what, rel, err);
    }
    return p;
}

// The PNG or GIF (its first frame) at the bundled path in idx, pushed as pixels.
static lh_pix_t *load_file(lua_State *L, int idx, const char *what)
{
    lh_t *s = lh_get(L);
    char rel[LUA_PKG_PATH_MAX + 1];
    lh_fs_check_path(L, idx, rel);
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    long size;
    FILE *f = lh_fs_fopen(s, rel, &size);
    if (!f) {
        luaL_error(L, "%s: %s not found", what, rel);
    }
    uint8_t sig[4] = {0};
    bool gif = fread(sig, 1, 4, f) == 4 && !memcmp(sig, "GIF8", 4);
    lh_png_info_t info;
    const char *err = gif ? NULL : fseek(f, 0, SEEK_SET) ? "couldn't read it from the microSD card"
                      : lh_png_info(file_read, f, &info);
    fclose(f);
    if (gif) {
        return load_gif(L, s, rel, size, what);
    }
    if (err) {
        luaL_error(L, "%s: %s: %s", what, rel, !strcmp(err, "not a PNG") ? "not a PNG or GIF" : err);
    }
    size_t raw_n = lh_png_raw_size(&info), need = raw_n + sizeof(lh_png_work_t) + (size_t)info.w * info.h * 2;
    if (need > LUA_HOST_HEAP_MAX) {
        luaL_error(L, "%s: %s (%d x %d) takes %d KB to load, more than an app's %d KB (fewer colours help: "
                   "save it with a palette)", what, rel, info.w, info.h, (int)(need / 1024), LUA_HOST_HEAP_MAX / 1024);
    }
    lh_charge_steps(L, (unsigned)(size / LH_FS_BYTES_PER_STEP + raw_n / 8));
    lh_pix_t *p = new_pix(L, what, info.w, info.h);
    uint8_t *raw = scratch(L, raw_n);
    lh_png_work_t *w = scratch(L, sizeof(*w));
    err = !raw || !w ? "not enough memory to load it" : NULL;
    f = err ? NULL : lh_fs_fopen(s, rel, &size);
    if (!err && (!f || fseek(f, PNG_SKIP, SEEK_SET))) {
        err = "couldn't read it from the microSD card";
    }
    if (!err) {
        err = lh_png_decode(file_read, f, &info, w, raw, p->px);
    }
    if (f) {
        fclose(f);
    }
    scratch_free(L, w, sizeof(*w));
    scratch_free(L, raw, raw_n);
    if (err) {
        luaL_error(L, "%s: %s: %s", what, rel, err);
    }
    return p;
}

// Pixel art's default colours: fixed, but for '#', which is the theme's text.
static const struct {
    char c;
    uint32_t rgb;
} ART[] = {
    {'k', 0x000000}, {'w', 0xFFFFFF}, {'p', COL_PURPLE}, {'g', COL_GREEN}, {'m', COL_PINK}, {'v', COL_LPURPLE},
    {'b', COL_BLUE}, {'a', COL_AQUA}, {'l', COL_LIME}, {'y', COL_YELLOW}, {'o', COL_ORANGE},
};

// Rows of characters (table at idx), one per pixel, through the default
// palette and opts.palette; pushed as pixels.
static lh_pix_t *load_art(lua_State *L, int idx, int opts, const char *what)
{
    int32_t map[128];
    for (int i = 0; i < 128; i++) {
        map[i] = -1;
    }
    map['.'] = map[' '] = LH_PX_CLEAR;
    map['#'] = px_of(ui_col(COL_TEXT));
    for (size_t i = 0; i < sizeof(ART) / sizeof(ART[0]); i++) {
        map[(int)ART[i].c] = px_of(lv_color_hex(ART[i].rgb));
    }
    if (lua_type(L, opts) == LUA_TTABLE) {
        lua_getfield(L, opts, "palette");
        int t = lua_gettop(L);
        if (!lua_isnil(L, t)) {
            luaL_argcheck(L, lua_istable(L, t), opts, "palette must be a table of colours by character");
            lua_pushnil(L);
            while (lua_next(L, t)) {
                size_t n;
                const char *k = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &n) : NULL;
                if (!k || n != 1 || k[0] < 33 || k[0] > 126) {
                    luaL_error(L, "%s: palette keys are single characters", what);
                }
                map[(int)k[0]] = lua_toboolean(L, -1) ? px_of(lh_color(L, -1)) : LH_PX_CLEAR;
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    int h = (int)lua_rawlen(L, idx), w = 0;
    if (h < 1 || h > LH_PNG_DIM_MAX) {
        luaL_error(L, "%s: pixel art takes 1 to %d rows", what, LH_PNG_DIM_MAX);
    }
    for (int y = 1; y <= h; y++) {
        if (lua_rawgeti(L, idx, y) != LUA_TSTRING) {
            luaL_error(L, "%s: row %d isn't a string", what, y);
        }
        int n = (int)lua_rawlen(L, -1);
        w = n > w ? n : w;
        lua_pop(L, 1);
    }
    if (w < 1 || w > LH_PNG_DIM_MAX) {
        luaL_error(L, "%s: pixel art rows are 1 to %d characters", what, LH_PNG_DIM_MAX);
    }
    lh_charge_steps(L, (unsigned)(w * h / PX_PER_STEP));
    lh_pix_t *p = new_pix(L, what, w, h);
    for (int y = 0; y < h; y++) {
        lua_rawgeti(L, idx, y + 1);
        size_t n;
        const char *row = lua_tolstring(L, -1, &n);
        for (int x = 0; x < w; x++) {
            unsigned char c = x < (int)n ? (unsigned char)row[x] : '.';
            int32_t v = c < 128 ? map[c] : -1;
            if (v < 0) {
                luaL_error(L, c > 32 && c < 127 ? "%s: row %d: '%c' isn't in the palette"
                           : "%s: row %d: character %d isn't in the palette", what, y + 1, c);
            }
            p->px[y * w + x] = (uint16_t)v;
        }
        lua_pop(L, 1);
    }
    return p;
}

// The image at idx (a ui.load result, a PNG path or pixel art rows), pushed.
static lh_pix_t *to_pix(lua_State *L, int idx, int opts, const char *what)
{
    idx = lua_absindex(L, idx);
    opts = lua_absindex(L, opts);
    lh_pix_t *p = luaL_testudata(L, idx, IMG_META);
    if (p) {
        lua_pushvalue(L, idx);
        return p;
    }
    switch (lua_type(L, idx)) {
    case LUA_TSTRING: return load_file(L, idx, what);
    case LUA_TTABLE: return load_art(L, idx, opts, what);
    }
    luaL_typeerror(L, idx, "image, PNG or GIF path, or table of rows");
    return NULL;
}

static int opt_scale(lua_State *L, int t)
{
    int scale = 1;
    if (lua_type(L, t) == LUA_TTABLE) {
        lua_getfield(L, t, "scale");
        if (!lua_isnil(L, -1)) {
            scale = (int)luaL_checkinteger(L, -1);
        }
        lua_pop(L, 1);
    }
    return scale;
}

static void check_shown(lua_State *L, const char *what, const lh_pix_t *p, int scale)
{
    if (scale < 1 || scale > SCALE_MAX) {
        luaL_error(L, "%s: scale must be 1 to %d", what, SCALE_MAX);
    }
    if (p->w * scale > SHOWN_MAX || p->h * scale > SHOWN_MAX) {
        luaL_error(L, "%s: %d px wide or high at most once scaled", what, SHOWN_MAX);
    }
}

static void view_set(lh_view_t *v, lh_pix_t *p, int scale)
{
    v->pix = p;
    v->scale = (uint8_t)scale;
    v->dsc.header.w = (uint32_t)(p->w * scale);
    v->dsc.header.h = (uint32_t)(p->h * scale);
    v->dsc.header.stride = v->dsc.header.w * 4;
}

// A view of the pixels on top of the stack, which it replaces.
static lh_view_t *new_view(lua_State *L, int scale)
{
    lh_pix_t *p = lua_touserdata(L, -1);
    lh_view_t *v = lua_newuserdatauv(L, sizeof(*v), 1);
    memset(v, 0, sizeof(*v));
    v->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    v->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    v->dsc.header.flags = LV_IMAGE_FLAGS_USER1;
    v->dsc.data = &s_tag;
    view_set(v, p, scale);
    lua_insert(L, -2);
    lua_setiuservalue(L, -2, 1);
    return v;
}

// Shows the view on top of the stack (popped) as a new object of kind; pushes its handle.
static void show_view(lua_State *L, lh_t *s, lh_kind_t kind, int x, int y)
{
    lh_view_t *v = lua_touserdata(L, -1);
    lv_obj_t *img = lv_image_create(s->area);
    lv_image_set_src(img, &v->dsc);
    lv_obj_set_pos(img, x, y);
    lh_obj_t *o = lh_ui_new(L, s, kind, img);       // pushes the handle
    lua_insert(L, -2);
    o->items_ref = luaL_ref(L, LUA_REGISTRYINDEX);  // the view, until the object goes
}

static lh_view_t *view_of(const lh_obj_t *o)
{
    return (lh_view_t *)lv_image_get_src(o->obj);
}

// ---------------------------------------------------------------------------
// ui.load, ui.image and their methods

int lh_ui_load(lua_State *L)
{
    to_pix(L, 1, 2, "ui.load");
    return 1;
}

static int img_size(lua_State *L)
{
    lh_pix_t *p = luaL_checkudata(L, 1, IMG_META);
    lua_pushinteger(L, p->w);
    lua_pushinteger(L, p->h);
    return 2;
}

int lh_ui_image(lua_State *L)
{
    int x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
    lh_pix_t *p = to_pix(L, 3, 4, "ui.image");
    int scale = opt_scale(L, 4);
    check_shown(L, "ui.image", p, scale);
    lh_t *s = lh_ui_session(L);
    if (!s) {
        return 0;
    }
    new_view(L, scale);
    show_view(L, s, LH_IMAGE, x, y);
    return 1;
}

int lh_img_set(lua_State *L)
{
    lh_obj_t *o = lh_ui_check(L, LH_IMAGE, "image");
    lh_pix_t *p = to_pix(L, 2, 3, "image:set");
    if (o) {
        lh_view_t *v = view_of(o);
        check_shown(L, "image:set", p, v->scale);
        lua_rawgeti(L, LUA_REGISTRYINDEX, o->items_ref);
        lua_insert(L, -2);
        lua_setiuservalue(L, -2, 1);
        view_set(v, p, v->scale);
        lv_image_set_src(o->obj, &v->dsc);
    }
    return 0;
}

static void rescale(lua_State *L, lh_obj_t *o, const char *what)
{
    int scale = (int)luaL_checkinteger(L, 2);
    if (o) {
        lh_view_t *v = view_of(o);
        check_shown(L, what, v->pix, scale);
        view_set(v, v->pix, scale);
        lv_image_set_src(o->obj, &v->dsc);
    }
}

int lh_img_scale(lua_State *L)
{
    rescale(L, lh_ui_check(L, LH_IMAGE, "image"), "image:scale");
    return 0;
}

// ---------------------------------------------------------------------------
// ui.anim: GIFs played

static lh_gif_work_t *gif_work(lua_State *L, lh_t *s)
{
    if (!s->gif_work) {
        s->gif_work = lua_newuserdatauv(L, sizeof(lh_gif_work_t), 0);
        luaL_ref(L, LUA_REGISTRYINDEX);         // for the app's life
    }
    return s->gif_work;
}

// opts.loop: true forever, false once, n times in all; nil: what the file says.
static int32_t opt_loops(lua_State *L, int t, bool *set)
{
    *set = false;
    int32_t loops = 0;
    if (lua_type(L, t) == LUA_TTABLE) {
        lua_getfield(L, t, "loop");
        if (lua_isboolean(L, -1)) {
            loops = lua_toboolean(L, -1) ? -1 : 0;
            *set = true;
        } else if (!lua_isnil(L, -1)) {
            lua_Integer n = luaL_checkinteger(L, -1);
            luaL_argcheck(L, n >= 1, t, "loop: true, false or a number of times (1 or more)");
            loops = (int32_t)(n > 30000 ? 30000 : n) - 1;
            *set = true;
        }
        lua_pop(L, 1);
    }
    return loops;
}

int lh_ui_anim(lua_State *L)
{
    int x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
    lh_t *s = lh_get(L);
    char rel[LUA_PKG_PATH_MAX + 1];
    lh_fs_check_path(L, 3, rel);
    int scale = opt_scale(L, 4);
    bool loop_set;
    int32_t loops = opt_loops(L, 4, &loop_set);
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    long size;
    FILE *f = lh_fs_fopen(s, rel, &size);
    if (!f) {
        luaL_error(L, "ui.anim: %s not found", rel);
    }
    fclose(f);
    lh_gif_work_t *w = gif_work(L, s);
    lh_gif_t g;
    uint32_t first;
    const char *err = gif_head(s, rel, &g, w, &first);
    if (err) {
        luaL_error(L, "ui.anim: %s: %s", rel, err);
    }
    lh_charge_steps(L, (unsigned)((size_t)g.w * g.h / PX_PER_STEP));
    lh_pix_t *p = new_clear_pix(L, "ui.anim", g.w, g.h);
    check_shown(L, "ui.anim", p, scale);
    lh_anim_t *a = lua_newuserdatauv(L, sizeof(*a), 1);
    memset(a, 0, sizeof(*a));
    lua_insert(L, -2);
    lua_setiuservalue(L, -2, 1);                // the anim keeps its canvas
    a->view.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    a->view.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    a->view.dsc.header.flags = LV_IMAGE_FLAGS_USER1;
    a->view.dsc.data = &s_tag;
    view_set(&a->view, p, scale);
    a->gif = g;
    strlcpy(a->rel, rel, sizeof(a->rel));
    a->first = a->next = first;
    bool done = false;
    err = gif_step(s, rel, &a->gif, w, p, &a->next, &done);
    if (err || done) {
        luaL_error(L, "ui.anim: %s: %s", rel, err ? err : "no frames");
    }
    // The file's loop count (NETSCAPE) is known once the first frame is read.
    a->loops = loop_set ? loops : a->gif.loops == 0 ? -1 : a->gif.loops > 0 ? a->gif.loops : 0;
    a->loops_set = a->loops;
    a->playing = true;
    a->due = lh_now() + a->gif.delay_ms;
    lh_t *ss = lh_ui_session(L);
    if (!ss) {
        return 0;
    }
    show_view(L, ss, LH_ANIM, x, y);
    return 1;
}

static lh_anim_t *anim_of(lh_obj_t *o)
{
    return (lh_anim_t *)view_of(o);
}

int lh_anim_play(lua_State *L)
{
    lh_obj_t *o = lh_ui_check(L, LH_ANIM, "anim");
    if (o) {
        lh_anim_t *a = anim_of(o);
        if (a->ended) {                         // from the start again
            a->ended = false;
            a->next = a->first;
            a->loops = a->loops_set;
            a->gif.disposal = 2;                // clears the whole canvas first
            a->gif.fx = a->gif.fy = 0;
            a->gif.fw = a->gif.w;
            a->gif.fh = a->gif.h;
        }
        if (!a->playing) {
            a->playing = true;
            a->due = lh_now();
        }
    }
    return 0;
}

int lh_anim_stop(lua_State *L)
{
    lh_obj_t *o = lh_ui_check(L, LH_ANIM, "anim");
    if (o) {
        anim_of(o)->playing = false;
    }
    return 0;
}

int lh_anim_playing(lua_State *L)
{
    lh_obj_t *o = lh_ui_check(L, LH_ANIM, "anim");
    lua_pushboolean(L, o && anim_of(o)->playing);
    return 1;
}

int lh_anim_scale(lua_State *L)
{
    rescale(L, lh_ui_check(L, LH_ANIM, "anim"), "anim:scale");
    return 0;
}

// The next frame of one animation. Not a script callback: nothing here raises.
static void anim_step(lh_t *s, lh_obj_t *o, int64_t now)
{
    lh_anim_t *a = anim_of(o);
    bool done = false;
    const char *err = NULL;
    for (int tries = 0; tries < 2; tries++) {
        err = gif_step(s, a->rel, &a->gif, s->gif_work, a->view.pix, &a->next, &done);
        if (err || !done) {
            break;
        }
        if (!a->loops) {                        // the end: the last frame stays
            a->playing = false;
            a->ended = true;
            break;
        }
        if (a->loops > 0) {
            a->loops--;
        }
        a->next = a->first;
    }
    if (err) {
        a->playing = false;
        printf("[%s] ui.anim: %s: %s\n", s->pkg.id, a->rel, err);
        return;
    }
    if (done && a->playing) {
        a->playing = false;                     // nothing but the end, twice
    }
    // On time if it can be, never a burst to catch up.
    a->due = a->due + a->gif.delay_ms > now ? a->due + a->gif.delay_ms : now + a->gif.delay_ms;
    lv_obj_invalidate(o->obj);
}

void lh_anims_run(lh_t *s)
{
    if (!s->gif_work || !s->area) {
        return;
    }
    int64_t now = lh_now();
    for (int i = 0; i < LH_OBJ_MAX; i++) {
        lh_obj_t *o = &s->objs[i];
        if (o->obj && o->kind == LH_ANIM && anim_of(o)->playing && anim_of(o)->due <= now) {
            anim_step(s, o, now);
        }
    }
}

int64_t lh_anims_next(const lh_t *s)
{
    int64_t next = INT64_MAX;
    if (!s->gif_work || !s->area) {
        return next;
    }
    for (int i = 0; i < LH_OBJ_MAX; i++) {
        const lh_obj_t *o = &s->objs[i];
        if (o->obj && o->kind == LH_ANIM && anim_of((lh_obj_t *)o)->playing && anim_of((lh_obj_t *)o)->due < next) {
            next = anim_of((lh_obj_t *)o)->due;
        }
    }
    return next;
}

// ---------------------------------------------------------------------------
// ui.canvas and drawing

int lh_ui_canvas(lua_State *L)
{
    int x = luaL_checkinteger(L, 1), y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3), h = luaL_checkinteger(L, 4);
    if (w < 1 || h < 1 || w > LH_PNG_DIM_MAX || h > LH_PNG_DIM_MAX) {
        luaL_error(L, "ui.canvas: 1 to %d pixels per side", LH_PNG_DIM_MAX);
    }
    int scale = opt_scale(L, 5);
    uint16_t fill = LH_PX_CLEAR;
    if (lua_type(L, 5) == LUA_TTABLE) {
        lua_getfield(L, 5, "color");
        if (!lua_isnil(L, -1)) {
            fill = px_of(lh_color(L, -1));
        }
        lua_pop(L, 1);
    }
    lh_charge_steps(L, (unsigned)(w * h / PX_PER_STEP));
    lh_pix_t *p = new_pix(L, "ui.canvas", w, h);
    check_shown(L, "ui.canvas", p, scale);
    for (int i = 0; i < w * h; i++) {
        p->px[i] = fill;
    }
    lh_t *s = lh_ui_session(L);
    if (!s) {
        return 0;
    }
    new_view(L, scale);
    show_view(L, s, LH_CANVAS, x, y);
    return 1;
}

static lh_pix_t *canvas(lua_State *L, lh_obj_t **out)
{
    lh_obj_t *o = lh_ui_check(L, LH_CANVAS, "canvas");
    *out = o;
    return o ? view_of(o)->pix : NULL;
}

// A colour argument: nil or none is see-through.
static uint16_t arg_px(lua_State *L, int idx)
{
    return lua_isnoneornil(L, idx) || (lua_isboolean(L, idx) && !lua_toboolean(L, idx)) ? LH_PX_CLEAR
           : px_of(lh_color(L, idx));
}

static void fill(lua_State *L, lh_pix_t *p, int x, int y, int w, int h, uint16_t c)
{
    int x2 = x + w > p->w ? p->w : x + w, y2 = y + h > p->h ? p->h : y + h;
    x = x < 0 ? 0 : x;
    y = y < 0 ? 0 : y;
    if (x >= x2 || y >= y2) {
        return;
    }
    lh_charge_steps(L, (unsigned)((x2 - x) * (y2 - y) / PX_PER_STEP));
    for (int j = y; j < y2; j++) {
        uint16_t *row = p->px + (size_t)j * p->w;
        for (int i = x; i < x2; i++) {
            row[i] = c;
        }
    }
}

int lh_cv_clear(lua_State *L)
{
    lh_obj_t *o;
    lh_pix_t *p = canvas(L, &o);
    uint16_t c = arg_px(L, 2);
    if (p) {
        fill(L, p, 0, 0, p->w, p->h, c);
        lv_obj_invalidate(o->obj);
    }
    return 0;
}

int lh_cv_pixel(lua_State *L)
{
    lh_obj_t *o;
    lh_pix_t *p = canvas(L, &o);
    int x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
    uint16_t c = arg_px(L, 4);
    if (p && x >= 0 && y >= 0 && x < p->w && y < p->h) {
        p->px[y * p->w + x] = c;
        lv_obj_invalidate(o->obj);
    }
    return 0;
}

int lh_cv_rect(lua_State *L)
{
    lh_obj_t *o;
    lh_pix_t *p = canvas(L, &o);
    int x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
    int w = luaL_checkinteger(L, 4), h = luaL_checkinteger(L, 5);
    uint16_t c = arg_px(L, 6);
    if (p && w > 0 && h > 0) {
        fill(L, p, x, y, w, h, c);
        lv_obj_invalidate(o->obj);
    }
    return 0;
}

// Clips the line to the box (Liang-Barsky); false if it misses it.
static bool clip_line(int64_t *x0, int64_t *y0, int64_t *x1, int64_t *y1, int64_t w, int64_t h)
{
    double dx = (double)(*x1 - *x0), dy = (double)(*y1 - *y0), t0 = 0, t1 = 1;
    double p[4] = {-dx, dx, -dy, dy};
    double q[4] = {(double)(*x0 + 1), (double)(w - *x0), (double)(*y0 + 1), (double)(h - *y0)};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) {
                return false;
            }
            continue;
        }
        double r = q[i] / p[i];
        if (p[i] < 0 ? r > t1 : r < t0) {
            return false;
        }
        if (p[i] < 0 && r > t0) {
            t0 = r;
        } else if (p[i] > 0 && r < t1) {
            t1 = r;
        }
    }
    int64_t ax = *x0, ay = *y0;
    *x0 = ax + (int64_t)(t0 * dx + (dx < 0 ? -0.5 : 0.5));
    *y0 = ay + (int64_t)(t0 * dy + (dy < 0 ? -0.5 : 0.5));
    *x1 = ax + (int64_t)(t1 * dx + (dx < 0 ? -0.5 : 0.5));
    *y1 = ay + (int64_t)(t1 * dy + (dy < 0 ? -0.5 : 0.5));
    return true;
}

int lh_cv_line(lua_State *L)
{
    lh_obj_t *o;
    lh_pix_t *p = canvas(L, &o);
    int64_t x0 = luaL_checkinteger(L, 2), y0 = luaL_checkinteger(L, 3);
    int64_t x1 = luaL_checkinteger(L, 4), y1 = luaL_checkinteger(L, 5);
    uint16_t c = arg_px(L, 6);
    if (!p) {
        return 0;
    }
    // A long line is cut to the canvas first, so one reaching far off it
    // costs what its visible part does (short ones stay exact Bresenham).
    int64_t len = (x1 > x0 ? x1 - x0 : x0 - x1) + (y1 > y0 ? y1 - y0 : y0 - y1);
    if (len > 2 * (p->w + p->h) && !clip_line(&x0, &y0, &x1, &y1, p->w, p->h)) {
        return 0;
    }
    int64_t dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int64_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    lh_charge_steps(L, (unsigned)((dx > -dy ? dx : -dy) / PX_PER_STEP));
    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < p->w && y0 < p->h) {
            p->px[y0 * p->w + x0] = c;
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int64_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    lv_obj_invalidate(o->obj);
    return 0;
}

// c:image(src, x, y [, opts]): see-through pixels leave the canvas as it was.
int lh_cv_image(lua_State *L)
{
    lh_obj_t *o;
    lh_pix_t *p = canvas(L, &o);
    lh_pix_t *src = to_pix(L, 2, 5, "canvas:image");
    int x = luaL_checkinteger(L, 3), y = luaL_checkinteger(L, 4);
    if (!p) {
        return 0;
    }
    int i0 = x < 0 ? -x : 0, j0 = y < 0 ? -y : 0;
    int i1 = src->w < p->w - x ? src->w : p->w - x, j1 = src->h < p->h - y ? src->h : p->h - y;
    if (i0 >= i1 || j0 >= j1) {
        return 0;
    }
    lh_charge_steps(L, (unsigned)((i1 - i0) * (j1 - j0) / PX_PER_STEP));
    for (int j = j0; j < j1; j++) {
        const uint16_t *from = src->px + (size_t)j * src->w;
        uint16_t *to = p->px + (size_t)(y + j) * p->w + x;
        for (int i = i0; i < i1; i++) {
            if (from[i] != LH_PX_CLEAR) {
                to[i] = from[i];
            }
        }
    }
    lv_obj_invalidate(o->obj);
    return 0;
}

// c:get(x, y): 0xRRGGBB, or nil where it's see-through or outside.
int lh_cv_get(lua_State *L)
{
    lh_obj_t *o;
    lh_pix_t *p = canvas(L, &o);
    int x = luaL_checkinteger(L, 2), y = luaL_checkinteger(L, 3);
    uint16_t c = p && x >= 0 && y >= 0 && x < p->w && y < p->h ? p->px[y * p->w + x] : LH_PX_CLEAR;
    if (c == LH_PX_CLEAR) {
        lua_pushnil(L);
    } else {
        uint32_t r = c >> 11, g = (c >> 5) & 63, b = c & 31;
        lua_pushinteger(L, (lua_Integer)((r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2)));
    }
    return 1;
}

// ---------------------------------------------------------------------------

void lh_open_img(lua_State *L)
{
    static const luaL_Reg img[] = {{"size", img_size}, {NULL, NULL}};
    luaL_newmetatable(L, IMG_META);
    luaL_newlib(L, img);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "image");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}
