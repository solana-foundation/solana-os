// Game Boy and Game Boy Color, as a C app (docs/C_API.md "Heavy apps and
// emulators"). The core is Walnut-CGB (walnut/, MIT); this file is the badge's
// side of it: the game you copied to the app's folder on the card, read 16 KB
// bank by bank as the game asks (files_read), frames straight to the screen
// (ui_frame), one 16.7 ms frame per tick (sys_tick), the game's battery save in
// save/, and the badge's six keys as the Game Boy's eight.
//
// Keys: the d-pad is the d-pad, SELECT is A, CANCEL is B. SELECT and CANCEL
// together are START; held together for a second they open the menu (the
// Game Boy's SELECT button, the screen size, save, quit). HOME leaves.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "badge.h"

#define ENABLE_SOUND 0 // the badge has no speaker: the sound registers still work, nothing is synthesised
struct gb_s;
#include "walnut/walnut_cgb.h"
#include "walnut/walnut_cgb_cpu.h"

#define BANK 16384
#define MAX_BANKS 512            // 8 MB, the biggest Game Boy cartridge
#define FRAME_US 16743           // 70224 clocks at 4.194304 MHz
#define NAME_MAX_ 72

static struct gb_s gb;
static char rom_name[NAME_MAX_], save_name[NAME_MAX_ + 8];
static uint32_t rom_size, rom_banks;
static uint8_t *bank_ptr[MAX_BANKS];   // where each bank is in the cache, NULL: on the card
static uint8_t *cache;                 // n_slots banks
static int32_t slot_bank[256];
static uint32_t n_slots, next_victim;
static uint32_t misses;
static uint8_t cart_ram[128 * 1024];
static uint32_t ram_size, ram_sum;

// Two frames, so the screen can take one while the core draws the next.
// 64-byte aligned, big-endian: ui_frame then sends them with no copy.
static uint16_t fb[2][LCD_WIDTH * LCD_HEIGHT] __attribute__((aligned(64)));
static int cur;
static int big;                        // 1x (160 x 144, sent with no copy) or 1.5x (240 x 216, scaled: ~15 ms more a shown frame)
static uint16_t dmg_pal[4];

enum { PICK, LOAD, RUN, MENU, NO_ROM };
static int state;
static ui_obj list, label, bar;
static char names[24][NAME_MAX_];
static int n_names;
static uint32_t loaded, load_t0;

static uint32_t acc_us, chord_ms, keys_before;
static int chosen = -1, menu_choice = -1;    // a list's choice, acted on at the next tick (not inside the list's own callback)
static int start_frames, select_frames;
static uint32_t stat_t0, stat_frames, stat_shown, stat_emu_us, stat_ticks, save_check_ms;

static uint32_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000000u + t.tv_nsec / 1000);
}

// ---------------------------------------------------------------------------
// The cartridge: banks come from the card the first time the game reads
// them, into a cache as big as memory allows; a game that fits stays whole.

static uint8_t *load_bank(uint32_t b)
{
    if (b >= rom_banks) {
        b %= rom_banks ? rom_banks : 1;
        if (bank_ptr[b]) {
            return bank_ptr[b];
        }
    }
    uint32_t slot = next_victim;
    if (n_slots < rom_banks) {          // a full cache: bank 0 stays, the others take turns
        next_victim = next_victim + 1 < n_slots ? next_victim + 1 : 1;
    } else {
        next_victim++;
    }
    if (slot_bank[slot] >= 0) {
        bank_ptr[slot_bank[slot]] = NULL;
    }
    uint8_t *p = cache + (size_t)slot * BANK;
    int32_t n = files_read(rom_name, b * BANK, p, BANK);
    if (n < BANK) {
        memset(p + (n > 0 ? n : 0), 0xFF, BANK - (n > 0 ? (uint32_t)n : 0));
    }
    slot_bank[slot] = (int32_t)b;
    bank_ptr[b] = p;
    misses++;
    return p;
}

static inline uint8_t rom_byte(uint32_t a)
{
    uint32_t b = a >> 14;
    const uint8_t *p = b < MAX_BANKS ? bank_ptr[b] : NULL;
    if (!p) {
        p = load_bank(b < MAX_BANKS ? b : 0);
    }
    return p[a & (BANK - 1)];
}

static uint8_t rd8(struct gb_s *g, const uint_fast32_t a)
{
    (void)g;
    return rom_byte((uint32_t)a);
}

static uint16_t rd16(struct gb_s *g, const uint_fast32_t a)
{
    (void)g;
    return (uint16_t)(rom_byte((uint32_t)a) | rom_byte((uint32_t)a + 1) << 8);
}

