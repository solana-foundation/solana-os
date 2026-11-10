// Beacon: this badge as a quest beacon (game/beacon.c, docs/MESH.md "Badge
// beacons"). Listed in the launcher only on a badge a QuestMaker paired (hello
// "beacon"); `beacon pair` on the console opens it on any badge.
//
// While it's open the badge plays its character: the beacon's HELLO goes out
// under the beacon id and badges' quest requests are relayed to the server.
// The screen shows the character and the scan code that lets an emulator visit
// (MESH.md "Scan codes"), plus whether badges nearby can hear it: the mesh
// channel follows the Wi-Fi network's. CANCEL or HOME stops it all.

#include <stdio.h>
#include <string.h>
#include "core/os.h"
#include "core/os_cloud.h"
#include "core/os_mesh.h"
#include "core/os_wifi.h"
#include "core/ui.h"
#include "game/beacon.h"
#include "game/game.h"

#define CONTENT_H   (OS_ROOT_H - UI_HINT_H)
#define QR_BOX      150         // a scan code is ~41 modules: 3 px each, like a 164 px box
#define QR_SIZE     134
#define COL_X       (10 + QR_BOX + 10)
#define COL_W       (OS_ROOT_W - COL_X - 8)

typedef enum { V_NONE = -1, V_NOT, V_CHECKING, V_PENDING, V_ASLEEP, V_ON_AIR } view_t;

static lv_obj_t *s_root, *s_view, *s_offline, *s_qr, *s_hint, *s_lines[3];
static view_t s_shown = V_NONE;
static uint32_t s_rev;
static bool s_light;
static long long s_window = -1;         // the scan code's 30 s window on screen
static int64_t s_next_lines;

// "https://host/path" -> "host/path"
static void site_text(char *out, size_t n)
{
    os_cloud_info_t *in = lv_malloc(sizeof(*in));
    out[0] = 0;
    if (in) {
        os_cloud_get_info(in);
        const char *p = strstr(in->site, "://");
        strlcpy(out, p ? p + 3 : in->site, n);
        lv_free(in);
    }
    if (!out[0]) {
        strlcpy(out, "the badge site", n);
    }
}

static lv_obj_t *label_at(const lv_font_t *font, uint32_t color, const char *text, int x, int y, int w)
{
    lv_obj_t *l = ui_label(s_view, font, color, text);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    return l;
}

// Round badge with an icon (or a spinner), then a centred title and body.
static lv_obj_t *message(const char *symbol, const char *title, const char *body)
{
    lv_obj_t *ring;
    if (symbol) {
        ring = lv_obj_create(s_view);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, 48, 48);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(ring, ui_col(COL_PANEL_HI), 0);
        lv_obj_set_style_border_width(ring, 2, 0);
        lv_obj_set_style_border_color(ring, ui_col(COL_ORANGE), 0);
        lv_obj_t *icon = ui_label(ring, &lv_font_montserrat_20, COL_ORANGE, symbol);
        lv_obj_center(icon);
    } else {
        ring = lv_spinner_create(s_view);
        lv_obj_set_size(ring, 48, 48);
        lv_obj_set_style_arc_width(ring, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(ring, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(ring, ui_col(COL_PANEL_HI), LV_PART_MAIN);
        lv_obj_set_style_arc_color(ring, ui_col(COL_ORANGE), LV_PART_INDICATOR);
    }
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_t *t = ui_label(s_view, &sol_font_20, COL_TEXT, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_t *b = ui_label(s_view, &sol_font_14, COL_DIM, body);
    lv_obj_set_width(b, OS_ROOT_W - 40);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 104);
    return b;
}

static lv_obj_t *footnote(const char *text)
{
    lv_obj_t *l = ui_label(s_view, &sol_mono_12, COL_DIM, text);
    lv_obj_set_width(l, OS_ROOT_W - 20);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, CONTENT_H - 22);
    return l;
}

