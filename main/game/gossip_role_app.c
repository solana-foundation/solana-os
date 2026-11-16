// Gossip roles: claims and acks, bids, cures, the roles heard (Solana OS
// 2.4.0; the parent repo's docs/GOSSIP.md §4 "APP messages", §7, §8).
// Builder: fw-roles. Everything here runs under gr_mx (gossip_role_int.h):
// the OS task's handlers and tick, and the Lua task's acting calls.
//
// Both sides of a claim are the OS's: the player's service sends `claim` and
// repeats it until the role's `ack` comes back, then uploads it with the next
// sync; the role's service answers by itself (closeness where the role needs
// it, one `ack` per claimant per 5 s) and never asks its owner. A cure is the
// same shape the other way round: the cured badge's service signs `cured`
// with its own mesh key, because being cured only helps.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "core/os_mem.h"
#include "core/os_mesh.h"
#include "game.h"
#include "gossip_role_int.h"

static const char *TAG = GR_TAG;

#define CLAIMS_PENDING  4
#define CLAIMS_DONE     8           // acknowledged, waiting to upload (≤ 4 a sync)
#define CLAIM_TRIES     7
#define CLAIM_FIGHT_MS  90000       // a fight's claim at the end tries for 90 s: a hall of claimants shares the
                                    // boss's four messages a second (two of them its broadcast), ~50 s of acks
                                    // for 100, and the boss stays on the air 90 s after the end (GOSSIP.md §8)
#define CLAIM_JITTER_MS 3000        // ... each try 0-3 s later than the plain rhythm, so a hall doesn't ask at once
#define CLAIM_EVERY_MS  3000        // a retry outlives the role's 5 s rule on its second try
#define ACK_GAP_MS      5000        // GOSSIP.md §8: one ack per claimant per 5 s
#define SEEN_N          32          // claimants (and curers) remembered for that rule
#define BIDS_MAX        16
#define BID_GAP_MS      2000        // one bid event per badge per 2 s
#define CURES_PENDING   2
#define CURES_DONE      8
#define CURE_TRIES      5
#define CURE_EVERY_MS   3000
#define CURED_GAP_MS    5000
#define HEARD_N         8
#define SYNC_N          4           // claims, cures, heard: ≤ 4 each a sync (GossipSync)

typedef struct {
    char role[OS_MESH_ID_LEN + 1];
    char n[17];
    int c;
    int tries;
    int64_t next;
    int64_t until;                  // a fight's claim: no ack by then, give up
    bool fight;                     // the fight's own claim at the end
} pclaim_t;

typedef struct {
    char role[OS_MESH_ID_LEN + 1];
    char n[17];
    int c, r;
    char g[OS_MESH_TAG_LEN + 1];
} claim_t;

typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    int64_t at;
} seen_t;

typedef struct {
    char from[OS_MESH_ID_LEN + 1];
    char name[25];
    char p[GOSSIP_ADDR_MAX + 1];
    bool fee;
    int rssi;
    int64_t at;
} bid_t;

typedef struct {
    char peer[OS_MESH_ID_LEN + 1];
    char n[17];
    int tries;
    int64_t next;
} pcure_t;

typedef struct {
    char peer[OS_MESH_ID_LEN + 1];
    char n[17];
    char p[GOSSIP_ADDR_MAX + 1];
    char g[OS_MESH_TAG_LEN + 1];
} cure_t;

typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    char ro;
    char g[GOSSIP_G_MAX + 1];
    int r;
    int64_t at;
    bool fresh, sending;            // heard since the last delivered sync; in the one now
} heard_t;

// The tables, in PSRAM when there is some (gr_app_init): SRAM is the badge's scarcest.
static struct {
    pclaim_t pc[CLAIMS_PENDING];
    claim_t claims[CLAIMS_DONE];
    seen_t acked[SEEN_N], cured_for[SEEN_N];
    bid_t bids[BIDS_MAX];
    pcure_t pcu[CURES_PENDING];
    cure_t cures[CURES_DONE];
    heard_t heard[HEARD_N];
} *T;
static int s_npc;
static int s_nclaims, s_claims_sent;
static int s_nbids;
static int s_acks;                  // acks this role sent this session
static int s_npcu;
static int s_ncures, s_cures_sent;
static struct {
    uint32_t claims_sent, acks_got, acks_sent, acks_far, acks_rate, cures_sent, cured_got, cured_sent, bids;
} s_n;

