// Mailbox (Yellow, 2.3.0; docs/HHGTS.md "Mailbox"): the owner's notifications
// from Solana, apps' servers and other badges' apps. The list is what is on
// the card (game/mailbox.h), so it reads the same offline; a line on top says
// where the badge stands ("synced 5 min ago", "Wi-Fi is off").
//
// List: SELECT reads, RIGHT collects now, LEFT twice deletes everything read;
// the last row lists the muted apps (from the server), where SELECT unmutes.
// Reader: SELECT opens the app it came from (its on_mail gets it), LEFT twice
// deletes, RIGHT twice mutes the app, DOWN twice reports another badge's mail
// (and blocks its sender). Nothing a message says is acted on: it is text.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/ui.h"
#include "game/game.h"
#include "game/mailbox.h"
#include "lua/lua_host.h"
#include "lua/lua_pkg.h"

#define REFRESH_MS  1000
#define ARM_MS      3000            // a second press within this confirms
#define STATUS_H    18
#define MUTED_MAX   16

typedef enum { VIEW_LIST, VIEW_READ, VIEW_MUTED } view_t;

static lv_obj_t *s_root, *s_status, *s_hint, *s_read;
static ui_list_t *s_list;
static view_t s_view;
static int s_since;
static void *s_tok;                 // os_api ctx of this visit
static mailbox_item_t s_row;        // scratch for the row callback
static mailbox_mail_t *s_mail;      // the message being read (PSRAM-sized struct: malloc'd)
static os_key_t s_armed;            // the key pressed once, OS_KEY_NONE-like when 0xff
static int64_t s_armed_at;
static int s_count;                 // rows of mail in the list (the muted row comes after)
static struct {
    char id[MAILBOX_APP_MAX + 1];
    char name[MAILBOX_LABEL_MAX + 1];
} s_muted[MUTED_MAX];
static int s_nmuted;
static bool s_muted_loading;

#define NOT_ARMED   ((os_key_t)0xff)

static bool from_badge(const mailbox_item_t *h)
{
    return h->from[0] && strcmp(h->from, "server") && strcmp(h->from, "solana");
}

// "Solana", "Chess", "Chess: @alice"
static void sender(const mailbox_item_t *h, char *out, size_t n)
{
    if (from_badge(h)) {
        strlcpy(out, h->an, n);
        strlcat(out, ": ", n);
        strlcat(out, h->by[0] ? h->by : h->from, n);
    } else {
        strlcpy(out, h->an[0] ? h->an : "Solana", n);
    }
}

static void ago(uint32_t at, char *out, size_t n)
{
    int64_t now = os_cloud_time_ms();
    if (!now || !at) {
        out[0] = 0;
        return;
    }
    ui_fmt_ago(out, n, now - (int64_t)at * 1000);
}

static bool installed(const char *app)
{
    lua_pkg_t *p = malloc(sizeof(*p));
    bool ok = p && app[0] && lua_pkg_load(app, p);
    free(p);
    return ok;
}

static void arm(os_key_t k, const char *hint)
{
    s_armed = k;
    s_armed_at = game_now_ms();
    lv_label_set_text(s_hint, hint);
}

static bool armed(os_key_t k)
{
    bool yes = s_armed == k && game_now_ms() - s_armed_at < ARM_MS;
    s_armed = NOT_ARMED;
    return yes;
}

// ---------------------------------------------------------------------------
// Status line and list

static void status_update(void)
{
    char line[64], synced[40];
    ui_net_t net = ui_net_state(true);
    game_synced(mailbox_synced_at(), synced, sizeof(synced));
    uint32_t col = COL_DIM;
    if (net == UI_NET_NOT_LINKED) {
        strlcpy(line, "Not linked yet: claim this badge first", sizeof(line));
    } else if (!mailbox_running()) {
        strlcpy(line, "Mailbox is off: turn it on in Services", sizeof(line));
        col = COL_WARN;
    } else if (mailbox_syncing()) {
        strlcpy(line, "Collecting mail...", sizeof(line));
        col = COL_YELLOW;
    } else if (net == UI_NET_OK) {
        strlcpy(line, mailbox_synced_at() ? synced : "Online", sizeof(line));
    } else {
        const char *why = net == UI_NET_WIFI_OFF ? "Wi-Fi is off" : net == UI_NET_NO_NETWORK ? "No network saved"
                        : net == UI_NET_PAUSED ? "Wi-Fi is paused" : "Offline";
        if (mailbox_synced_at()) {
            snprintf(line, sizeof(line), "%s - %s", why, synced);
        } else {
            strlcpy(line, why, sizeof(line));
        }
    }
    lv_label_set_text(s_status, line);
    lv_obj_set_style_text_color(s_status, ui_col(col), 0);
}