// The status lines next to the code: can badges hear it, visits, ids.
static void update_lines(void)
{
    const beacon_info_t *b = beacon_info();
    if (!s_lines[0]) {
        return;
    }
    os_wifi_info_t w;
    os_wifi_get_info(&w);
    int mesh = b->mesh;
    char line[64];
    // ~19 characters a line.
    if (w.state != OS_WIFI_CONNECTED) {
        lv_label_set_text(s_lines[0], "No Wi-Fi: silent");
        lv_obj_set_style_text_color(s_lines[0], ui_col(COL_ORANGE), 0);
    } else if (mesh && w.channel != mesh) {
        // Its radio follows the network: badges on the mesh channel can't hear it.
        // Another band says which band the network has to be on.
        if (strcmp(os_mesh_band(w.channel), os_mesh_band(mesh))) {
            snprintf(line, sizeof(line), "Mesh %d needs %sGHz", mesh, os_mesh_band(mesh));
        } else {
            snprintf(line, sizeof(line), "Wi-Fi %d, mesh %d", w.channel, mesh);
        }
        lv_label_set_text(s_lines[0], line);
        lv_obj_set_style_text_color(s_lines[0], ui_col(COL_ORANGE), 0);
    } else {
        snprintf(line, sizeof(line), "On air ch %d %sGHz", w.channel, os_mesh_band(w.channel));
        lv_label_set_text(s_lines[0], line);
        lv_obj_set_style_text_color(s_lines[0], ui_col(COL_GREEN), 0);
    }
    int ago = b->visits ? (int)((game_now_ms() - b->last_visit_ms) / 60000) : 0;
    if (!b->visits) {
        snprintf(line, sizeof(line), "No visits yet");
    } else if (ago < 1) {
        snprintf(line, sizeof(line), "%d visit%s, just now", b->visits, b->visits == 1 ? "" : "s");
    } else {
        snprintf(line, sizeof(line), "%d visit%s, %dm ago", b->visits, b->visits == 1 ? "" : "s", ago);
    }
    lv_label_set_text(s_lines[1], line);
    snprintf(line, sizeof(line), "Beacon %s", b->id);
    lv_label_set_text(s_lines[2], line);
}

static void update_qr(void)
{
    if (!s_qr) {
        return;
    }
    char url[160];
    long long w = os_cloud_time_ms() / 1000 / 30;
    if (w == s_window || !beacon_scan_url(url, sizeof(url))) {
        return;
    }
    s_window = w;
    lv_qrcode_update(s_qr, url, strlen(url));
}

static void build_on_air(const beacon_info_t *b)
{
    // The code follows the theme, as My Badge's does.
    bool light = ui_light();
    lv_color_t fg = light ? lv_color_black() : lv_color_white();
    lv_color_t bg = light ? lv_color_white() : lv_color_black();
    lv_obj_t *box = lv_obj_create(s_view);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, QR_BOX, QR_BOX);
    lv_obj_set_pos(box, 10, (CONTENT_H - QR_BOX) / 2 - 6);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, bg, 0);
    lv_obj_set_style_radius(box, 8, 0);
    if (!light) {
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, ui_col(COL_BORDER), 0);
    }
    lv_obj_set_scrollable(box, false);
    s_qr = lv_qrcode_create(box);
    lv_qrcode_set_size(s_qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_qr, fg);
    lv_qrcode_set_light_color(s_qr, bg);
    lv_obj_center(s_qr);
    s_window = -1;
    update_qr();
    lv_obj_t *cap = ui_label(s_view, &sol_mono_12, COL_DIM, "Scan in the emulator");
    lv_obj_set_width(cap, QR_BOX + 8);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(cap, 6, (CONTENT_H - QR_BOX) / 2 - 6 + QR_BOX + 3);

    int y = 10;
    label_at(&sol_mono_12, COL_DIM, "QUEST BEACON", COL_X, y, 0);
    lv_obj_t *n = label_at(&sol_font_20, COL_TEXT, b->name, COL_X, y + 18, COL_W);
    lv_obj_set_style_text_color(n, lv_color_hex(game_ink(b->color)), 0);
    lv_obj_t *t = label_at(&sol_font_14, COL_DIM, b->title, COL_X, 0, COL_W);
    lv_obj_align_to(t, n, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    for (int i = 0; i < 3; i++) {
        s_lines[i] = label_at(&sol_mono_12, COL_DIM, "", COL_X, CONTENT_H - 64 + i * 18, COL_W);
        lv_label_set_long_mode(s_lines[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_lines[i], lv_font_get_line_height(&sol_mono_12));
    }
    update_lines();
}

static void build(view_t v, const beacon_info_t *b)
{
    if (s_view) {
        lv_obj_delete(s_view);
    }
    s_qr = NULL;
    memset(s_lines, 0, sizeof(s_lines));
    s_view = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_view);
    lv_obj_set_size(s_view, OS_ROOT_W, CONTENT_H);
    lv_obj_set_scrollable(s_view, false);
    lv_obj_move_to_index(s_view, 0);    // under the offline panel
    char site[96], text[320], foot[48];
    snprintf(foot, sizeof(foot), "Beacon %s", b->id);
    switch (v) {
    case V_NOT:
        site_text(site, sizeof(site));
        snprintf(text, sizeof(text), "A QuestMaker turns a badge into a quest beacon at %s/admin/beacons/badge, "
                 "with the badge on USB. Nothing on it is erased.", site);
        message(LV_SYMBOL_BELL, "Not a quest beacon", text);
        break;
    case V_CHECKING:
        message(NULL, "Checking in", b->error[0] ? b->error : "Asking the server which character to play...");
        footnote(foot);
        break;
    case V_PENDING: {
        site_text(site, sizeof(site));
        lv_obj_t *head = label_at(&sol_mono_12, COL_DIM, "PAIRING CODE", 0, 22, OS_ROOT_W);
        lv_obj_t *code = label_at(&sol_font_32, COL_ORANGE, b->code, 0, 42, OS_ROOT_W);
        lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_letter_space(code, 6, 0);
        snprintf(text, sizeof(text), "Waiting for a QuestMaker to adopt it in %s/admin/beacons and give it a character. "
                 "This badge keeps everything of its owner's.", site);
        lv_obj_t *body = label_at(&sol_font_14, COL_DIM, text, 20, 96, OS_ROOT_W - 40);
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
        footnote(foot);
        break;
    }
    case V_ASLEEP:
        site_text(site, sizeof(site));
        snprintf(text, sizeof(text), "Pick a character for beacon %s in %s/admin/beacons. It starts playing it within a minute.",
                 b->id, site);
        message(LV_SYMBOL_BELL, "No character yet", text);
        footnote(foot);
        break;
    case V_ON_AIR:
        build_on_air(b);
        break;
    default:
        break;
    }
    lv_label_set_text(s_hint, v == V_NOT ? "CANCEL back" : "CANCEL stop   SELECT check in");
    s_shown = v;
    s_rev = b->rev;
    s_light = ui_light();
}

