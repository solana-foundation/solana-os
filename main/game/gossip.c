// Gossip, the node service (Solana OS 2.4.0, game/gossip.h; the parent repo's
// docs/GOSSIP.md §3-§7). Builder: fw-node.
//
// What runs where:
// - Mesh task: on_mesh() keeps a 20-slot "badges heard" table for the peer
//   count and copies HELLOs that carry `g` or `ro` into an 8-slot ring (no
//   parsing), then posts one drain to the OS task.
// - OS task: everything else. The drain parses each HELLO (cJSON, ≤ 232
//   bytes), updates the radar and the hearing table and hands it to the role
//   service. gossip_tick (250 ms, game.c) does the once-a-second exposure
//   maths in integers, the HELLO's own st/g (os_mesh_hello_extra), the sync
//   cadence and the notices. The sync itself runs on the cloud task (os_api);
//   its answer comes back here.
// - A short-lived "gossip_v" task checks records (the PDA in one try, then
//   the Ed25519 signature, ~57 ms each), a batch at a time, so the OS task
//   never stalls on one; it posts each verdict back.
// - Any task: the getters (gossip_*_json, gossip_on, ...) copy out under s_mx.
//   The OS task is the only writer and takes s_mx around every change.
//
// The state lives in one allocation in PSRAM (os_mem_bulk_caps), made at
// boot: ~45 KB with 64 records. The card keeps /sdcard/hh/gossip/{node.json,
// out.jsonl, progs/<address>.json} (§5 "On the card"); without a card it is
// all RAM, 8 records, gone at a restart.

#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "core/ed25519.h"
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"
#include "core/os_data.h"
#include "core/os_mem.h"
#include "core/os_mesh.h"
#include "core/os_service.h"
#include "core/os_settings.h"
#include "core/theme.h"
#include "lua/lua_host.h"
#include "game.h"
#include "gossip.h"
#include "gossip_genome.h"
#include "gossip_role.h"

static const char *TAG = "gossip";

#define SVC_ID          "gossip"
#define NVS_NS          "gossip"
#define DIR_REL         "gossip"
#define NODE_REL        "gossip/node.json"
#define OUT_REL         "gossip/out.jsonl"
#define PROGS_REL       "gossip/progs"
#define ADDR1           (GOSSIP_ADDR_MAX + 1)

#define HEAR_MAX        8           // §5: programs heard at once
#define PEER_MAX        20          // the radar
#define SEEN_MAX        20          // badges heard (any HELLO), for the peer count
#define HOP_MAX         8           // HELLOs waiting for the OS task
#define HOP_JSON        232         // one fragment
#define REC_MAX         64          // §5: exposure records waiting
#define REC_MAX_SRAM    16          // ... on a badge without PSRAM
#define ACT_MAX         4           // §7: actions waiting
#define PROG_MAX        64          // §5: records kept on the card
#define PROG_MAX_RAM    8           // §5: without a card
#define DONE_MAX        16          // programs the server said this node already took
#define DORM_MAX        4           // trait 3's programs adopted lately
#define EV_MAX          4           // notices waiting for their turn
#define VERIFY_BATCH    6

#define HEARD_MS        5000        // §5: "heard in the last 5 s"
#define HEAR_PER_SRC    1           // hearing entries one source may hold at once (§5) ...
#define HEAR_PER_ROLE   4           // ... a listed role id: a Leader's block
#define HEAR_GAP_MS     1000        // one source's HELLOs count for hearing at most once a second
#define BAD_MAX         8           // sources whose record came back bad or unknown ...
#define BAD_MS          (10 * 60 * 1000)    // ... are not heard for 10 min
#define DURABLE_MS      (10 * 60 * 1000)    // trait 4
#define FORGET_MS       60000       // an entry at 0 not heard for this long goes
#define PEER_TTL_MS     OS_MESH_PEER_TTL_MS
#define DORMANT_MS      (30 * 60 * 1000)    // trait 3
#define SYNC_IDLE_MS    60000       // §5
#define SYNC_ROLES_MS   20000
#define SYNC_FAST_MS    2000
#define SYNC_GAP_MS     5000        // "at once (≥ 5 s apart)"
#define SYNC_MIN_MS     2000
#define SYNC_MAX_MS     300000
#define RETRY_MS        10000
#define BODY_MAX        4096        // §7
#define RESP_CAP        4096
#define KEY_RETRY_MS    (2 * 60 * 1000)     // a failed signature fetches the key again, at most this often (a rotation: §3)
#define ANSWER_LOST_MS  30000       // a sync or key request with no answer by then is given up (os_api drops an
                                    // answer when the OS queue stays full; the request itself gives up in 10 s)
#define NOTICE_GAP_MS   10500       // os_service_notify takes one per 10 s
#define SRAM_SYNC_MIN   (24 * 1024) // malloc-able SRAM a sync's answer needs (its cJSON tree)
#define SRAM_HEAR_MIN   (12 * 1024) // below this HELLOs are dropped unread
#define W_SCALE         1000        // exposure is kept ×1000 (§5 says ×10; finer, same maths)

enum { ST_HEARING, ST_EXPOSED, ST_INCUBATING };     // gossip.hearing()'s states
enum { PV_NEW, PV_OK, PV_BAD, PV_BADSHAPE };        // a record's check

typedef struct {
    char a[ADDR1];
    gossip_genome_t gen;
    char from[OS_MESH_ID_LEN + 1];
    char g[GOSSIP_G_MAX + 1];
    int8_t rssi, best;
    uint8_t boost, peers, state;
    uint16_t secs;
    int64_t last_ms;
    int32_t exp, last_w;            // ×W_SCALE
    uint32_t rec_n;                 // its record while exposed or incubating
} hear_t;

typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    char name[25];
    char st[ADDR1];
    char c;                         // class letter, 0 = none heard
    char ro;                        // a role station's letter, 0 = a badge
    uint8_t b;
    int8_t rssi;
    int64_t last_ms;
    int64_t heard_ms;               // the last of its HELLOs that counted for hearing
} peer_t;

typedef struct {
    char id[OS_MESH_ID_LEN + 1];
    bool station;
    int8_t rssi;
    char json[HOP_JSON + 1];
} hop_t;

typedef struct {
    uint32_t n;
    bool sent;                      // went with the sync in flight
    char *json;                     // the Exposure or Action object (with n)
} out_t;

typedef struct {
    char a[ADDR1], par[ADDR1];
    uint8_t ok, fl;
    bool checking;
    int64_t used;
    char *json;                     // the record as the server sent it, compact
} prog_t;

typedef struct {
    char k;                         // 'f' fee, 'q' qos, 'd' deny, 'e' explorer
    int64_t until;
    char p[ADDR1];
} fx_t;

typedef struct {
    char p[ADDR1];
    int64_t until;
} imm_t;

typedef struct {
    // The node (the last Node of a sync), parsed for the service's own rules;
    // node_json keeps the whole object for gossip.me().
    char *node_json;
    int seq, cfg_rev;
    char cls, act[ADDR1];
    int boost, nslots, ninbox, nfx, nimm;
    char slots[5][ADDR1];           // "" = an empty slot
    char inbox[3][ADDR1];
    fx_t fx[6];
    imm_t imm[8];
    char *cfg_json;                 // the last Cfg as sent (node.json keeps it)
    gossip_cfg_t cfg;

    hear_t hear[HEAR_MAX];
    struct {
        char id[OS_MESH_ID_LEN + 1];
        int64_t until;              // esp ms
    } bad[BAD_MAX];                 // sources not heard for now (§5)
    peer_t peer[PEER_MAX];
    char done[DONE_MAX][ADDR1];     // seen/adopted: never heard again
    int ndone;
    struct {
        char a[ADDR1];
        int64_t until;              // server ms
    } dorm[DORM_MAX];

    out_t rec[REC_MAX];
    int nrec;
    out_t act_q[ACT_MAX];
    int nact;
    char *pending[ACT_MAX];         // gossip_action's, waiting for the OS task to number them
    int npending;

    prog_t prog[PROG_MAX];
    int nprog;

    struct {
        char kind[12], text[81];
        uint32_t col;
    } ev[EV_MAX];
    int nev;

    hop_t hop[HOP_MAX];
    int nhop;
} state_t;

static state_t *S;
static SemaphoreHandle_t s_mx, s_hop_mx;
static portMUX_TYPE s_seen_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t h;
    int64_t t;
} s_seen[SEEN_MAX];                 // mesh task writes, OS task counts
static volatile bool s_run;         // the service runs (any task reads)
static volatile bool s_listen;      // the mesh handler copies HELLOs
static volatile bool s_drain_queued, s_soon, s_flush_queued;
static bool s_card;                 // the card holds our files
static uint32_t s_epoch;
static char s_id[OS_MESH_ID_LEN + 1];
static bool s_claimed;
static int s_rec_cap = REC_MAX, s_prog_cap = PROG_MAX;
static uint32_t s_rn, s_an;         // the next record / action number (NVS)
static int64_t s_clock_base, s_clock_at;    // server unix ms at esp ms
static int64_t s_synced;            // server ms of the last good sync, 0 = never
static int64_t s_next_sync, s_last_sync, s_srv_next = SYNC_IDLE_MS;
static void *s_tok;                 // the sync in flight
static int64_t s_tok_at;
static int s_last_status;           // HTTP status of the last sync (0 = none)
static char s_why[48];              // the status line's reason when it isn't running well
static int64_t s_last_sec, s_notice_at;
// Notices held while this badge is in a boss fight (a toast would cover the fight's lower
// bars): the last HELD_MAX, the rest counted. Under s_mx; shown after the fight, one a turn.
#define HELD_MAX        3
static struct {
    char text[81];
    uint32_t col;
} s_held[HELD_MAX];
static int s_nheld, s_held_drop;
static char s_extra[GOSSIP_ADDR_MAX + GOSSIP_G_MAX + 24];  // the HELLO fields set last
static int64_t s_extra_w = -1;
static uint8_t s_key[32];
static bool s_has_key, s_key_asked;
static void *s_key_tok;             // the key request in flight
static int64_t s_key_at;
static char s_key_srv[96];
static volatile bool s_verifying;
static int s_verify_stack = -1;     // the checker's stack never used, bytes (for `gossip status`)
static int64_t s_verify_us;         // the last check's time
static uint32_t s_hops_dropped, s_hellos, s_rx_bad;

static void flush(void *arg);

// ---------------------------------------------------------------------------
// Small things

static int64_t now_ms(void)
{
    return game_now_ms();
}

static inline void lock(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
}

static inline void unlock(void)
{
    xSemaphoreGive(s_mx);
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = heap_caps_malloc(n, os_mem_bulk_caps());
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

static void short_addr(const char *a, char *out, size_t n)
{
    size_t l = strlen(a);
    if (l > 10) {
        snprintf(out, n, "%.4s..%s", a, a + l - 4);
    } else {
        strlcpy(out, a, n);
    }
}

const char *gossip_rarity_name(int rarity)
{
    static const char *const names[] = {"common", "rare", "epic", "legendary"};
    return names[rarity >= 0 && rarity <= 3 ? rarity : 0];
}

static uint32_t nvs_u32(const char *key)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void nvs_set(const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool addr_in(const char *a, const char list[][ADDR1], int n)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(list[i], a)) {
            return true;
        }
    }
    return false;
}

// The server's clock: unix ms, 0 without one.
int64_t gossip_server_ms(void)
{
    int64_t base = s_clock_base;
    return base ? base + (now_ms() - s_clock_at) : 0;
}

// ---------------------------------------------------------------------------
// Cfg (§7) and its defaults

// Before the first sync's Cfg: the server's tuned defaults (its simulator
// found the contract's first numbers spread one program through a third of a
// 500-badge hall in five minutes). After it, always the Cfg the server sent.
static void cfg_defaults(gossip_cfg_t *c)
{
    static const int rssi[4] = {-50, -60, -70, -80}, wt[4] = {10, 5, 1, 0};
    memset(c, 0, sizeof(*c));
    c->thr = 6000;
    c->decay = 10;
    memcpy(c->wt_rssi, rssi, sizeof(rssi));
    memcpy(c->wt, wt, sizeof(wt));
    c->offpeak_from = c->offpeak_to = -1;
    c->close = -55;
    c->name_nodes = 50;
}

static int jint(const cJSON *o, const char *k, int def, int lo, int hi)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (!cJSON_IsNumber(v)) {
        return def;
    }
    double d = v->valuedouble;
    return d < lo ? lo : d > hi ? hi : (int)d;
}

static int64_t jms(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) && v->valuedouble > 0 && v->valuedouble < 9e15 ? (int64_t)v->valuedouble : 0;
}

