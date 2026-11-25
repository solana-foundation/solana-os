// Gossip roles: the boss fight (Solana OS 2.4.0; the parent repo's
// docs/GOSSIP.md §8 "The fight", Tower BFT lockouts). Builder: fw-roles.
// Under gr_mx like the rest (gossip_role_int.h).
//
// The boss's side (a Byzantine Leader session on this badge): a metronome at
// the session's bpm, a `boss` broadcast twice a second whose "ms" is computed
// the moment it goes out (gr_flush asks gr_boss_due), fork beats on the
// owner's request (one in eight beats at most), the latest vote summary per
// voter (≤ 160, in PSRAM), progress and the win condition, the report.
//
// The player's side: the fight heard nearby (gossip.boss(), beat and ms moved
// forward to the moment it is read), the Lab's vote summaries sent one per
// ~2 s (they are cumulative, so a lost one costs freshness, not votes), and
// the claim at the end. It also measures how far apart consecutive broadcasts
// put the same beat (`grole` prints it): the jitter a player's clock sees.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "core/os_mem.h"
#include "core/theme.h"
#include "game.h"
#include "gossip_role_int.h"

static const char *TAG = GR_TAG;

#define VOTERS_MAX      160
#define RECENT_MS       10000       // voters seen in the last 10 s count (GOSSIP.md §8)
#define CONFIRMED       4           // lockout >= 4: confirmed; 32: finalized
#define FINALIZED       32
#define FORK_EVERY      8           // at most one fork in 8 beats
#define FORK_LEAD_MS    400         // a fork is announced at least this long before its beat
#define BCAST_MS        500         // twice a second
#define BCAST_END_MS    1000        // after the end: once a second ...
#define BCAST_END_FOR   90000       // ... for 90 s, so every player hears how it ended and its claim is answered
#define HEARD_MS        10000       // a boss not heard for this long is gone
#define VOTE_EVERY_MS   2000        // a player's summary: one per ~2 s
#define CLAIM_SILENT_MS 15000       // the boss went quiet after we voted: claim anyway
#define EV_GAP_MS       250         // on_gossip "boss": at most four a second
#define WINDOW_MS       120         // ±120 ms: the jitter histogram's mark
#define TWO_PERIODS_MS  1500        // a voter counts once two summaries ~2 s apart came in (one alone proves little)

typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    uint16_t votes;
    uint8_t lockout;
    int64_t at;
    int64_t first;                  // its first summary this fight
} voter_t;

// The boss's side.
static struct {
    bool on, done, won;
    long sid;
    int bpm, beats, beat_ms;
    int min;                        // bossMinVoters, from RoleState.boss.min
    int min_beats;                  // bossMinBeats, from RoleState.boss.minBeats: no rooting before this beat
    int64_t t0;                     // local ms of beat 0
    int fk;                         // the announced fork beat, -1 none
    int64_t next_bcast, ended_at;
    voter_t *v;                     // VOTERS_MAX, PSRAM when there is some
    int nv;
    int n, conf, fin, p;            // the recent window, as of the last tick
    uint32_t votes_in, votes_full;
} B;

// The player's side.
static struct {
    bool have, real;
    char id[OS_MESH_ID_LEN + 1];
    long sid;
    int b, ms, bpm, fk, p, ph, rssi;
    int mb;                         // the first beat it may root at ("mb"; -1 not sent)
    int64_t at;                     // when the mesh task had it
    int64_t pred_at;                // the beat after b, by the last broadcast (local ms)
    int pred_b;
    int64_t ev_at;
    // the vote
    char vto[OS_MESH_ID_LEN + 1];
    long vsid;
    int votes, lockout;
    int64_t vset_at, vsent_at;
    bool claimed;
    // jitter: |where two broadcasts put the same beat|
    uint32_t jn, jover;
    int64_t jsum;
    int jmax;
    uint32_t heard, votes_sent;
} P;

static int beat_ms_of(int bpm)
{
    return 60000 / (bpm > 0 ? bpm : 100);
}

// ---------------------------------------------------------------------------
// The boss's side

static void boss_now(int64_t now, int *b, int *ms, int *ph)
{
    int64_t e = now - B.t0;
    if (e < 0) {
        *b = 0;
        *ms = (int)(-e);
        *ph = 0;                        // before the first beat
        return;
    }
    *b = (int)(e / B.beat_ms);
    *ms = B.beat_ms - (int)(e % B.beat_ms);
    *ph = B.done ? (B.won ? 2 : 3) : 1;
}

