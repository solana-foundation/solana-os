// Hello C: labels, the keys and a timer. The smallest useful C app.
#include <stdio.h>

#include "badge.h"

static ui_obj clock_label, key_label;
static int32_t seconds, presses;

static void every_second(void *user)
{
    (void)user;
    char text[32];
    seconds++;
    snprintf(text, sizeof text, "%ld s since launch", (long)seconds);
    ui_label_text(clock_label, text);
}

void on_start(void)
{
    ui_label_opts big = {.size = 32, .color = COL_GREEN, .w = UI_W, .align = UI_CENTER};
    ui_label_opts dim = {.size = 14, .color = COL_DIM, .w = UI_W, .align = UI_CENTER};
    ui_label(0, 40, "Hello, badge", &big);
    clock_label = ui_label(0, 100, "0 s since launch", &dim);
    key_label = ui_label(0, 130, "Press a key", &dim);
    ui_hint("Any key   CANCEL exit");
    if (timer_every(1000, every_second, NULL) < 0) {
        badge_print(badge_error());
    }
    printf("hello-c started\n"); // stdout goes to the serial console
}

int32_t on_key(int32_t key, int32_t repeat)
{
    char text[48];
    if (key == KEY_BACK) {
        return 0; // not handled: CANCEL exits
    }
    if (!repeat) {
        presses++;
    }
    snprintf(text, sizeof text, "%s%s  (%ld presses)", key_name(key), repeat ? " (held)" : "", (long)presses);
    ui_label_text(key_label, text);
    return 1;
}