static view_t view_of(const beacon_info_t *b)
{
    switch (b->phase) {
    case BEACON_CHECKING: return V_CHECKING;
    case BEACON_PENDING:  return V_PENDING;
    case BEACON_ASLEEP:   return V_ASLEEP;
    case BEACON_ON_AIR:   return V_ON_AIR;
    default:              return V_NOT;
    }
}

static void on_tick(void)
{
    const beacon_info_t *b = beacon_info();
    view_t v = view_of(b);
    if (v != s_shown || b->rev != s_rev || ui_light() != s_light) {
        build(v, b);
    }
    // Not a beacon: nothing needs the network, so no offline panel over the explanation.
    ui_net_t net = v == V_NOT ? UI_NET_OK : ui_offline_update(s_offline);
    lv_obj_set_hidden(s_offline, v == V_NOT || net == UI_NET_OK);
    update_qr();
    int64_t now = game_now_ms();
    if (now >= s_next_lines) {
        s_next_lines = now + 1000;
        update_lines();
    }
}

static void on_enter(lv_obj_t *root)
{
    s_root = root;
    s_view = s_qr = NULL;
    memset(s_lines, 0, sizeof(s_lines));
    s_shown = V_NONE;
    s_next_lines = 0;
    beacon_serve(true);
    s_offline = ui_offline_create(root, 0, CONTENT_H, false);
    s_hint = ui_hint(root, "");
    on_tick();
}

static void on_leave(void)
{
    beacon_serve(false);
    s_view = s_qr = s_offline = s_hint = NULL;  // deleted with the root
    memset(s_lines, 0, sizeof(s_lines));
}

static bool on_key(os_key_t k, bool repeat)
{
    if (ui_offline_key(s_offline, k)) {
        return true;
    }
    if (k == OS_KEY_SELECT && !repeat) {
        if (beacon_info()->id[0]) {
            beacon_sync();
            os_toast("Checking in with the server...");
        }
        return true;
    }
    return false;                       // CANCEL: back to the launcher, which stops the beacon
}

const os_app_t app_beacon = {
    .id = "beacon",
    .name = "Beacon",
    .symbol = LV_SYMBOL_BELL,
    .accent = COL_ORANGE,
    .on_enter = on_enter,
    .on_exit = on_leave,
    .on_key = on_key,
    .on_tick = on_tick,
    .visible = beacon_listed,
};