static void cfg_parse(const cJSON *j, gossip_cfg_t *c)
{
    cfg_defaults(c);
    if (!cJSON_IsObject(j)) {
        return;
    }
    // Bounded so exposure ×W_SCALE stays inside int32 (thr ≤ 1e6 → 1e9).
    c->thr = jint(j, "thr", 6000, 1, 1000000);
    c->decay = jint(j, "decay", 10, 0, 1000000);
    const cJSON *wt = cJSON_GetObjectItem(j, "wt");
    if (cJSON_IsArray(wt) && cJSON_GetArraySize(wt) >= 1) {
        int n = 0;
        const cJSON *row;
        cJSON_ArrayForEach(row, wt) {
            const cJSON *r = cJSON_GetArrayItem(row, 0), *w = cJSON_GetArrayItem(row, 1);
            if (n < 4 && cJSON_IsNumber(r) && cJSON_IsNumber(w) && r->valuedouble >= -127 && r->valuedouble <= 0
                && w->valuedouble >= 0 && w->valuedouble <= 1000) {
                c->wt_rssi[n] = (int)r->valuedouble;
                c->wt[n] = (int)w->valuedouble;
                n++;
            }
        }
        for (int i = n; i < 4; i++) {
            c->wt_rssi[i] = -127;
            c->wt[i] = 0;
        }
    }
    const cJSON *op = cJSON_GetObjectItem(j, "offpeak");
    if (cJSON_IsArray(op) && cJSON_GetArraySize(op) == 2) {
        int f = (int)cJSON_GetNumberValue(cJSON_GetArrayItem(op, 0));
        int t = (int)cJSON_GetNumberValue(cJSON_GetArrayItem(op, 1));
        if (f >= 0 && f < 24 && t >= 0 && t <= 24) {
            c->offpeak_from = f;
            c->offpeak_to = t;
        }
    }
    c->close = jint(j, "close", -55, -127, 0);
    c->name_nodes = jint(j, "nameNodes", 50, 0, 1000000);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "roles")) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(e, "id"));
        const char *ro = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ro"));
        if (c->nroles < GOSSIP_CFG_ROLES && id && strlen(id) == OS_MESH_ID_LEN && ro && strlen(ro) == 1
            && strchr("lmaz", ro[0])) {
            strlcpy(c->roles[c->nroles].id, id, sizeof(c->roles[0].id));
            c->roles[c->nroles].ro = ro[0];
            c->nroles++;
        }
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "spam")) {
        if (c->nspam < GOSSIP_CFG_SPAM && cJSON_IsString(e) && gg_addr_ok(e->valuestring)) {
            strlcpy(c->spam[c->nspam++], e->valuestring, ADDR1);
        }
    }
}

void gossip_cfg(gossip_cfg_t *out)
{
    if (!S) {
        cfg_defaults(out);
        return;
    }
    lock();
    *out = S->cfg;
    unlock();
}

// ---------------------------------------------------------------------------
// The node (§7 Node), parsed for the rules the badge applies itself

static bool node_holds(const char *a)
{
    if (!a[0]) {
        return false;
    }
    if (!strcmp(S->act, a)) {
        return true;
    }
    for (int i = 0; i < S->nslots; i++) {
        if (!strcmp(S->slots[i], a)) {
            return true;
        }
    }
    for (int i = 0; i < S->ninbox; i++) {
        if (!strcmp(S->inbox[i], a)) {
            return true;
        }
    }
    return false;
}

static void node_parse(const cJSON *n)
{
    const char *cls = cJSON_GetStringValue(cJSON_GetObjectItem(n, "cls"));
    S->cls = cls && strlen(cls) == 1 && strchr("rvib", cls[0]) ? cls[0] : 0;
    const char *act = cJSON_GetStringValue(cJSON_GetObjectItem(n, "act"));
    strlcpy(S->act, act && gg_addr_ok(act) ? act : "", ADDR1);
    S->boost = jint(n, "boost", 0, 0, 9);
    S->nslots = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(n, "slots")) {
        if (S->nslots < 5) {
            const char *a = cJSON_GetStringValue(e);
            strlcpy(S->slots[S->nslots++], a && gg_addr_ok(a) ? a : "", ADDR1);
        }
    }
    S->ninbox = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(n, "inbox")) {
        if (S->ninbox < 3 && cJSON_IsString(e) && gg_addr_ok(e->valuestring)) {
            strlcpy(S->inbox[S->ninbox++], e->valuestring, ADDR1);
        }
    }
    S->nfx = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(n, "fx")) {
        const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(e, "k"));
        const char *p = cJSON_GetStringValue(cJSON_GetObjectItem(e, "p"));
        char kc = !k ? 0 : !strcmp(k, "fee") ? 'f' : !strcmp(k, "qos") ? 'q' : !strcmp(k, "deny") ? 'd'
                         : !strcmp(k, "explorer") ? 'e' : 0;
        if (kc && S->nfx < 6) {
            fx_t *f = &S->fx[S->nfx++];
            f->k = kc;
            f->until = jms(e, "until");
            strlcpy(f->p, p && gg_addr_ok(p) ? p : "", ADDR1);
        }
    }
    S->nimm = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(n, "imm")) {
        const char *p = cJSON_GetStringValue(cJSON_GetObjectItem(e, "p"));
        if (p && gg_addr_ok(p) && S->nimm < 8) {
            strlcpy(S->imm[S->nimm].p, p, ADDR1);
            S->imm[S->nimm].until = jms(e, "until");
            S->nimm++;
        }
    }
}

char gossip_class(void)
{
    return S ? S->cls : 0;
}

bool gossip_held_spam(char out[GOSSIP_ADDR_MAX + 1])
{
    out[0] = 0;
    if (!S) {
        return false;
    }
    lock();
    for (int i = 0; i < S->nslots && !out[0]; i++) {
        if (S->slots[i][0] && addr_in(S->slots[i], (const char (*)[ADDR1])S->cfg.spam, S->cfg.nspam)) {
            strlcpy(out, S->slots[i], ADDR1);
        }
    }
    unlock();
    return out[0] != 0;
}

bool gossip_genome(const char *addr, gossip_genome_t *out)
{
    uint8_t a[32];
    if (!gg_b58_decode32(addr, a)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    gg_decode(a, out);
    return true;
}

// ---------------------------------------------------------------------------
// Records (§3 "Record"): kept, checked off the OS task, on the card

static prog_t *prog_find(const char *a)
{
    for (int i = 0; i < S->nprog; i++) {
        if (!strcmp(S->prog[i].a, a)) {
            return &S->prog[i];
        }
    }
    return NULL;
}

static void prog_path(char *out, size_t n, const char *a)
{
    snprintf(out, n, PROGS_REL "/%s.json", a);
}

static void prog_save(const prog_t *p)
{
    if (!s_card || !p->json) {
        return;
    }
    // The record as sent, plus "ok" once its signature checked (§5).
    char rel[80];
    prog_path(rel, sizeof(rel), p->a);
    cJSON *j = cJSON_Parse(p->json);
    if (!j) {
        return;
    }
    cJSON_DeleteItemFromObject(j, "ok");
    if (p->ok == PV_OK) {
        cJSON_AddTrueToObject(j, "ok");
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (s) {
        os_data_write(rel, s, strlen(s));
        cJSON_free(s);
    }
}

static bool prog_held(const char *a)
{
    if (node_holds(a)) {
        return true;
    }
    for (int i = 0; i < HEAR_MAX; i++) {
        if (!strcmp(S->hear[i].a, a)) {
            return true;
        }
    }
    return false;
}

// Room for one more: the least recently used record nobody holds goes.
static prog_t *prog_slot(void)
{
    if (S->nprog < s_prog_cap) {
        return &S->prog[S->nprog++];
    }
    int victim = -1;
    for (int pass = 0; pass < 2 && victim < 0; pass++) {
        for (int i = 0; i < S->nprog; i++) {
            if ((pass || !prog_held(S->prog[i].a)) && !S->prog[i].checking
                && (victim < 0 || S->prog[i].used < S->prog[victim].used)) {
                victim = i;
            }
        }
    }
    if (victim < 0) {
        return NULL;
    }
    prog_t *p = &S->prog[victim];
    if (s_card) {
        char rel[80];
        prog_path(rel, sizeof(rel), p->a);
        os_data_remove(rel);
    }
    free(p->json);
    memset(p, 0, sizeof(*p));
    return p;
}

// A record from the server or the card. ok: what the card says (a record
// from the server is checked again).
static void prog_put(const cJSON *r, bool from_card)
{
    const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(r, "a"));
    const char *par = cJSON_GetStringValue(cJSON_GetObjectItem(r, "par"));
    if (!a || !gg_addr_ok(a) || !par || (par[0] && !gg_addr_ok(par))) {
        return;
    }
    cJSON *copy = cJSON_Duplicate(r, true);
    if (!copy) {
        return;
    }
    bool ok_card = from_card && cJSON_IsTrue(cJSON_GetObjectItem(copy, "ok"));
    cJSON_DeleteItemFromObject(copy, "ok");
    char *text = cJSON_PrintUnformatted(copy);
    cJSON_Delete(copy);
    char *json = text ? dup_str(text) : NULL;
    cJSON_free(text);
    if (!json) {
        return;
    }
    lock();
    prog_t *p = prog_find(a);
    bool same_sig = false;
    if (p && p->json) {
        // A record we have again (a new name or node count): its signature is
        // the same unless a signed field changed.
        cJSON *old = cJSON_Parse(p->json);
        const char *os = cJSON_GetStringValue(cJSON_GetObjectItem(old, "sig"));
        const char *ns = cJSON_GetStringValue(cJSON_GetObjectItem(r, "sig"));
        same_sig = os && ns && !strcmp(os, ns) && jint(old, "fl", -1, -1, 99) == jint(r, "fl", -2, -2, 99);
        cJSON_Delete(old);
    }
    if (!p) {
        p = prog_slot();
    }
    if (!p) {
        unlock();
        free(json);
        return;
    }
    free(p->json);
    if (!same_sig) {
        p->ok = ok_card ? PV_OK : PV_NEW;
        p->checking = false;
    }
    strlcpy(p->a, a, ADDR1);
    strlcpy(p->par, par, ADDR1);
    p->fl = (uint8_t)jint(r, "fl", 0, 0, 15);
    p->json = json;
    p->used = now_ms();
    unlock();
    if (!from_card) {
        prog_save(p);
    }
}

// The fields the server signed, from a record's JSON.
static bool record_fields(const char *json, gg_record_t *r)
{
    cJSON *j = cJSON_Parse(json);
    memset(r, 0, sizeof(*r));
    bool ok = false;
    if (j) {
        const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(j, "a"));
        const char *par = cJSON_GetStringValue(cJSON_GetObjectItem(j, "par"));
        const char *seed = cJSON_GetStringValue(cJSON_GetObjectItem(j, "seed"));
        const char *sig = cJSON_GetStringValue(cJSON_GetObjectItem(j, "sig"));
        const cJSON *gen = cJSON_GetObjectItem(j, "gen"), *bump = cJSON_GetObjectItem(j, "bump");
        const cJSON *fl = cJSON_GetObjectItem(j, "fl");
        ok = a && par && seed && sig && cJSON_IsNumber(gen) && cJSON_IsNumber(bump) && cJSON_IsNumber(fl)
             && strlen(a) <= GOSSIP_ADDR_MAX && strlen(par) <= GOSSIP_ADDR_MAX && strlen(seed) < sizeof(r->seed)
             && gen->valuedouble == (int)gen->valuedouble && bump->valuedouble == (int)bump->valuedouble
             && fl->valuedouble == (int)fl->valuedouble && gg_unhex(sig, r->sig, 64);
        if (ok) {
            strlcpy(r->a, a, sizeof(r->a));
            strlcpy(r->par, par, sizeof(r->par));
            strlcpy(r->seed, seed, sizeof(r->seed));
            r->gen = (int)gen->valuedouble;
            r->bump = (int)bump->valuedouble;
            r->fl = (int)fl->valuedouble;
        }
        cJSON_Delete(j);
    }
    return ok;
}

typedef struct {
    int n;
    uint8_t key[32];
    gg_record_t r[VERIFY_BATCH];
} vbatch_t;

typedef struct {
    char a[ADDR1];
    uint8_t ok;
} vres_t;

static void key_fetch(void);

static void verify_done(void *arg)
{
    vres_t *v = arg;
    if (S) {
        lock();
        prog_t *p = prog_find(v->a);
        if (p && p->checking) {
            p->checking = false;
            p->ok = v->ok;
        }
        unlock();
        if (p && v->ok == PV_OK) {
            prog_save(p);
        }
        if (v->ok == PV_BAD) {
            ESP_LOGW(TAG, "record %s: the signature doesn't check", v->a);
            key_fetch();        // the server's key may have changed (§3): fetched again, rate-limited
        } else if (v->ok == PV_BADSHAPE) {
            ESP_LOGW(TAG, "record %s: not a program address", v->a);
        }
    }
    free(v);
}

// The checker: one batch, then it ends. ~57 ms a signature, ~5 ms a PDA.
static void verify_task(void *arg)
{
    vbatch_t *b = arg;
    for (int i = 0; i < b->n; i++) {
        int64_t t0 = esp_timer_get_time();
        uint8_t ok = PV_BADSHAPE;
        if (gg_record_shape_ok(&b->r[i])) {
            char msg[200];
            int len = gg_record_msg(&b->r[i], msg, sizeof(msg));
            ok = len > 0 && ed25519_verify(b->key, (const uint8_t *)msg, (size_t)len, b->r[i].sig) ? PV_OK : PV_BAD;
        }
        s_verify_us = esp_timer_get_time() - t0;
        vres_t *v = malloc(sizeof(*v));
        if (v) {
            strlcpy(v->a, b->r[i].a, sizeof(v->a));
            v->ok = ok;
            while (!os_post(verify_done, v)) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        vTaskDelay(1);      // let the OS task in between checks
    }
    s_verify_stack = (int)uxTaskGetStackHighWaterMark(NULL) * (int)sizeof(StackType_t);
    heap_caps_free(b);
    s_verifying = false;
    vTaskDelete(NULL);
}


// OS task: start the checker on the records waiting, if it isn't running.
static void verify_kick(void)
{
    if (s_verifying || !s_has_key) {
        if (!s_has_key) {
            key_fetch();
        }
        return;
    }
    vbatch_t *b = heap_caps_calloc(1, sizeof(*b), os_mem_bulk_caps());
    if (!b) {
        return;
    }
    memcpy(b->key, s_key, 32);
    lock();
    for (int i = 0; i < S->nprog && b->n < VERIFY_BATCH; i++) {
        prog_t *p = &S->prog[i];
        if (p->ok == PV_NEW && !p->checking && p->json) {
            if (record_fields(p->json, &b->r[b->n])) {
                p->checking = true;
                b->n++;
            } else {
                p->ok = PV_BADSHAPE;
            }
        }
    }
    unlock();
    if (!b->n) {
        heap_caps_free(b);
        return;
    }
    s_verifying = true;
    // 4 KB: ed25519_verify's deepest frame measured at ~2.3 KB (see `gossip status`).
    if (xTaskCreate(verify_task, "gossip_v", 4096, b, 1, NULL) != pdPASS) {
        s_verifying = false;
        lock();
        for (int i = 0; i < S->nprog; i++) {
            S->prog[i].checking = false;
        }
        unlock();
        heap_caps_free(b);
    }
}

// The server's key (§3): NVS "gossip/key" with the server it came from.
static void key_load(void)
{
    nvs_handle_t h;
    char hex[65] = "", srv[96] = "";
    size_t n = sizeof(hex), m = sizeof(srv);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, "key", hex, &n);
        nvs_get_str(h, "srv", srv, &m);
        nvs_close(h);
    }
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return;
    }
    os_cloud_get_info(in);
    s_has_key = gg_unhex(hex, s_key, 32) && !strcmp(srv, in->server);
    strlcpy(s_key_srv, in->server, sizeof(s_key_srv));
    free(in);
}

