// Services: everything that runs in the background (core/os_service.h,
// docs/HHGTS.md "Background services"): app services first, then Solana OS's
// own. Each row shows the status line and On/Off; SELECT opens the details,
// where SELECT stops (after a confirm) or starts it and RIGHT turns starting
// at boot on or off. The list re-reads the registry every tick, so it's
// always live; nothing here holds memory past on_exit.

#include <stdio.h>
#include <string.h>
#include "core/os.h"
#include "core/os_service.h"
#include "core/ui.h"

#define REFRESH_MS  1000

static lv_obj_t *s_root, *s_hint;
static ui_list_t *s_list;
static lv_obj_t *s_info;                // details panel, NULL on the list
static lv_obj_t *s_i_state, *s_i_status, *s_i_mem, *s_i_boot, *s_i_why;
static char s_id[OS_SERVICE_ID_MAX + 1];    // the service the details show
static int s_since;                     // ms since the last refresh
static os_service_view_t s_v;           // scratch (row callback, details)

static const char *state_text(os_service_state_t s, uint32_t *col)
{
    switch (s) {
    case OS_SVC_RUNNING:  *col = COL_OK;   return "On";
    case OS_SVC_STOPPING: *col = COL_WARN; return "Stopping";
    case OS_SVC_FAILED:   *col = COL_ERR;  return "Failed";
    default:              *col = COL_DIM;  return "Off";
    }
}

static void fmt_mem(int32_t bytes, char *out, size_t n)
{
    if (bytes < 0) {
        strlcpy(out, "not known", n);
    } else if (bytes < 1024) {
        snprintf(out, n, "%ld B", (long)bytes);
    } else {
        snprintf(out, n, "%ld KB", (long)((bytes + 512) / 1024));
    }
}

// ---------------------------------------------------------------------------
// List

static void row(int i, ui_row_t *r, void *ctx)
{
    if (!os_service_get(i, &s_v)) {
        return;
    }
    snprintf(r->title, sizeof(r->title), "%s  %s", s_v.symbol ? s_v.symbol : LV_SYMBOL_LOOP, s_v.name);
    strlcpy(r->sub, s_v.status[0] ? s_v.status : (s_v.state == OS_SVC_RUNNING ? "Running" : "Stopped"),
            sizeof(r->sub));
    uint32_t col;
    strlcpy(r->right, state_text(s_v.state, &col), sizeof(r->right));
    r->right_color = col;
    if (s_v.unread > 0) {
        snprintf(r->right, sizeof(r->right), LV_SYMBOL_BELL " %d  %s", s_v.unread > 99 ? 99 : s_v.unread,
                 s_v.state == OS_SVC_RUNNING ? "On" : "Off");
    }
}

static void list_hint(void)
{
    lv_label_set_text(s_hint, os_service_count() > 0 ? "SELECT details   CANCEL exit" : "CANCEL exit");
}

// ---------------------------------------------------------------------------
// Details

static void info_update(void)
{
    if (!os_service_find(s_id, &s_v)) {
        lv_label_set_text(s_i_status, "It's gone: its app was removed.");
        lv_label_set_text(s_i_state, "");
        lv_label_set_text(s_hint, "CANCEL back");
        return;
    }
    uint32_t col;
    const char *st = state_text(s_v.state, &col);
    lv_label_set_text(s_i_state, st);
    lv_obj_set_style_text_color(s_i_state, ui_col(col), 0);
    lv_label_set_text(s_i_status, s_v.status[0] ? s_v.status : "-");
    char mem[24], line[64];
    fmt_mem(s_v.memory, mem, sizeof(mem));
    snprintf(line, sizeof(line), "Memory: %s", mem);
    lv_label_set_text(s_i_mem, line);
    if (s_v.stoppable) {
        snprintf(line, sizeof(line), "Starts at boot: %s", s_v.boot ? "yes" : "no");
        lv_label_set_text(s_i_boot, line);
        lv_label_set_text(s_i_why, "");
        bool on = s_v.state == OS_SVC_RUNNING || s_v.state == OS_SVC_STOPPING;
        lv_label_set_text(s_hint, on ? "SELECT stop   " LV_SYMBOL_RIGHT " boot   CANCEL back"
                                     : "SELECT start   " LV_SYMBOL_RIGHT " boot   CANCEL back");
    } else {
        lv_label_set_text(s_i_boot, "Always on");
        lv_label_set_text(s_i_why, s_v.why_always);
        lv_label_set_text(s_hint, "CANCEL back");
    }
}

