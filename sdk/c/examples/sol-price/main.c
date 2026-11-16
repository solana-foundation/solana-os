// SOL Price: an http call with a callback. The badge server makes the request
// for the app (only to the hosts in app.json) and the answer arrives later in
// a callback, between ticks. JSON answers decode on the badge (json_push), and
// the fields read with badge_field_*.
#include "badge.h"

#define URL "https://api.coinbase.com/v2/prices/SOL-USD/spot"

static ui_obj price_label, status_label;
static int waiting;

static void got_price(void *user, const http_response *res, const char *err)
{
    (void)user;
    waiting = 0;
    if (err) { // no answer: "offline", "not linked", "denied", "timeout", ...
        ui_label_text(status_label, err);
        return;
    }
    if (res->status != 200) {
        ui_label_text(status_label, "The server said no");
        return;
    }
    char body[512];
    if (http_body(res, body, sizeof body) >= (int32_t)sizeof body) {
        ui_label_text(status_label, "Answer too long");
        return;
    }
    // {"data":{"amount":"187.12","base":"SOL","currency":"USD"}}
    if (json_push(body) < 0) {
        ui_label_text(status_label, badge_error());
        return;
    }
    char amount[24] = "";
    badge_val data = badge_push_field(badge_top(), "data");
    badge_field_str(data, "amount", amount, sizeof amount);
    badge_pop(2); // data, then the decoded answer

    char text[40] = "$";
    strncat(text, amount[0] ? amount : "?", sizeof text - 2);
    ui_label_text(price_label, text);
    ui_label_text(status_label, "SELECT to refresh");
}

static void refresh(void)
{
    if (waiting) {
        return;
    }
    ui_label_text(status_label, "Asking...");
    if (http_get(URL, got_price, NULL) < 0) { // refused at once: see badge_error()
        ui_label_text(status_label, badge_error());
        return;
    }
    waiting = 1;
}

void on_start(void)
{
    ui_label(0, 30, "SOL / USD", &(ui_label_opts){.size = 20, .color = COL_DIM, .w = UI_W, .align = UI_CENTER});
    price_label = ui_label(0, 70, "...", &(ui_label_opts){.size = 32, .color = COL_GREEN, .w = UI_W, .align = UI_CENTER});
    status_label = ui_label(0, 130, "", &(ui_label_opts){.size = 14, .w = UI_W, .align = UI_CENTER});
    ui_hint("SELECT refresh   CANCEL exit");
    refresh();
}

int32_t on_key(int32_t key, int32_t repeat)
{
    if (key == KEY_SELECT && !repeat) {
        refresh();
        return 1;
    }
    return key != KEY_BACK;
}
