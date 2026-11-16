// lab_source_feed.c: a test stand-in for the Lab's lab_source.c (lab-emu.ts builds
// a copy of the Lab with this file in its place). The gossip module's reading
// functions answer from the bundled data file feed.json, re-read at every call,
// so the test changes the state by rewriting the file on the emulated card;
// acting calls print "feed: <name>" and answer true. Never in the shipped app.
#include "lab_gossip.h"

static const char *const NAMES[] = {"me", "programs", "program", "nearby", "hearing", "role", "boss"};
#define READS 7

static const char *s_acting[24];
static int s_nacting;

static int32_t feed(void *ud, int32_t nargs)
{
    int k = (int)(uintptr_t)ud;
    if (k >= 100) {
        char line[64] = "feed: ";
        uint32_t n = 6;
        for (const char *c = s_acting[k - 100]; *c && n < sizeof line - 1; c++) {
            line[n++] = *c;
        }
        line[n] = 0;
        badge_print(line);
        sos_push_bool(1);
        return 1;
    }
    if (fs_json_push("feed.json")) {
        sos_push_nil();
        return 1;
    }
    sos_get(-1, NAMES[k], (uint32_t)strlen(NAMES[k])); // [root field]
    if (k == 2) {                                        // program(a): feed.program[a]
        char a[LAB_ADDR];
        sos_to_str(1, a, sizeof a);
        sos_get(-1, a, (uint32_t)strlen(a));             // [root map value]
        sos_remove(-2);
    }
    sos_remove(-2); // [value]
    (void)nargs;
    return 1;
}

int lab_source_fn(const char *fn)
{
    for (int k = 0; k < READS; k++) {
        if (!strcmp(NAMES[k], fn)) {
            sos_push_fn(feed, (void *)(uintptr_t)k);
            return 0;
        }
    }
    for (int k = 0; k < s_nacting; k++) {
        if (!strcmp(s_acting[k], fn)) {
            sos_push_fn(feed, (void *)(uintptr_t)(100 + k));
            return 0;
        }
    }
    if (s_nacting == 24) {
        return BADGE_ERR;
    }
    s_acting[s_nacting] = fn; // the Lab's names are string literals
    sos_push_fn(feed, (void *)(uintptr_t)(100 + s_nacting));
    s_nacting++;
    return 0;
}
