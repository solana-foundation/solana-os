// C vs Lua benchmark, the C side (README.md). ../lua/main.lua is the same app
// in Lua: the same menu, phases and console lines, so run.py drives both.
//
// Pick a workload with UP/DOWN and SELECT (RIGHT: the same, then a ramp that
// grows the work per callback until the badge stops the app). Each run:
//   CAL     doubles the units in one callback until they take 8 ms
//   CHECK   a fixed amount of work, spread over callbacks: "CHK wl=... chk=..."
//   RUN     3 s of callbacks, a chunk each (a whole frame per tick when it
//           fits): "BENCH wl=... units= us= wall= frames= blit="
//   PROBE   one callback at least as big as the run's, then the app closes, so
//           `lua mem` reports the most any callback counted against its limit;
//           "PROBE wl=... n= us= max_us=" (max_us: the longest callback of work)
//   (RAMP)  "FIT wl=... n= us=" after every callback that ran, 6 % more each
// Times come from the monotonic clock (microseconds); us counts only the
// work, wall the whole run.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "badge.h"
#include "kernels.h"

#define SAFE_US 120000 // a frame drawn in one callback if it takes less (native: 200 ms)

static uint32_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec * 1000000u + (uint32_t)(ts.tv_nsec / 1000);
}

static void out(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    badge_print(line);
}

static int verifying; // the CHECK phase: units return what the checksum adds up

// --- Workloads ---------------------------------------------------------------------

// Rows in a scattered order (97 is prime to 240): the set's middle rows cost
// many times its edges, so consecutive units sample the whole picture.
#define ROW(i) ((int)(((i) * 97u) % K_MANDEL_H))
static uint32_t u_mandel(uint32_t i) { return k_mandel_row(ROW(i)); }
static uint32_t u_mandelf(uint32_t i) { return k_mandelf_row(ROW(i)); }
static uint32_t u_mandeld(uint32_t i) { return k_mandeld_row(ROW(i)); }

static uint8_t sieve_flags[K_SIEVE_N + 1];
static uint32_t u_sieve(uint32_t i)
{
    (void)i;
    return k_sieve(sieve_flags);
}

static uint32_t sha_w[K_SHA_MSG / 4], sha_h[8];
static int s_sha(int step)
{
    (void)step;
    k_sha_msg_words(sha_w);
    return 0;
}
static uint32_t u_sha(uint32_t i)
{
    // 64 blocks of the message, then its padding block: unit 64 of each 65
    // finishes the hash and returns the digest's first word.
    uint32_t b = i % 65;
    if (b == 0) {
        k_sha_init(sha_h);
    }
    if (b < 64) {
        k_sha_block(sha_h, sha_w + b * 16);
        return 0;
    }
    uint32_t pad[16] = {0x80000000u};
    pad[15] = K_SHA_MSG * 8;
    k_sha_block(sha_h, pad);
    return sha_h[0];
}