static void key_answer(int status, const char *body, void *ctx)
{
    if (!ctx || ctx != s_key_tok) {
        return;                     // one we gave up on
    }
    s_key_tok = NULL;
    s_key_asked = false;
    cJSON *j = status == 200 ? cJSON_Parse(body) : NULL;
    const char *hex = cJSON_GetStringValue(cJSON_GetObjectItem(j, "key"));
    uint8_t k[32];
    if (hex && gg_unhex(hex, k, 32)) {
        bool changed = !s_has_key || memcmp(k, s_key, 32);
        memcpy(s_key, k, 32);
        s_has_key = true;
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "key", hex);
            nvs_set_str(h, "srv", s_key_srv);
            nvs_commit(h);
            nvs_close(h);
        }
        if (changed && S) {
            // A new key: what failed under the old one is checked again.
            lock();
            for (int i = 0; i < S->nprog; i++) {
                if (S->prog[i].ok == PV_BAD) {
                    S->prog[i].ok = PV_NEW;
                }
            }
            unlock();
            ESP_LOGI(TAG, "the server's key: %.16s...", hex);
        }
    }
    cJSON_Delete(j);
}

static void key_fetch(void)
{
    int64_t now = now_ms();
    if (s_key_asked && now - s_key_at > ANSWER_LOST_MS) {
        os_api_cancel(s_key_tok);   // its answer was lost: ask again
        s_key_tok = NULL;
        s_key_asked = false;
    }
    if (s_key_asked || (s_key_at && now - s_key_at < KEY_RETRY_MS && s_has_key)
        || (s_key_at && now - s_key_at < 30000)) {
        return;
    }
    void *tok = os_api_token();
    if (os_api_call("GET", "/api/device/gossip/key", NULL, 256, key_answer, tok) == ESP_OK) {
        s_key_tok = tok;
        s_key_asked = true;
        s_key_at = now;
    }
}

// ---------------------------------------------------------------------------
// The card

static void out_save(void)
{
    if (!s_card) {
        return;
    }
    size_t cap = 64;
    for (int i = 0; i < S->nrec; i++) {
        cap += strlen(S->rec[i].json) + 8;
    }
    for (int i = 0; i < S->nact; i++) {
        cap += strlen(S->act_q[i].json) + 8;
    }
    char *buf = heap_caps_malloc(cap, os_mem_bulk_caps());
    if (!buf) {
        return;
    }
    size_t len = 0;
    for (int i = 0; i < S->nrec; i++) {
        len += (size_t)snprintf(buf + len, cap - len, "{\"r\":%s}\n", S->rec[i].json);
    }
    for (int i = 0; i < S->nact; i++) {
        len += (size_t)snprintf(buf + len, cap - len, "{\"a\":%s}\n", S->act_q[i].json);
    }
    if (len) {
        os_data_write(OUT_REL, buf, len);
    } else {
        os_data_remove(OUT_REL);
    }
    heap_caps_free(buf);
}

static void node_save(void)
{
    if (!s_card) {
        return;
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "seq", S->seq);
    cJSON_AddNumberToObject(j, "cfgRev", S->cfg_rev);
    if (S->node_json) {
        cJSON_AddItemToObject(j, "node", cJSON_Parse(S->node_json));
    }
    if (S->cfg_json) {
        cJSON_AddItemToObject(j, "conf", cJSON_Parse(S->cfg_json));
    }
    cJSON *done = cJSON_AddArrayToObject(j, "done");
    for (int i = 0; i < S->ndone; i++) {
        cJSON_AddItemToArray(done, cJSON_CreateString(S->done[i]));
    }
    cJSON *dorm = cJSON_AddArrayToObject(j, "dorm");
    for (int i = 0; i < DORM_MAX; i++) {
        if (S->dorm[i].a[0]) {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "a", S->dorm[i].a);
            cJSON_AddNumberToObject(d, "until", (double)S->dorm[i].until);
            cJSON_AddItemToArray(dorm, d);
        }
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (s) {
        os_data_write(NODE_REL, s, strlen(s));
        cJSON_free(s);
    }
}

static void out_clear(out_t *q, int *n)
{
    for (int i = 0; i < *n; i++) {
        free(q[i].json);
    }
    memset(q, 0, sizeof(out_t) * (size_t)*n);
    *n = 0;
}

// Everything of the owner's goes: a new owner, a wipe, a release.
static void forget_all(void)
{
    lock();
    out_clear(S->rec, &S->nrec);
    out_clear(S->act_q, &S->nact);
    for (int i = 0; i < S->npending; i++) {
        free(S->pending[i]);
        S->pending[i] = NULL;
    }
    S->npending = 0;
    for (int i = 0; i < S->nprog; i++) {
        free(S->prog[i].json);
    }
    memset(S->prog, 0, sizeof(S->prog));
    S->nprog = 0;
    free(S->node_json);
    free(S->cfg_json);
    S->node_json = S->cfg_json = NULL;
    S->seq = S->cfg_rev = 0;
    S->cls = 0;
    S->act[0] = 0;
    S->boost = S->nslots = S->ninbox = S->nfx = S->nimm = 0;
    cfg_defaults(&S->cfg);
    memset(S->hear, 0, sizeof(S->hear));
    memset(S->bad, 0, sizeof(S->bad));
    memset(S->done, 0, sizeof(S->done));
    S->ndone = 0;
    memset(S->dorm, 0, sizeof(S->dorm));
    S->nev = 0;
    unlock();
}

static void out_load_line(const char *line)
{
    cJSON *j = cJSON_Parse(line);
    const cJSON *r = cJSON_GetObjectItem(j, "r"), *a = cJSON_GetObjectItem(j, "a");
    const cJSON *o = cJSON_IsObject(r) ? r : a;
    const cJSON *n = cJSON_GetObjectItem(o, "n");
    if (cJSON_IsObject(o) && cJSON_IsNumber(n) && n->valuedouble >= 1) {
        char *s = cJSON_PrintUnformatted(o);
        char *json = s ? dup_str(s) : NULL;
        cJSON_free(s);
        out_t *q = r ? S->rec : S->act_q;
        int *cnt = r ? &S->nrec : &S->nact;
        int cap = r ? s_rec_cap : ACT_MAX;
        if (json && *cnt < cap) {
            q[*cnt].n = (uint32_t)n->valuedouble;
            q[*cnt].json = json;
            (*cnt)++;
            uint32_t *next = r ? &s_rn : &s_an;
            if (q[*cnt - 1].n >= *next) {
                *next = q[*cnt - 1].n + 1;     // never reuse a number the server may have seen
            }
            if (r) {
                // The program shows as incubating again (§5), its exposure full.
                const char *p = cJSON_GetStringValue(cJSON_GetObjectItem(o, "p"));
                for (int i = 0; p && i < HEAR_MAX; i++) {
                    hear_t *h = &S->hear[i];
                    if (!h->a[0]) {
                        uint8_t b[32];
                        if (gg_b58_decode32(p, b)) {
                            strlcpy(h->a, p, ADDR1);
                            gg_decode(b, &h->gen);
                            strlcpy(h->from, cJSON_GetStringValue(cJSON_GetObjectItem(o, "f")) ?: "", sizeof(h->from));
                            h->state = ST_INCUBATING;
                            h->rec_n = q[*cnt - 1].n;
                            h->exp = S->cfg.thr * W_SCALE;
                            h->last_ms = now_ms();
                        }
                        break;
                    }
                }
            }
        } else {
            free(json);
        }
    }
    cJSON_Delete(j);
}

// Boot (and after a wipe): what the card holds.
static void load_all(void)
{
    s_card = os_data_ready() && os_data_mkdirs(PROGS_REL);
    s_prog_cap = s_card ? (os_mem_psram() ? PROG_MAX : 16) : PROG_MAX_RAM;
    if (!s_card) {
        return;
    }
    size_t len = 0;
    char *raw = os_data_read(NODE_REL, 12 * 1024, &len);
    cJSON *j = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (cJSON_IsObject(j)) {
        lock();
        const cJSON *node = cJSON_GetObjectItem(j, "node");
        if (cJSON_IsObject(node)) {
            char *s = cJSON_PrintUnformatted(node);
            S->node_json = s ? dup_str(s) : NULL;
            cJSON_free(s);
            node_parse(node);
            S->seq = jint(j, "seq", 0, 0, INT32_MAX);
        }
        const cJSON *conf = cJSON_GetObjectItem(j, "conf");
        if (cJSON_IsObject(conf)) {
            char *s = cJSON_PrintUnformatted(conf);
            S->cfg_json = s ? dup_str(s) : NULL;
            cJSON_free(s);
            cfg_parse(conf, &S->cfg);
            S->cfg_rev = jint(j, "cfgRev", 0, 0, INT32_MAX);
        }
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "done")) {
            if (S->ndone < DONE_MAX && cJSON_IsString(e) && gg_addr_ok(e->valuestring)) {
                strlcpy(S->done[S->ndone++], e->valuestring, ADDR1);
            }
        }
        int k = 0;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "dorm")) {
            const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(e, "a"));
            if (k < DORM_MAX && a && gg_addr_ok(a)) {
                strlcpy(S->dorm[k].a, a, ADDR1);
                S->dorm[k].until = jms(e, "until");
                k++;
            }
        }
        unlock();
    }
    cJSON_Delete(j);

    raw = os_data_read(OUT_REL, 40 * 1024, &len);
    if (raw) {
        lock();
        for (char *line = raw, *nl; line && *line; line = nl) {
            nl = strchr(line, '\n');
            if (nl) {
                *nl++ = 0;
            }
            out_load_line(line);
        }
        unlock();
        free(raw);
    }

    char path[96];
    os_data_path(path, sizeof(path), PROGS_REL);
    DIR *d = opendir(path);
    struct dirent *de;
    int n = 0;
    while (d && (de = readdir(d)) && n < s_prog_cap) {
        size_t l = strlen(de->d_name);
        if (l < 6 || l > GOSSIP_ADDR_MAX + 5 || strcmp(de->d_name + l - 5, ".json")) {
            continue;
        }
        char rel[80];
        snprintf(rel, sizeof(rel), PROGS_REL "/%.*s", (int)l, de->d_name);
        char *rec = os_data_read(rel, 2048, NULL);
        cJSON *r = rec ? cJSON_Parse(rec) : NULL;
        free(rec);
        if (cJSON_IsObject(r)) {
            prog_put(r, true);
            n++;
        }
        cJSON_Delete(r);
    }
    if (d) {
        closedir(d);
    }
    ESP_LOGI(TAG, "card: node seq %d, %d records waiting, %d actions, %d programs", S->seq, S->nrec, S->nact,
             S->nprog);
}

// ---------------------------------------------------------------------------
// Hearing (§5)

static bool immune(const char *a, int64_t srv)
{
    for (int i = 0; i < S->nimm; i++) {
        if (!strcmp(S->imm[i].p, a) && (!srv || !S->imm[i].until || S->imm[i].until > srv)) {
            return true;
        }
    }
    return false;
}

static bool fx_on(char k, int64_t srv)
{
    for (int i = 0; i < S->nfx; i++) {
        if (S->fx[i].k == k && (!srv || !S->fx[i].until || S->fx[i].until > srv)) {
            return true;
        }
    }
    return false;
}

// A deny-list covers the program's family: itself, its parent and its
// children, as far as the records we hold say (the server decides anyway).
static bool denied(const char *a, int64_t srv)
{
    for (int i = 0; i < S->nfx; i++) {
        const fx_t *f = &S->fx[i];
        if (f->k != 'd' || !f->p[0] || (srv && f->until && f->until <= srv)) {
            continue;
        }
        if (!strcmp(f->p, a)) {
            return true;
        }
        const prog_t *pa = prog_find(a), *pd = prog_find(f->p);
        if ((pa && !strcmp(pa->par, f->p)) || (pd && !strcmp(pd->par, a))) {
            return true;
        }
    }
    return false;
}

// Not to be heard at all (§5 "Skipped entirely").
static bool skipped(const char *a, int64_t srv)
{
    return node_holds(a) || immune(a, srv) || addr_in(a, (const char (*)[ADDR1])S->done, S->ndone)
           || fx_on('q', srv) || denied(a, srv);
}

static peer_t *peer_get(const char *id, bool make)
{
    int64_t now = now_ms();
    peer_t *free_one = NULL, *oldest = NULL;
    for (int i = 0; i < PEER_MAX; i++) {
        peer_t *p = &S->peer[i];
        if (p->id[0] && !strcmp(p->id, id)) {
            return p;
        }
        if (!p->id[0] || now - p->last_ms > PEER_TTL_MS) {
            free_one = free_one ? free_one : p;
        } else if (!oldest || p->last_ms < oldest->last_ms) {
            oldest = p;
        }
    }
    if (!make) {
        return NULL;
    }
    peer_t *p = free_one ? free_one : oldest;
    memset(p, 0, sizeof(*p));
    strlcpy(p->id, id, sizeof(p->id));
    return p;
}

