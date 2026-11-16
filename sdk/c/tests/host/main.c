// What badge.h's wrappers return, checked against a real host: the emulator
// (main/lua/tests/c/run.ts runs it with the SDK's examples) or a badge (lua
// recv + lua run). Each check prints "ok <what>" or "FAIL <what>", then
// "done". Offline answers only: nothing here needs the server, a card file
// or another badge. docs/C_API.md documents what is checked.
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "badge.h"

static int s_fails;

static void check(int ok, const char *what)
{
    char line[160];
    snprintf(line, sizeof line, "%s %s", ok ? "ok" : "FAIL", what);
    badge_print(line);
    s_fails += !ok;
}

static void checkf(int ok, const char *what, long got)
{
    char line[160];
    snprintf(line, sizeof line, "%s %s (got %ld; %s)", ok ? "ok" : "FAIL", what, got, badge_error());
    badge_print(line);
    s_fails += !ok;
}

static int s_after, s_every;
static int32_t s_every_id;

static void after_fn(void *user)
{
    (void)user;
    s_after++;
}

static void every_fn(void *user)
{
    (void)user;
    if (++s_every == 2) {
        timer_cancel(s_every_id);
    }
}

static int32_t raw_cb(void *ud, int32_t nargs)
{
    (void)ud;
    sos_push_int(nargs * 10);
    return 1;
}

static void finish(void *user)
{
    (void)user;
    checkf(s_after == 2, "both timer_after timers ran once", s_after);
    checkf(s_every == 2, "timer_every ran until timer_cancel", s_every);
    badge_print(s_fails ? "done with failures" : "done");
}

// Each stage is its own callback (a timer), so its own instruction budget:
// snprintf alone costs thousands of WebAssembly instructions a call.
static void stage2(void *user);
static void stage3(void *user);