static uint8_t sha_bytes[K_SHA_MSG];
static int s_shaapi(int step)
{
    (void)step;
    for (int i = 0; i < K_SHA_MSG; i++) {
        sha_bytes[i] = (uint8_t)((i * 7 + 3) & 255);
    }
    return 0;
}
static uint32_t u_shaapi(uint32_t i)
{
    uint8_t d[32];
    crypto_sha256(sha_bytes + (i % 4) * 1024, 1024, d);
    return (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
}

static uint32_t *buf_a, *buf_b;
static int s_buffers(int step) // 2 MB to set up: a 64 KB segment of each a callback
{
    if (step == 0) {
        buf_a = malloc(K_BUF_WORDS * 4);
        buf_b = malloc(K_BUF_WORDS * 4);
        if (!buf_a || !buf_b) {
            badge_fail("no memory for the 1 MB buffers");
        }
        return 1;
    }
    uint32_t seg = (uint32_t)(step - 1) * K_SEG_WORDS;
    k_fill(buf_a + seg, seg, K_SEG_WORDS, 0x12345678);
    memset(buf_b + seg, 0, K_SEG_WORDS * 4);
    return seg + K_SEG_WORDS < K_BUF_WORDS;
}
static uint32_t u_fill(uint32_t i)
{
    uint32_t seg = (i % (K_BUF_WORDS / K_SEG_WORDS)) * K_SEG_WORDS;
    k_fill(buf_a + seg, seg, K_SEG_WORDS, 0x5A5A5A5A);
    return verifying ? k_sum(buf_a + seg, K_SEG_WORDS) : 0;
}
static uint32_t u_copy(uint32_t i)
{
    uint32_t seg = (i % (K_BUF_WORDS / K_SEG_WORDS)) * K_SEG_WORDS;
    k_copy(buf_b + seg, buf_a + seg, K_SEG_WORDS);
    return verifying ? k_sum(buf_b + seg, K_SEG_WORDS) : 0;
}

// Drawing: a frame computed pixel by pixel into our own RGB565 buffer, rows
// handed to the canvas with ui_canvas_blit at the end of each callback.
static ui_obj canvas;
static uint16_t *frame;
static int fw, fh, dirty_lo, dirty_hi;
static uint32_t blit_us;
static void draw_setup(int w, int h, int scale)
{
    fw = w;
    fh = h;
    frame = malloc((size_t)w * h * 2);
    if (!frame) {
        badge_fail("no memory for the frame");
        return;
    }
    canvas = ui_canvas(0, 0, w, h, scale, COL_BLACK);
    dirty_lo = h;
    dirty_hi = -1;
}
static int s_draw(int step)
{
    (void)step;
    draw_setup(320, 216, 1);
    return 0;
}
static int s_draw80(int step)
{
    (void)step;
    draw_setup(80, 54, 4);
    return 0;
}
static uint32_t u_draw(uint32_t i)
{
    int y = (int)(i % (uint32_t)fh), t = (int)(i / (uint32_t)fh);
    if (y < dirty_lo) {
        dirty_lo = y;
    }
    if (y > dirty_hi) {
        dirty_hi = y;
    }
    return k_draw_row(frame + y * fw, fw, y, t);
}
static void f_draw(void)
{
    if (dirty_hi >= dirty_lo) {
        uint32_t t0 = now_us();
        ui_canvas_blit(canvas, frame + dirty_lo * fw, 0, dirty_lo, fw, dirty_hi - dirty_lo + 1);
        blit_us += now_us() - t0;
    }
    dirty_lo = fh;
    dirty_hi = -1;
}

// API-heavy: what C can't speed up.
#define NLABELS 16
static ui_obj labels[NLABELS];
static int s_labels(int step)
{
    (void)step;
    for (int i = 0; i < NLABELS; i++) {
        labels[i] = ui_label(10 + (i % 4) * 75, 20 + (i / 4) * 30, "0", &(ui_label_opts){.size = 14});
    }
    return 0;
}
static uint32_t u_labels(uint32_t i)
{
    char text[12], rev[12];
    int n = 0, k = 0;
    uint32_t v = i;
    do {
        rev[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k) {
        text[n++] = rev[--k];
    }
    text[n] = 0;
    ui_label_text(labels[i % NLABELS], text);
    return 0;
}

#define NITEMS 20
static ui_obj list;
static const char *items_a[NITEMS], *items_b[NITEMS];
static char item_text[2][NITEMS][16];
static int s_list(int step)
{
    (void)step;
    for (int i = 0; i < NITEMS; i++) {
        snprintf(item_text[0][i], 16, "Item %d", i + 1);
        snprintf(item_text[1][i], 16, "Row %d", i + 1);
        items_a[i] = item_text[0][i];
        items_b[i] = item_text[1][i];
    }
    list = ui_list(0, 0, UI_W, 180, items_a, NITEMS, NULL, NULL);
    return 0;
}
static uint32_t u_list(uint32_t i)
{
    ui_list_items(list, (i & 1) ? items_b : items_a, NITEMS);
    return 0;
}

// A game tick: 256 particles moved, bounced and drawn 2 x 2 on a 160 x 108
// canvas shown at 2x, then the frame blitted. One unit = one whole tick.
static k_part parts[K_PART_N];
static int s_game(int step)
{
    (void)step;
    draw_setup(K_PART_W, K_PART_H, 2);
    return 0;
}
static void r_game(void) { k_part_init(parts); }
static uint32_t u_game(uint32_t i)
{
    for (int p = 0; p < K_PART_W * K_PART_H; p++) {
        frame[p] = K_PART_BG;
    }
    for (int p = 0; p < K_PART_N; p++) {
        k_part_step(parts, p);
        k_part_draw(frame, parts, p);
    }
    uint32_t t0 = now_us();
    ui_canvas_blit(canvas, frame, 0, 0, K_PART_W, K_PART_H);
    blit_us += now_us() - t0;
    return verifying && i == 99 ? k_part_sum(parts) : 0;
}

typedef struct {
    const char *name;
    uint32_t verify; // units in the checksum run
    uint32_t frame;  // units in one frame (0: not drawn)
    uint32_t check;  // most units a callback while checking (0: the run's chunk)
    int (*setup)(int step); // UI and buffers; 1: call again next tick
    void (*reset)(void);
    uint32_t (*unit)(uint32_t i);
    void (*flush)(void);
} workload;

static const workload wls[] = {
    {"mandel", K_MANDEL_H, 0, 0, NULL, NULL, u_mandel, NULL},
    {"mandelf", K_MANDEL_H, 0, 0, NULL, NULL, u_mandelf, NULL},
    {"mandeld", K_MANDEL_H, 0, 0, NULL, NULL, u_mandeld, NULL},
    {"sieve", 4, 0, 0, NULL, NULL, u_sieve, NULL},
    {"sha", 65, 0, 0, s_sha, NULL, u_sha, NULL},
    {"shaapi", 4, 0, 0, s_shaapi, NULL, u_shaapi, NULL},
    {"fill", K_BUF_WORDS / K_SEG_WORDS, 0, 1, s_buffers, NULL, u_fill, NULL},
    {"copy", K_BUF_WORDS / K_SEG_WORDS, 0, 1, s_buffers, NULL, u_copy, NULL},
    {"draw", 216, 216, 0, s_draw, NULL, u_draw, f_draw},
    {"draw80", 54, 54, 0, s_draw80, NULL, u_draw, f_draw},
    {"labels", 0, 0, 0, s_labels, NULL, u_labels, NULL},
    {"list", 0, 0, 0, s_list, NULL, u_list, NULL},
    {"game", 100, 1, 0, s_game, r_game, u_game, NULL},
};
#define NWL ((int)(sizeof wls / sizeof wls[0]))
static const char *const limits[] = {"alloc", "stack", "ui"};
#define NMENU (NWL + 3)

// --- Limits ------------------------------------------------------------------------

static uint32_t alloc_total;
static char *hoard[256]; // kept (and touched), so the compiler can't drop the mallocs
static int alloc_tick(void) // 1: done
{
    for (int k = 0; k < 8; k++) {
        char *p = alloc_total / (64 * 1024) < 256 ? malloc(64 * 1024) : NULL;
        if (!p) {
            int32_t used = 0, max = 0;
            sys_memory(&used, &max);
            out("ALLOC lang=c got=%u used=%ld max=%ld stop=malloc_null", (unsigned)alloc_total, (long)used,
                (long)max);
            return 1;
        }
        p[0] = (char)k;
        hoard[alloc_total / (64 * 1024)] = p;
        alloc_total += 64 * 1024;
    }
    return 0;
}

static int __attribute__((noinline)) rec(int n)
{
    if (n <= 0) {
        return 0;
    }
    int r = rec(n - 1);
    return (r ^ n) + 1;
}
static uint32_t stack_depth = 100;
static volatile int sink;
static void stack_tick(void)
{
    uint32_t t0 = now_us();
    sink = rec((int)stack_depth);
    out("STACK lang=c ok=%u us=%u", (unsigned)stack_depth, (unsigned)(now_us() - t0));
    stack_depth += stack_depth / 4;
}

static void ui_limit(void)
{
    ui_clear();
    int n = 0;
    for (;;) {
        ui_obj l = ui_label(0, (n % 15) * 14, "x", NULL);
        if (!l.ref) {
            break;
        }
        n++;
    }
    out("UI lang=c labels=%d err=%s", n, badge_error());
}

// --- The runner --------------------------------------------------------------------

#define MENU_X(i) ((i) < 8 ? 24 : 174)
#define MENU_Y(i) (24 + ((i) % 8) * 20)
static const char *menu_name(int i) { return i < NWL ? wls[i].name : limits[i - NWL]; }

enum { MENU, SETUP, CAL, CHECK, RUN, PROBE, PROBED, RAMP, LIMIT };
static int phase = MENU, sel, cur, ramp, lim;
static ui_obj cursor;
static int setup_step;
static uint32_t max_us; // the longest callback of work so far
static uint32_t n_cal, unit_us, chunk, done, chk, units, work_us, frames, t_run, n_ramp;

static uint32_t run_units(uint32_t n) // n units from `done`; their time in us
{
    const workload *w = &wls[cur];
    uint32_t t0 = now_us();
    for (uint32_t k = 0; k < n; k++) {
        chk += w->unit(done++);
    }
    if (w->flush) {
        w->flush();
    }
    uint32_t dt = now_us() - t0;
    max_us = dt > max_us ? dt : max_us;
    return dt;
}

static void restart(void)
{
    done = 0;
    chk = 0;
    blit_us = 0;
    if (wls[cur].reset) {
        wls[cur].reset();
    }
}

static void start(int i, int with_ramp)
{
    cur = i;
    ramp = with_ramp;
    ui_clear();
    badge_obj_free(&cursor);
    ui_label(4, 4, menu_name(i), &(ui_label_opts){.size = 12, .color = COL_DIM});
    if (i >= NWL) {
        lim = i - NWL;
        phase = LIMIT;
        alloc_total = 0;
        return;
    }
    setup_step = 0;
    max_us = 0;
    phase = SETUP;
}

static void finish(void)
{
    out("DONE wl=%s", cur < NWL ? wls[cur].name : limits[cur - NWL]);
    sys_exit();
}

void on_tick(int32_t ms)
{
    (void)ms;
    const workload *w = &wls[cur];
    switch (phase) {
    case SETUP:
        if (w->setup && w->setup(setup_step++)) {
            break;
        }
        restart();
        n_cal = 1;
        phase = CAL;
        break;
    case CAL: {
        uint32_t dt = run_units(n_cal);
        if (dt < 8000 && n_cal < (1u << 20)) {
            n_cal *= 2;
            break;
        }
        unit_us = dt / n_cal ? dt / n_cal : 1;
        chunk = 15000 / unit_us ? 15000 / unit_us : 1;
        if (w->frame) {
            chunk = w->frame * unit_us <= SAFE_US ? w->frame : (SAFE_US / unit_us ? SAFE_US / unit_us : 1);
        }
        restart();
        verifying = 1;
        phase = w->verify ? CHECK : RUN;
        if (!w->verify) {
            verifying = 0;
            t_run = now_us();
            units = work_us = frames = 0;
        }
        break;
    }
    case CHECK: {
        uint32_t left = w->verify - done, n = w->check && w->check < chunk ? w->check : chunk;
        run_units(left < n ? left : n);
        if (done == w->verify) {
            out("CHK wl=%s chk=%08lx", w->name, (unsigned long)chk);
            verifying = 0;
            restart();
            t_run = now_us();
            units = work_us = frames = 0;
            phase = RUN;
        }
        break;
    }
    case RUN:
        work_us += run_units(chunk);
        units += chunk;
        if (now_us() - t_run >= 3000000u) {
            uint32_t wall = (now_us() - t_run) / 1000;
            out("BENCH wl=%s lang=c units=%u us=%u wall=%u frames=%u blit=%u", w->name, (unsigned)units,
                (unsigned)work_us, (unsigned)wall, (unsigned)(w->frame ? units / w->frame : 0), (unsigned)blit_us);
            unit_us = work_us / units ? work_us / units : 1;
            if (ramp) {
                n_ramp = 20000 / unit_us ? 20000 / unit_us : 1;
                phase = RAMP;
            } else {
                phase = PROBE;
            }
        }
        break;
    case PROBE: {
        // The biggest callback of the run (at least the run's chunk, and
        // about 20 ms): `lua mem` reads what it counted.
        n_ramp = 20000 / unit_us ? 20000 / unit_us : 1;
        n_ramp = n_ramp > chunk ? n_ramp : chunk;
        work_us = run_units(n_ramp);
        phase = PROBED;
        break;
    }
    case PROBED:
        out("PROBE wl=%s n=%u us=%u max_us=%u", w->name, (unsigned)n_ramp, (unsigned)work_us, (unsigned)max_us);
        finish();
        phase = MENU;
        break;
    case RAMP: {
        uint32_t dt = run_units(n_ramp);
        out("FIT wl=%s n=%u us=%u", w->name, (unsigned)n_ramp, (unsigned)dt);
        n_ramp += n_ramp / 16 > 1 ? n_ramp / 16 : 1;
        break;
    }
    case LIMIT:
        if (lim == 0) {
            if (alloc_tick()) {
                finish();
                phase = MENU;
            }
        } else if (lim == 1) {
            stack_tick();
        } else {
            ui_limit();
            finish();
            phase = MENU;
        }
        break;
    default:
        break;
    }
}

static void show_cursor(void) { ui_obj_pos(cursor, MENU_X(sel) - 14, MENU_Y(sel)); }

void on_start(void)
{
    char names[200] = "";
    ui_label(4, 2, "C vs Lua: the C app", &(ui_label_opts){.size = 12, .color = COL_GREEN});
    for (int i = 0; i < NMENU; i++) {
        ui_label(MENU_X(i), MENU_Y(i), menu_name(i), &(ui_label_opts){.size = 14});
        strncat(names, menu_name(i), sizeof names - strlen(names) - 2);
        if (i < NMENU - 1) {
            strcat(names, ",");
        }
    }
    cursor = ui_label(MENU_X(0) - 14, MENU_Y(0), ">", &(ui_label_opts){.size = 14, .color = COL_GREEN});
    ui_hint("SELECT run   RIGHT run + ramp   CANCEL exit");
    out("MENU %s", names);
    out("READY lang=c ms=%u", (unsigned)sys_millis());
}

int32_t on_key(int32_t key, int32_t repeat)
{
    (void)repeat;
    if (key == KEY_BACK) {
        return 0;
    }
    if (phase != MENU) {
        return 1;
    }
    if (key == KEY_UP && sel > 0) {
        sel--;
        show_cursor();
    } else if (key == KEY_DOWN && sel < NMENU - 1) {
        sel++;
        show_cursor();
    } else if (key == KEY_SELECT || key == KEY_RIGHT) {
        start(sel, key == KEY_RIGHT);
    }
    return 1;
}