// Under the lock: a source whose record came back bad or unknown, for 10 min.
static bool bad_source(const char *id, int64_t now)
{
    for (int i = 0; i < BAD_MAX; i++) {
        if (S->bad[i].id[0] && !strcmp(S->bad[i].id, id) && now < S->bad[i].until) {
            return true;
        }
    }
    return false;
}

// Under the lock: not heard for BAD_MS, and what we hear from it now goes.
static void bad_add(const char *id, int64_t now)
{
    int slot = 0;
    for (int i = 0; i < BAD_MAX; i++) {
        if (!strcmp(S->bad[i].id, id) || now >= S->bad[i].until) {
            slot = i;
            break;
        }
        slot = S->bad[i].until < S->bad[slot].until ? i : slot;
    }
    strlcpy(S->bad[slot].id, id, sizeof(S->bad[slot].id));
    S->bad[slot].until = now + BAD_MS;
    for (int i = 0; i < HEAR_MAX; i++) {
        if (S->hear[i].a[0] && S->hear[i].state == ST_HEARING && !strcmp(S->hear[i].from, id)) {
            memset(&S->hear[i], 0, sizeof(S->hear[i]));
        }
    }
    ESP_LOGI(TAG, "%s: its record came back bad: not heard for %d min", id, BAD_MS / 60000);
}

static bool listed_role(const char *id)
{
    for (int i = 0; i < S->cfg.nroles && i < GOSSIP_CFG_ROLES; i++) {
        if (!strcmp(S->cfg.roles[i].id, id)) {
            return true;
        }
    }
    return false;
}

static void hear(const char *a, const char *from, const char *g, int boost, int rssi)
{
    int64_t now = now_ms();
    hear_t *h = NULL, *lru = NULL;
    if (bad_source(from, now)) {
        return;
    }
    for (int i = 0; i < HEAR_MAX; i++) {
        if (!strcmp(S->hear[i].a, a)) {
            h = &S->hear[i];
            break;
        }
    }
    if (!h) {
        if (skipped(a, gossip_server_ms())) {
            return;
        }
        // One source holds at most one entry heard in the last 5 s (a listed role
        // id four): a badge making up addresses can't fill the table (§5). Its
        // newest program takes its own entry's place.
        int mine = 0, cap = listed_role(from) ? HEAR_PER_ROLE : HEAR_PER_SRC;
        hear_t *own = NULL;
        for (int i = 0; i < HEAR_MAX; i++) {
            hear_t *x = &S->hear[i];
            if (x->a[0] && x->state == ST_HEARING && now - x->last_ms <= HEARD_MS && !strcmp(x->from, from)) {
                mine++;
                own = !own || x->last_ms < own->last_ms ? x : own;
            }
        }
        if (mine >= cap) {
            h = own;
        }
        for (int i = 0; i < HEAR_MAX && !h; i++) {
            if (!S->hear[i].a[0]) {
                h = &S->hear[i];
            } else if (S->hear[i].state == ST_HEARING && now - S->hear[i].last_ms > HEARD_MS
                       && (!lru || S->hear[i].last_ms < lru->last_ms)) {
                lru = &S->hear[i];
            }
        }
        // Never one already exposed, never one heard in the last 5 s: with
        // all eight fresh, a new program waits until one goes quiet.
        h = h ? h : lru;
        if (!h) {
            return;
        }
        uint8_t b[32];
        if (!gg_b58_decode32(a, b)) {
            return;
        }
        memset(h, 0, sizeof(*h));
        strlcpy(h->a, a, ADDR1);
        gg_decode(b, &h->gen);
        h->best = -127;
    }
    if (h->state != ST_HEARING) {
        h->last_ms = now;
        return;                 // one record per program until answered
    }
    strlcpy(h->from, from, sizeof(h->from));
    strlcpy(h->g, g, sizeof(h->g));
    h->rssi = (int8_t)rssi;
    h->best = (int8_t)(rssi > h->best ? rssi : h->best);
    h->boost = (uint8_t)boost;
    h->last_ms = now;
}

// rssiWeight with the program's traits 0 and 1 (§3): a whole number.
static int rssi_weight(const hear_t *h, int *num, int *den)
{
    const gossip_cfg_t *c = &S->cfg;
    int rssi = h->rssi, w = 0;
    for (int i = 0; i < 4; i++) {
        if (c->wt[i] > 0 && rssi >= c->wt_rssi[i]) {
            w = c->wt[i];
            break;
        }
    }
    bool native = gg_has_trait(&h->gen, 0), direct = gg_has_trait(&h->gen, 1);
    if (direct && rssi >= -55) {
        *num *= 3;              // Direct: × 1.5 at -55 dBm and nearer
        *den *= 2;
    } else if (direct && !native) {
        w = 0;                  // Direct alone counts only that near
    } else if (native && w == 0 && rssi >= -90) {
        w = 1;                  // Gossip-native: down to -90, weight 1
    }
    return w;
}

// One second's exposure (§5): ×W_SCALE, integers only.
static int32_t weight(const hear_t *h, int peers, int64_t srv)
{
    int num = 1, den = 1;
    int rw = rssi_weight(h, &num, &den);
    if (!rw) {
        return 0;
    }
    if (gg_has_trait(&h->gen, 2)) {
        int f = 5 + peers;      // × min(4, 1 + peers / 5)
        num *= f > 20 ? 20 : f;
        den *= 5;
    }
    if (gg_has_trait(&h->gen, 3)) {
        num *= 2;
    }
    if (gg_has_trait(&h->gen, 5) && srv && S->cfg.offpeak_from >= 0) {
        int hour = (int)((srv / 3600000) % 24), f = S->cfg.offpeak_from, t = S->cfg.offpeak_to;
        bool in = f <= t ? hour >= f && hour < t : hour >= f || hour < t;
        if (in) {
            num *= 3;
        } else {
            den *= 2;
        }
    }
    // rw × (0.5 + vir / 10) × (1 + boost / 3) × factors
    int64_t w = (int64_t)rw * W_SCALE * (5 + h->gen.vir) * (3 + h->boost) * num / (10 * 3 * (int64_t)den);
    return w > INT32_MAX / 4 ? INT32_MAX / 4 : (int32_t)w;
}

static int peers_near(void)
{
    int64_t now = now_ms();
    int n = 0;
    portENTER_CRITICAL(&s_seen_mux);
    for (int i = 0; i < SEEN_MAX; i++) {
        n += s_seen[i].h && now - s_seen[i].t < PEER_TTL_MS;
    }
    portEXIT_CRITICAL(&s_seen_mux);
    return n;
}

// Under the lock: a program shown as exposed or incubating whose record left
// the queue without an outcome (dropped when 64 waited, or acknowledged with
// none) is heard again from zero, never stuck.
static void hear_orphans(void)
{
    for (int i = 0; i < HEAR_MAX; i++) {
        hear_t *h = &S->hear[i];
        if (!h->a[0] || h->state == ST_HEARING) {
            continue;
        }
        bool queued = false;
        for (int k = 0; k < S->nrec && !queued; k++) {
            queued = S->rec[k].n == h->rec_n;
        }
        if (!queued) {
            h->state = ST_HEARING;
            h->exp = 0;
            h->secs = 0;
            h->best = -127;
            h->rec_n = 0;
        }
    }
}

static void sync_soon_now(void)
{
    s_soon = true;
}

static void exposed(hear_t *h, int64_t srv)
{
    prog_t *p = prog_find(h->a);
    char name[24] = "";
    if (p && p->json) {
        cJSON *j = cJSON_Parse(p->json);
        game_ascii(name, sizeof(name), cJSON_GetStringValue(cJSON_GetObjectItem(j, "name")) ?: "");
        cJSON_Delete(j);
    }
    // §7 Exposure
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "n", s_rn);
    cJSON_AddStringToObject(e, "p", h->a);
    cJSON_AddStringToObject(e, "f", h->from);
    cJSON_AddStringToObject(e, "g", h->g);
    cJSON_AddNumberToObject(e, "s", h->secs > 3600 ? 3600 : h->secs);
    cJSON_AddNumberToObject(e, "r", h->best);
    cJSON_AddNumberToObject(e, "k", h->peers);
    cJSON_AddNumberToObject(e, "t", srv ? (double)(srv / 1000) : 0);
    char *s = cJSON_PrintUnformatted(e);
    cJSON_Delete(e);
    char *json = s ? dup_str(s) : NULL;
    cJSON_free(s);
    if (!json) {
        return;
    }
    lock();
    if (S->nrec >= s_rec_cap) {
        // Full: the oldest unanswered goes (§5); one in flight stays for its answer.
        int drop = 0;
        while (drop < S->nrec - 1 && S->rec[drop].sent) {
            drop++;
        }
        free(S->rec[drop].json);
        memmove(&S->rec[drop], &S->rec[drop + 1], sizeof(out_t) * (size_t)(S->nrec - drop - 1));
        S->nrec--;
    }
    S->rec[S->nrec].n = s_rn;
    S->rec[S->nrec].sent = false;
    S->rec[S->nrec].json = json;
    S->nrec++;
    h->state = ST_EXPOSED;
    h->rec_n = s_rn;
    hear_orphans();             // the one dropped, if any
    unlock();
    s_rn++;
    nvs_set("rn", s_rn);
    out_save();
    sync_soon_now();

    char shown[24];
    if (!name[0]) {
        short_addr(h->a, shown, sizeof(shown));
    }
    ESP_LOGI(TAG, "exposed to %s from %s (%d s, %d dBm)", h->a, h->from, h->secs, h->best);
    // The LEDs: a flicker in its colour, unless an app may own the strip (a Lua
    // app's own led.* calls would be wiped by the flash's end) or a takeover.
    uint32_t col = (uint32_t)h->gen.r << 16 | (uint32_t)h->gen.gr << 8 | h->gen.b;
    if (!lua_host_running()) {
        game_led_flash(col);
    }
    char note[64];
    snprintf(note, sizeof(note), "exposed to %s", name[0] ? name : shown);
    gossip_notice(0, note);
    char ev[200];
    snprintf(ev, sizeof(ev), "{\"t\":\"exposed\",\"a\":\"%s\",\"from\":\"%s\",\"rarity\":\"%s\"}", h->a, h->from,
             gossip_rarity_name(h->gen.rarity));
    lua_host_gossip(ev);
}

// Once a second (§5).
static void second(int64_t now)
{
    int64_t srv = gossip_server_ms();
    int peers = peers_near();
    for (int i = 0; i < HEAR_MAX; i++) {
        hear_t *h = &S->hear[i];
        if (!h->a[0] || h->state != ST_HEARING) {
            continue;
        }
        if (skipped(h->a, srv)) {
            lock();
            memset(h, 0, sizeof(*h));
            unlock();
            continue;
        }
        int64_t quiet = now - h->last_ms;
        lock();
        if (quiet <= HEARD_MS) {
            h->peers = (uint8_t)(peers > 255 ? 255 : peers);
            int32_t w = weight(h, peers, srv);
            h->last_w = w;
            h->exp = h->exp > INT32_MAX - w ? INT32_MAX : h->exp + w;
            if (w > 0 && h->secs < 0xFFFF) {
                h->secs++;
            }
        } else if (gg_has_trait(&h->gen, 4) && quiet <= DURABLE_MS) {
            // Durable nonce: at the last weight for 10 min.
            h->exp = h->exp > INT32_MAX - h->last_w ? INT32_MAX : h->exp + h->last_w;
        } else {
            int32_t d = S->cfg.decay * W_SCALE;
            h->exp = h->exp > d ? h->exp - d : 0;
            if (!h->exp && quiet > FORGET_MS) {
                memset(h, 0, sizeof(*h));
            }
        }
        unlock();
        if (h->a[0] && (int64_t)h->exp >= (int64_t)S->cfg.thr * W_SCALE) {
            exposed(h, srv);
        }
    }
}

// ---------------------------------------------------------------------------
// HELLOs: the mesh task copies, the OS task parses

static uint32_t id_hash(const char *id)
{
    uint32_t h = 2166136261u;
    for (; *id; id++) {
        h = (h ^ (uint8_t)*id) * 16777619u;
    }
    return h ? h : 1;
}

static void drain(void *arg);

// Mesh task: no parsing, no SD, no LVGL (MESH.md; ~2 KB of stack to spare).
static void on_mesh(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx)
{
    if (!s_listen || from->bridged || from->kind == OS_PEER_BEACON) {
        return;
    }
    int64_t now = now_ms();
    if (from->kind == OS_PEER_BADGE) {
        uint32_t h = id_hash(from->id);
        portENTER_CRITICAL(&s_seen_mux);
        int slot = 0;
        for (int i = 0; i < SEEN_MAX; i++) {
            if (s_seen[i].h == h) {
                slot = i;
                break;
            }
            if (s_seen[i].t < s_seen[slot].t) {
                slot = i;
            }
        }
        s_seen[slot].h = h;
        s_seen[slot].t = now;
        portEXIT_CRITICAL(&s_seen_mux);
    }
    size_t len = strlen(json);
    if (len > HOP_JSON || (!strstr(json, "\"g\":\"") && !strstr(json, "\"ro\":\""))) {
        return;
    }
    bool post = false;
    xSemaphoreTake(s_hop_mx, portMAX_DELAY);
    if (S->nhop < HOP_MAX) {
        hop_t *h = &S->hop[S->nhop++];
        strlcpy(h->id, from->id, sizeof(h->id));
        h->station = from->kind == OS_PEER_STATION;
        h->rssi = from->rssi;
        memcpy(h->json, json, len + 1);
        post = !s_drain_queued;
        s_drain_queued = true;
    } else {
        s_hops_dropped++;
    }
    xSemaphoreGive(s_hop_mx);
    if (post && !os_post(drain, NULL)) {
        s_drain_queued = false;     // the next HELLO tries again
    }
}

