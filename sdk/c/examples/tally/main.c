// Tally: a counter kept across launches with storage (16 KB per app, on the
// card). Numbers with storage_get_int/storage_set_int; anything else as JSON
// text with storage_get_json/storage_set_json.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "badge.h"

static ui_obj count_label, log_label;
static int32_t count;
static int32_t history[5]; // the last saved counts, newest first
static int nhistory;

// Adds s to the text in buf (cap bytes, len of them used), cut to fit, and
// returns the new length. The room left is worked out from lengths that were
// checked, never from what snprintf says it would have written.
static size_t add(char *buf, size_t cap, size_t len, const char *s)
{
    size_t n = strlen(s);
    if (n > cap - 1 - len) {
        n = cap - 1 - len;
    }
    memcpy(buf + len, s, n);
    buf[len + n] = 0;
    return len + n;
}

static void show(void)
{
    char text[64], num[16];
    snprintf(text, sizeof text, "%ld", (long)count);
    ui_label_text(count_label, text);
    size_t len = add(text, sizeof text, 0, "Saved before:");
    for (int i = 0; i < nhistory; i++) {
        snprintf(num, sizeof num, " %ld", (long)history[i]);
        len = add(text, sizeof text, len, num);
    }
    ui_label_text(log_label, nhistory ? text : "Nothing saved before");
}

// The history is a JSON array in storage: [12, 7, 3].
static void load_history(void)
{
    char json[128];
    if (storage_get_json("history", json, sizeof json) < 0) {
        return;
    }
    nhistory = 0;
    for (char *p = json; *p && nhistory < 5;) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) {
            p++; // '[', ',' or ']'
            continue;
        }
        history[nhistory++] = (int32_t)v;
        p = end;
    }
}

static void save(void)
{
    char json[128], num[16];
    snprintf(num, sizeof num, "[%ld", (long)count);
    size_t len = add(json, sizeof json, 0, num);
    for (int i = 0; i < nhistory && i < 4; i++) {
        snprintf(num, sizeof num, ",%ld", (long)history[i]);
        len = add(json, sizeof json, len, num);
    }
    add(json, sizeof json, len, "]");
    storage_set_int("count", count);
    if (storage_set_json("history", json) < 0) {
        ui_toast(badge_error());
    }
}

static void reset_answer(void *user, int yes)
{
    (void)user;
    if (yes) {
        count = 0;
        storage_remove("count");
        show();
        ui_toast("Reset");
    }
}

void on_start(void)
{
    count = storage_get_int("count", 0);
    load_history();
    ui_label(0, 24, "Tally", &(ui_label_opts){.size = 20, .w = UI_W, .align = UI_CENTER});
    count_label = ui_label(0, 70, "", &(ui_label_opts){.size = 32, .color = COL_BLUE, .w = UI_W, .align = UI_CENTER});
    log_label = ui_label(0, 140, "", &(ui_label_opts){.size = 12, .color = COL_DIM, .w = UI_W, .align = UI_CENTER});
    ui_hint("SELECT +1   LEFT -1   RIGHT reset   CANCEL exit");
    show();
}

int32_t on_key(int32_t key, int32_t repeat)
{
    (void)repeat;
    switch (key) {
    case KEY_SELECT:
        count++;
        break;
    case KEY_LEFT:
        count--;
        break;
    case KEY_RIGHT:
        ui_confirm("Reset the tally?", reset_answer, NULL);
        return 1;
    default:
        return key != KEY_BACK;
    }
    storage_set_int("count", count);
    show();
    return 1;
}

void on_exit(void)
{
    save();
}