static bool tag_ok(const char *g)
{
    if (!g || strlen(g) != OS_MESH_TAG_LEN) {
        return false;
    }
    for (int i = 0; i < OS_MESH_TAG_LEN; i++) {
        if (!((g[i] >= '0' && g[i] <= '9') || (g[i] >= 'a' && g[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

// The answer seen_allow let through could not be queued: the id may ask again.
static void seen_undo(seen_t *t, const char *id)
{
    for (int i = 0; i < SEEN_N; i++) {
        if (!strcmp(t[i].id, id)) {
            t[i].at = 0;
        }
    }
}

// True (and noted) if id may have one more answer now: one per id per gap.
static bool seen_allow(seen_t *t, const char *id, int64_t now, int gap)
{
    seen_t *old = &t[0];
    for (int i = 0; i < SEEN_N; i++) {
        if (!strcmp(t[i].id, id)) {
            if (now - t[i].at < gap) {
                return false;
            }
            t[i].at = now;
            return true;
        }
        if (t[i].at < old->at) {
            old = &t[i];
        }
    }
    strlcpy(old->id, id, sizeof(old->id));
    old->at = now;
    return true;
}

static const heard_t *heard_get(const char *id)
{
    for (int i = 0; i < HEARD_N; i++) {
        if (T->heard[i].id[0] && !strcmp(T->heard[i].id, id)) {
            return &T->heard[i];
        }
    }
    return NULL;
}

// A role on the air near us, as the radio says (a station, not over a bridge):
// in the peer table or, in a crowded hall, the senders heard (MESH.md).
static bool role_peer(const char *id, os_mesh_peer_t *p)
{
    return gr_id_ok(id) && os_mesh_near(id, p) && p->kind == OS_PEER_STATION && !p->bridged;
}

static cJSON *ev(const char *t)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "t", t);
    return e;
}

// ---------------------------------------------------------------------------
// Claims

static const char *claim_locked(const char *role, int c, bool fight)
{
    os_mesh_peer_t p;
    if (!gr_on()) {
        return "gossip is off";
    }
    if (c < 0 || c > GR_CLAIM_C_MAX) {
        return "count: 0-9999";
    }
    if (!role_peer(role, &p)) {
        return "not near";
    }
    const heard_t *h = heard_get(role);
    if (h && (h->ro == 'm' || h->ro == 'a') && p.rssi < gr_close()) {
        return "too far";               // both ways (GOSSIP.md §8): we must hear it close too
    }
    pclaim_t *pc = NULL;
    for (int i = 0; i < s_npc; i++) {
        if (!strcmp(T->pc[i].role, role)) {
            pc = &T->pc[i];              // a new count for the same role: start again
        }
    }
    if (!pc) {
        if (s_npc == CLAIMS_PENDING) {
            return "busy";
        }
        pc = &T->pc[s_npc++];
    }
    strlcpy(pc->role, role, sizeof(pc->role));
    os_mesh_nonce(pc->n);
    pc->c = c;
    pc->tries = 0;
    pc->fight = fight;
    // A fight's claim: the whole hall claims as it hears the end, so the first try waits 0-3 s.
    pc->next = fight ? gr_ms() + (int64_t)(esp_random() % CLAIM_JITTER_MS) : 0;
    pc->until = fight ? gr_ms() + CLAIM_FIGHT_MS : 0;
    gossip_sync_soon();                 // the 2 s sync while it waits
    return NULL;
}

const char *gossip_role_claim(const char *role, int c)
{
    if (!gr_mx) {
        return "off";
    }
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    const char *err = claim_locked(role ? role : "", c, false);
    if (!err) {
        gr_app_tick(gr_ms());           // the first try goes now
    }
    xSemaphoreGive(gr_mx);
    return err;
}

bool gr_app_claim_auto(const char *role, int c)
{
    return claim_locked(role, c, true) == NULL;
}

static void pc_remove(int i)
{
    memmove(&T->pc[i], &T->pc[i + 1], (s_npc - i - 1) * sizeof(pclaim_t));
    s_npc--;
}

// The role's side: answer by itself (GOSSIP.md §8).
static void on_claim(const char *from, os_peer_kind_t kind, int rssi, const cJSON *d)
{
    gr_live_t L;
    const char *n = game_jstr(d, "n");
    int c = gr_jint(d, "c", 0, GR_CLAIM_C_MAX, -1);
    if (!gr_role_live(&L) || kind != OS_PEER_BADGE || !gr_nonce_ok(n) || c < 0) {
        return;
    }
    if ((L.ro == 'm' || L.ro == 'a') && rssi < gr_close()) {
        s_n.acks_far++;                 // catching and finding are close up
        return;
    }
    if (!seen_allow(T->acked, from, gr_ms(), ACK_GAP_MS)) {
        s_n.acks_rate++;
        return;
    }
    char msg[96], tag[OS_MESH_TAG_LEN + 1];
    snprintf(msg, sizeof(msg), "claim|%s|%s|%s|%d|%d", L.id, from, n, c, rssi);
    os_mesh_tag(L.key, msg, tag);
    cJSON *a = ev("ack");
    cJSON_AddStringToObject(a, "n", n);
    cJSON_AddNumberToObject(a, "c", c);
    cJSON_AddNumberToObject(a, "r", rssi);
    cJSON_AddStringToObject(a, "g", tag);
    if (gr_send_role(from, a)) {
        s_n.acks_sent++;
        s_acks++;
    } else {
        seen_undo(T->acked, from);      // the queue was full: its retry gets the ack, not the 5 s rule
    }
    cJSON_Delete(a);
}

// The claimant's side: the ack that answers one of ours.
static void on_ack(const char *from, os_peer_kind_t kind, const cJSON *d)
{
    const char *n = game_jstr(d, "n"), *g = game_jstr(d, "g");
    int c = gr_jint(d, "c", 0, GR_CLAIM_C_MAX, -1), r = gr_jint(d, "r", -127, 0, 1);
    if (kind != OS_PEER_STATION || !gr_nonce_ok(n) || !tag_ok(g) || r > 0) {
        return;
    }
    for (int i = 0; i < s_npc; i++) {
        pclaim_t *pc = &T->pc[i];
        if (strcmp(pc->role, from) || strcmp(pc->n, n) || pc->c != c) {
            continue;
        }
        if (s_nclaims == CLAIMS_DONE) {
            memmove(&T->claims[0], &T->claims[1], (CLAIMS_DONE - 1) * sizeof(claim_t));   // the oldest goes
            s_nclaims--;
            s_claims_sent = s_claims_sent > 0 ? s_claims_sent - 1 : 0;
        }
        claim_t *k = &T->claims[s_nclaims++];
        strlcpy(k->role, from, sizeof(k->role));
        strlcpy(k->n, n, sizeof(k->n));
        k->c = c;
        k->r = r;
        strlcpy(k->g, g, sizeof(k->g));
        s_n.acks_got++;
        cJSON *e = ev("ack");
        cJSON_AddStringToObject(e, "role", from);
        cJSON_AddNumberToObject(e, "c", c);
        cJSON_AddNumberToObject(e, "r", r);
        cJSON_AddBoolToObject(e, "ok", true);
        gr_event(e);
        pc_remove(i);
        gossip_sync_soon();
        ESP_LOGI(TAG, "claim to %s acknowledged (c %d, %d dBm)", from, c, r);
        return;
    }
    ESP_LOGI(TAG, "an ack from %s for no claim of ours (n %.6s, c %d)", from, n, c);
}

// ---------------------------------------------------------------------------
// Bids

const char *gossip_role_bid(const char *role, const char *addr, bool fee)
{
    if (!gr_mx) {
        return "off";
    }
    os_mesh_peer_t p;
    const char *err = NULL;
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    const heard_t *h = role ? heard_get(role) : NULL;
    if (!gr_on()) {
        err = "gossip is off";
    } else if (!gr_addr_ok(addr)) {
        err = "bad program";
    } else if (!role || !role_peer(role, &p)) {
        err = "not near";
    } else if (h && h->ro != 'l') {
        err = "not a leader";
    } else {
        cJSON *d = ev("bid");
        cJSON_AddStringToObject(d, "p", addr);
        cJSON_AddNumberToObject(d, "fee", fee ? 1 : 0);
        err = gr_send_badge(role, d) ? NULL : "busy";
        cJSON_Delete(d);
    }
    xSemaphoreGive(gr_mx);
    if (!err && fee) {
        // The fee is the server's to take (an Action; GOSSIP.md §7 `bid`): the Leader only reports the bid.
        char a[160];
        snprintf(a, sizeof(a), "{\"t\":\"bid\",\"role\":\"%s\",\"p\":\"%s\",\"fee\":true}", role, addr);
        if (!gossip_action(a)) {
            err = "sent, but the fee could not be queued";
        }
    }
    return err;
}

static void on_bid(const char *from, os_peer_kind_t kind, int rssi, const cJSON *d)
{
    gr_live_t L;
    const char *p = game_jstr(d, "p");
    int fee = gr_jint(d, "fee", 0, 1, -1);
    if (!gr_role_live(&L) || L.ro != 'l' || kind != OS_PEER_BADGE || !gr_addr_ok(p) || fee < 0) {
        return;
    }
    int64_t now = gr_ms();
    bid_t *b = NULL;
    for (int i = 0; i < s_nbids; i++) {
        if (!strcmp(T->bids[i].from, from)) {
            if (now - T->bids[i].at < BID_GAP_MS) {
                return;                 // one a badge every 2 s
            }
            b = &T->bids[i];
        }
    }
    if (!b) {
        if (s_nbids == BIDS_MAX) {
            int old = 0;
            for (int i = 1; i < s_nbids; i++) {
                old = T->bids[i].at < T->bids[old].at ? i : old;
            }
            b = &T->bids[old];           // the latest per badge, the oldest badge goes
        } else {
            b = &T->bids[s_nbids++];
        }
    }
    os_mesh_peer_t peer;
    strlcpy(b->from, from, sizeof(b->from));
    char nm[25] = "";
    if (os_mesh_peer(from, &peer)) {
        game_ascii(nm, sizeof(nm), peer.name);
    }
    strlcpy(b->name, nm, sizeof(b->name));
    strlcpy(b->p, p, sizeof(b->p));
    b->fee = fee == 1;
    b->rssi = rssi;
    b->at = now;
    s_n.bids++;
    cJSON *e = ev("bid");
    cJSON_AddStringToObject(e, "from", from);
    cJSON_AddStringToObject(e, "name", nm);
    cJSON_AddStringToObject(e, "p", p);
    cJSON_AddBoolToObject(e, "fee", b->fee);
    cJSON_AddNumberToObject(e, "rssi", rssi);
    gr_event(e);
}

char *gossip_role_bids_json(void)
{
    if (!gr_mx) {
        return NULL;
    }
    int64_t now = gr_ms();
    cJSON *a = cJSON_CreateArray();
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    gr_live_t L;
    bool leader = gr_role_live(&L) && L.ro == 'l';
    for (int i = 0; leader && i < s_nbids; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "from", T->bids[i].from);
        cJSON_AddStringToObject(e, "name", T->bids[i].name);
        cJSON_AddStringToObject(e, "p", T->bids[i].p);
        cJSON_AddBoolToObject(e, "fee", T->bids[i].fee);
        cJSON_AddNumberToObject(e, "rssi", T->bids[i].rssi);
        cJSON_AddNumberToObject(e, "age", (double)((now - T->bids[i].at) / 1000));
        cJSON_AddItemToArray(a, e);
    }
    xSemaphoreGive(gr_mx);
    char *j = leader ? cJSON_PrintUnformatted(a) : NULL;
    cJSON_Delete(a);
    return j;
}

// ---------------------------------------------------------------------------
// Cures

const char *gossip_role_cure(const char *id)
{
    if (!gr_mx) {
        return "off";
    }
    os_mesh_peer_t p;
    const char *err = NULL;
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    if (!gr_on()) {
        err = "gossip is off";
    } else if (gr_class() != 'v') {
        err = "not a validator";
    } else if (!id || !gr_id_ok(id) || !os_mesh_near(id, &p) || p.kind != OS_PEER_BADGE || p.bridged) {
        err = "not near";
    } else {
        for (int i = 0; i < s_npcu && !err; i++) {
            err = strcmp(T->pcu[i].peer, id) ? NULL : "curing";
        }
        if (!err && s_npcu == CURES_PENDING) {
            err = "busy";
        }
        if (!err) {
            pcure_t *c = &T->pcu[s_npcu++];
            strlcpy(c->peer, id, sizeof(c->peer));
            os_mesh_nonce(c->n);
            c->tries = 0;
            c->next = 0;
            gossip_sync_soon();
            gr_app_tick(gr_ms());
        }
    }
    xSemaphoreGive(gr_mx);
    return err;
}

// Every badge: a Validator asks to remove our spam. Being cured only helps, so
// the service answers by itself, signed with this badge's mesh key.
static void on_cure(const char *from, os_peer_kind_t kind, const cJSON *d)
{
    const char *n = game_jstr(d, "n");
    char me[OS_MESH_ID_LEN + 1], spam[GOSSIP_ADDR_MAX + 1], msg[128], tag[OS_MESH_TAG_LEN + 1];
    if (kind != OS_PEER_BADGE || !gr_nonce_ok(n) || !gr_on() || !gr_me(me) || !gr_held_spam(spam)) {
        return;
    }
    if (!seen_allow(T->cured_for, from, gr_ms(), CURED_GAP_MS)) {
        return;
    }
    snprintf(msg, sizeof(msg), "cure|%s|%s|%s|%s", me, from, n, spam);
    if (!os_mesh_sign(msg, tag)) {
        return;                         // no mesh key yet
    }
    cJSON *a = ev("cured");
    cJSON_AddStringToObject(a, "n", n);
    cJSON_AddStringToObject(a, "p", spam);
    cJSON_AddStringToObject(a, "g", tag);
    if (gr_send_badge(from, a)) {
        s_n.cured_sent++;
    } else {
        seen_undo(T->cured_for, from);  // not queued: its retry gets the answer
    }
    cJSON_Delete(a);
}

// The Validator's side.
static void on_cured(const char *from, os_peer_kind_t kind, const cJSON *d)
{
    const char *n = game_jstr(d, "n"), *p = game_jstr(d, "p"), *g = game_jstr(d, "g");
    if (kind != OS_PEER_BADGE || !gr_nonce_ok(n) || !gr_addr_ok(p) || !tag_ok(g)) {
        return;
    }
    for (int i = 0; i < s_npcu; i++) {
        if (strcmp(T->pcu[i].peer, from) || strcmp(T->pcu[i].n, n)) {
            continue;
        }
        if (s_ncures == CURES_DONE) {
            memmove(&T->cures[0], &T->cures[1], (CURES_DONE - 1) * sizeof(cure_t));
            s_ncures--;
            s_cures_sent = s_cures_sent > 0 ? s_cures_sent - 1 : 0;
        }
        cure_t *k = &T->cures[s_ncures++];
        strlcpy(k->peer, from, sizeof(k->peer));
        strlcpy(k->n, n, sizeof(k->n));
        strlcpy(k->p, p, sizeof(k->p));
        strlcpy(k->g, g, sizeof(k->g));
        memmove(&T->pcu[i], &T->pcu[i + 1], (s_npcu - i - 1) * sizeof(pcure_t));
        s_npcu--;
        s_n.cured_got++;
        cJSON *e = ev("cured");
        cJSON_AddStringToObject(e, "peer", from);
        cJSON_AddStringToObject(e, "p", p);
        cJSON_AddBoolToObject(e, "ok", true);
        gr_event(e);
        gossip_sync_soon();
        ESP_LOGI(TAG, "%s cured of %.8s...", from, p);
        return;
    }
}

// ---------------------------------------------------------------------------
// Roles heard (from the node service's HELLO subscriber)

void gr_app_heard(const gossip_hello_t *h)
{
    if (!gr_id_ok(h->id) || !strchr("lmaz", h->ro) || !h->g[0]) {
        return;
    }
    heard_t *e = NULL, *old = NULL;
    for (int i = 0; i < HEARD_N && !e; i++) {
        heard_t *x = &T->heard[i];
        if (!strcmp(x->id, h->id)) {
            e = x;
        } else if (!old || (old->id[0] && (!x->id[0] || x->at < old->at))) {
            old = x;                    // an empty entry, else the least recently heard
        }
    }
    if (!e) {
        e = old;
        if (e->sending) {
            return;                     // keep what the sync in flight holds
        }
        memset(e, 0, sizeof(*e));
        strlcpy(e->id, h->id, sizeof(e->id));
    }
    e->ro = h->ro;
    game_ascii(e->g, sizeof(e->g), h->g);
    e->r = h->rssi;
    e->at = gr_ms();
    e->fresh = true;
}

// ---------------------------------------------------------------------------
// The seam's parts

void gr_app_rx(const char *t, const char *from, os_peer_kind_t kind, int rssi, bool to_role, const cJSON *d)
{
    if (!strcmp(t, "claim") && to_role) {
        on_claim(from, kind, rssi, d);
    } else if (!strcmp(t, "ack") && !to_role) {
        on_ack(from, kind, d);
    } else if (!strcmp(t, "bid") && to_role) {
        on_bid(from, kind, rssi, d);
    } else if (!strcmp(t, "cure") && !to_role) {
        on_cure(from, kind, d);
    } else if (!strcmp(t, "cured") && !to_role) {
        on_cured(from, kind, d);
    }
}

void gr_app_tick(int64_t now)
{
    for (int i = 0; i < s_npc; i++) {
        pclaim_t *pc = &T->pc[i];
        if (now < pc->next) {
            continue;
        }
        if (pc->fight ? now >= pc->until : pc->tries >= CLAIM_TRIES) {
            cJSON *e = ev("ack");
            cJSON_AddStringToObject(e, "role", pc->role);
            cJSON_AddNumberToObject(e, "c", pc->c);
            cJSON_AddBoolToObject(e, "ok", false);
            gr_event(e);
            ESP_LOGI(TAG, "claim to %s: no ack", pc->role);
            pc_remove(i--);
            continue;
        }
        cJSON *d = ev("claim");
        cJSON_AddStringToObject(d, "n", pc->n);
        cJSON_AddNumberToObject(d, "c", pc->c);
        if (gr_send_badge(pc->role, d)) {
            pc->tries++;
            s_n.claims_sent++;
        }
        cJSON_Delete(d);
        pc->next = now + CLAIM_EVERY_MS + (pc->fight ? (int64_t)(esp_random() % CLAIM_JITTER_MS) : 0);
    }
    for (int i = 0; i < s_npcu; i++) {
        pcure_t *c = &T->pcu[i];
        if (now < c->next) {
            continue;
        }
        if (c->tries >= CURE_TRIES) {
            cJSON *e = ev("cured");
            cJSON_AddStringToObject(e, "peer", c->peer);
            cJSON_AddBoolToObject(e, "ok", false);
            gr_event(e);
            memmove(&T->pcu[i], &T->pcu[i + 1], (s_npcu - i - 1) * sizeof(pcure_t));
            s_npcu--;
            i--;
            continue;
        }
        cJSON *d = ev("cure");
        cJSON_AddStringToObject(d, "n", c->n);
        if (gr_send_badge(c->peer, d)) {
            c->tries++;
            s_n.cures_sent++;
        }
        cJSON_Delete(d);
        c->next = now + CURE_EVERY_MS;
    }
}

static bool add_room(cJSON *arr, cJSON *item, int *room)
{
    char *j = cJSON_PrintUnformatted(item);
    int need = j ? (int)strlen(j) + 1 : 0;
    cJSON_free(j);
    if (!j || need > *room) {
        cJSON_Delete(item);
        return false;
    }
    *room -= need;
    cJSON_AddItemToArray(arr, item);
    return true;
}

void gr_app_fill(cJSON *body, int *room)
{
    s_claims_sent = s_cures_sent = 0;
    for (int i = 0; i < HEARD_N; i++) {
        T->heard[i].sending = false;
    }
    if (s_nclaims && *room > 16) {
        cJSON *a = cJSON_AddArrayToObject(body, "claims");
        *room -= 12;
        for (int i = 0; i < s_nclaims && i < SYNC_N; i++) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "role", T->claims[i].role);
            cJSON_AddStringToObject(e, "n", T->claims[i].n);
            cJSON_AddNumberToObject(e, "c", T->claims[i].c);
            cJSON_AddNumberToObject(e, "r", T->claims[i].r);
            cJSON_AddStringToObject(e, "g", T->claims[i].g);
            if (!add_room(a, e, room)) {
                break;
            }
            s_claims_sent++;
        }
    }
    if (s_ncures && *room > 16) {
        cJSON *a = cJSON_AddArrayToObject(body, "cures");
        *room -= 11;
        for (int i = 0; i < s_ncures && i < SYNC_N; i++) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "peer", T->cures[i].peer);
            cJSON_AddStringToObject(e, "n", T->cures[i].n);
            cJSON_AddStringToObject(e, "p", T->cures[i].p);
            cJSON_AddStringToObject(e, "g", T->cures[i].g);
            if (!add_room(a, e, room)) {
                break;
            }
            s_cures_sent++;
        }
    }
    // Roles heard since the last delivered sync: the live ones first, then the strongest.
    int order[HEARD_N], n = 0;
    for (int i = 0; i < HEARD_N; i++) {
        if (T->heard[i].id[0] && T->heard[i].fresh) {
            order[n++] = i;
        }
    }
    for (int i = 1; i < n; i++) {
        for (int k = i; k > 0; k--) {
            const heard_t *a = &T->heard[order[k - 1]], *b = &T->heard[order[k]];
            bool ra = gr_real(a->id), rb = gr_real(b->id);
            if (ra != rb ? rb : b->r > a->r) {
                int t = order[k];
                order[k] = order[k - 1];
                order[k - 1] = t;
            }
        }
    }
    if (n && *room > 16) {
        cJSON *a = cJSON_AddArrayToObject(body, "heard");
        *room -= 10;
        for (int i = 0; i < n && i < SYNC_N; i++) {
            heard_t *h = &T->heard[order[i]];
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "id", h->id);
            cJSON_AddStringToObject(e, "g", h->g);
            cJSON_AddNumberToObject(e, "r", h->r);
            if (!add_room(a, e, room)) {
                break;
            }
            h->sending = true;
        }
    }
}