static void stage1(void *user)
{
    (void)user;
    int32_t n;

    // --- ui -----------------------------------------------------------------
    ui_obj l = ui_label(0, 0, "probe", &(ui_label_opts){.size = 14});
    check(l.ref > 0, "ui_label returns a handle");
    checkf(ui_label_text(l, "probe 2") == 0, "ui_label_text 0", 0);
    checkf(ui_obj_pos(l, 4, 4) == 0, "ui_obj_pos 0", 0);
    checkf(ui_obj_remove(&l) == 0 && l.ref == 0, "ui_obj_remove 0 and clears the handle", l.ref);
    checkf(ui_label_text(l, "x") == BADGE_ERR, "a removed handle fails", 0);

    ui_obj r = ui_rect(0, 20, 10, 10, &(ui_rect_opts){.color = COL_GREEN, .border = COL_BLACK});
    check(r.ref > 0, "ui_rect");
    ui_obj b = ui_bar(0, 40, 100, 8, 30);
    checkf(b.ref > 0 && ui_bar_set(b, 70) == 0, "ui_bar, ui_bar_set", b.ref);

    static const char *const items[] = {"one", "two", "three"};
    ui_obj list = ui_list(0, 60, 160, 108, items, 3, NULL, NULL);
    check(list.ref > 0, "ui_list");
    n = ui_list_selected(list);
    checkf(n == 0, "ui_list_selected starts at 0", n);
    checkf(ui_list_select(list, 2) == 0, "ui_list_select", 0);
    n = ui_list_selected(list);
    checkf(n == 2, "ui_list_selected after select(2)", n);
    checkf(ui_obj_remove(&list) == 0, "remove the list", 0);

    ui_obj q = ui_qr(200, 0, 100, "solana:test");
    checkf(q.ref > 0 && ui_qr_text(q, "solana:other") == 0, "ui_qr, ui_qr_text", q.ref);
    ui_obj_remove(&q);

    checkf(ui_hint("probe") == 0, "ui_hint 0", 0);
    checkf(ui_toast("probe") == 0, "ui_toast 0", 0);

    static const char *const rows[] = {"pg", "x."};
    ui_pal pal[] = {{'x', 0xFF0000}};
    ui_img img = ui_load_rows(rows, 2, pal, 1);
    int w = 0, h = 0;
    checkf(img.ref > 0 && ui_img_size(img, &w, &h) == 0 && w == 2 && h == 2, "ui_load_rows, ui_img_size 2x2", w * 100 + h);
    static const char *const bad[] = {"pZ"};
    ui_img none = ui_load_rows(bad, 1, NULL, 0);
    checkf(none.ref == 0 && badge_error()[0], "a character not in the palette: no image, a reason", none.ref);

    // --- canvas and blit --------------------------------------------------------
    ui_obj c = ui_canvas(0, 100, 8, 8, 2, 0);
    check(c.ref > 0, "ui_canvas, see-through");
    n = ui_canvas_get(c, 0, 0);
    checkf(n == -1, "ui_canvas_get of a see-through pixel: -1", n);
    checkf(ui_canvas_pixel(c, 1, 1, 0x9945FF) == 0, "ui_canvas_pixel", 0);
    n = ui_canvas_get(c, 1, 1);
    checkf(n == 0x9C45FF, "ui_canvas_get gives RGB565's colour (0x9C45FF for 0x9945FF)", n);
    n = ui_canvas_get(c, 99, 99);
    checkf(n == -1, "ui_canvas_get outside: -1", n);
    uint16_t px[4] = {0x0001, 0x0000, RGB565(255, 255, 255), RGB565_HEX(0x14F195)};
    checkf(ui_canvas_blit(c, px, 2, 2, 2, 2) == 0, "ui_canvas_blit 0", 0);
    n = ui_canvas_get(c, 2, 2);
    checkf(n == 0, "blit writes 0x0001 as 0x0000: black, not see-through", n);
    n = ui_canvas_get(c, 3, 2);
    checkf(n == 0, "blit 0x0000 is black", n);
    n = ui_canvas_get(c, 2, 3);
    checkf(n == 0xFFFFFF, "blit white", n);
    checkf(ui_canvas_blit(c, px, -1, -1, 2, 2) == 0, "a blit partly off the canvas is clipped", 0);
    checkf(ui_canvas_blit(r, px, 0, 0, 2, 2) == BADGE_ERR, "a blit onto a rect fails", 0);
    checkf(ui_canvas_image(c, img, 4, 4) == 0, "ui_canvas_image", 0);
    n = ui_canvas_get(c, 4, 5);
    checkf(n == 0xFF0000, "the stamp's red pixel", n);
    n = ui_canvas_get(c, 5, 5);
    checkf(n == -1, "the stamp's see-through pixel leaves the canvas as it was", n);
    checkf(ui_canvas_clear(c, COL_NONE) == 0 && ui_canvas_get(c, 1, 1) == -1, "ui_canvas_clear(COL_NONE)", 0);
    badge_obj_free(&img);
    timer_after(20, stage2, NULL);
}