static void muted_row(int i, ui_row_t *r, void *ctx);

static void row(int i, ui_row_t *r, void *ctx)
{
    if (s_view == VIEW_MUTED) {
        muted_row(i, r, ctx);
        return;
    }
    if (i >= s_count) {
        snprintf(r->title, sizeof(r->title), LV_SYMBOL_MUTE "  Muted apps");
        strlcpy(r->sub, "Apps whose mail you don't get", sizeof(r->sub));
        return;
    }
    if (!mailbox_item(i, &s_row)) {
        return;
    }
    char who[56];
    sender(&s_row, who, sizeof(who));
    strlcpy(r->title, s_row.read ? "" : LV_SYMBOL_BULLET " ", sizeof(r->title));
    strlcat(r->title, who, sizeof(r->title));
    r->title_color = s_row.read ? 0 : COL_YELLOW;
    strlcpy(r->sub, s_row.title, sizeof(r->sub));
    ago(s_row.at, r->right, sizeof(r->right));
}

static void list_hint(void)
{
    lv_label_set_text(s_hint, s_count ? "SELECT read   " LV_SYMBOL_RIGHT " collect   " LV_SYMBOL_LEFT " delete read"
                                      : LV_SYMBOL_RIGHT " collect   CANCEL exit");
}

static void list_reload(void)
{
    int sel = ui_list_selected(s_list);
    s_count = mailbox_count();
    ui_list_set_count(s_list, s_count + (game_claimed() ? 1 : 0));
    if (sel > 0) {
        int n = s_count + (game_claimed() ? 1 : 0);
        ui_list_select(s_list, sel < n ? sel : n - 1);
    }
}

// ---------------------------------------------------------------------------
// Muted apps (from the server)

static void muted_parse(const char *body)
{
    s_nmuted = 0;
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "muted")) {
        if (s_nmuted >= MUTED_MAX) {
            break;
        }
        game_ascii(s_muted[s_nmuted].id, sizeof(s_muted[0].id), game_jstr(it, "id"));
        game_ascii(s_muted[s_nmuted].name, sizeof(s_muted[0].name), game_jstr(it, "name"));
        if (s_muted[s_nmuted].id[0]) {
            s_nmuted++;
        }
    }
    cJSON_Delete(j);
}

static void muted_row(int i, ui_row_t *r, void *ctx)
{
    if (i < s_nmuted) {
        snprintf(r->title, sizeof(r->title), LV_SYMBOL_MUTE "  %s", s_muted[i].name);
        strlcpy(r->sub, "SELECT to get its mail again", sizeof(r->sub));
    }
}

static void muted_cb(int status, const char *body, void *ctx)
{
    s_muted_loading = false;
    if (s_view != VIEW_MUTED) {
        return;
    }
    if (status == 200) {
        muted_parse(body);
        ui_list_set_empty(s_list, "No muted apps");
    } else {
        ui_list_set_empty(s_list, status ? "The server said no. Try again later." : "Wi-Fi is off");
    }
    ui_list_set_count(s_list, s_nmuted);
}

static void show_muted(void)
{
    s_view = VIEW_MUTED;
    s_nmuted = 0;
    ui_list_set_empty(s_list, "Loading...");
    ui_list_set_count(s_list, 0);
    lv_label_set_text(s_hint, "SELECT unmute   CANCEL back");
    s_muted_loading = os_api_call("GET", "/api/device/mail/mutes", NULL, 0, muted_cb, s_tok) == ESP_OK;
    if (!s_muted_loading) {
        ui_list_set_empty(s_list, "Wi-Fi is off");
    }
}

