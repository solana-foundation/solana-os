// Regression test (not shipped): badge.h's mail wrappers (Mailbox, Solana OS
// 2.3.0) from a C app, the C twin of tests/t-mail. At launch it checks what
// mail_send refuses, lists this app's mail on the badge (mail_count, mail_item,
// mail_get), and, when storage holds {"to": "<badge id>"} (`lua put`), sends one
// there. SELECT sends again, DOWN lists; on_mail prints what arrives (mail_arg).
// Lines start "t-c-mail". Built by tests/c/build.sh through badge-cc.
#include <stdio.h>
#include <string.h>

#include "badge.h"

static ui_obj s_log;
static int s_n;

static void say(const char *text)
{
    char line[300];
    snprintf(line, sizeof line, "t-c-mail %s", text);
    badge_print(line);
    ui_label_text(s_log, text);
}

static void refuse(const char *what, const char *to, const char *title, const char *text, const char *data)
{
    char line[160];
    int r = mail_send(to, title, text, data);
    snprintf(line, sizeof line, "t-c-mail %s: %s %s", what, r == 0 ? "sent" : "refused", r == 0 ? "" : badge_error());
    badge_print(line);
}

static void list(void)
{
    char line[300];
    int n = mail_count();
    snprintf(line, sizeof line, "%d in the mailbox", n);
    say(line);
    for (int i = 0; i < n; i++) {
        mail_t m, full;
        if (mail_item(i, &m) != 0) {
            continue;
        }
        int got = mail_get(m.id, &full);
        snprintf(line, sizeof line, "t-c-mail item %ld from %s by %s read %ld: %s | %s | %s", (long)m.id, m.from, m.by,
                 (long)m.read, m.title, got == 0 ? full.text : "?", got == 0 && full.data[0] ? full.data : "-");
        badge_print(line);
    }
    mail_t none;
    snprintf(line, sizeof line, "t-c-mail get nothing: %s", mail_get(999999999, &none) == 0 ? "found" : badge_error());
    badge_print(line);
}

static void send(const char *to)
{
    char title[33], data[32], line[160];
    s_n++;
    snprintf(title, sizeof title, "t-c-mail #%d", s_n);
    snprintf(data, sizeof data, "{\"n\":%d}", s_n);
    int r = mail_send(to, title, "Sent by the C mail test", data);
    snprintf(line, sizeof line, "send to %s: %s %s", to, r == 0 ? "ok" : "refused", r == 0 ? "" : badge_error());
    say(line);
    r = mail_send(to, "too soon", "", NULL);
    snprintf(line, sizeof line, "t-c-mail rate: %s %s", r == 0 ? "sent" : "refused", r == 0 ? "" : badge_error());
    badge_print(line);
}

void on_mail(void)
{
    mail_t m;
    char line[300];
    mail_arg(&m);
    snprintf(line, sizeof line, "on_mail %ld from %s: %s (%s) data %s", (long)m.id, m.from, m.title, m.text, m.data[0] ? m.data : "-");
    say(line);
}

void on_start(void)
{
    ui_label(8, 8, "t-c-mail: SELECT mails storage's badge", &(ui_label_opts){.size = 12, .w = 304});
    s_log = ui_label(8, 30, "", &(ui_label_opts){.size = 12, .w = 304});
    ui_hint("SELECT send   DOWN list");
    refuse("bad id", "x", "hi", "", NULL);
    refuse("emu id", "eZZZZ1", "", "", NULL);
    refuse("long title", "7K2Q9M", "tttttttttttttttttttttttttttttttttt", "", NULL);
    refuse("newline", "7K2Q9M", "ok", "two\nlines", NULL);
    refuse("bad data", "7K2Q9M", "ok", "", "[1,2]");
    list();
    char to[16];
    if (storage_get_str("to", to, sizeof to) != BADGE_ERR) {
        send(to);
    }
    badge_print("t-c-mail ready");
}

int32_t on_key(int32_t key, int32_t repeat)
{
    (void)repeat;
    char to[16];
    if (key == KEY_SELECT) {
        if (storage_get_str("to", to, sizeof to) != BADGE_ERR) {
            send(to);
        } else {
            say("nobody to mail: lua put a storage \"to\"");
        }
        return 1;
    }
    if (key == KEY_DOWN) {
        list();
        return 1;
    }
    return 0;
}
