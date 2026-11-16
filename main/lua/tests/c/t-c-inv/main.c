// Regression test (not shipped): badge.h's profile_items and profile_pages
// (permission "inventory", Solana OS 2.3.0) from a C app, the C twin of
// tests/t-inv. At launch it walks both lists to the end through each result's
// "next", checks that no entry comes twice and that the totals match, and shows
// how many of the Guide's pages the player holds. SELECT walks again. Lines
// start "t-c-inv". Built by tests/c/build.sh through badge-cc.
#include <stdio.h>
#include <string.h>

#include "badge.h"

#define SEEN_MAX 256

static ui_obj s_status, s_log;
static char s_seen[SEEN_MAX][24];
static int s_nseen, s_calls, s_twice;

static void say(const char *text)
{
    char line[200];
    snprintf(line, sizeof line, "t-c-inv %s", text);
    badge_print(line);
    ui_label_text(s_log, text);
}

static void note(const char *id)
{
    for (int i = 0; i < s_nseen; i++) {
        if (!strcmp(s_seen[i], id)) {
            s_twice++;
            return;
        }
    }
    if (s_nseen < SEEN_MAX) {
        snprintf(s_seen[s_nseen++], sizeof s_seen[0], "%s", id);
    }
}

// Copies the result's list into s_seen; returns the cursor for the rest in next ("" at the end).
static void take(badge_val r, const char *field, char *next, uint32_t cap)
{
    badge_val list = badge_push_field(r, field);
    for (uint32_t i = 0; i < badge_len(list); i++) {
        badge_val e = badge_push_index(list, (int32_t)i);
        if (!strcmp(field, "pages")) {
            profile_page_t p;
            profile_page_info(e, &p);
            note(p.id);
        } else {
            profile_item_t it;
            profile_item_info(e, &it);
            note(it.id);
            if (s_calls == 1 && i == 0) {
                char line[200];
                snprintf(line, sizeof line, "t-c-inv first: %s %s %s app=%s quest=%s tradable=%d", it.id, it.kind,
                         it.name, it.app[0] ? it.app : "-", it.quest[0] ? it.quest : "-", it.tradable);
                badge_print(line);
            }
        }
        badge_pop(1);
    }
    badge_pop(1);
    if (badge_field_str(r, "next", next, cap) <= 0) {
        next[0] = 0;
    }
}

static void walk_pages(void);

static void on_pages(void *user, badge_val r, const char *err)
{
    char next[160], line[160];
    (void)user;
    if (err) {
        snprintf(line, sizeof line, "pages error: %s", err);
        say(line);
        return;
    }
    s_calls++;
    take(r, "pages", next, sizeof next);
    if (next[0]) {
        if (profile_pages(next, on_pages, NULL)) {
            say(badge_error());
        }
        return;
    }
    int owned = badge_field_int(r, "owned", -1), total = badge_field_int(r, "total", -1);
    snprintf(line, sizeof line, "pages %d of %d (owned %d) in %d calls", s_nseen, total, owned, s_calls);
    say(line);
    if (s_nseen != owned || s_twice) {
        badge_print("t-c-inv FAIL pages count");
    }
    snprintf(line, sizeof line, "You hold %d of the Guide's %d pages", owned, total);
    ui_label_text(s_status, line);
    badge_print("t-c-inv done");
}

static void walk_pages(void)
{
    s_nseen = s_calls = s_twice = 0;
    if (profile_pages(NULL, on_pages, NULL)) {
        say(badge_error());
    }
}

static void on_items(void *user, badge_val r, const char *err)
{
    char next[160], line[160];
    (void)user;
    if (err) {
        snprintf(line, sizeof line, "items error: %s", err);
        say(line);
        return;
    }
    s_calls++;
    take(r, "items", next, sizeof next);
    if (next[0]) {
        if (profile_items(next, on_items, NULL)) {
            say(badge_error());
        }
        return;
    }
    int total = badge_field_int(r, "total", -1);
    snprintf(line, sizeof line, "items %d of %d in %d calls", s_nseen, total, s_calls);
    say(line);
    if (s_nseen != total || s_twice) {
        badge_print("t-c-inv FAIL items count");
    }
    walk_pages();
}

static void run(void)
{
    s_nseen = s_calls = s_twice = 0;
    ui_label_text(s_status, "t-c-inv: walking items");
    if (profile_items(NULL, on_items, NULL)) {
        say(badge_error());
    }
}

int on_key(int32_t key, int32_t rep)
{
    (void)rep;
    if (key == KEY_SELECT) {
        run();
        return 1;
    }
    return 0;
}

void on_start(void)
{
    s_status = ui_label(8, 8, "t-c-inv: reading the inventory", &(ui_label_opts){.size = 12, .w = 304});
    s_log = ui_label(8, 30, "", &(ui_label_opts){.size = 12, .w = 304});
    ui_hint("SELECT again");
    run();
}