static uint32_t rd32(struct gb_s *g, const uint_fast32_t a)
{
    (void)g;
    uint32_t x = (uint32_t)a;
    return (uint32_t)rom_byte(x) | (uint32_t)rom_byte(x + 1) << 8 | (uint32_t)rom_byte(x + 2) << 16 |
           (uint32_t)rom_byte(x + 3) << 24;
}

static uint8_t ram_rd(struct gb_s *g, const uint_fast32_t a)
{
    (void)g;
    return a < sizeof cart_ram ? cart_ram[a] : 0xFF;
}

static void ram_wr(struct gb_s *g, const uint_fast32_t a, const uint8_t v)
{
    (void)g;
    if (a < sizeof cart_ram) {
        cart_ram[a] = v;
    }
}

static void gb_err(struct gb_s *g, const enum gb_error_e e, const uint16_t a)
{
    (void)g;
    char m[64];
    snprintf(m, sizeof m, "The game stopped (error %d at %04X)", (int)e, (unsigned)a);
    badge_fail(m);
}

// ---------------------------------------------------------------------------
// Saves: the cartridge's battery RAM, in save/<game>.sav

static uint32_t sum(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void save_now(void)
{
    if (!ram_size || sum(cart_ram, ram_size) == ram_sum) {
        return;
    }
    for (uint32_t off = 0; off < ram_size; off += 65536) {
        uint32_t n = ram_size - off < 65536 ? ram_size - off : 65536;
        if (files_write(save_name, off, cart_ram + off, n)) {
            ui_toast(badge_error());
            return;
        }
    }
    ram_sum = sum(cart_ram, ram_size);
}

// ---------------------------------------------------------------------------
// The screen

static void draw_line(struct gb_s *g, const uint8_t *px, const uint_fast8_t line)
{
    uint16_t *o = &fb[cur][line * LCD_WIDTH];
    if (g->cgb.cgbMode) {
        for (int x = 0; x < LCD_WIDTH; x++) {
            o[x] = RGB565_BE(g->cgb.fixPalette[px[x]]);
        }
    } else {
        for (int x = 0; x < LCD_WIDTH; x++) {
            o[x] = dmg_pal[px[x] & 3];
        }
    }
}

static void show_frame(void)
{
    int r = big ? ui_frame(fb[cur], LCD_WIDTH, LCD_HEIGHT, 40, 1, 240, 216, UI_FRAME_BE)
                : ui_frame(fb[cur], LCD_WIDTH, LCD_HEIGHT, 80, 37, LCD_WIDTH, LCD_HEIGHT, UI_FRAME_BE);
    stat_shown += r > 0;
    cur ^= 1;
}

// ---------------------------------------------------------------------------
// Running

static uint8_t joypad(uint32_t k)
{
    int a = KEY_HELD(k, KEY_SELECT), b = KEY_HELD(k, KEY_BACK);
    uint8_t pressed = 0;                // bits: A B SELECT START RIGHT LEFT UP DOWN
    if (a && b) {
        a = b = 0;                      // the chord is START (or the menu), not A + B
    }
    pressed |= a ? JOYPAD_A : 0;
    pressed |= b ? JOYPAD_B : 0;
    pressed |= start_frames ? JOYPAD_START : 0;
    pressed |= select_frames ? JOYPAD_SELECT : 0;
    pressed |= KEY_HELD(k, KEY_RIGHT) ? JOYPAD_RIGHT : 0;
    pressed |= KEY_HELD(k, KEY_LEFT) ? JOYPAD_LEFT : 0;
    pressed |= KEY_HELD(k, KEY_UP) ? JOYPAD_UP : 0;
    pressed |= KEY_HELD(k, KEY_DOWN) ? JOYPAD_DOWN : 0;
    return (uint8_t)~pressed;           // the Game Boy's are active low
}

static void open_menu(void);

static void run_tick(int32_t ms)
{
    uint32_t k = sys_keys();
    int chord = KEY_HELD(k, KEY_SELECT) && KEY_HELD(k, KEY_BACK);
    int was = KEY_HELD(keys_before, KEY_SELECT) && KEY_HELD(keys_before, KEY_BACK);
    if (chord) {
        chord_ms += (uint32_t)ms;
        if (chord_ms >= 1000) {
            chord_ms = 0;
            open_menu();
            return;
        }
    } else if (was && chord_ms) {
        start_frames = 6;               // a tap of both: START
        chord_ms = 0;
    }
    keys_before = k;

    acc_us += (uint32_t)ms * 1000;
    if (acc_us > 4 * FRAME_US) {
        acc_us = 4 * FRAME_US;          // behind after a pause: don't race to catch up
    }
    uint32_t t = now_us();
    int n = 0;
    while (acc_us >= FRAME_US && n < 3) {
        acc_us -= FRAME_US;
        int last = acc_us < FRAME_US || n == 2;
        gb.direct.joypad = joypad(k);
        gb.display.lcd_draw_line = last ? draw_line : NULL;   // only the frame shown is drawn
        gb_run_frame_dualfetch(&gb);
        start_frames -= start_frames > 0;
        select_frames -= select_frames > 0;
        n++;
        if (last) {
            show_frame();
        }
    }
    stat_emu_us += now_us() - t;
    stat_frames += (uint32_t)n;
    stat_ticks++;
    save_check_ms += (uint32_t)ms;
    if (save_check_ms > 30000) {
        save_check_ms = 0;
        save_now();
    }
    uint32_t el = now_us() - stat_t0;
    if (el >= 3000000) {
        printf("GBSTAT fps=%.1f shown=%.1f emu_ms=%.2f ticks=%u misses=%u slots=%u/%u cgb=%d\n",
               stat_frames * 1e6 / el, stat_shown * 1e6 / el,
               stat_frames ? stat_emu_us / 1000.0 / stat_frames : 0.0, (unsigned)stat_ticks, (unsigned)misses,
               (unsigned)n_slots, (unsigned)rom_banks, gb.cgb.cgbMode);
        stat_t0 = now_us();
        stat_frames = stat_shown = stat_emu_us = stat_ticks = 0;
    }
}

static void start_game(void)
{
    enum gb_init_error_e e = gb_init(&gb, rd8, rd16, rd32, ram_rd, ram_wr, gb_err, NULL);
    if (e != GB_INIT_NO_ERROR) {
        ui_clear();
        ui_label(0, 90, e == GB_INIT_INVALID_CHECKSUM ? "That isn't a Game Boy ROM (bad header)." : "This cartridge type isn't supported.",
                 &(ui_label_opts){.size = 14, .w = UI_W, .align = UI_CENTER});
        state = NO_ROM;
        return;
    }
    gb_init_lcd(&gb, draw_line);
    size_t rs = 0;
    gb_get_save_size_s(&gb, &rs);
    ram_size = rs <= sizeof cart_ram ? (uint32_t)rs : 0;
    memset(cart_ram, 0xFF, sizeof cart_ram);
    if (ram_size) {
        int32_t n = files_read(save_name, 0, cart_ram, ram_size);
        (void)n;
    }
    ram_sum = sum(cart_ram, ram_size);
    int32_t now = sys_time();
    if (now > 0) {
        time_t tt = (time_t)now;
        struct tm *tm = gmtime(&tt);
        if (tm) {
            gb_set_rtc(&gb, tm);
        }
    }
    printf("%s: %u KB, %u of %u banks in memory, %u ms to start\n", rom_name, (unsigned)(rom_size / 1024),
           (unsigned)n_slots, (unsigned)rom_banks, (unsigned)((now_us() - load_t0) / 1000));
    ui_clear();
    ui_bg(COL_BLACK);
    sys_tick(16);                       // a frame every 16.7 ms: ticks of 16, the rest carried over
    stat_t0 = now_us();
    state = RUN;
}

// The cache: as many banks as fit next to everything else (each 16 KB), and
// at least enough for bank 0 and two others.
static int make_cache(void)
{
    uint32_t want = rom_banks < 256 ? rom_banks : 256;
    uint32_t least = rom_banks < 3 ? rom_banks : 3;
    while (want >= least && want) {
        cache = malloc((size_t)want * BANK);
        if (cache) {
            n_slots = want;
            break;
        }
        want = want * 3 / 4;
    }
    if (!cache) {
        return -1;
    }
    for (uint32_t i = 0; i < 256; i++) {
        slot_bank[i] = -1;
    }
    next_victim = 0;
    return 0;
}

static void pick(int i)
{
    snprintf(rom_name, sizeof rom_name, "%s", names[i]);
    char base[NAME_MAX_];
    snprintf(base, sizeof base, "%s", rom_name);
    char *dot = strrchr(base, '.');
    if (dot) {
        *dot = 0;
    }
    snprintf(save_name, sizeof save_name, "save/%s.sav", base);
    int32_t size = files_size(rom_name);
    if (size < 0x150) {
        ui_toast("That file is too small for a game");
        return;
    }
    rom_size = (uint32_t)size;
    rom_banks = (rom_size + BANK - 1) / BANK;
    if (rom_banks > MAX_BANKS) {
        ui_toast("Bigger than any Game Boy cartridge");
        return;
    }
    if (make_cache()) {
        ui_toast("Not enough memory");
        return;
    }
    ui_clear();
    label = ui_label(0, 80, rom_name, &(ui_label_opts){.size = 14, .w = UI_W, .align = UI_CENTER});
    bar = ui_bar(60, 110, 200, 8, 0);
    loaded = 0;
    load_t0 = now_us();
    state = n_slots >= rom_banks ? LOAD : RUN;   // a game that fits is read whole first
    if (state == RUN) {
        start_game();
    }
}

static void picked(void *user, int index)
{
    (void)user;
    chosen = index;
}

// Whole games load 256 KB a tick (a read costs host steps: about four ticks a MB).
static void load_tick(void)
{
    uint32_t b = loaded;
    uint32_t n = rom_banks - b < 16 ? rom_banks - b : 16;
    uint8_t *p = cache + (size_t)b * BANK;
    int32_t got = files_read(rom_name, b * BANK, p, n * BANK);
    if (got < 0) {
        badge_fail("Couldn't read the game from the card");
        return;
    }
    if ((uint32_t)got < n * BANK) {
        memset(p + got, 0xFF, n * BANK - (uint32_t)got);
    }
    for (uint32_t i = 0; i < n; i++) {
        bank_ptr[b + i] = cache + (size_t)(b + i) * BANK;
        slot_bank[b + i] = (int32_t)(b + i);
    }
    loaded += n;
    ui_bar_set(bar, (int)(loaded * 100 / rom_banks));
    if (loaded >= rom_banks) {
        start_game();
    }
}

static int is_rom(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || strchr(name, '/')) {
        return 0;
    }
    char e[5] = {0};
    for (int i = 0; i < 4 && dot[1 + i]; i++) {
        e[i] = (char)(dot[1 + i] | 0x20);
    }
    return !strcmp(e, "gb") || !strcmp(e, "gbc") || !strcmp(e, "cgb");
}