static void hello_one(const hop_t *hp)
{
    cJSON *j = cJSON_Parse(hp->json);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        s_rx_bad++;
        return;
    }
    gossip_hello_t h = {0};
    strlcpy(h.id, hp->id, sizeof(h.id));
    h.station = hp->station;
    h.rssi = hp->rssi;
    const char *ro = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ro"));
    if (hp->station && ro && strlen(ro) == 1 && strchr("lmaz", ro[0])) {
        h.ro = ro[0];
    }
    game_ascii(h.name, sizeof(h.name), cJSON_GetStringValue(cJSON_GetObjectItem(j, "n")) ?: "");
    const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(j, "st"));
    const char *g = cJSON_GetStringValue(cJSON_GetObjectItem(j, "g"));
    gg_g_t gp;
    bool g_ok = g && strlen(g) <= GOSSIP_G_MAX && gg_g_parse(g, &gp);
    bool st_ok = st && (!st[0] || gg_addr_ok(st));
    if (g_ok && st_ok) {
        strlcpy(h.st, st, sizeof(h.st));
        strlcpy(h.g, g, sizeof(h.g));
    } else if (g) {
        s_rx_bad++;
    }
    cJSON_Delete(j);                // st and g point into it: copied above
    if (hp->station && !h.ro) {
        return;                     // a Collaborator station: nothing of Gossip's
    }
    lock();
    peer_t *p = peer_get(h.id, true);
    strlcpy(p->name, h.name, sizeof(p->name));
    p->ro = h.ro;
    p->rssi = (int8_t)h.rssi;
    p->last_ms = now_ms();
    bool counts = p->last_ms - p->heard_ms >= HEAR_GAP_MS;     // once a second a source (§5)
    if (counts) {
        p->heard_ms = p->last_ms;
    }
    if (h.g[0]) {
        strlcpy(p->st, h.st, sizeof(p->st));
        p->c = gp.c == 'n' ? 0 : gp.c;
        p->b = (uint8_t)gp.b;
    }
    unlock();
    s_hellos++;
    if (!h.g[0]) {
        return;
    }
    // A tag from another window than now's is a replay (with a clock): heard on
    // the radar, never counted.
    int64_t srv = gossip_server_ms();
    bool fresh = !srv || (gp.w >= srv / 30000 - 2 && gp.w <= srv / 30000 + 2);
    if (h.st[0] && fresh && counts && gossip_on()) {
        lock();
        hear(h.st, h.id, h.g, gp.b, h.rssi);
        unlock();
    }
    gossip_role_on_hello(&h);
}

static void drain(void *arg)
{
    (void)arg;
    hop_t *batch = heap_caps_malloc(sizeof(hop_t) * HOP_MAX, os_mem_bulk_caps());
    xSemaphoreTake(s_hop_mx, portMAX_DELAY);
    int n = batch ? S->nhop : 0;
    if (batch) {
        memcpy(batch, S->hop, sizeof(hop_t) * (size_t)n);
    }
    S->nhop = 0;
    s_drain_queued = false;
    xSemaphoreGive(s_hop_mx);
    bool room = os_mem_sram_malloc_free() >= SRAM_HEAR_MIN;
    for (int i = 0; i < n && s_run && room; i++) {
        hello_one(&batch[i]);
    }
    heap_caps_free(batch);
}

// ---------------------------------------------------------------------------
// Spreading (§4): the badge's own HELLO fields

static bool dormant(const char *a, int64_t srv)
{
    for (int i = 0; i < DORM_MAX; i++) {
        if (S->dorm[i].a[0] && !strcmp(S->dorm[i].a, a)) {
            return srv < S->dorm[i].until;
        }
    }
    return false;
}

static void spread(int64_t srv)
{
    char want[sizeof(s_extra)] = "";
    if (gossip_on() && srv) {
        int64_t w = srv / 30000;
        char st[ADDR1];
        strlcpy(st, S->act, sizeof(st));
        if (st[0] && dormant(st, srv)) {
            st[0] = 0;          // Scheduled: not for 30 min after adopting it (§3 trait 3)
        }
        char c = S->cls ? S->cls : 'n';
        int b = S->boost;
        char msg[120], tag[OS_MESH_TAG_LEN + 1];
        // The tag only changes with the window, st, c or b: sign again only then.
        static char last[ADDR1 + 4];
        char key[ADDR1 + 4];
        snprintf(key, sizeof(key), "%s%c%d", st, c, b);
        if (w == s_extra_w && !strcmp(key, last) && s_extra[0]) {
            return;
        }
        strlcpy(last, key, sizeof(last));
        if (gg_tag_msg(msg, sizeof(msg), s_id, st, w, c, b) > 0 && os_mesh_sign(msg, tag)) {
            snprintf(want, sizeof(want), "\"st\":\"%s\",\"g\":\"%c%d.%lld.%s\"", st, c, b, (long long)w, tag);
        }
        s_extra_w = w;
    }
    if (strcmp(want, s_extra)) {
        strlcpy(s_extra, want, sizeof(s_extra));
        os_mesh_hello_extra(want[0] ? want : NULL);
    }
}

// ---------------------------------------------------------------------------
// The sync (§7)

static int sync_interval(void)
{
    int ms = (int)s_srv_next;
    if (gossip_role_opted_in() && ms > SYNC_ROLES_MS) {
        ms = SYNC_ROLES_MS;
    }
    if (gossip_role_fast_sync() && ms > SYNC_FAST_MS) {
        ms = SYNC_FAST_MS;
    }
    return ms;
}

static void add_list(cJSON *arr, out_t *q, int n, int max, bool mark)
{
    for (int i = 0; i < n && i < max; i++) {
        cJSON *o = cJSON_Parse(q[i].json);
        if (o) {
            cJSON_AddItemToArray(arr, o);
            if (mark) {
                q[i].sent = true;
            }
        }
    }
}

static bool asked(cJSON *ask, const char *a)
{
    const cJSON *e;
    cJSON_ArrayForEach(e, ask) {
        if (!strcmp(e->valuestring, a)) {
            return true;
        }
    }
    return false;
}

static void ask_add(cJSON *ask, const char *a)
{
    if (a[0] && cJSON_GetArraySize(ask) < 6 && !prog_find(a) && !asked(ask, a)) {
        cJSON_AddItemToArray(ask, cJSON_CreateString(a));
    }
}

static char *sync_body(bool on)
{
    cJSON *b = cJSON_CreateObject();
    if (!b) {
        return NULL;
    }
    cJSON_AddNumberToObject(b, "v", 1);
    cJSON_AddBoolToObject(b, "on", on);
    lock();
    cJSON_AddNumberToObject(b, "seq", S->seq);
    cJSON_AddNumberToObject(b, "cfg", S->cfg_rev);
    cJSON_AddStringToObject(b, "act", S->act);
    for (int i = 0; i < S->nrec; i++) {
        S->rec[i].sent = false;
    }
    for (int i = 0; i < S->nact; i++) {
        S->act_q[i].sent = false;
    }
    if (on) {
        if (S->nrec) {
            add_list(cJSON_AddArrayToObject(b, "rec"), S->rec, S->nrec, 8, true);
        }
        if (S->nact) {
            add_list(cJSON_AddArrayToObject(b, "acts"), S->act_q, S->nact, ACT_MAX, true);
        }
        cJSON *ask = cJSON_CreateArray();
        ask_add(ask, S->act);
        for (int i = 0; i < S->nslots; i++) {
            ask_add(ask, S->slots[i]);
        }
        for (int i = 0; i < S->ninbox; i++) {
            ask_add(ask, S->inbox[i]);
        }
        for (int i = 0; i < HEAR_MAX; i++) {
            ask_add(ask, S->hear[i].a);
        }
        if (cJSON_GetArraySize(ask)) {
            cJSON_AddItemToObject(b, "ask", ask);
        } else {
            cJSON_Delete(ask);
        }
    }
    unlock();
    char *s = cJSON_PrintUnformatted(b);
    int used = s ? (int)strlen(s) : BODY_MAX;
    cJSON_free(s);
    if (on) {
        gossip_role_sync_fill(b, BODY_MAX - used - 16);
    }
    s = cJSON_PrintUnformatted(b);
    // Over 4 KB (a role filled more than it was given): records wait for the next one.
    cJSON *rec = cJSON_GetObjectItem(b, "rec");
    while (s && strlen(s) > BODY_MAX && cJSON_GetArraySize(rec) > 0) {
        int last = cJSON_GetArraySize(rec) - 1;
        lock();
        S->rec[last].sent = false;
        unlock();
        cJSON_DeleteItemFromArray(rec, last);
        cJSON_free(s);
        s = cJSON_PrintUnformatted(b);
    }
    cJSON_Delete(b);
    if (s && strlen(s) > BODY_MAX) {
        cJSON_free(s);
        s = NULL;
    }
    return s;
}

static void out_ack(out_t *q, int *n, uint32_t ack, const cJSON *res, bool records)
{
    int k = 0;
    for (int i = 0; i < *n; i++) {
        // Only `ack` removes: a cooled record and everything after it stay
        // unacknowledged, in order, for a later sync (§6 step 3, §7).
        (void)res;
        (void)records;
        if (q[i].n > ack) {
            q[k++] = q[i];
        } else {
            free(q[i].json);
        }
    }
    for (int i = k; i < *n; i++) {
        memset(&q[i], 0, sizeof(q[i]));
    }
    *n = k;
}

static void done_add(const char *a)
{
    if (addr_in(a, (const char (*)[ADDR1])S->done, S->ndone)) {
        return;
    }
    if (S->ndone == DONE_MAX) {
        memmove(S->done[0], S->done[1], sizeof(S->done[0]) * (DONE_MAX - 1));
        S->ndone--;
    }
    strlcpy(S->done[S->ndone++], a, ADDR1);
}

static void outcome(const cJSON *o, int64_t srv)
{
    uint32_t n = (uint32_t)jint(o, "n", 0, 0, INT32_MAX);
    const char *p = cJSON_GetStringValue(cJSON_GetObjectItem(o, "p"));
    const char *r = cJSON_GetStringValue(cJSON_GetObjectItem(o, "r"));
    if (!p || !r || !gg_addr_ok(p)) {
        return;
    }
    static const char *const known[] = {"adopted", "forked", "seen",    "immune", "blocked",
                                        "missed",  "unknown", "own",    "bad",    "cool"};
    bool ok = false;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        ok |= !strcmp(r, known[i]);
    }
    if (!ok) {
        return;
    }
    lock();
    for (int i = 0; i < HEAR_MAX; i++) {
        hear_t *h = &S->hear[i];
        if (!h->a[0] || strcmp(h->a, p) || (h->rec_n != n && h->state != ST_HEARING)) {
            continue;
        }
        if (!strcmp(r, "cool")) {
            break;              // still incubating: it goes again
        }
        if ((!strcmp(r, "bad") || !strcmp(r, "unknown")) && h->rec_n == n && h->state != ST_HEARING) {
            char src[OS_MESH_ID_LEN + 1];
            strlcpy(src, h->from, sizeof(src));
            memset(h, 0, sizeof(*h));
            bad_add(src, now_ms());     // a forged tag or a made-up program: its source is ignored (§5)
            break;
        }
        bool took = !strcmp(r, "adopted") || !strcmp(r, "forked");
        if (took || !strcmp(r, "seen")) {
            done_add(p);
            if (!strcmp(r, "adopted") && gg_has_trait(&h->gen, 3) && srv) {
                // Scheduled: its new carrier keeps it quiet for 30 min (§3 trait 3).
                int slot = 0;
                for (int d = 0; d < DORM_MAX; d++) {
                    if (!S->dorm[d].a[0] || S->dorm[d].until < S->dorm[slot].until) {
                        slot = d;
                    }
                }
                strlcpy(S->dorm[slot].a, p, ADDR1);
                S->dorm[slot].until = srv + DORMANT_MS;
            }
            memset(h, 0, sizeof(*h));
        } else {
            h->state = ST_HEARING;  // missed, immune, blocked, ...: the exposure starts over
            h->exp = 0;
            h->secs = 0;
            h->best = -127;
            h->rec_n = 0;
        }
        break;
    }
    unlock();
    char ev[256];
    const char *fork = cJSON_GetStringValue(cJSON_GetObjectItem(o, "fork"));
    int len = snprintf(ev, sizeof(ev), "{\"t\":\"result\",\"n\":%" PRIu32 ",\"p\":\"%s\",\"r\":\"%s\"", n, p, r);
    if (fork && gg_addr_ok(fork)) {
        len += snprintf(ev + len, sizeof(ev) - (size_t)len, ",\"fork\":\"%s\"", fork);
    }
    snprintf(ev + len, sizeof(ev) - (size_t)len, ",\"stake\":%d}", jint(o, "stake", 0, 0, 1000000));
    lua_host_gossip(ev);
}

static void act_result(const cJSON *o)
{
    uint32_t n = (uint32_t)jint(o, "n", 0, 0, INT32_MAX);
    bool ok = cJSON_IsTrue(cJSON_GetObjectItem(o, "ok"));
    char err[64];
    game_ascii(err, sizeof(err), cJSON_GetStringValue(cJSON_GetObjectItem(o, "error")) ?: "");
    // The action's own kind, so the Lab knows what it answers.
    char t[12] = "";
    lock();
    for (int i = 0; i < S->nact; i++) {
        if (S->act_q[i].n == n) {
            cJSON *a = cJSON_Parse(S->act_q[i].json);
            game_ascii(t, sizeof(t), cJSON_GetStringValue(cJSON_GetObjectItem(a, "t")) ?: "");
            cJSON_Delete(a);
        }
    }
    unlock();
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "t", "result");
    cJSON_AddNumberToObject(e, "n", n);
    cJSON_AddStringToObject(e, "act", t);
    cJSON_AddBoolToObject(e, "ok", ok);
    if (err[0]) {
        cJSON_AddStringToObject(e, "error", err);
    }
    char *s = cJSON_PrintUnformatted(e);
    cJSON_Delete(e);
    if (s) {
        lua_host_gossip(s);
        cJSON_free(s);
    }
}

