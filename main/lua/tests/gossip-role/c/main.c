// Regression test (not shipped): badge.h's gossip role wrappers (Solana OS
// 2.4.0), the C twin of tests/t-gossip-role. The headless suite
// (tests/gossip-role/run.ts) builds it with tools/cc/badge-cc and installs it
// as gossip-lab, once with no role (each acting call says why) and once on a
// boss (gossip_role and gossip_voters fill in). Lines start "t-c-gossip-role".
#include <stdio.h>
#include <string.h>

#include "badge.h"

static int s_fails;

static void check(int ok, const char *what)
{
    char line[200];
    s_fails += !ok;
    snprintf(line, sizeof line, "t-c-gossip-role %s %s", ok ? "ok" : "FAIL", what);
    badge_print(line);
}

static void said(const char *what, int r)
{
    char line[200];
    snprintf(line, sizeof line, "t-c-gossip-role %s: %d %s", what, r, r == BADGE_ERR ? badge_error() : "");
    badge_print(line);
}

void on_start(void)
{
    char line[200];
    gossip_role_t role;
    int have = gossip_role(&role);
    snprintf(line, sizeof line, "t-c-gossip-role role %d kind %s sid %ld ro %c id %s bpm %ld beats %ld nst %d left %ld", have,
             role.kind, (long)role.sid, role.ro ? role.ro : '-', role.id, (long)role.bpm, (long)role.beats, role.nst,
             (long)role.left);
    badge_print(line);
    check(have == 0 || have == 1, "gossip_role");
    gossip_boss_t boss;
    int b = gossip_boss(&boss);
    check(b == 0 || b == 1, "gossip_boss");
    gossip_voters_t v;
    int nv = gossip_voters(&v);
    snprintf(line, sizeof line, "t-c-gossip-role voters %d: %ld recent, %ld fin, beat %ld/%ld phase %ld", nv, (long)v.voters,
             (long)v.fin, (long)v.beat, (long)v.beats, (long)v.phase);
    badge_print(line);
    gossip_bid_t bids[4];
    int nb = gossip_bids(bids, 4);
    check(nb >= 0, "gossip_bids");
    said("vote", gossip_vote(1, 1));
    said("claim", gossip_claim("RABCDE", 3));
    said("cure", gossip_cure("ABCDEF"));
    said("bid", gossip_bid("RABCDE", "59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMruo", 0));
    gossip_pick_t pick = {"59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMruo", "ABCDEF"};
    said("block", gossip_block(&pick, 1));
    said("answer", gossip_role_answer(1));
    said("fork", gossip_fork());
    snprintf(line, sizeof line, "t-c-gossip-role done, %d failed", s_fails);
    badge_print(line);
}