void gr_app_done(bool delivered)
{
    if (delivered) {
        memmove(&T->claims[0], &T->claims[s_claims_sent], (s_nclaims - s_claims_sent) * sizeof(claim_t));
        s_nclaims -= s_claims_sent;
        memmove(&T->cures[0], &T->cures[s_cures_sent], (s_ncures - s_cures_sent) * sizeof(cure_t));
        s_ncures -= s_cures_sent;
    }
    for (int i = 0; i < HEARD_N; i++) {
        if (delivered && T->heard[i].sending) {
            T->heard[i].fresh = false;
        }
        T->heard[i].sending = false;
    }
    s_claims_sent = s_cures_sent = 0;
}

bool gr_app_waiting(void)
{
    return s_npc || s_nclaims || s_npcu || s_ncures;
}

void gr_app_session(long sid)
{
    s_nbids = 0;
    s_acks = 0;
    memset(T->acked, 0, sizeof(T->acked));
}

void gr_app_status(void)
{
    printf("  claims: %d trying, %d to upload; sent %lu, acks got %lu; role acks sent %lu (%d this session), "
           "too far %lu, too often %lu\n",
           s_npc, s_nclaims, (unsigned long)s_n.claims_sent, (unsigned long)s_n.acks_got,
           (unsigned long)s_n.acks_sent, s_acks, (unsigned long)s_n.acks_far, (unsigned long)s_n.acks_rate);
    printf("  cures: %d trying, %d to upload; sent %lu, cured got %lu, cured sent %lu; bids %d (%lu heard)\n", s_npcu,
           s_ncures, (unsigned long)s_n.cures_sent, (unsigned long)s_n.cured_got, (unsigned long)s_n.cured_sent,
           s_nbids, (unsigned long)s_n.bids);
    for (int i = 0; i < HEARD_N; i++) {
        if (T->heard[i].id[0]) {
            printf("  heard %s ro %c %d dBm %s%s g %s\n", T->heard[i].id, T->heard[i].ro, T->heard[i].r,
                   gr_real(T->heard[i].id) ? "real" : "unlisted", T->heard[i].fresh ? " (new)" : "", T->heard[i].g);
        }
    }
}

void gr_app_stats_reset(void)
{
    memset(&s_n, 0, sizeof(s_n));
}

bool gr_app_init(void)
{
    T = heap_caps_calloc(1, sizeof(*T), os_mem_bulk_caps());
    return T != NULL;
}
