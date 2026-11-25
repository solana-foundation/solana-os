// Regression test (not shipped): badge.h's gossip wrappers (fw-node's block,
// Solana OS 2.4.0) from a C app, the C twin of tests/t-gossip. Prints the
// reads; the acting calls answer "lab only" under this id. Lines start
// "t-c-gossip". Built and run by tests/gossip/run.ts when the C toolchain is
// there (tools/cc/badge-cc).
#include <stdio.h>
#include <string.h>

#include "badge.h"

static void line(const char *s)
{
    char out[320];
    snprintf(out, sizeof out, "t-c-gossip %s", s);
    badge_print(out);
}

void on_gossip(void)
{
    char kind[16], json[240], out[280];
    gossip_event_kind(kind, sizeof kind);
    gossip_event_json(json, sizeof json);
    snprintf(out, sizeof out, "ev %s %s", kind, json);
    line(out);
}

void on_start(void)
{
    char out[300];
    gossip_program_t g;
    int r = gossip_genome("31UFvJfpgAT4Paado1LNpEor9LL5DXMFAiCYzE2j5Cdj", &g);
    snprintf(out, sizeof out, "genome: %s colour %06lx body %ld traits %ld:%ld,%ld,%ld vir %ld rarity %ld", r == 0 ? "ok" : "FAIL",
             (unsigned long)g.colour, (long)g.body, (long)g.ntraits, (long)g.traits[0], (long)g.traits[1],
             (long)g.traits[2], (long)g.vir, (long)g.rarity);
    line(out);
    r = gossip_genome("nope", &g);
    snprintf(out, sizeof out, "bad genome: %s %s", r == 0 ? "FAIL" : "refused", badge_error());
    line(out);

    gossip_me_t me;
    if (gossip_me(&me) == 0) {
        snprintf(out, sizeof out, "me: id %s on %d class %c stake %ld slots %ld inbox %ld act %s synced %ld", me.id, me.on,
                 me.cls ? me.cls : '-', (long)me.stake, (long)me.nslots, (long)me.ninbox, me.act[0] ? me.act : "-",
                 (long)me.synced);
        line(out);
        for (int i = 0; i < me.nslots; i++) {
            if (gossip_slot(i, &g) == 1) {
                snprintf(out, sizeof out, "slot %d: %s verified %d nodes %ld", i, g.a, g.verified, (long)g.nodes);
                line(out);
            }
        }
        if (me.act[0] && gossip_program(me.act, &g) == 0) {
            snprintf(out, sizeof out, "program: %s gen %ld verified %d", g.a, (long)g.gen, g.verified);
            line(out);
        }
    } else {
        snprintf(out, sizeof out, "me: %s", badge_error());
        line(out);
    }
    gossip_peer_t peers[20];
    int n = gossip_nearby(peers, 20);
    for (int i = 0; i < n && i < 20; i++) {
        snprintf(out, sizeof out, "nearby: %s %s class %c rssi %ld st %s", peers[i].id, peers[i].role ? "role" : "badge",
                 peers[i].cls ? peers[i].cls : '-', (long)peers[i].rssi, peers[i].st[0] ? peers[i].st : "-");
        line(out);
    }
    gossip_heard_t h[8];
    n = gossip_hearing(h, 8);
    snprintf(out, sizeof out, "hearing: %d", n);
    line(out);
    r = gossip_set_class('r');
    snprintf(out, sizeof out, "set_class: %s", r == 0 ? "queued" : badge_error());
    line(out);
    line("done");
}