void gr_boss_start(long sid, int bpm, int beats, int min, int min_beats, int64_t since)
{
    int64_t now = gr_ms(), t = gr_server_ms();
    if (!B.v) {
        B.v = heap_caps_calloc(VOTERS_MAX, sizeof(voter_t), os_mem_bulk_caps());
    }
    B.on = true;
    B.done = B.won = false;
    B.sid = sid;
    B.bpm = bpm;
    B.beats = beats;
    B.min = min > 0 ? min : 3;
    B.min_beats = min_beats >= 0 ? min_beats : 60;
    B.beat_ms = beat_ms_of(bpm);
    // Beat 0 is at `since` by the server's clock, as close as we know it.
    int64_t off = t && since ? t - since : 0;
    off = off < -60000 ? -60000 : off > (int64_t)beats * B.beat_ms ? 0 : off;
    B.t0 = now - off;
    B.fk = -1;
    B.next_bcast = now;
    B.nv = 0;
    B.n = B.conf = B.fin = B.p = 0;
    B.votes_in = B.votes_full = 0;
    ESP_LOGI(TAG, "boss %ld: %d bpm, %d beats, rooted from beat %d%s", sid, bpm, beats, B.min_beats,
             B.v ? "" : " (no voter table: no memory)");
}

bool gr_boss_over(void)
{
    return B.on && B.done;
}

void gr_boss_stop(void)
{
    B.on = false;
    free(B.v);                          // heap_caps memory frees with free()
    B.v = NULL;
    B.nv = 0;
}

// The recent window: voters heard in the last 10 s, and in two vote periods at least.
static void count(int64_t now)
{
    B.n = B.conf = B.fin = 0;
    for (int i = 0; i < B.nv; i++) {
        if (now - B.v[i].at <= RECENT_MS && B.v[i].at - B.v[i].first >= TWO_PERIODS_MS) {
            B.n++;
            B.conf += B.v[i].lockout >= CONFIRMED;
            B.fin += B.v[i].lockout >= FINALIZED;
        }
    }
    B.p = B.n ? B.conf * 100 / B.n : 0;
}

static void boss_end(int64_t now, bool won)
{
    B.done = true;
    B.won = won;
    B.ended_at = now;
    B.next_bcast = now;
    gossip_sync_soon();                 // the report goes at once
    gr_notice(won ? COL_GREEN : COL_PURPLE, won ? "the players rooted the block" : "the boss outlasted the players");
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "t", "boss");
    cJSON_AddBoolToObject(e, "own", true);
    cJSON_AddNumberToObject(e, "sid", B.sid);
    cJSON_AddNumberToObject(e, "phase", won ? 2 : 3);
    cJSON_AddNumberToObject(e, "voters", B.n);
    cJSON_AddNumberToObject(e, "fin", B.fin);
    gr_event(e);
    ESP_LOGI(TAG, "boss %ld: %s (%d voters, %d confirmed, %d finalized)", B.sid, won ? "rooted" : "lost", B.n, B.conf,
             B.fin);
}

static void on_vote(const char *from, os_peer_kind_t kind, const cJSON *d)
{
    gr_live_t L;
    int64_t now = gr_ms();
    int b, ms, ph;
    if (!B.on || !B.v || B.done || !gr_role_live(&L) || L.ro != 'z' || kind != OS_PEER_BADGE) {
        return;
    }
    boss_now(now, &b, &ms, &ph);
    int v = gr_jint(d, "v", 0, GR_CLAIM_C_MAX, -1), l = gr_jint(d, "l", 0, FINALIZED, -1);
    // Plausible only: at most one vote a beat so far (ph 0: none yet), and a lockout
    // needs votes behind it: each vote in time doubles it, so l <= 2^(v-1).
    if (v < 0 || l < 0 || ph != 1 || v > b + 1 || (l > 0 && (v == 0 || (v <= 6 && l > (1 << (v - 1)))))) {
        return;
    }
    B.votes_in++;
    voter_t *x = NULL, *stale = NULL;
    for (int i = 0; i < B.nv && !x; i++) {
        if (!strcmp(B.v[i].id, from)) {
            x = &B.v[i];
        } else if (!stale || B.v[i].at < stale->at) {
            stale = &B.v[i];
        }
    }
    bool fresh = !x;
    if (!x && B.nv < VOTERS_MAX) {
        x = &B.v[B.nv++];
    } else if (!x && stale && now - stale->at > RECENT_MS) {
        x = stale;                      // full: someone no longer voting makes room
    }
    if (!x) {
        B.votes_full++;
        return;
    }
    if (fresh) {
        x->first = now;
    }
    strlcpy(x->id, from, sizeof(x->id));
    x->votes = (uint16_t)v;
    x->lockout = (uint8_t)l;
    x->at = now;
}

