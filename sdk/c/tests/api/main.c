// A compile-only probe: calls every function of badge.h once, so the whole
// header builds warning-free (also as C++ and with -pedantic) and the module
// imports nothing beyond toolchain/imports. Not an app to run.
#include "badge.h"

static void on_result(void *user, badge_val r, const char *err)
{
    item_t it;
    quest_t q;
    char buf[64];
    (void)user;
    if (err) {
        badge_print(err);
        return;
    }
    item_info(r, &it);
    quest_info(r, &q);
    json_encode(r, buf, sizeof buf);
    badge_field_json(r, "data", buf, sizeof buf);
}
static void on_select(void *user, int i) { (void)user; (void)i; }
static void on_yes(void *user, int yes) { (void)user; (void)yes; }
static void on_text(void *user, const char *t) { (void)user; (void)t; }
static void on_timer(void *user) { (void)user; }
static void on_name(void *user, const char *n) { (void)user; (void)n; }
static void on_line(void *user, const char *l, uint32_t n) { (void)user; (void)l; (void)n; }
static void on_scan(void *user, badge_val chs, int n)
{
    mesh_channel_t c;
    mesh_peer p;
    (void)user;
    for (int i = 0; i < n; i++) {
        mesh_scan_channel(chs, i, &c);
        for (int j = 0; j < c.npeers; j++) {
            mesh_scan_peer(chs, i, j, &p);
        }
    }
}
static void on_meet_done(void *user, const char *peer, badge_val data, const char *err)
{
    (void)user; (void)peer; (void)data; (void)err;
}
static void on_dev(void *user, const ble_dev *d) { (void)user; (void)d; }
static void on_ble(void *user, const ble_event *e) { (void)user; (void)e; }
static void on_read(void *user, const uint8_t *d, uint32_t n, const char *err) { (void)user; (void)d; (void)n; (void)err; }
static void on_done(void *user, const char *err) { (void)user; (void)err; }
static void on_lamps(void *user, const lamps_result *r, const char *err) { (void)user; (void)r; (void)err; }
static void on_profile(void *user, const profile_t *p, const char *err) { (void)user; (void)p; (void)err; }
static void on_http(void *user, const http_response *r, const char *err)
{
    char body[256];
    (void)user;
    if (r) {
        http_body(r, body, sizeof body);
    }
    (void)err;
}
static void on_ws(void *user, const ws_event *e) { (void)user; (void)e; }

static uint16_t frame[16 * 16];