static void stage2(void *user)
{
    (void)user;
    char buf[128];
    int32_t n;

    // --- storage ----------------------------------------------------------------
    storage_remove("p_int");
    storage_remove("p_str");
    storage_remove("p_json");
    n = storage_get_int("p_int", -7);
    checkf(n == -7, "storage_get_int unset: the default", n);
    n = storage_get_str("p_str", buf, sizeof buf);
    checkf(n == BADGE_ERR && buf[0] == 0, "storage_get_str unset: BADGE_ERR", n);
    n = storage_get_json("p_json", buf, sizeof buf);
    checkf(n == 4 && !strcmp(buf, "null"), "storage_get_json unset: \"null\"", n);
    checkf(storage_set_int("p_int", 42) == 0 && storage_get_int("p_int", 0) == 42, "storage int", 0);
    checkf(storage_set_str("p_str", "hey") == 0 && storage_get_str("p_str", buf, sizeof buf) == 3, "storage str", 0);
    checkf(storage_set_json("p_json", "{\"a\":[1,2]}") == 0, "storage_set_json", 0);
    n = storage_get_json("p_json", buf, sizeof buf);
    checkf(n > 0 && strstr(buf, "\"a\"") != NULL, "storage_get_json", n);
    badge_val v = storage_push("p_json");
    checkf(badge_type(v) == SOS_TABLE, "storage_push: a table", badge_type(v));
    badge_pop(1);
    checkf(storage_set_json("p_json", "{bad") == BADGE_ERR, "storage_set_json of bad JSON fails", 0);
    checkf(storage_remove("p_int") == 0 && storage_get_int("p_int", -1) == -1, "storage_remove", 0);

    // --- fs (this probe bundles no files) ----------------------------------------
    n = fs_exists("nope.txt");
    checkf(n == 0, "fs_exists of a missing file: 0", n);
    n = fs_size("nope.txt");
    checkf(n == -1, "fs_size of a missing file: -1", n);
    n = fs_read("nope.txt", buf, sizeof buf);
    checkf(n == BADGE_ERR, "fs_read of a missing file: BADGE_ERR", n);
    n = fs_json_push("nope.json");
    checkf(n == BADGE_ERR, "fs_json_push of a missing file: BADGE_ERR", n);
    n = fs_lines("nope.txt", NULL, NULL);
    checkf(n == BADGE_ERR, "fs_lines of a missing file: BADGE_ERR", n);

    // --- json ---------------------------------------------------------------
    checkf(json_push("{\"x\":3,\"s\":\"hi\",\"t\":true}") == 0, "json_push", 0);
    badge_val t = badge_top();
    checkf(badge_field_int(t, "x", 0) == 3, "badge_field_int", 0);
    checkf(badge_field_int(t, "s", -1) == -1, "badge_field_int of a string: the default", 0);
    checkf(badge_field_str(t, "s", buf, sizeof buf) == 2 && !strcmp(buf, "hi"), "badge_field_str", 0);
    checkf(badge_field_str(t, "nope", buf, sizeof buf) == BADGE_ERR, "badge_field_str missing: BADGE_ERR", 0);
    checkf(badge_field_bool(t, "t") == 1 && badge_field_bool(t, "nope") == 0, "badge_field_bool", 0);
    n = json_encode(t, buf, sizeof buf);
    checkf(n > 0 && strstr(buf, "\"x\":3") != NULL, "json_encode", n);
    n = json_encode(t, buf, 4);
    checkf(n > 4 && strlen(buf) == 3, "json_encode cut: the full length, cap - 1 bytes", n);
    badge_pop(1);
    checkf(json_push("{nope") == BADGE_ERR, "json_push of bad JSON: BADGE_ERR", 0);
    checkf(sos_top() == 0, "the frame is empty again", sos_top());
    timer_after(20, stage3, NULL);
}