cJSON *gr_boss_due(int64_t now)
{
    if (!B.on || now < B.next_bcast) {
        return NULL;
    }
    if (B.done && now - B.ended_at > BCAST_END_FOR) {
        return NULL;
    }
    int b, ms, ph;
    boss_now(now, &b, &ms, &ph);
    int every = B.done ? BCAST_END_MS : BCAST_MS;
    B.next_bcast += every;
    if (B.next_bcast <= now) {
        B.next_bcast = now + every;     // late (a busy queue): no burst to catch up
    }
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "t", "boss");
    cJSON_AddNumberToObject(d, "sid", B.sid);
    cJSON_AddNumberToObject(d, "b", b);
    cJSON_AddNumberToObject(d, "ms", ms);
    cJSON_AddNumberToObject(d, "bpm", B.bpm);
    cJSON_AddNumberToObject(d, "fk", B.fk);
    cJSON_AddNumberToObject(d, "mb", B.min_beats);
    cJSON_AddNumberToObject(d, "p", B.p);
    cJSON_AddNumberToObject(d, "ph", ph);
    return d;
}

const char *gossip_role_fork(void)
{
    if (!gr_mx) {
        return "off";
    }
    gr_live_t L;
    const char *err = NULL;
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    int b, ms, ph;
    if (!gr_role_live(&L) || L.ro != 'z' || !B.on) {
        err = "not the boss";
    } else {
        boss_now(gr_ms(), &b, &ms, &ph);
        int at = b + (ms >= FORK_LEAD_MS ? 1 : 2);     // announced at least 400 ms ahead
        if (ph != 1) {
            err = "no fight";
        } else if (B.fk >= 0 && at - B.fk < FORK_EVERY) {
            err = "too soon";
        } else if (at >= B.beats) {
            err = "too late";
        } else {
            B.fk = at;
            B.next_bcast = 0;           // announce it with the next send
        }
    }
    xSemaphoreGive(gr_mx);
    return err;
}

void gr_boss_report(cJSON *role)
{
    if (!B.on) {
        return;
    }
    int b, ms, ph;
    boss_now(gr_ms(), &b, &ms, &ph);
    cJSON *o = cJSON_AddObjectToObject(role, "boss");
    cJSON_AddNumberToObject(o, "beat", b);
    cJSON_AddNumberToObject(o, "voters", B.n);
    cJSON_AddNumberToObject(o, "conf", B.conf);
    cJSON_AddNumberToObject(o, "fin", B.fin);
    if (B.done) {
        cJSON_AddBoolToObject(o, "won", B.won);
    }
}

char *gossip_role_voters_json(void)
{
    if (!gr_mx) {
        return NULL;
    }
    cJSON *o = NULL;
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    gr_live_t L;
    if (B.on && gr_role_live(&L) && L.ro == 'z') {
        int b, ms, ph;
        boss_now(gr_ms(), &b, &ms, &ph);
        o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "voters", B.n);
        cJSON_AddNumberToObject(o, "conf", B.conf);
        cJSON_AddNumberToObject(o, "fin", B.fin);
        cJSON_AddNumberToObject(o, "p", B.p);
        cJSON_AddNumberToObject(o, "all", B.nv);
        cJSON_AddNumberToObject(o, "beat", b);
        cJSON_AddNumberToObject(o, "beats", B.beats);
        cJSON_AddNumberToObject(o, "minBeats", B.min_beats);
        cJSON_AddNumberToObject(o, "fork", B.fk);
        cJSON_AddNumberToObject(o, "phase", ph);
    }
    xSemaphoreGive(gr_mx);
    char *j = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    return j;
}

// ---------------------------------------------------------------------------
// The player's side

// Beat and ms moved forward from when it was heard to now.
static void player_now(int64_t now, int *b, int *ms)
{
    int64_t e = now - P.at;
    *b = P.b;
    *ms = P.ms;
    if (P.ph != 1 || e <= 0) {
        *ms = P.ms - (int)(e > 0 ? e : 0);
        *ms = *ms < 0 ? 0 : *ms;
        return;
    }
    int bm = beat_ms_of(P.bpm);
    if (e < P.ms) {
        *ms = P.ms - (int)e;
    } else {
        int64_t past = e - P.ms;
        *b = P.b + 1 + (int)(past / bm);
        *ms = bm - (int)(past % bm);
    }
}