// Each call must leave the stack as it found it (badge.h's rule); the mock
// host (tests/mock-host.ts) runs this and prints any that don't.
#define T(e)                                                                    \
    do {                                                                        \
        int32_t before_ = sos_top();                                            \
        (void)(e);                                                              \
        if (sos_top() != before_) {                                             \
            sos_log("stack changed by: " #e, sizeof("stack changed by: " #e) - 1); \
        }                                                                       \
    } while (0)

// profile_items / profile_pages: the list under its field, each entry, and the cursor.
static void on_inv(void *user, badge_val r, const char *err)
{
    char next[160];
    (void)user;
    if (err) {
        return;
    }
    int pages = badge_field_type(r, "pages") == SOS_TABLE;
    badge_val list = badge_push_field(r, pages ? "pages" : "items");
    for (uint32_t i = 0; i < badge_len(list); i++) {
        badge_val e = badge_push_index(list, (int32_t)i);
        if (pages) {
            profile_page_t pg;
            profile_page_info(e, &pg);
        } else {
            profile_item_t it;
            profile_item_info(e, &it);
        }
        badge_pop(1);
    }
    badge_pop(1);
    if (badge_field_str(r, "next", next, sizeof next) > 0) {
        T(profile_items(next, on_inv, NULL));
    }
    (void)badge_field_int(r, "total", 0);
}

void on_start(void)
{
    char buf[64];
    int32_t a, b;
    int w, h, bump;
    uint8_t h32[32], sig[64] = {0}, pda[32];
    const char *items[] = {"one", "two"};
    ui_row rows[] = {{"title", "sub", "right"}};
    ui_pal pal[] = {{'r', 0xFF0000}, {'x', COL_NONE}};
    const char *art[] = {"..r..", ".rxr."};
    mesh_peer peers[4];
    mesh_info_t mi;
    mesh_scan_opts so = {0, 0, "2.4", 0};
    ble_adv adv = {"Dice", "ffe0", "\1\2", 2, 1, 0};
    ble_scan_opts bso = {"ffe0", 0, 1, 10, 0};
    ble_dev dev;
    ble_char chars[] = {{"ffe1", 1, 1, 1, "hi", 2}};
    ble_service svc = {"ffe0", chars, 1};
    http_header hd[] = {{"Accept", "text/plain"}};
    http_req rq = {"PUT", "https://api.example.com/x", hd, 1, "{\"a\":1}", 0, 1};
    const char *protos[] = {"chat.v1"};
    badge_bytes seeds[] = {{"multisig", 8}, {h32, 32}};
    ui_label_opts lo = {20, COL_GREEN, 0, UI_W, UI_CENTER};
    ui_rect_opts ro = {COL_PANEL, 4, COL_BORDER};
    ui_anim_opts ao = {2, UI_LOOP_FOREVER};

    sos_push_cstr(key_name(KEY_UP));
    badge_pop(1);
    T(badge_print(badge_error()));
    badge_new_table(); // a table to read: these leave one value, popped below
    badge_set_int("k", 1);
    badge_set_num("n", 1.5f);
    badge_set_bool("b", 1);
    badge_set_str("s", "x");
    badge_set_bytes("y", "\0\1", 2);
    badge_val t = badge_top();
    T(badge_type(t)); T(badge_int(t)); T(badge_num(t)); T(badge_bool(t)); T(badge_str(t, buf, sizeof buf)); T(badge_len(t));
    badge_push_field(t, "k");
    badge_push_index(t, 0);
    badge_pop(2);
    T(badge_field_type(t, "k")); T(badge_field_int(t, "k", 0)); T(badge_field_num(t, "n", 0)); T(badge_field_bool(t, "b"));
    T(badge_field_str(t, "s", buf, sizeof buf)); T(json_encode(t, buf, sizeof buf)); T(badge_field_json(t, "k", buf, sizeof buf));
    T(mesh_send_top("*") + (badge_new_table(), 0)); // mesh_send_top pops the table; a new one takes its place
    badge_pop(1);
    if (json_push("{\"a\":[1,2]}") == 0) {
        badge_top();
        badge_pop(1);
    }

    T(ui_bg(COL_BG));
    ui_obj l; T(l = ui_label(0, 0, "hi", &lo));
    T(ui_label_text(l, "there")); T(ui_label_color(l, 0x9945FF)); T(ui_obj_pos(l, 1, 2)); T(ui_obj_show(l, 1));
    ui_obj r; T(r = ui_rect(0, 0, 10, 10, &ro));
    T(ui_rect_size(r, 5, 5)); T(ui_rect_color(r, COL_BLACK));
    ui_obj bar; T(bar = ui_bar(0, 0, 100, 8, 50)); T(ui_bar_set(bar, 60));
    ui_obj g; T(g = ui_gradient(0, 0, 10, 10));
    ui_obj li; T(li = ui_list(0, 0, UI_W, 100, items, 2, on_select, NULL));
    T(ui_list_items(li, items, 2)); T(ui_list_set_rows(li, rows, 1)); T(ui_list_select(li, ui_list_selected(li)));
    ui_obj li2; T(li2 = ui_list_rows(0, 0, UI_W, 100, rows, 1, NULL, NULL));
    ui_obj q; T(q = ui_qr(0, 0, 100, "solana:x")); T(ui_qr_text(q, "y"));
    T(ui_hint("SELECT go")); T(ui_toast("hi")); T(ui_confirm("Sure?", on_yes, NULL)); T(ui_input("Name", 16, on_text, NULL));
    ui_img img; T(img = ui_load("a.png")); ui_img img2; T(img2 = ui_load_rows(art, 2, pal, 2)); T(ui_img_size(img, &w, &h));
    ui_obj im; T(im = ui_image(0, 0, "a.png", 2)); ui_obj im2; T(im2 = ui_image_of(0, 0, img, 1));
    ui_obj im3; T(im3 = ui_image_rows(0, 0, art, 2, pal, 2, 8));
    T(ui_image_set(im, "b.png")); T(ui_image_set_img(im2, img2)); T(ui_image_scale(im3, 3));
    ui_obj c; T(c = ui_canvas(0, 0, 16, 16, 4, COL_BG));
    T(ui_canvas_clear(c, COL_NONE)); T(ui_canvas_pixel(c, 1, 1, COL_GREEN)); T(ui_canvas_rect(c, 0, 0, 4, 4, 0xFF754A));
    T(ui_canvas_line(c, 0, 0, 15, 15, COL_AQUA)); T(ui_canvas_image(c, img, 0, 0)); T(ui_canvas_image_path(c, "a.png", 0, 0));
    T(ui_canvas_get(c, 1, 1));
    frame[0] = RGB565(255, 0, 0); frame[1] = RGB565_HEX(0x14F195);
    T(ui_canvas_blit(c, frame, 0, 0, 16, 16));
    ui_obj an; T(an = ui_anim(0, 0, "d.gif", &ao)); T(ui_anim_stop(an)); T(ui_anim_play(an)); T(ui_anim_playing(an));
    T(ui_obj_remove(&l)); T(ui_obj_remove(&r)); T(ui_obj_remove(&bar)); T(ui_obj_remove(&g)); T(ui_obj_remove(&li));
    T(ui_obj_remove(&li2)); T(ui_obj_remove(&q)); T(ui_obj_remove(&im)); T(ui_obj_remove(&im2)); T(ui_obj_remove(&im3));
    T(ui_obj_remove(&c)); T(ui_obj_remove(&an)); T(badge_obj_free(&img)); T(badge_obj_free(&img2));
    T(ui_clear());

    T(led_set(0, 255, 0, 0)); T(led_hex(1, 0x9945FF)); T(led_fill(0, 0, 0)); T(led_off());
    int32_t id; T(id = timer_after(100, on_timer, NULL)); T(timer_cancel(id)); T(timer_cancel(timer_every(50, on_timer, NULL)));

    T(sys_millis()); T(sys_keys()); T(sys_time()); T(sys_battery()); T(sys_memory(&a, &b));
    T(badge_id(buf, sizeof buf)); T(badge_owner(buf, sizeof buf)); T(badge_online());

    storage_push("k");
    badge_pop(1);
    T(storage_get_int("k", 0)); T(storage_get_str("k", buf, sizeof buf)); T(storage_get_json("k", buf, sizeof buf));
    T(storage_set_int("k", 1)); T(storage_set_bool("k", 1)); T(storage_set_str("k", "v")); T(storage_set_json("k", "[1]"));
    T(storage_remove("k"));

    T(fs_exists("a")); T(fs_size("a")); T(fs_read("a", buf, sizeof buf)); T(fs_read_at("a", 0, 8, buf, sizeof buf));
    T(fs_list(NULL, on_name, NULL)); T(fs_lines("a", on_line, NULL));
    if (fs_json_push("a.json") == 0) {
        badge_pop(1);
    }

    T(wifi_status()); T(wifi_ssid(buf, sizeof buf)); T(wifi_on()); T(wifi_off());

    T(mesh_nearby(peers, 4)); T(mesh_send(NULL, "{\"ping\":1}")); T(mesh_bridge()); T(mesh_pause()); T(mesh_resume());
    T(mesh_info(&mi)); T(mesh_channel(6)); T(mesh_scan(&so, on_scan, NULL));
    T(mesh_meet("7K2Q9M", "{\"tag\":1}", on_meet_done, NULL)); T(mesh_meets(0, on_result, NULL));

    T(ble_on()); T(ble_state()); T(ble_address(buf, sizeof buf)); T(ble_advertise(&adv)); T(ble_advertise(NULL));
    T(ble_scan(&bso, on_dev, NULL)); T(ble_scan_stop());
    memset(&dev, 0, sizeof dev);
    memcpy(dev.addr, "7c:df:a1:00:12:34", 18);
    ble_link lk; T(lk = ble_connect(&dev, on_ble, NULL));
    T(ble_link_read(lk, "ffe0", "ffe1", on_read, NULL)); T(ble_link_write(lk, "ffe0", "ffe1", "x", 1));
    T(ble_link_write_cb(lk, "ffe0", "ffe1", "x", 1, on_done, NULL)); T(ble_link_subscribe(lk, "ffe0", "ffe1", 1));
    T(ble_link_state(lk)); T(ble_link_close(lk)); T(badge_obj_free(&lk));
    T(ble_serve(&svc, on_ble, NULL)); T(ble_set("ffe1", "a", 1)); T(ble_notify("ffe1", "b", 1)); T(ble_drop()); T(ble_off());

    T(lamps_balance(on_lamps, NULL)); T(lamps_spend(50, "sku", on_lamps, NULL));
    T(lamps_transfer("7K2Q9M", 5, "hi", on_lamps, NULL)); T(lamps_deposit("ESC-7K2Q9M3X", 5, "m", on_lamps, NULL));
    T(lamps_award(1, "taps", on_lamps, NULL));
    T(items_list(on_result, NULL)); T(items_all(on_result, NULL)); T(items_grant("k", "n", "{}", on_result, NULL));
    T(items_update("id", "{}", on_result, NULL)); T(items_remove("id", on_result, NULL));
    T(profile_me(on_profile, NULL)); T(profile_get("7K2Q9M", on_profile, NULL));
    T(profile_items(NULL, on_inv, NULL)); T(profile_pages("", on_inv, NULL));
    T(quest_report("done", "{}", on_result, NULL)); T(quest_active(on_result, NULL));

    T(http_get("https://api.example.com/", on_http, NULL));
    T(http_get_headers("https://api.example.com/", hd, 1, on_http, NULL));
    T(http_post("https://api.example.com/", "text", on_http, NULL));
    T(http_post_json("https://api.example.com/", "{\"score\":1}", on_http, NULL));
    T(http_request(&rq, on_http, NULL));
    ws_socket ws; T(ws = ws_open("wss://chat.example.com/", protos, 1, on_ws, NULL));
    T(ws_send(ws, "hi")); T(ws_send_bytes(ws, "\0", 1)); T(ws_send_json(ws, "{}")); T(ws_state(ws)); T(ws_close(ws));
    T(rpc_call("getSlot", NULL, NULL, on_result, NULL)); T(rpc_call("getBalance", "[\"x\"]", "devnet", on_result, NULL));

    T(crypto_sha256("abc", 3, h32)); T(crypto_on_curve(h32)); T(crypto_verify(h32, "m", 1, sig));
    T(solana_encode(h32, 32, buf, sizeof buf)); T(solana_decode("11111111111111111111111111111111", h32, sizeof h32));
    T(solana_pda(seeds, 2, h32, pda, &bump));
    T(sys_exit());
}

int32_t on_key(int32_t key, int32_t repeat)
{
    (void)repeat;
    return key != KEY_BACK && KEY_HELD(sys_keys(), key);
}

int32_t on_meet(void)
{
    return mesh_meet_accept("{\"ok\":true}");
}