static void stage3(void *user)
{
    (void)user;
    char buf[128];
    int32_t n;

    // --- sys, badge -------------------------------------------------------------
    int32_t used = 0, max = 0;
    checkf(sys_memory(&used, &max) == 0 && used > 0 && max > 0, "sys_memory", used);
    checkf(max == 6 * 1024 * 1024, "sys_memory max: 6 MB", max);
    checkf(used > 64 * 1024, "sys_memory used counts the module's memory too", used);
    n = sys_battery();
    checkf(n >= -1 && n <= 100, "sys_battery: -1 or a percent", n);
    n = sys_time();
    checkf(n == 0 || n > 1700000000, "sys_time: 0 or a recent time", n);
    if (n == 0) {
        struct timespec ts;
        checkf(clock_gettime(CLOCK_REALTIME, &ts) != 0, "no server time: clock_gettime(CLOCK_REALTIME) fails", 0);
        checkf(time(NULL) == 0, "no server time: time() is 0, as sys_time()", (long)time(NULL));
    }
    struct timespec mono;
    checkf(clock_gettime(CLOCK_MONOTONIC, &mono) == 0, "clock_gettime(CLOCK_MONOTONIC)", 0);
    n = badge_id(buf, sizeof buf);
    checkf(n == BADGE_ERR || n == 6, "badge_id: BADGE_ERR before the first hello, else 6", n);
    n = badge_owner(buf, sizeof buf);
    checkf(n == BADGE_ERR || n > 0, "badge_owner", n);
    checkf(!strcmp(key_name(KEY_BACK), "back"), "key_name", 0);
    checkf(sys_keys() == 0, "sys_keys: nothing held", (long)sys_keys());

    // --- led, mesh, wifi, ble (permissions led, mesh, wifi, ble) -----------------
    checkf(led_set(0, 255, 0, 0) == 0 && led_fill(0, 0, 0) == 0 && led_off() == 0, "led", 0);
    checkf(led_set(8, 0, 0, 0) == BADGE_ERR, "led_set(8): BADGE_ERR", 0);
    mesh_info_t mi;
    checkf(mesh_info(&mi) == 0 && mi.channel > 0, "mesh_info", mi.channel);
    mesh_peer peers[4];
    n = mesh_nearby(peers, 4);
    checkf(n >= 0, "mesh_nearby", n);
    n = wifi_status();
    checkf(n >= WIFI_OFF && n <= WIFI_FAILED, "wifi_status", n);
    checkf(ble_state() == 0, "ble_state: off", 0);

    // --- crypto, solana ---------------------------------------------------------
    uint8_t h32[32];
    static const uint8_t abc_sha[4] = {0xba, 0x78, 0x16, 0xbf};
    checkf(crypto_sha256("abc", 3, h32) == 0 && !memcmp(h32, abc_sha, 4), "crypto_sha256(\"abc\")", h32[0]);
    uint8_t zero[32] = {0};
    n = solana_encode(zero, 32, buf, sizeof buf);
    checkf(n == 32 && !strcmp(buf, "11111111111111111111111111111111"), "solana_encode of 32 zero bytes", n);
    uint8_t back[32];
    n = solana_decode(buf, back, sizeof back);
    checkf(n == 32 && !memcmp(back, zero, 32), "solana_decode", n);
    n = solana_decode("0OIl", back, sizeof back);
    checkf(n == BADGE_ERR && !strcmp(badge_error(), "not base58"), "solana_decode of bad text: \"not base58\"", n);
    badge_bytes seeds[1] = {{"seed", 4}};
    int bump = -1;
    checkf(solana_pda(seeds, 1, zero, h32, &bump) == 0 && bump >= 0 && bump <= 255, "solana_pda", bump);
    checkf(crypto_on_curve(h32) == 0, "a PDA is off the curve", 0);
    uint8_t sig[64] = {0};
    checkf(crypto_verify(zero, "m", 1, sig) == 0, "crypto_verify of junk: 0", 0);

    // --- the raw ABI ------------------------------------------------------------
    sos_global("error", 5);
    sos_push_str("boom", 4);
    n = sos_call(1, 0);
    checkf(n == 2 && sos_type(-1) == SOS_STR, "sos_call of a failing function: 2 and the error's text", n);
    sos_pop(1);
    sos_global("pcall", 5);
    sos_push_fn(raw_cb, NULL);
    sos_push_int(1);
    sos_push_int(2);
    n = sos_call(3, SOS_MULTRET);
    checkf(n == 0 && sos_top() == 2 && sos_to_int(-1) == 20, "a C function through pcall: its nargs and result", sos_to_int(-1));
    sos_pop(sos_top());
    checkf(sos_global("ui.W", 4) == SOS_INT && sos_to_int(-1) == UI_W, "sos_global(\"ui.W\")", 0);
    sos_pop(1);
    checkf(sos_global("no.such", 7) == SOS_NIL, "sos_global of an unknown name: nil", 0);
    sos_pop(1);
    checkf(sos_host_abi() == SOS_ABI, "sos_host_abi", sos_host_abi());

    // --- timers: finish() runs after both ----------------------------------------
    checkf(timer_after(20, after_fn, NULL) > 0, "timer_after", 0);
    s_every_id = timer_every(20, every_fn, NULL);
    checkf(s_every_id > 0, "timer_every", s_every_id);
    checkf(timer_after(10, after_fn, NULL) > 0, "timer_after under 20 ms: taken as 20 ms", 0);
    timer_after(300, finish, NULL);
}

void on_start(void)
{
    stage1(NULL);
}

int32_t on_key(int32_t key, int32_t repeat)
{
    (void)repeat;
    return key != KEY_BACK;
}