static cJSON *player_obj(int64_t now)
{
    int b, ms;
    player_now(now, &b, &ms);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", P.id);
    cJSON_AddNumberToObject(o, "sid", P.sid);
    cJSON_AddNumberToObject(o, "beat", b);
    cJSON_AddNumberToObject(o, "ms", ms);
    cJSON_AddNumberToObject(o, "bpm", P.bpm);
    cJSON_AddNumberToObject(o, "fork", P.fk);
    if (P.mb >= 0) {
        cJSON_AddNumberToObject(o, "minBeats", P.mb);
    }
    cJSON_AddNumberToObject(o, "p", P.p);
    cJSON_AddNumberToObject(o, "phase", P.ph);
    cJSON_AddNumberToObject(o, "rssi", P.rssi);
    cJSON_AddBoolToObject(o, "real", P.real);
    return o;
}

static void claim_end(void)
{
    if (!P.claimed && P.vto[0] && P.votes > 0) {
        P.claimed = gr_app_claim_auto(P.vto, P.votes);   // c = the votes in time (GOSSIP.md §8)
    }
}

static void on_boss(const char *from, os_peer_kind_t kind, int rssi, int64_t at, const cJSON *d)
{
    if (kind != OS_PEER_STATION) {
        return;
    }
    long sid = gr_jint(d, "sid", 1, 0x7fffffff, 0);
    int bpm = gr_jint(d, "bpm", 30, 240, 0), b = gr_jint(d, "b", 0, 100000, -1);
    int ms = gr_jint(d, "ms", 0, 60000, -1), fk = gr_jint(d, "fk", -1, 100000, -2);
    int p = gr_jint(d, "p", 0, 100, -1), ph = gr_jint(d, "ph", 0, 3, -1);
    int mb = gr_jint(d, "mb", 0, 100000, -1);
    if (!sid || !bpm || b < 0 || ms < 0 || (ph >= 1 && ms > beat_ms_of(bpm)) || fk < -1 || p < 0 || ph < 0) {
        return;
    }
    bool real = gr_real(from);
    bool same = P.have && !strcmp(P.id, from) && P.sid == sid;
    if (P.have && strcmp(P.id, from) && at - P.at < HEARD_MS && P.ph <= 1 && !(real && !P.real)) {
        return;                         // one fight at a time: the one we follow (until it ends), unless a listed one appears
    }
    P.heard++;
    if (same && P.ph == 1 && ph == 1 && P.bpm == bpm) {
        // Where this broadcast puts the next beat against where the last one put it.
        int64_t mine = at + ms, theirs = P.pred_at + (int64_t)(b + 1 - P.pred_b) * beat_ms_of(bpm);
        int j = (int)(mine > theirs ? mine - theirs : theirs - mine);
        P.jn++;
        P.jsum += j;
        P.jmax = j > P.jmax ? j : P.jmax;
        P.jover += j > WINDOW_MS;
    }
    if (!same) {
        P.claimed = false;
    }
    P.have = true;
    P.real = real;
    strlcpy(P.id, from, sizeof(P.id));
    P.sid = sid;
    P.b = b;
    P.ms = ms;
    P.bpm = bpm;
    P.fk = fk;
    P.mb = mb;
    P.p = p;
    P.ph = ph;
    P.rssi = rssi;
    P.at = at;
    P.pred_at = at + ms;
    P.pred_b = b + 1;
    if (ph >= 2 && P.vsid == sid && !strcmp(P.vto, from)) {
        claim_end();
    }
    int64_t now = gr_ms();
    if (now - P.ev_at >= EV_GAP_MS) {
        P.ev_at = now;
        cJSON *e = player_obj(now);
        cJSON_AddStringToObject(e, "t", "boss");
        gr_event(e);
    }
}

const char *gossip_role_vote(int votes, int lockout)
{
    if (!gr_mx) {
        return "off";
    }
    const char *err = NULL;
    int64_t now = gr_ms();
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    if (!gr_on()) {
        err = "gossip is off";
    } else if (!P.have || now - P.at > HEARD_MS) {
        err = "no fight near";
    } else if (P.ph >= 2) {
        err = "over";
    } else if (votes < 0 || votes > GR_CLAIM_C_MAX || lockout < 0 || lockout > FINALIZED) {
        err = "votes 0-9999, lockout 0-32";
    } else {
        if (P.vsid != P.sid || strcmp(P.vto, P.id)) {
            P.claimed = false;
            P.vsent_at = 0;             // a new fight: the first summary goes at once
        }
        strlcpy(P.vto, P.id, sizeof(P.vto));
        P.vsid = P.sid;
        P.votes = votes;
        P.lockout = lockout;
        P.vset_at = now;
    }
    xSemaphoreGive(gr_mx);
    return err;
}