static void event_add(const cJSON *o)
{
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(o, "t"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(o, "text"));
    if (!t || !text || !text[0]) {
        return;
    }
    const char *col = cJSON_GetStringValue(cJSON_GetObjectItem(o, "col"));
    lock();
    if (S->nev == EV_MAX) {
        memmove(&S->ev[0], &S->ev[1], sizeof(S->ev[0]) * (EV_MAX - 1));
        S->nev--;
    }
    game_ascii(S->ev[S->nev].kind, sizeof(S->ev[0].kind), t);
    game_ascii(S->ev[S->nev].text, sizeof(S->ev[0].text), text);
    S->ev[S->nev].col = col ? game_parse_hex(col[0] == '#' ? col + 1 : col, 0) & 0xFFFFFF : 0;
    S->nev++;
    unlock();
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "t", "event");
    cJSON_AddStringToObject(e, "kind", S->ev[S->nev - 1].kind);
    cJSON_AddStringToObject(e, "text", S->ev[S->nev - 1].text);
    if (col) {
        cJSON_AddNumberToObject(e, "col", S->ev[S->nev - 1].col);
    }
    char *s = cJSON_PrintUnformatted(e);
    cJSON_Delete(e);
    if (s) {
        lua_host_gossip(s);
        cJSON_free(s);
    }
}

static void on_sync(int status, const char *body, void *ctx)
{
    if (!ctx || ctx != s_tok) {
        return;
    }
    s_tok = NULL;
    int64_t now = now_ms();
    s_last_status = status;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsObject(j) || !cJSON_IsNumber(cJSON_GetObjectItem(j, "time"))) {
        cJSON *e = status && body ? cJSON_Parse(body) : NULL;
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(e, "error"));
        if (status == 403) {
            strlcpy(s_why, "Not linked yet", sizeof(s_why));
        } else if (status == 409) {
            game_ascii(s_why, sizeof(s_why), msg ? msg : "Update Solana OS");
        } else if (!status) {
            strlcpy(s_why, "Can't reach the server", sizeof(s_why));
        } else {
            game_ascii(s_why, sizeof(s_why), msg ? msg : "The server didn't answer properly");
        }
        cJSON_Delete(e);
        cJSON_Delete(j);
        lock();
        for (int i = 0; i < S->nrec; i++) {
            S->rec[i].sent = false;
        }
        for (int i = 0; i < S->nact; i++) {
            S->act_q[i].sent = false;
        }
        unlock();
        gossip_role_sync_done(false, NULL);
        int retry = status == 429 ? 30000 : status == 403 || status == 409 ? 120000 : RETRY_MS;
        if (gossip_role_fast_sync() && retry > SYNC_FAST_MS * 2 && status != 403 && status != 409) {
            retry = SYNC_FAST_MS * 2;
        }
        s_next_sync = now + retry;
        ESP_LOGW(TAG, "sync: HTTP %d", status);
        return;
    }
    s_why[0] = 0;
    // The server stamped `time` somewhere in the round trip: the middle.
    int64_t t = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "time"));
    s_clock_at = (s_tok_at + now) / 2;
    s_clock_base = t;
    s_synced = t;
    int64_t srv = gossip_server_ms();
    int next = jint(j, "next", SYNC_IDLE_MS, SYNC_MIN_MS, SYNC_MAX_MS);
    s_srv_next = next;

    bool card_dirty = false, node_dirty = false;
    const cJSON *node = cJSON_GetObjectItem(j, "node");
    if (cJSON_IsObject(node)) {
        char *s = cJSON_PrintUnformatted(node);
        char *copy = s ? dup_str(s) : NULL;
        cJSON_free(s);
        if (copy) {
            lock();
            free(S->node_json);
            S->node_json = copy;
            node_parse(node);
            S->seq = jint(j, "seq", S->seq, 0, INT32_MAX);
            unlock();
            node_dirty = true;
            char ev[48];
            snprintf(ev, sizeof(ev), "{\"t\":\"node\",\"seq\":%d}", S->seq);
            lua_host_gossip(ev);
        }
    }
    const cJSON *conf = cJSON_GetObjectItem(j, "conf");
    if (cJSON_IsObject(conf)) {
        char *s = cJSON_PrintUnformatted(conf);
        char *copy = s ? dup_str(s) : NULL;
        cJSON_free(s);
        gossip_cfg_t c;
        cfg_parse(conf, &c);
        lock();
        S->cfg = c;
        free(S->cfg_json);
        S->cfg_json = copy;
        S->cfg_rev = jint(j, "cfg", S->cfg_rev, 0, INT32_MAX);
        unlock();
        node_dirty = true;
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "progs")) {
        if (cJSON_IsObject(e)) {
            prog_put(e, false);
        }
    }
    const cJSON *res = cJSON_GetObjectItem(j, "res");
    cJSON_ArrayForEach(e, res) {
        outcome(e, srv);
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "ares")) {
        act_result(e);
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "ev")) {
        event_add(e);
    }
    const cJSON *ack = cJSON_GetObjectItem(j, "ack"), *aack = cJSON_GetObjectItem(j, "aack");
    lock();
    int before = S->nrec + S->nact;
    if (cJSON_IsNumber(ack)) {
        out_ack(S->rec, &S->nrec, (uint32_t)ack->valuedouble, res, true);
    }
    if (cJSON_IsNumber(aack)) {
        out_ack(S->act_q, &S->nact, (uint32_t)aack->valuedouble, NULL, false);
    }
    for (int i = 0; i < S->nrec; i++) {
        S->rec[i].sent = false;
    }
    for (int i = 0; i < S->nact; i++) {
        S->act_q[i].sent = false;
    }
    card_dirty = S->nrec + S->nact != before;
    hear_orphans();
    // Exposed programs whose record went out are incubating now (§5).
    for (int i = 0; i < HEAR_MAX; i++) {
        if (S->hear[i].a[0] && S->hear[i].state == ST_EXPOSED) {
            S->hear[i].state = ST_INCUBATING;
        }
    }
    unlock();
    gossip_role_sync_done(true, cJSON_GetObjectItem(j, "role"));
    cJSON_Delete(j);
    if (card_dirty) {
        out_save();
    }
    if (node_dirty) {
        node_save();
    }
    // More waiting goes 5 s later, but only while the server is taking it:
    // an answer that acknowledged nothing new (a cooldown holds the records,
    // up to 20 min) goes back to the normal cadence.
    lock();
    bool took = S->nrec + S->nact < before;
    bool more = S->npending > 0 || (took && (S->nrec > 0 || S->nact > 0));
    if (!took && S->npending == 0) {
        s_soon = false;
    }
    unlock();
    s_next_sync = now + (more ? SYNC_GAP_MS : sync_interval());
    verify_kick();
}

static void on_bye(int status, const char *body, void *ctx)
{
    (void)status;
    (void)body;
    (void)ctx;
}

static void sync_now(int64_t now)
{
    if (os_mem_sram_malloc_free() < SRAM_SYNC_MIN) {
        s_next_sync = now + RETRY_MS;       // the answer's tree wouldn't fit: later
        strlcpy(s_why, "Low on memory", sizeof(s_why));
        return;
    }
    char *body = sync_body(true);
    void *tok = os_api_token();
    if (body && os_api_call("POST", "/api/device/gossip/sync", body, RESP_CAP, on_sync, tok) == ESP_OK) {
        s_tok = tok;
        s_tok_at = now;
        s_last_sync = now;
        s_soon = false;
    } else {
        lock();
        for (int i = 0; i < S->nrec; i++) {
            S->rec[i].sent = false;
        }
        for (int i = 0; i < S->nact; i++) {
            S->act_q[i].sent = false;
        }
        unlock();
        s_next_sync = now + RETRY_MS;
        if (!os_api_online()) {
            strlcpy(s_why, "Offline", sizeof(s_why));
        }
    }
    cJSON_free(body);
}

void gossip_sync_soon(void)
{
    s_soon = true;
}

// ---------------------------------------------------------------------------
// Actions (§7): any task queues, the OS task numbers and keeps them

static void flush(void *arg)
{
    (void)arg;
    s_flush_queued = false;
    if (!S) {
        return;
    }
    bool changed = false;
    lock();
    while (S->npending && S->nact < ACT_MAX) {
        char *raw = S->pending[0];
        memmove(&S->pending[0], &S->pending[1], sizeof(char *) * (ACT_MAX - 1));
        S->pending[--S->npending] = NULL;
        cJSON *a = cJSON_Parse(raw);
        free(raw);
        if (!cJSON_IsObject(a)) {
            cJSON_Delete(a);
            continue;
        }
        cJSON_DeleteItemFromObject(a, "n");
        cJSON_AddNumberToObject(a, "n", s_an);
        char *s = cJSON_PrintUnformatted(a);
        cJSON_Delete(a);
        char *json = s ? dup_str(s) : NULL;
        cJSON_free(s);
        if (json) {
            S->act_q[S->nact].n = s_an++;
            S->act_q[S->nact].sent = false;
            S->act_q[S->nact].json = json;
            S->nact++;
            changed = true;
        }
    }
    unlock();
    if (changed) {
        nvs_set("an", s_an);
        out_save();
        s_soon = true;
    }
}