static void mute_cb(int status, const char *body, void *ctx)
{
    if (status != 200) {
        os_toast(status ? "The server said no" : "Offline: try again on Wi-Fi");
        return;
    }
    if (s_view == VIEW_MUTED) {
        muted_parse(body);
        ui_list_set_count(s_list, s_nmuted);
    }
}

static void mute(const char *app, bool muted)
{
    char req[96];
    snprintf(req, sizeof(req), "{\"app\":\"%s\",\"muted\":%s}", app, muted ? "true" : "false");
    if (os_api_call("POST", "/api/device/mail/mute", req, 0, mute_cb, s_tok) != ESP_OK) {
        os_toast("Offline: try again on Wi-Fi");
        return;
    }
}

// ---------------------------------------------------------------------------
// Reader

static void read_hint(void)
{
    const mailbox_item_t *h = &s_mail->h;
    char hint[96] = "";
    if (h->app[0] && installed(h->app)) {
        strlcat(hint, "SELECT open   ", sizeof(hint));
    }
    strlcat(hint, LV_SYMBOL_LEFT " delete", sizeof(hint));
    if (h->app[0] && strcmp(h->from, "solana")) {
        strlcat(hint, "   " LV_SYMBOL_RIGHT " mute", sizeof(hint));
    }
    if (from_badge(h)) {
        strlcat(hint, "   " LV_SYMBOL_DOWN " report", sizeof(hint));
    }
    lv_label_set_text(s_hint, hint);
}