static void found(void *user, const char *name, int32_t size)
{
    (void)user;
    (void)size;
    if (n_names < 24 && is_rom(name) && strlen(name) < NAME_MAX_) {
        snprintf(names[n_names++], NAME_MAX_, "%s", name);
    }
}

void on_start(void)
{
    for (int i = 0; i < 4; i++) {
        static const uint32_t greens[4] = {0xE0F8D0, 0x88C070, 0x346856, 0x081820};
        dmg_pal[i] = RGB565_BE(RGB565_HEX(greens[i]));
    }
    files_list(found, NULL);
    if (!n_names) {
        char dir[64], text[160];
        files_dir(dir, sizeof dir);
        snprintf(text, sizeof text, "Copy a .gb or .gbc game you own to\n%s\non the microSD card, then open this again.", dir);
        ui_label(0, 70, text, &(ui_label_opts){.size = 14, .w = UI_W, .align = UI_CENTER});
        ui_hint("HOME leave");
        state = NO_ROM;
        return;
    }
    if (n_names == 1) {
        pick(0);
        return;
    }
    const char *items[24];
    for (int i = 0; i < n_names; i++) {
        items[i] = names[i];
    }
    ui_label(8, 4, "Pick a game", &(ui_label_opts){.size = 14, .color = COL_DIM});
    list = ui_list(0, 24, UI_W, UI_H - 40, items, n_names, picked, NULL);
    ui_hint("SELECT play   HOME leave");
    state = PICK;
}