bool gossip_action(const char *json)
{
    if (!S || !json || json[0] != '{' || strlen(json) > 512 || !gossip_on()) {
        return false;
    }
    char *copy = dup_str(json);
    if (!copy) {
        return false;
    }
    lock();
    bool ok = S->nact + S->npending < ACT_MAX;
    if (ok) {
        S->pending[S->npending++] = copy;
    }
    unlock();
    if (!ok) {
        free(copy);
        return false;
    }
    if (!s_flush_queued) {
        s_flush_queued = true;
        if (!os_post(flush, NULL)) {
            s_flush_queued = false;     // the next tick picks it up
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// On and off

bool gossip_on(void)
{
    return s_run && s_claimed && os_setting(OS_SET_GOSSIP);
}

static void listen_apply(void)
{
    s_listen = s_run && s_claimed;
}

static void bye(void)
{
    // The last sync: on false (§5). Best effort.
    if (!s_claimed) {
        return;
    }
    char *body = sync_body(false);
    if (body) {
        os_api_call("POST", "/api/device/gossip/sync", body, 0, on_bye, os_api_token());
        cJSON_free(body);
    }
}

static os_service_state_t svc_state(void *ctx)
{
    return s_run ? OS_SVC_RUNNING : OS_SVC_STOPPED;
}

static void svc_status(void *ctx, char *out, size_t n)
{
    if (!s_run || !os_setting(OS_SET_GOSSIP)) {
        strlcpy(out, "Off", n);
        return;
    }
    if (!s_claimed) {
        strlcpy(out, "Not linked yet", n);
        return;
    }
    int inc = 0, held = 0;
    lock();
    for (int i = 0; i < HEAR_MAX; i++) {
        inc += S->hear[i].a[0] && S->hear[i].state != ST_HEARING;
    }
    for (int i = 0; i < S->nslots; i++) {
        held += S->slots[i][0] != 0;
    }
    held += S->ninbox;
    unlock();
    if (s_why[0] && !s_synced) {
        strlcpy(out, s_why, n);
        return;
    }
    snprintf(out, n, "%d program%s, %d incubating", held, held == 1 ? "" : "s", inc);
}

static int32_t svc_memory(void *ctx)
{
    if (!S) {
        return 0;
    }
    int32_t m = (int32_t)sizeof(*S);
    lock();
    for (int i = 0; i < S->nprog; i++) {
        m += S->prog[i].json ? (int32_t)strlen(S->prog[i].json) + 1 : 0;
    }
    for (int i = 0; i < S->nrec; i++) {
        m += (int32_t)strlen(S->rec[i].json) + 1;
    }
    m += S->node_json ? (int32_t)strlen(S->node_json) + 1 : 0;
    unlock();
    return m;
}

static bool svc_start(void *ctx, const char **why)
{
    if (!S) {
        *why = "Not enough memory";
        return false;
    }
    if (!os_setting(OS_SET_GOSSIP)) {
        os_setting_set(OS_SET_GOSSIP, 1);       // one switch: Services and Settings
    }
    if (s_run) {
        return true;
    }
    s_run = true;
    s_next_sync = 0;
    s_why[0] = 0;
    listen_apply();
    ESP_LOGI(TAG, "on");
    return true;
}

static void svc_stop(void *ctx)
{
    if (os_setting(OS_SET_GOSSIP)) {
        os_setting_set(OS_SET_GOSSIP, 0);
    }
    if (!s_run) {
        return;
    }
    os_api_cancel(s_tok);
    s_tok = NULL;
    bye();
    s_run = false;
    listen_apply();
    os_mesh_hello_extra(NULL);
    s_extra[0] = 0;
    lock();
    memset(S->hear, 0, sizeof(S->hear));
    unlock();
    ESP_LOGI(TAG, "off");
}

static const os_service_def_t s_def = {
    .id = SVC_ID,
    .name = "Gossip",
    .symbol = LV_SYMBOL_SHUFFLE,
    .accent = COL_PURPLE,
    .flags = OS_SVC_STOPPABLE | OS_SVC_BOOT,
    .state = svc_state,
    .status = svc_status,
    .memory = svc_memory,
    .start = svc_start,
    .stop = svc_stop,
};

void gossip_setting_changed(void)
{
    const char *why = "";
    if (os_setting(OS_SET_GOSSIP)) {
        if (!os_service_start(SVC_ID, &why)) {
            ESP_LOGW(TAG, "didn't start: %s", why);
        }
    } else {
        os_service_stop(SVC_ID, &why);
    }
}

static void lead_set(void *arg)
{
    os_setting_set(OS_SET_LEADER, arg ? 1 : 0);
    gossip_role_setting_changed();
}

void gossip_set_lead(bool on)
{
    os_post(lead_set, on ? (void *)1 : NULL);
}

// ---------------------------------------------------------------------------
// The tick (OS task, every 250 ms)

static void hold_locked(uint32_t col, const char *text)
{
    if (s_nheld == HELD_MAX) {
        memmove(&s_held[0], &s_held[1], sizeof(s_held[0]) * (HELD_MAX - 1));
        s_nheld--;
        s_held_drop++;                  // the oldest goes: only the last three are shown
    }
    strlcpy(s_held[s_nheld].text, text, sizeof(s_held[0].text));
    s_held[s_nheld].col = col;
    s_nheld++;
}

bool gossip_notice(uint32_t col, const char *text)
{
    if (!S) {
        return false;
    }
    if (gossip_role_fighting()) {       // gr_mx, then s_mx: the module's lock order
        lock();
        hold_locked(col, text);
        unlock();
        return true;
    }
    return os_service_notify(SVC_ID, col, "%s", text);
}

static void notices(int64_t now)
{
    if (gossip_role_fighting()) {
        lock();                         // the queue's notices wait with the others
        for (int i = 0; i < S->nev; i++) {
            hold_locked(S->ev[i].col, S->ev[i].text);
        }
        S->nev = 0;
        unlock();
        return;
    }
    if (now - s_notice_at < NOTICE_GAP_MS) {
        return;
    }
    lock();
    char text[112];
    uint32_t col;
    bool held = s_nheld > 0;
    if (held) {                      // after a fight: what it held first, oldest first
        col = s_held[0].col;
        strlcpy(text, s_held[0].text, sizeof(text));
        if (s_held_drop) {
            size_t n = strlen(text);
            snprintf(text + n, sizeof(text) - n, " (+%d more in the fight)", s_held_drop);
            s_held_drop = 0;
        }
        memmove(&s_held[0], &s_held[1], sizeof(s_held[0]) * (HELD_MAX - 1));
        s_nheld--;
    } else if (S->nev) {
        col = S->ev[0].col;
        strlcpy(text, S->ev[0].text, sizeof(text));
        memmove(&S->ev[0], &S->ev[1], sizeof(S->ev[0]) * (EV_MAX - 1));
        S->nev--;
    } else {
        unlock();
        return;
    }
    unlock();
    if (os_service_notify(SVC_ID, col, "%s", text)) {
        s_notice_at = now;
        ESP_LOGI(TAG, "notice: %s", text);
    } else if (held) {
        lock();                         // another notice took the service's 10 s: this one next turn
        if (s_nheld < HELD_MAX) {
            memmove(&s_held[1], &s_held[0], sizeof(s_held[0]) * s_nheld);
            strlcpy(s_held[0].text, text, sizeof(s_held[0].text));
            s_held[0].col = col;
            s_nheld++;
        }
        unlock();
        s_notice_at = now;
    }
}

void gossip_tick(int64_t now)
{
    if (!S) {
        return;
    }

    // Who we are, and whose the card is.
    const char *id = game_badge_id();
    bool claimed = game_claimed();
    uint32_t ep = os_data_epoch();
    if (ep != s_epoch || strcmp(id, s_id) || (s_claimed && !claimed)) {
        bool first = !s_id[0] && ep == s_epoch;
        s_epoch = ep;
        if (!first) {
            ESP_LOGI(TAG, "a new owner or a wiped card: starting over");
            os_api_cancel(s_tok);       // its answer is the previous owner's
            s_tok = NULL;
            forget_all();
            load_all();
            s_extra[0] = 0;
            os_mesh_hello_extra(NULL);
            s_clock_base = 0;
            s_synced = 0;
        }
        strlcpy(s_id, id, sizeof(s_id));
    }
    if (claimed != s_claimed) {
        s_claimed = claimed;
        listen_apply();
        s_next_sync = 0;
    }
    if (!s_run) {
        return;
    }
    if (S->npending && !s_flush_queued) {
        flush(NULL);            // a post that didn't get through
    }
    int64_t srv = gossip_server_ms();
    if (now - s_last_sec >= 1000) {
        s_last_sec = now - (now - s_last_sec) % 1000;
        if (gossip_on()) {
            second(now);
        }
        // The server may have changed (`server <url>`): its key is another's.
        os_cloud_info_t *in = malloc(sizeof(*in));
        if (in) {
            os_cloud_get_info(in);
            if (strcmp(in->server, s_key_srv)) {
                strlcpy(s_key_srv, in->server, sizeof(s_key_srv));
                s_has_key = false;
                s_clock_base = 0;
            }
            free(in);
        }
    }
    spread(srv);
    notices(now);
    if (s_tok && now - s_tok_at > ANSWER_LOST_MS) {
        // No answer in 30 s: os_api dropped it (the OS queue stayed full). Give it
        // up, as a failed sync: what it carried goes again, at the normal cadence.
        ESP_LOGW(TAG, "sync: no answer in %d s, giving up on it", ANSWER_LOST_MS / 1000);
        os_api_cancel(s_tok);
        s_tok = NULL;
        lock();
        for (int i = 0; i < S->nrec; i++) {
            S->rec[i].sent = false;
        }
        for (int i = 0; i < S->nact; i++) {
            S->act_q[i].sent = false;
        }
        unlock();
        gossip_role_sync_done(false, NULL);
        s_next_sync = now + sync_interval();
    }
    if (!s_claimed || s_tok) {
        return;
    }
    int64_t due = s_next_sync;
    if (s_soon && s_last_sync + SYNC_GAP_MS < due) {
        due = s_last_sync + SYNC_GAP_MS;
    }
    if (now >= due && os_api_online()) {
        sync_now(now);
    }
    if (s_has_key) {
        lock();
        bool waiting = false;
        for (int i = 0; i < S->nprog && !waiting; i++) {
            waiting = S->prog[i].ok == PV_NEW && !S->prog[i].checking;
        }
        unlock();
        if (waiting) {
            verify_kick();
        }
    } else if (S->nprog) {
        key_fetch();
    }
}

// ---------------------------------------------------------------------------
// What apps read (lua/lua_gossip.c): JSON built under the lock

static char *print_free(cJSON *j)
{
    char *s = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    return s;
}

static void add_genome(cJSON *o, const gossip_genome_t *g)
{
    cJSON_AddNumberToObject(o, "colour", (double)((uint32_t)g->r << 16 | (uint32_t)g->gr << 8 | g->b));
    cJSON_AddNumberToObject(o, "hue", g->hue);
    cJSON_AddNumberToObject(o, "body", g->body);
    cJSON_AddNumberToObject(o, "eyes", g->eyes);
    cJSON_AddNumberToObject(o, "marks", g->marks);
    cJSON_AddNumberToObject(o, "mark_seed", g->mark_seed);
    cJSON *t = cJSON_AddArrayToObject(o, "traits");
    for (int i = 0; i < g->ntraits; i++) {
        cJSON_AddItemToArray(t, cJSON_CreateNumber(g->traits[i]));
    }
    cJSON_AddNumberToObject(o, "vir", g->vir);
    cJSON_AddNumberToObject(o, "hard", g->hard);
    cJSON_AddNumberToObject(o, "stab", g->stab);
    cJSON_AddStringToObject(o, "rarity", gossip_rarity_name(g->rarity));
}

char *gossip_genome_json(const char *addr)
{
    gossip_genome_t g;
    if (!addr || !gossip_genome(addr, &g)) {
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "a", addr);
    add_genome(o, &g);
    return print_free(o);
}

// A program table (§10 gossip.program): the record's fields (when held) and
// the genome. Under the lock. with_record false: the genome and a alone.
static cJSON *program_obj(const char *a, bool need_record)
{
    gossip_genome_t g;
    if (!gossip_genome(a, &g)) {
        return NULL;
    }
    const prog_t *p = prog_find(a);
    cJSON *r = p && p->json ? cJSON_Parse(p->json) : NULL;
    if (need_record && !r) {
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "a", a);
    if (r) {
        const char *par = cJSON_GetStringValue(cJSON_GetObjectItem(r, "par"));
        cJSON_AddStringToObject(o, "par", par ? par : "");
        cJSON_AddNumberToObject(o, "gen", jint(r, "gen", 0, 0, 1000000));
        char name[17];
        game_ascii(name, sizeof(name), cJSON_GetStringValue(cJSON_GetObjectItem(r, "name")) ?: "");
        if (name[0]) {
            cJSON_AddStringToObject(o, "name", name);
        }
        const cJSON *nodes = cJSON_GetObjectItem(r, "nodes");
        if (cJSON_IsNumber(nodes)) {
            cJSON_AddNumberToObject(o, "nodes", jint(r, "nodes", 0, 0, INT32_MAX));
        }
        cJSON_Delete(r);
    }
    cJSON_AddBoolToObject(o, "verified", p && p->ok == PV_OK);
    bool spam = (p && p->ok == PV_OK && (p->fl & 1)) || addr_in(a, (const char (*)[ADDR1])S->cfg.spam, S->cfg.nspam);
    cJSON_AddBoolToObject(o, "spam", spam);
    if (p && p->ok == PV_OK) {
        if (p->fl & 2) {
            cJSON_AddTrueToObject(o, "genesis");
        }
        if (p->fl & 4) {
            cJSON_AddTrueToObject(o, "hybrid");
        }
    }
    add_genome(o, &g);
    return o;
}

char *gossip_program_json(const char *addr)
{
    if (!S || !addr || !gg_addr_ok(addr)) {
        return NULL;
    }
    lock();
    prog_t *p = prog_find(addr);
    if (p) {
        p->used = now_ms();
    }
    cJSON *o = program_obj(addr, true);
    unlock();
    return print_free(o);
}

char *gossip_programs_json(void)
{
    if (!S) {
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON *slots = cJSON_AddArrayToObject(o, "slots"), *inbox = cJSON_AddArrayToObject(o, "inbox");
    lock();
    for (int i = 0; i < S->nslots; i++) {
        cJSON *p = S->slots[i][0] ? program_obj(S->slots[i], false) : NULL;
        cJSON_AddItemToArray(slots, p ? p : cJSON_CreateFalse());
    }
    for (int i = 0; i < S->ninbox; i++) {
        cJSON *p = program_obj(S->inbox[i], false);
        cJSON_AddItemToArray(inbox, p ? p : cJSON_CreateFalse());
    }
    unlock();
    return print_free(o);
}

// Lua on the badge has 32-bit numbers: server milliseconds (1.8e12) would
// arrive rounded by minutes. Apps get the Node's times as unix seconds.
static void ms_to_s(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsNumber(v)) {
        cJSON_SetNumberValue(v, (double)(int64_t)(v->valuedouble / 1000));
    }
}

char *gossip_me_json(void)
{
    if (!S || !s_claimed) {
        return NULL;
    }
    lock();
    cJSON *o = S->node_json ? cJSON_Parse(S->node_json) : NULL;
    int name_nodes = S->cfg.name_nodes, close = S->cfg.close;
    int inc = 0;
    for (int i = 0; i < HEAR_MAX; i++) {
        inc += S->hear[i].a[0] && S->hear[i].state != ST_HEARING;
    }
    unlock();
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        o = cJSON_CreateObject();       // no Node yet: the badge's own fields alone
    }
    ms_to_s(o, "clsAt");
    ms_to_s(cJSON_GetObjectItem(o, "epoch"), "ends");
    // An empty slot is null on the wire, which a Lua table can't hold (a hole: `#slots`
    // would miss a 4th or 5th slot): false, as gossip.programs() says it.
    cJSON *slots = cJSON_GetObjectItem(o, "slots");
    for (int i = 0; cJSON_IsArray(slots) && i < cJSON_GetArraySize(slots); i++) {
        if (cJSON_IsNull(cJSON_GetArrayItem(slots, i))) {
            cJSON_ReplaceItemInArray(slots, i, cJSON_CreateFalse());
        }
    }
    cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(o, "fx")) {
        ms_to_s(e, "until");
    }
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(o, "imm")) {
        ms_to_s(e, "until");
    }
    cJSON_DeleteItemFromObject(o, "id");
    cJSON_AddStringToObject(o, "id", s_id);
    cJSON_DeleteItemFromObject(o, "on");
    cJSON_AddBoolToObject(o, "on", gossip_on());
    cJSON_DeleteItemFromObject(o, "synced");
    cJSON_AddNumberToObject(o, "synced", (double)(s_synced / 1000));
    cJSON_DeleteItemFromObject(o, "incubating");
    cJSON_AddNumberToObject(o, "incubating", inc);
    cJSON_DeleteItemFromObject(o, "lead");
    cJSON_AddBoolToObject(o, "lead", os_setting(OS_SET_LEADER) != 0);   // "May become a leader"
    cJSON_DeleteItemFromObject(o, "name_nodes");
    cJSON_AddNumberToObject(o, "name_nodes", name_nodes);
    cJSON_DeleteItemFromObject(o, "close");
    cJSON_AddNumberToObject(o, "close", close);                         // Cfg close: claims' range (dBm)
    os_cloud_info_t *ci = malloc(sizeof(*ci));                         // ~1 KB: not on the Lua task's stack
    cJSON_DeleteItemFromObject(o, "site");
    if (ci) {
        os_cloud_get_info(ci);
        cJSON_AddStringToObject(o, "site", ci->site);                   // hello "site": the website's address, "" not told yet
        free(ci);
    }
    if (!cJSON_IsObject(cJSON_GetObjectItem(o, "ach"))) {
        cJSON *a = cJSON_AddObjectToObject(o, "ach");   // Node.ach before the server sent one
        cJSON_AddNumberToObject(a, "n", 0);
        cJSON_AddNumberToObject(a, "pts", 0);
        cJSON_AddArrayToObject(a, "last");
    }
    return print_free(o);
}

static const char *role_name(char ro)
{
    return ro == 'l' ? "l" : ro == 'm' ? "m" : ro == 'a' ? "a" : ro == 'z' ? "z" : "";
}