static void show_info(void)
{
    s_info = ui_panel(s_root, 0, 0, OS_ROOT_W, OS_ROOT_H - UI_HINT_H);
    lv_obj_set_style_radius(s_info, 0, 0);
    lv_obj_set_style_border_width(s_info, 0, 0);
    lv_obj_set_style_pad_all(s_info, 10, 0);
    lv_obj_set_flex_flow(s_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_info, 4, 0);

    char line[64];
    snprintf(line, sizeof(line), "%s  %s", s_v.symbol ? s_v.symbol : LV_SYMBOL_LOOP, s_v.name);
    lv_obj_t *t = ui_label(s_info, &sol_font_20, COL_TEXT, line);
    lv_obj_set_style_text_color(t, ui_col(s_v.accent ? s_v.accent : COL_TEXT), 0);
    if (s_v.kind == OS_SVC_APP) {
        snprintf(line, sizeof(line), "From the app %s", s_v.app);
    } else {
        strlcpy(line, "Part of Solana OS", sizeof(line));
    }
    ui_label(s_info, &sol_mono_12, COL_DIM, line);
    s_i_state = ui_label(s_info, &sol_font_14, COL_TEXT, "");
    s_i_status = ui_label(s_info, &sol_font_14, COL_TEXT, "");
    lv_obj_set_width(s_i_status, lv_pct(100));
    s_i_mem = ui_label(s_info, &sol_mono_12, COL_DIM, "");
    s_i_boot = ui_label(s_info, &sol_mono_12, COL_DIM, "");
    s_i_why = ui_label(s_info, &sol_mono_12, COL_DIM, "");
    lv_obj_set_width(s_i_why, lv_pct(100));
    if (s_v.screen) {
        lv_obj_t *w = ui_label(s_info, &sol_mono_12, COL_DIM, "Takes the screen while it works. HOME ends it.");
        lv_obj_set_width(w, lv_pct(100));
    }
    ui_list_show(s_list, false);
    info_update();
}

static void close_info(void)
{
    if (s_info) {
        lv_obj_delete(s_info);
        s_info = NULL;
    }
    ui_list_show(s_list, true);
    ui_list_set_count(s_list, os_service_count());
    list_hint();
}

static void stop_answer(bool yes, void *ctx)
{
    if (!yes || !s_info) {
        return;
    }
    const char *why = "";
    if (os_service_stop(s_id, &why)) {
        os_toast("Stopped");
    } else {
        os_toast("%s", why);
    }
    info_update();
}

static void info_select(void)
{
    if (!os_service_find(s_id, &s_v) || !s_v.stoppable) {
        return;
    }
    if (s_v.state == OS_SVC_RUNNING || s_v.state == OS_SVC_STOPPING) {
        char q[160];
        snprintf(q, sizeof(q), "Stop %s?\n%s", s_v.name,
                 s_v.kind == OS_SVC_APP ? "Its app can start it again only with your yes."
                                        : "It won't start again until you start it here.");
        os_confirm(q, stop_answer, NULL);
        return;
    }
    const char *why = "";
    if (os_service_start(s_id, &why)) {
        os_toast("Started");
    } else {
        os_toast("%s", why[0] ? why : "It didn't start");
    }
    info_update();
}

static void info_boot(void)
{
    if (!os_service_find(s_id, &s_v) || !s_v.stoppable) {
        return;
    }
    const char *why = "";
    if (!os_service_set_boot(s_id, !s_v.boot, &why)) {
        os_toast("%s", why);
    }
    info_update();
}

// ---------------------------------------------------------------------------

static void enter(lv_obj_t *root)
{
    s_root = root;
    s_info = NULL;
    s_since = 0;
    s_list = ui_list_create(root, 0, OS_ROOT_H - UI_HINT_H, row, NULL);
    ui_list_set_empty(s_list, "Nothing runs in the background");
    s_hint = ui_hint(root, "");
    ui_list_set_count(s_list, os_service_count());
    list_hint();
}

static void leave(void)
{
    s_info = NULL;
    s_list = NULL;
}

static void tick(void)
{
    s_since += OS_TICK_MS;
    if (s_since < REFRESH_MS) {
        return;
    }
    s_since = 0;
    if (s_info) {
        info_update();
        return;
    }
    static int s_last_n = -1;
    int n = os_service_count();
    if (n == s_last_n) {
        ui_list_refresh(s_list);                        // status lines are live
        return;
    }
    s_last_n = n;
    int sel = ui_list_selected(s_list);
    ui_list_set_count(s_list, n);
    if (sel > 0) {
        ui_list_select(s_list, sel < n ? sel : n - 1);
    }
}

static bool key(os_key_t k, bool repeat)
{
    if (s_info) {
        switch (k) {
        case OS_KEY_BACK:
        case OS_KEY_LEFT:
            if (!repeat) {
                close_info();
            }
            return true;
        case OS_KEY_SELECT:
            info_select();
            return true;
        case OS_KEY_RIGHT:
            if (!repeat) {
                info_boot();
            }
            return true;
        default:
            return true;
        }
    }
    switch (k) {
    case OS_KEY_UP:
    case OS_KEY_DOWN:
        return ui_list_key(s_list, k, repeat);
    case OS_KEY_SELECT: {
        int sel = ui_list_selected(s_list);
        if (sel >= 0 && os_service_get(sel, &s_v)) {
            strlcpy(s_id, s_v.id, sizeof(s_id));
            show_info();
        }
        return true;
    }
    default:
        return false;
    }
}

const os_app_t app_services = {
    .id = "services",
    .name = "Services",
    .symbol = LV_SYMBOL_LOOP,
    .accent = COL_PURPLE,
    .on_enter = enter,
    .on_exit = leave,
    .on_key = key,
    .on_tick = tick,
};