// ---------------------------------------------------------------------------
// The menu (SELECT + CANCEL held a second)

static void menu_pick(void *user, int index)
{
    (void)user;
    menu_choice = index;
}

static void menu_done(int index)
{
    ui_obj_remove(&list);
    ui_clear();
    ui_bg(COL_BLACK);
    state = RUN;
    acc_us = 0;
    if (index == 0) {
        select_frames = 6;
    } else if (index == 1) {
        big = !big;
    } else if (index == 2) {
        save_now();
        ui_toast("Saved");
    } else if (index == 3) {
        save_now();
        sys_exit();
    }
}

static void open_menu(void)
{
    static const char *const items[] = {"Press SELECT", "Screen: 1x / 1.5x", "Save now", "Save and quit", "Back to the game"};
    state = MENU;
    ui_clear();
    ui_bg(COL_BG);
    list = ui_list(0, 0, UI_W, UI_H - 16, items, 5, menu_pick, NULL);
    ui_hint("SELECT choose");
}

void on_tick(int32_t ms)
{
    if (state == PICK && chosen >= 0 && chosen < n_names) {
        int i = chosen;
        chosen = -1;
        pick(i);
        return;
    }
    if (state == MENU && menu_choice >= 0) {
        int i = menu_choice;
        menu_choice = -1;
        menu_done(i);
        return;
    }
    if (state == LOAD) {
        load_tick();
    } else if (state == RUN) {
        run_tick(ms);
    }
}

int32_t on_key(int32_t key, int32_t repeat)
{
    (void)repeat;
    if (state == MENU && key == KEY_BACK) {
        menu_choice = 4;                // back to the game
    }
    return state != NO_ROM && state != PICK; // CANCEL is B while a game runs; elsewhere it leaves
}

void on_exit(void)
{
    if (state == RUN || state == MENU) {
        save_now();
    }
}