char *gossip_nearby_json(void)
{
    if (!S) {
        return NULL;
    }
    peer_t *list = heap_caps_malloc(sizeof(peer_t) * PEER_MAX, os_mem_bulk_caps());
    if (!list) {
        return NULL;
    }
    int64_t now = now_ms();
    int n = 0;
    gossip_cfg_t *cfg = heap_caps_malloc(sizeof(*cfg), os_mem_bulk_caps());
    if (!cfg) {
        heap_caps_free(list);
        return NULL;
    }
    lock();
    *cfg = S->cfg;
    for (int i = 0; i < PEER_MAX; i++) {
        if (S->peer[i].id[0] && now - S->peer[i].last_ms <= PEER_TTL_MS) {
            list[n++] = S->peer[i];
        }
    }
    unlock();
    // Strongest first.
    for (int i = 1; i < n; i++) {
        peer_t t = list[i];
        int k = i - 1;
        while (k >= 0 && list[k].rssi < t.rssi) {
            list[k + 1] = list[k];
            k--;
        }
        list[k + 1] = t;
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const peer_t *p = &list[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", p->id);
        cJSON_AddStringToObject(o, "name", p->name);
        cJSON_AddStringToObject(o, "kind", p->ro ? "role" : "badge");
        if (p->ro) {
            bool real = false;
            for (int r = 0; r < cfg->nroles; r++) {
                real |= !strcmp(cfg->roles[r].id, p->id) && cfg->roles[r].ro == p->ro;
            }
            cJSON_AddStringToObject(o, "role", role_name(p->ro));
            cJSON_AddBoolToObject(o, "real", real);
        }
        if (p->c) {
            char c[2] = {p->c, 0};
            cJSON_AddStringToObject(o, "class", c);
        }
        cJSON_AddNumberToObject(o, "rssi", p->rssi);
        cJSON_AddStringToObject(o, "st", p->st);
        if (p->st[0] && addr_in(p->st, (const char (*)[ADDR1])cfg->spam, cfg->nspam)) {
            cJSON_AddTrueToObject(o, "spam");                           // Cfg spam: a Validator may cure it
        }
        cJSON_AddNumberToObject(o, "boost", p->b);
        cJSON_AddItemToArray(arr, o);
    }
    heap_caps_free(list);
    heap_caps_free(cfg);
    return print_free(arr);
}

char *gossip_hearing_json(void)
{
    if (!S) {
        return NULL;
    }
    static const char *const states[] = {"hearing", "exposed", "incubating"};
    cJSON *arr = cJSON_CreateArray();
    lock();
    int64_t thr = (int64_t)S->cfg.thr * W_SCALE;
    for (int i = 0; i < HEAR_MAX; i++) {
        const hear_t *h = &S->hear[i];
        if (!h->a[0]) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "a", h->a);
        int pct = h->state != ST_HEARING ? 100 : (int)((int64_t)h->exp * 100 / (thr ? thr : 1));
        cJSON_AddNumberToObject(o, "pct", pct > 100 ? 100 : pct);
        cJSON_AddStringToObject(o, "state", states[h->state <= ST_INCUBATING ? h->state : 0]);
        cJSON_AddStringToObject(o, "from", h->from);
        cJSON_AddNumberToObject(o, "rssi", h->rssi);
        cJSON_AddItemToArray(arr, o);
    }
    unlock();
    return print_free(arr);
}

// ---------------------------------------------------------------------------
// Console: gossip [status|peers|progs|sync|on|off]

// `gossip bench`: what the OS task spends on 20 peers' HELLOs and a full
// hearing table, measured with made-up peers (ids ZZ00..ZZ19, a vector's
// program, a bad tag: nothing of it reaches the server), then put back.
static void bench(void)
{
    static const char *const addrs[3] = {"59qkXRBPMBADVrovwJYTsqXF21MvW9YYSGofQneQMruo",
                                         "G2QqkHaYPwAg64t6soMZvrDDT48gH3SF8DYsfutQ2JWC",
                                         "31UFvJfpgAT4Paado1LNpEor9LL5DXMFAiCYzE2j5Cdj"};
    state_t *save = heap_caps_malloc(sizeof(*S), os_mem_bulk_caps());
    hop_t *hop = heap_caps_malloc(sizeof(hop_t), os_mem_bulk_caps());
    if (!save || !hop) {
        heap_caps_free(save);
        heap_caps_free(hop);
        printf("gossip bench: no memory\n");
        return;
    }
    lock();
    memcpy(save, S, sizeof(*S));
    memset(S->hear, 0, sizeof(S->hear));
    S->nslots = S->ninbox = S->nfx = S->nimm = S->ndone = 0;
    S->act[0] = 0;
    unlock();
    bool was_run = s_run, was_claimed = s_claimed;
    s_run = s_claimed = true;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 20; i++) {
        memset(hop, 0, sizeof(*hop));
        snprintf(hop->id, sizeof(hop->id), "ZZ%02d", i);
        hop->rssi = (int8_t)(-45 - i * 2);
        snprintf(hop->json, sizeof(hop->json),
                 "{\"k\":\"b\",\"n\":\"@bench%02d\",\"lv\":%d,\"st\":\"%s\",\"g\":\"r%d.%lld.0123456789abcdef\"}", i, i,
                 addrs[i % 3], i % 10, (long long)(gossip_server_ms() / 30000));
        hello_one(hop);
    }
    int64_t t1 = esp_timer_get_time();
    // Fill all eight entries, then the once-a-second maths, 100 times.
    lock();
    for (int i = 0; i < HEAR_MAX; i++) {
        uint8_t b[32], a[32];
        gg_b58_decode32(addrs[i % 3], b);
        a[0] = (uint8_t)i;
        memcpy(a + 1, b + 1, 31);
        hear_t *h = &S->hear[i];
        memset(h, 0, sizeof(*h));
        gg_b58_encode32(a, h->a);
        gg_decode(a, &h->gen);
        strlcpy(h->from, "ZZ00", sizeof(h->from));
        h->rssi = (int8_t)(-50 - i * 3);
        h->best = h->rssi;
        h->boost = (uint8_t)(i % 4);
        h->last_ms = now_ms();
    }
    S->cfg.thr = 1000000;           // nobody gets exposed meanwhile
    unlock();
    int64_t t2 = esp_timer_get_time();
    for (int k = 0; k < 100; k++) {
        second(now_ms());
    }
    int64_t t3 = esp_timer_get_time();
    // The tag of the badge's own HELLO, as spread() makes it on a change.
    char msg[120], tag[OS_MESH_TAG_LEN + 1];
    gg_tag_msg(msg, sizeof(msg), s_id[0] ? s_id : "ZZ0000", addrs[0], 59000000, 'r', 0);
    int64_t t4 = esp_timer_get_time();
    bool signed_ok = os_mesh_sign(msg, tag);
    int64_t t5 = esp_timer_get_time();
    lock();
    memcpy(S, save, sizeof(*S));
    for (int i = 0; i < PEER_MAX; i++) {
        if (!strncmp(S->peer[i].id, "ZZ", 2)) {
            memset(&S->peer[i], 0, sizeof(S->peer[i]));
        }
    }
    unlock();
    s_run = was_run;
    s_claimed = was_claimed;
    heap_caps_free(save);
    heap_caps_free(hop);
    printf("gossip bench: 20 HELLOs parsed in %lld us (%lld us each); a second's maths over 8 programs %lld us; "
           "the HELLO tag %lld us%s\n",
           (long long)(t1 - t0), (long long)((t1 - t0) / 20), (long long)((t3 - t2) / 100), (long long)(t5 - t4),
           signed_ok ? "" : " (no mesh key)");
}

static void cmd_run(void *arg)
{
    char *line = arg;
    const char *why = "";
    if (!strcmp(line, "on")) {
        os_setting_set(OS_SET_GOSSIP, 1);
        gossip_setting_changed();
    } else if (!strcmp(line, "off")) {
        os_setting_set(OS_SET_GOSSIP, 0);
        gossip_setting_changed();
    } else if (!strcmp(line, "sync")) {
        s_next_sync = 0;
        s_soon = true;
    } else if (!strcmp(line, "peers")) {
        char *s = gossip_nearby_json();
        printf("gossip peers: %s\n", s ? s : "[]");
        cJSON_free(s);
        free(line);
        return;
    } else if (!strcmp(line, "bench")) {
        bench();
        free(line);
        return;
    } else if (!strcmp(line, "notice")) {
        // A test notice through the service's one path (held while this badge is in a fight).
        static int n;
        char t[32];
        snprintf(t, sizeof(t), "test notice %d", ++n);
        bool done = gossip_notice(0, t);
        lock();
        printf("gossip notice: %s, held %d, dropped %d\n", done ? "ok" : "not shown (one per 10 s)", s_nheld,
               s_held_drop);
        unlock();
        free(line);
        return;
    } else if (!strcmp(line, "progs")) {
        lock();
        printf("gossip progs: %d of %d%s\n", S->nprog, s_prog_cap, s_card ? " (card)" : " (RAM)");
        for (int i = 0; i < S->nprog; i++) {
            const prog_t *p = &S->prog[i];
            static const char *const oks[] = {"unchecked", "verified", "BAD SIGNATURE", "BAD ADDRESS"};
            printf("  %s %s%s fl %d%s\n", p->a, oks[p->ok & 3], p->checking ? " (checking)" : "", p->fl,
                   node_holds(p->a) ? ", held" : "");
        }
        unlock();
        free(line);
        return;
    } else if (line[0] && strcmp(line, "status")) {
        printf("usage: gossip [status|peers|progs|sync|on|off|notice]\n");
        free(line);
        return;
    }
    (void)why;
    char st[OS_SERVICE_STATUS_MAX + 1];
    svc_status(NULL, st, sizeof(st));
    int64_t srv = gossip_server_ms();
    bool fighting = gossip_role_fighting();     // gr_mx before s_mx
    lock();
    printf("gossip: %s (%s), setting %s, %s, clock %s, key %s\n", gossip_on() ? "on" : "off", st,
           os_setting(OS_SET_GOSSIP) ? "on" : "off", s_claimed ? "claimed" : "unclaimed", srv ? "yes" : "no",
           s_has_key ? "yes" : "no");
    printf("  node seq %d, cfg %d, class %c, act %s, boost %d, %d slots, %d inbox, %d effects, %d immune\n", S->seq,
           S->cfg_rev, S->cls ? S->cls : '-', S->act[0] ? S->act : "-", S->boost, S->nslots, S->ninbox, S->nfx,
           S->nimm);
    printf("  waiting: %d records (next %" PRIu32 "), %d actions (next %" PRIu32 "), %d pending; %d programs kept\n",
           S->nrec, s_rn, S->nact, s_an, S->npending, S->nprog);
    printf("  sync: last HTTP %d, %s, next in %lld ms, every %d ms%s%s%s%s\n", s_last_status,
           s_tok ? "in flight" : "idle", (long long)(s_next_sync - now_ms()), sync_interval(),
           s_why[0] ? ", " : "", s_why, os_api_online() ? "" : ", offline (os_api)", s_run ? "" : ", not running");
    printf("  hello: %s\n", s_extra[0] ? s_extra : "(no gossip fields)");
    printf("  notices: held %d, dropped %d (in a fight: %s)\n", s_nheld, s_held_drop,
           fighting ? "yes" : "no");
    printf("  heard: %" PRIu32 " hellos, %" PRIu32 " bad, %" PRIu32 " dropped, %d badges near; checker %s, "
           "last %lld us, stack left %d B\n",
           s_hellos, s_rx_bad, s_hops_dropped, peers_near(), s_verifying ? "running" : "idle",
           (long long)s_verify_us, s_verify_stack);
    for (int i = 0; i < HEAR_MAX; i++) {
        const hear_t *h = &S->hear[i];
        if (h->a[0]) {
            static const char *const states[] = {"hearing", "exposed", "incubating"};
            printf("  %s %s %ld.%03ld/%d from %s %d dBm (best %d), %d s, w %ld.%03ld, %d peers\n", h->a,
                   states[h->state % 3], (long)(h->exp / W_SCALE), (long)(h->exp % W_SCALE), S->cfg.thr, h->from,
                   h->rssi, h->best, h->secs, (long)(h->last_w / W_SCALE), (long)(h->last_w % W_SCALE), h->peers);
        }
    }
    unlock();
    free(line);
}

static int cmd_gossip(int argc, char **argv)
{
    char *line = calloc(1, 16);
    if (!line) {
        return 1;
    }
    if (argc > 1) {
        strlcpy(line, argv[1], 16);
    }
    if (!os_post(cmd_run, line)) {
        free(line);
        printf("gossip: busy, try again\n");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------

void gossip_init(void)
{
    // One allocation, at boot: long-lived things made later split the heap.
    S = heap_caps_calloc(1, sizeof(*S), os_mem_bulk_caps());
    s_mx = xSemaphoreCreateMutex();
    s_hop_mx = xSemaphoreCreateMutex();
    if (!S || !s_mx || !s_hop_mx) {
        ESP_LOGE(TAG, "no memory: Gossip is off");
        heap_caps_free(S);
        S = NULL;
        return;
    }
    if (!os_mem_psram()) {
        s_rec_cap = REC_MAX_SRAM;
    }
    cfg_defaults(&S->cfg);
    s_rn = nvs_u32("rn");
    s_an = nvs_u32("an");
    s_rn = s_rn ? s_rn : 1;
    s_an = s_an ? s_an : 1;
    s_epoch = os_data_epoch();
    load_all();
    key_load();
    os_service_register(&s_def);
    os_mesh_subscribe(OS_MESH_HELLO, on_mesh, NULL);
    os_cmd_register("gossip", "gossip [status|peers|progs|sync|on|off] - Gossip (2.4.0)", cmd_gossip);
    ESP_LOGI(TAG, "ready: %u B of state", (unsigned)sizeof(*S));
}