static void show_read(uint32_t id)
{
    if (!s_mail || !mailbox_get(id, s_mail)) {
        return;
    }
    mailbox_mark_read(id);
    s_view = VIEW_READ;
    ui_list_show(s_list, false);
    lv_obj_set_hidden(s_status, true);
    s_read = ui_panel(s_root, 0, 0, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_style_radius(s_read, 0, 0);
    lv_obj_set_style_border_width(s_read, 0, 0);
    lv_obj_set_style_pad_all(s_read, 10, 0);
    lv_obj_set_flex_flow(s_read, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_read, 4, 0);

    char who[56], when[24], line[88];
    sender(&s_mail->h, who, sizeof(who));
    ago(s_mail->h.at, when, sizeof(when));
    snprintf(line, sizeof(line), "%s%s%s", who, when[0] ? "  -  " : "", when);
    ui_label(s_read, &sol_mono_12, COL_YELLOW, line);
    lv_obj_t *t = ui_label(s_read, &sol_font_20, COL_TEXT, s_mail->h.title);
    lv_obj_set_width(t, lv_pct(100));
    if (s_mail->text[0]) {
        lv_obj_t *b = ui_label(s_read, &sol_font_14, COL_TEXT, s_mail->text);
        lv_obj_set_width(b, lv_pct(100));
    }
    if (from_badge(&s_mail->h)) {
        lv_obj_t *w = ui_label(s_read, &sol_mono_12, COL_DIM, "Written by another attendee, not by Solana.");
        lv_obj_set_width(w, lv_pct(100));
    }
    s_armed = NOT_ARMED;
    read_hint();
}

static void close_read(void)
{
    if (s_read) {
        lv_obj_delete(s_read);
        s_read = NULL;
    }
    lv_obj_set_hidden(s_status, false);
    s_view = VIEW_LIST;
    ui_list_show(s_list, true);
    list_reload();
    list_hint();
}

static void report_cb(int status, const char *body, void *ctx)
{
    os_toast(status == 200 ? "Reported: you won't get mail from them" : status ? "The server said no" : "Offline: try again on Wi-Fi");
}

static void read_key(os_key_t k, bool repeat)
{
    const mailbox_item_t *h = &s_mail->h;
    char req[64];
    switch (k) {
    case OS_KEY_SELECT:
        if (h->app[0] && installed(h->app)) {
            lua_host_open_mail(h->app, h->id);
        }
        break;
    case OS_KEY_LEFT:
        if (repeat) {
            break;
        }
        if (!armed(k)) {
            arm(k, "Press " LV_SYMBOL_LEFT " again to delete");
            break;
        }
        mailbox_delete(h->id);
        close_read();
        break;
    case OS_KEY_RIGHT:
        if (repeat || !h->app[0] || !strcmp(h->from, "solana")) {
            break;
        }
        if (!armed(k)) {
            arm(k, "Press " LV_SYMBOL_RIGHT " again to mute this app");
            break;
        }
        mute(h->app, true);
        os_toast("Muted %s: unmute it in Mailbox > Muted apps", h->an);
        read_hint();
        break;
    case OS_KEY_DOWN:
        if (repeat || !from_badge(h)) {
            break;
        }
        if (!armed(k)) {
            arm(k, "Report and block them? Press " LV_SYMBOL_DOWN " again");
            break;
        }
        snprintf(req, sizeof(req), "/api/device/mail/%" PRIu32 "/report", h->id);
        if (os_api_call("POST", req, "{}", 0, report_cb, s_tok) != ESP_OK) {
            os_toast("Offline: try again on Wi-Fi");
        }
        read_hint();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------

static void show_list(void)
{
    s_view = VIEW_LIST;
    ui_list_set_empty(s_list, "Nothing here yet");
    list_reload();
    list_hint();
}

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_read = NULL;
    s_list = NULL;
    s_since = 0;
    s_armed = NOT_ARMED;
    s_tok = os_api_token();
    s_mail = malloc(sizeof(*s_mail));
    s_status = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_obj_set_pos(s_status, 8, 2);
    s_hint = ui_hint(root, "");
    s_list = ui_list_create(root, STATUS_H, OS_ROOT_H - UI_HINT_H - STATUS_H, row, NULL);
    show_list();
    status_update();
}

static void leave(void)
{
    os_api_cancel(s_tok);
    free(s_mail);
    s_mail = NULL;
    s_read = NULL;
    s_list = NULL;
}

static void tick(void)
{
    s_since += OS_TICK_MS;
    if (s_since < REFRESH_MS) {
        return;
    }
    s_since = 0;
    if (s_view == VIEW_LIST) {
        status_update();
        int n = mailbox_count();
        if (n != s_count) {
            list_reload();
            list_hint();
        } else {
            ui_list_refresh(s_list);    // "5 min ago", read marks
        }
    }
}

static bool key(os_key_t k, bool repeat)
{
    if (s_view == VIEW_READ) {
        if (k == OS_KEY_BACK) {
            if (!repeat) {
                close_read();
            }
            return true;
        }
        read_key(k, repeat);
        return true;
    }
    if (s_view == VIEW_MUTED) {
        if (k == OS_KEY_BACK) {
            if (!repeat) {
                show_list();
            }
            return true;
        }
        if (k == OS_KEY_SELECT) {
            int sel = ui_list_selected(s_list);
            if (sel >= 0 && sel < s_nmuted) {
                mute(s_muted[sel].id, false);
                os_toast("You get %s's mail again", s_muted[sel].name);
            }
            return true;
        }
        return ui_list_key(s_list, k, repeat) || k != OS_KEY_BACK;
    }
    switch (k) {
    case OS_KEY_UP:
    case OS_KEY_DOWN:
        return ui_list_key(s_list, k, repeat);
    case OS_KEY_SELECT: {
        int sel = ui_list_selected(s_list);
        if (sel >= s_count && sel >= 0) {
            show_muted();
        } else if (sel >= 0 && mailbox_item(sel, &s_row)) {
            show_read(s_row.id);
        }
        return true;
    }
    case OS_KEY_RIGHT:
        if (!repeat) {
            if (mailbox_sync()) {
                status_update();
            } else if (!mailbox_syncing()) {
                os_toast(!mailbox_running() ? "Mailbox is off: turn it on in Services"
                         : ui_net_state(true) == UI_NET_OK ? "Can't collect now" : "Turn Wi-Fi on to collect mail");
            }
        }
        return true;
    case OS_KEY_LEFT:
        if (repeat) {
            return true;
        }
        if (!armed(k)) {
            arm(k, "Press " LV_SYMBOL_LEFT " again to delete every read message");
            return true;
        }
        os_toast("Deleted %d", mailbox_delete_read());
        list_reload();
        list_hint();
        return true;
    default:
        return false;
    }
}

const os_app_t app_mailbox = {
    .id = "mailbox",
    .name = "Mailbox",
    .symbol = LV_SYMBOL_ENVELOPE,
    .accent = COL_YELLOW,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};