cJSON *gr_boss_vote_due(int64_t now, char to[OS_MESH_ID_LEN + 1])
{
    if (!P.vto[0] || P.vsid != P.sid || strcmp(P.vto, P.id) || P.ph != 1 || now - P.at > HEARD_MS ||
            now - P.vset_at > HEARD_MS || now - P.vsent_at < VOTE_EVERY_MS) {
        return NULL;
    }
    P.vsent_at = now;
    P.votes_sent++;
    strlcpy(to, P.vto, OS_MESH_ID_LEN + 1);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "t", "vote");
    cJSON_AddNumberToObject(d, "v", P.votes);
    cJSON_AddNumberToObject(d, "l", P.lockout);
    return d;
}

bool gr_boss_fighting(void)
{
    int64_t now = gr_ms();
    return (P.vto[0] && P.have && now - P.at < HEARD_MS && P.ph <= 1 && now - P.vset_at < HEARD_MS) ||
           (B.on && !B.done);
}

bool gossip_role_fighting(void)
{
    if (!gr_mx) {
        return false;
    }
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    bool f = gr_boss_fighting();
    xSemaphoreGive(gr_mx);
    return f;
}

char *gossip_role_boss_json(void)
{
    if (!gr_mx) {
        return NULL;
    }
    int64_t now = gr_ms();
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    cJSON *o = P.have && now - P.at <= HEARD_MS ? player_obj(now) : NULL;
    xSemaphoreGive(gr_mx);
    char *j = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    return j;
}

// ---------------------------------------------------------------------------

void gr_boss_rx(const char *t, const char *from, os_peer_kind_t kind, int rssi, int64_t at, bool to_role,
                const cJSON *d)
{
    if (!strcmp(t, "vote") && to_role) {
        on_vote(from, kind, d);
    } else if (!strcmp(t, "boss") && !to_role) {
        on_boss(from, kind, rssi, at, d);
    }
}

void gr_boss_tick(int64_t now)
{
    if (B.on) {
        count(now);
        int b, ms, ph;
        boss_now(now, &b, &ms, &ph);
        if (!B.done && ph == 1) {
            if (b >= B.min_beats && B.n >= B.min && B.fin * 3 >= B.n * 2) {
                boss_end(now, true);    // rooted: past minBeats, >= 2/3 of the recent voters finalized
            } else if (b >= B.beats) {
                boss_end(now, false);   // bossSeconds without that: the boss wins
            }
        }
    }
    if (P.vto[0] && !P.claimed && P.vsid == P.sid && now - P.at > CLAIM_SILENT_MS && now - P.at < 120000) {
        claim_end();                    // the boss went quiet (it ended, or walked away)
    }
}

void gr_boss_status(void)
{
    int64_t now = gr_ms();
    if (B.on) {
        int b, ms, ph;
        boss_now(now, &b, &ms, &ph);
        printf("  boss: sid %ld beat %d/%d (rooted from %d) ms %d phase %d fork %d; voters %d recent of %d, %d "
               "confirmed, %d finalized, p %d; votes in %lu, table full %lu\n",
               B.sid, b, B.beats, B.min_beats, ms, ph, B.fk, B.n, B.nv, B.conf, B.fin, B.p, (unsigned long)B.votes_in,
               (unsigned long)B.votes_full);
    }
    if (P.have) {
        int b, ms;
        player_now(now, &b, &ms);
        printf("  fight heard: %s sid %ld beat %d ms %d phase %d p %d %d dBm, %lu broadcasts, %lus ago; "
               "votes %d lockout %d, %lu summaries sent%s\n",
               P.id, P.sid, b, ms, P.ph, P.p, P.rssi, (unsigned long)P.heard, (unsigned long)((now - P.at) / 1000),
               P.votes, P.lockout, (unsigned long)P.votes_sent, P.claimed ? ", claimed" : "");
        printf("  beat jitter: %lu pairs, mean %ld ms, max %d ms, over %d ms %lu\n", (unsigned long)P.jn,
               P.jn ? (long)(P.jsum / P.jn) : 0L, P.jmax, WINDOW_MS, (unsigned long)P.jover);
    }
}

void gr_boss_stats_reset(void)
{
    P.jn = P.jover = 0;
    P.jsum = 0;
    P.jmax = 0;
    P.heard = P.votes_sent = 0;
    B.votes_in = B.votes_full = 0;
}

bool gr_boss_init(void)
{
    B.fk = -1;
    return true;
}
