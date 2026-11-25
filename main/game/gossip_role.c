// Gossip roles (Solana OS 2.4.0, game/gossip_role.h; the parent repo's
// docs/GOSSIP.md §4 and §8). Builder: fw-roles.
//
// This file: the role identity (the offer, the session as a station with the
// role key's HELLO tags, its ends), the node service's sync seam, and the
// plumbing of the reserved app id `gossip`: the mesh task copies what passes a
// cheap pre-filter into a ring and the OS task parses it; what we send waits
// in two small queues (the badge's, the role's) that keep GOSSIP.md §4's
// rates. Claims, bids and cures are in gossip_role_app.c, the fight in
// gossip_role_boss.c.
//
// The role key only ever leaves this file as tags: no read gives it to an app.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp.h"
#include "core/os.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"
#include "core/os_mem.h"
#include "core/os_service.h"
#include "core/os_settings.h"
#include "core/os_wifi.h"
#include "core/theme.h"
#include "game.h"
#include "gossip_role_int.h"
#include "lua/lua_host.h"

static const char *TAG = GR_TAG;

#define OFFER_ACCEPT_MS     30000       // GOSSIP.md §8: accepts by itself after 30 s
#define OFFER_FORGET_MS     120000      // an answered offer the server never followed up
#define GRACE_MS            90000       // roleGraceSeconds: no sync for this long ends it here too
#define UNTIL_SLACK_MS      5000        // past `until` by the server's clock: off the air
#define LEADER_TURN_MS      10000       // a Leader's block: 10 s a program, in turn
#define BATT_MIN            15          // % off USB (as Collaborator)
#define BATT_EVERY_MS       10000
#define MISSES_MAX          2           // answers without `role` in a row before a role is dropped
#define TALLY_MAX           512
#define BOSS_LINGER_MS      90000       // a boss whose fight is over answers claims this long after the server ended it (GOSSIP.md §8)
#define RING_N              24          // messages the mesh task may queue for the OS task
#define RX_MAX              300         // longest APP message kept (d <= 200 plus the envelope)
#define QUEUE_N             8           // per sender (badge, role)
#define GAP_BADGE_MS        1000        // a badge: one gossip message a second ...
#define GAP_FIGHT_MS        500         // ... two in a boss fight
#define GAP_ROLE_MS         250         // a role: four a second
#define EV_N                8
#define BLOCK_MAX           4
#define ST_MAX              4

SemaphoreHandle_t gr_mx;
gr_stats_t gr_stats;

// ---------------------------------------------------------------------------
// The role: an offer, a session, or nothing (all under gr_mx)

typedef enum { PH_NONE, PH_OFFER, PH_SESSION } phase_t;

typedef struct {
    phase_t phase;
    long sid;
    char ro;
    // offer
    int64_t offer_until;            // server ms
    int64_t offer_seen;             // local ms
    int answer;                     // 0 not yet, 1 yes, -1 no
    // session
    char id[OS_MESH_ID_LEN + 1];
    uint8_t key[16];
    char name[25];
    int64_t since, until;           // server ms
    char st[ST_MAX][GOSSIP_ADDR_MAX + 1];
    int nst;
    int boost;
    int bpm, beats, min, min_beats; // the boss: RoleState.boss (min = bossMinVoters, min_beats = bossMinBeats)
    char spam[GOSSIP_ADDR_MAX + 1];
    char *tally;                    // the server's tally as JSON text, or NULL
    bool on_air;
    int64_t on_air_at;              // local ms
    gossip_role_pick_t block[BLOCK_MAX];
    int nblock;
    int misses;
    int64_t linger_until;           // local ms: a finished boss on the air for its fight's claims (0 = no)
    char hello[220];                // the HELLO last handed to os_mesh
    // an end to report
    const char *end;                // "owner", "battery", "busy"
    long end_sid;
    // the last end the server told us of
    long ended_sid;
    char ended_ro;
    char ended_by[12];
    int ended_stake, ended_lamps;
    int64_t ended_at;
} role_t;

// The role and the other big state live in PSRAM when there is some (SRAM is
// the badge's scarcest): allocated in gossip_role_init, which gives up (and
// leaves gr_mx NULL, so every entry point returns at once) without them.
static role_t *s_role;
#define R (*s_role)

static int64_t s_last_sync;         // local ms of the last delivered sync
static long s_quit_sid;             // the last session this badge ended itself: never restarted
static int64_t s_batt_at;
static int s_batt = 100;
static bool s_usb = true;
static char s_me[OS_MESH_ID_LEN + 1];
static os_cloud_info_t *s_infobuf;  // the tick's copy of os_cloud's
static gossip_cfg_t *s_cfgnext;     // the tick's copy of the Cfg before it takes the lock
// What the last sync_fill put in, so sync_done knows what was delivered.
static bool s_fill_end, s_fill_no;
static long s_fill_end_sid;

// Test mode (console `grole test …`, RAM only, gone at a restart): stands in
// for the node service and the server until both exist (GOSSIP.md §7's sync
// comes from fw-node's gossip.c). It makes gossip "on", gives a clock, a
// class, a held spam program and live role ids.
static bool s_test;
static int64_t s_test_offset;       // server ms - local ms
static char s_test_class;
static char s_test_spam[GOSSIP_ADDR_MAX + 1];
static char s_test_real[GOSSIP_CFG_ROLES][OS_MESH_ID_LEN + 1];
static int s_test_close;            // dBm, 0 = Cfg's

// ---------------------------------------------------------------------------
// Small helpers

int64_t gr_ms(void)
{
    return esp_timer_get_time() / 1000;
}

int64_t gr_server_ms(void)
{
    int64_t t = gossip_server_ms();
    if (!t && s_test && s_test_offset) {
        t = gr_ms() + s_test_offset;
    }
    return t;
}

bool gr_on(void)
{
    return s_test || gossip_on();
}

char gr_class(void)
{
    return s_test_class ? s_test_class : gossip_class();
}

bool gr_held_spam(char out[GOSSIP_ADDR_MAX + 1])
{
    if (s_test_spam[0]) {
        strlcpy(out, s_test_spam, GOSSIP_ADDR_MAX + 1);
        return true;
    }
    return gossip_held_spam(out);
}

static gossip_cfg_t *s_cfgp;        // under gr_mx, refreshed every tick (PSRAM: it is 500 B)
#define s_cfg (*s_cfgp)

int gr_close(void)
{
    if (s_test && s_test_close) {
        return s_test_close;
    }
    return s_cfg.close ? s_cfg.close : GR_CLOSE_DEF;
}

bool gr_real(const char *id)
{
    for (int i = 0; i < s_cfg.nroles && i < GOSSIP_CFG_ROLES; i++) {
        if (!strcmp(s_cfg.roles[i].id, id)) {
            return true;
        }
    }
    for (int i = 0; s_test && i < GOSSIP_CFG_ROLES; i++) {
        if (s_test_real[i][0] && !strcmp(s_test_real[i], id)) {
            return true;
        }
    }
    return false;
}

// The owner opted in to roles, or test mode stands in for it (no NVS write).
static bool opted(void)
{
    return s_test || gossip_role_opted_in();
}

bool gr_me(char out[OS_MESH_ID_LEN + 1])
{
    strlcpy(out, s_me, OS_MESH_ID_LEN + 1);
    return out[0] != 0;
}

bool gr_id_ok(const char *s)
{
    if (!s || strlen(s) != OS_MESH_ID_LEN) {
        return false;
    }
    for (int i = 0; i < OS_MESH_ID_LEN; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (i == 0 && c == 'e'))) {
            return false;
        }
    }
    return true;
}

bool gr_addr_ok(const char *s)
{
    static const char B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    size_t n = s ? strlen(s) : 0;
    if (n < 32 || n > GOSSIP_ADDR_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!strchr(B58, s[i])) {
            return false;
        }
    }
    return true;
}

bool gr_nonce_ok(const char *s)
{
    if (!s || strlen(s) != 16) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

int gr_jint(const cJSON *o, const char *key, int lo, int hi, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) {
        return def;
    }
    return (int)v->valuedouble;
}

static int64_t jms(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) && v->valuedouble > 0 && v->valuedouble < 1e15 ? (int64_t)v->valuedouble : 0;
}

static bool hex16(const char *s, uint8_t out[16])
{
    if (!s || strlen(s) != 32) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) {
            return false;
        }
        out[i / 2] = (uint8_t)(i % 2 ? out[i / 2] | v : v << 4);
    }
    return true;
}

// Printable ASCII without the characters a hand-built JSON string would need to escape.
static void name_copy(char *out, size_t n, const char *in)
{
    size_t j = 0;
    for (const char *p = in ? in : ""; *p && j + 1 < n; p++) {
        if (*p >= 0x20 && *p <= 0x7e && *p != '"' && *p != '\\') {
            out[j++] = *p;
        }
    }
    out[j] = 0;
}

static const char *role_name(char ro)
{
    switch (ro) {
    case 'l': return "Leader";
    case 'm': return "MEV Bot";
    case 'a': return "Anonymous Deployer";
    case 'z': return "Byzantine Leader";
    default:  return "role";
    }
}

static bool ro_ok(char ro)
{
    return ro == 'l' || ro == 'm' || ro == 'a' || ro == 'z';
}

bool gr_role_live(gr_live_t *out)
{
    if (R.phase != PH_SESSION || !R.on_air) {
        return false;
    }
    out->sid = R.sid;
    out->ro = R.ro;
    strlcpy(out->id, R.id, sizeof(out->id));
    memcpy(out->key, R.key, 16);
    return true;
}

// ---------------------------------------------------------------------------
// Out: two send queues, on_gossip events and one notice, flushed off the lock

typedef struct {
    char to[OS_MESH_ID_LEN + 1];    // "" = everyone (role broadcasts)
    char json[GR_MSG_MAX + 1];
} msg_t;

static msg_t *s_bq, *s_rq;          // PSRAM when there is some
static msg_t *s_msgbuf;             // gr_flush's one message in hand
static int s_nbq, s_nrq;
static int64_t s_badge_at, s_role_at;
static cJSON *s_ev[EV_N];
static int s_nev;
static char s_notice[96];
static uint32_t s_notice_led;

static bool envelope(const char *to, const cJSON *d, char out[GR_MSG_MAX + 1])
{
    char dj[GR_D_MAX + 1];
    if (!cJSON_IsObject(d) || !cJSON_PrintPreallocated((cJSON *)d, dj, sizeof(dj), false)) {
        return false;                   // d over 200 bytes (MESH.md §6)
    }
    int n = snprintf(out, GR_MSG_MAX + 1, "{\"app\":\"" GR_APP "\",\"to\":\"%s\",\"d\":%s}", to && to[0] ? to : "*", dj);
    return n > 0 && n <= GR_MSG_MAX;
}

static bool enqueue(msg_t *q, int *n, const char *to, const cJSON *d)
{
    if (!q || *n >= QUEUE_N) {
        gr_stats.q_drop++;
        return false;
    }
    msg_t *m = &q[*n];
    if (!envelope(to, d, m->json)) {
        gr_stats.q_drop++;
        return false;
    }
    strlcpy(m->to, to ? to : "", sizeof(m->to));
    (*n)++;
    return true;
}

bool gr_send_badge(const char *to, const cJSON *d)
{
    return gr_id_ok(to) && enqueue(s_bq, &s_nbq, to, d);
}

bool gr_send_role(const char *to, const cJSON *d)
{
    return (!to || gr_id_ok(to)) && enqueue(s_rq, &s_nrq, to, d);
}

void gr_event(cJSON *ev)
{
    if (!ev) {
        return;
    }
    if (s_nev == EV_N) {
        cJSON_Delete(s_ev[0]);          // the oldest goes: apps read state, events only wake them
        memmove(&s_ev[0], &s_ev[1], (EV_N - 1) * sizeof(cJSON *));
        s_nev--;
    }
    s_ev[s_nev++] = ev;
}

void gr_notice(uint32_t led, const char *text)
{
    strlcpy(s_notice, text, sizeof(s_notice));
    s_notice_led = led;
}

static void pop(msg_t *q, int *n, msg_t *out)
{
    *out = q[0];
    memmove(&q[0], &q[1], (*n - 1) * sizeof(msg_t));
    (*n)--;
}

// OS task. Sends what the rates allow now (the rest waits for the next tick or
// kick), then the events and the notice.
static void gr_flush(void)
{
    msg_t *m = s_msgbuf;                // OS task only: off the stack
    for (int round = 0; round < 4; round++) {
        bool sent = false;
        int64_t now = gr_ms();
        // The role's: the boss's beat first (its "ms" is computed now), then the queue.
        xSemaphoreTake(gr_mx, portMAX_DELAY);
        bool role = false;
        if (now - s_role_at >= GAP_ROLE_MS) {
            cJSON *d = gr_boss_due(now);
            if (d) {
                role = envelope(NULL, d, m->json);
                m->to[0] = 0;
                cJSON_Delete(d);
            } else if (s_nrq) {
                pop(s_rq, &s_nrq, m);
                role = true;
            }
            if (role) {
                s_role_at = now;
            }
        }
        xSemaphoreGive(gr_mx);
        if (role) {
            esp_err_t err = os_mesh_station_send(m->to[0] ? m->to : NULL, m->json);
            err == ESP_OK ? gr_stats.tx_role++ : gr_stats.tx_fail++;
            sent = true;
        }
        // The badge's: a vote summary when one is due, else the queue.
        xSemaphoreTake(gr_mx, portMAX_DELAY);
        bool badge = false;
        int gap = gr_boss_fighting() ? GAP_FIGHT_MS : GAP_BADGE_MS;
        if (now - s_badge_at >= gap) {
            char to[OS_MESH_ID_LEN + 1];
            cJSON *d = gr_boss_vote_due(now, to);
            if (d) {
                badge = envelope(to, d, m->json);
                strlcpy(m->to, to, sizeof(m->to));
                cJSON_Delete(d);
            } else if (s_nbq) {
                pop(s_bq, &s_nbq, m);
                badge = true;
            }
            if (badge) {
                s_badge_at = now;
            }
        }
        xSemaphoreGive(gr_mx);
        if (badge) {
            os_mesh_peer_t p;
            // Unicast only, and never over a bridge: bridges carry no gossip (GOSSIP.md §1).
            esp_err_t err = os_mesh_near(m->to, &p) && !p.bridged ? os_mesh_send(OS_MESH_APP, m->to, m->json)
                                                                 : ESP_ERR_NOT_FOUND;
            err == ESP_OK ? gr_stats.tx_badge++ : gr_stats.tx_fail++;
            sent = true;
        }
        if (!sent) {
            break;
        }
    }
    cJSON *ev[EV_N];
    char notice[sizeof(s_notice)];
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    int nev = s_nev;
    memcpy(ev, s_ev, sizeof(ev));
    s_nev = 0;
    strlcpy(notice, s_notice, sizeof(notice));
    uint32_t led = s_notice_led;
    s_notice[0] = 0;
    xSemaphoreGive(gr_mx);
    for (int i = 0; i < nev; i++) {
        char *j = cJSON_PrintUnformatted(ev[i]);
        if (j) {
            lua_host_gossip(j);
            if (s_test) {
                printf("grole ev %s\n", j);
            }
            cJSON_free(j);
        }
        cJSON_Delete(ev[i]);
    }
    if (notice[0]) {
        gossip_notice(led, notice);     // held while this badge is in a fight
        ESP_LOGI(TAG, "notice: %s", notice);
    }
}

static volatile bool s_kick_queued;

static void kick(void *arg)
{
    s_kick_queued = false;
    gr_flush();
}

// Any task: send what waits soon (the OS task runs it).
static void gr_kick(void)
{
    if (!s_kick_queued) {
        s_kick_queued = true;
        if (!os_post(kick, NULL)) {
            s_kick_queued = false;      // the tick catches up
        }
    }
}

// ---------------------------------------------------------------------------
// In: the mesh task copies, the OS task parses (MESH.md "Gossip", hostile input)

typedef struct {
    char from[OS_MESH_ID_LEN + 1];
    uint8_t kind;
    int8_t rssi;
    volatile uint8_t ready;         // the mesh task filled it (set last, under the spinlock)
    int64_t at;                     // ms the mesh task had it: the fight's timing reads it
    char json[RX_MAX + 1];
} rx_t;

static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static rx_t *s_ring;                // RING_N, PSRAM when there is some
static rx_t *s_rxbuf;               // the drain's copy of one
static int s_ring_head, s_ring_n;
static volatile bool s_drain_queued;

static void drain(void *arg);

// Mesh task: a cheap filter, a copy, a post. No parsing, no locks but the ring's.
static void on_mesh(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx)
{
    if (!s_ring || from->bridged || from->kind == OS_PEER_BEACON || !strstr(json, "\"" GR_APP "\"")) {
        return;
    }
    size_t len = strlen(json);
    if (len > RX_MAX) {
        gr_stats.bad++;
        return;
    }
    int64_t at = gr_ms();
    bool room;
    portENTER_CRITICAL(&s_rx_mux);
    room = s_ring_n < RING_N;
    rx_t *r = room ? &s_ring[(s_ring_head + s_ring_n) % RING_N] : NULL;
    if (room) {
        s_ring_n++;                     // claimed: only this task writes slots, the OS task reads after
    }
    portEXIT_CRITICAL(&s_rx_mux);
    if (!r) {
        gr_stats.ring_drop++;
        return;
    }
    // The slot is ours until it is ready: the drain stops at the first that isn't.
    strlcpy(r->from, from->id, sizeof(r->from));
    r->kind = from->kind;
    r->rssi = from->rssi;
    r->at = at;
    memcpy(r->json, json, len + 1);
    portENTER_CRITICAL(&s_rx_mux);
    r->ready = 1;
    portEXIT_CRITICAL(&s_rx_mux);
    if (!s_drain_queued) {
        s_drain_queued = true;
        if (!os_post(drain, NULL)) {
            s_drain_queued = false;     // the tick drains
        }
    }
}

static int type_of(const char *t)
{
    static const char *const names[] = {"claim", "ack", "bid", "vote", "boss", "cure", "cured"};
    for (int i = 0; i < GR_T_OTHER; i++) {
        if (!strcmp(t, names[i])) {
            return i;
        }
    }
    return GR_T_OTHER;
}

static void handle(const rx_t *r)
{
    cJSON *m = cJSON_Parse(r->json);
    const char *app = game_jstr(m, "app"), *to = game_jstr(m, "to");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(m, "d");
    const char *t = game_jstr(d, "t");
    char me[OS_MESH_ID_LEN + 1];
    gr_me(me);
    bool to_role = R.phase == PH_SESSION && R.on_air && !strcmp(to, R.id);
    bool to_me = me[0] && !strcmp(to, me);
    bool all = !strcmp(to, "*");
    int k = type_of(t);
    char dj[GR_D_MAX + 2];
    bool ok = !strcmp(app, GR_APP) && cJSON_IsObject(d) && (to_role || to_me || all) && k != GR_T_OTHER &&
              cJSON_PrintPreallocated((cJSON *)d, dj, sizeof(dj), false) && strlen(dj) <= GR_D_MAX &&
              gr_id_ok(r->from) && strcmp(r->from, me) && strcmp(r->from, R.id);
    if (ok) {
        gr_stats.rx[k]++;
        if (k == GR_T_VOTE || k == GR_T_BOSS) {
            gr_boss_rx(t, r->from, (os_peer_kind_t)r->kind, r->rssi, r->at, to_role, d);
        } else {
            gr_app_rx(t, r->from, (os_peer_kind_t)r->kind, r->rssi, to_role, d);
        }
    } else {
        gr_stats.bad++;
    }
    cJSON_Delete(m);
}

static void drain_locked(void)
{
    rx_t *r = s_rxbuf;                  // OS task only
    for (;;) {
        bool have = false;
        portENTER_CRITICAL(&s_rx_mux);
        if (s_ring_n && s_ring[s_ring_head].ready) {
            *r = s_ring[s_ring_head];
            s_ring[s_ring_head].ready = 0;
            s_ring_head = (s_ring_head + 1) % RING_N;
            s_ring_n--;
            have = true;
        }
        portEXIT_CRITICAL(&s_rx_mux);
        if (!have) {
            break;
        }
        handle(r);
    }
}

static void drain(void *arg)
{
    s_drain_queued = false;
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    drain_locked();
    xSemaphoreGive(gr_mx);
    gr_flush();
}

// ---------------------------------------------------------------------------
// The role identity

static bool busy_now(void)
{
    char other[OS_MESH_ID_LEN + 1];
    // A badge holds one of: a Collaborator station, a badge beacon, a role (GOSSIP.md §4).
    return os_mesh_beacon_on() || (os_mesh_station_id(other) && strcmp(other, R.id));
}

// The role HELLO (GOSSIP.md §4), rebuilt when its window, program or boost changes.
static void role_hello(int64_t now)
{
    char hj[sizeof(R.hello)];
    int64_t t = gr_server_ms();
    if (!t) {
        hj[0] = 0;                      // without a clock: the plain station HELLO
    } else {
        long long w = t / 30000;
        const char *st = "";
        if (R.nst && R.ro != 'm') {
            st = R.ro == 'l' ? R.st[((now - R.on_air_at) / LEADER_TURN_MS) % R.nst] : R.st[0];
        }
        char msg[128], tag[OS_MESH_TAG_LEN + 1];
        snprintf(msg, sizeof(msg), "gossip|%s|%s|%lld|n%d", R.id, st, w, R.boost);
        os_mesh_tag(R.key, msg, tag);
        snprintf(hj, sizeof(hj), "{\"k\":\"s\",\"n\":\"%s\",\"ro\":\"%c\",\"st\":\"%s\",\"g\":\"n%d.%lld.%s\"}",
                 R.name, R.ro, st, R.boost, w, tag);
    }
    if (strcmp(hj, R.hello)) {
        strlcpy(R.hello, hj, sizeof(R.hello));
        os_mesh_station_hello(hj[0] ? hj : NULL);
    }
}

static cJSON *role_ev_of(const char *state, long sid, char ro)
{
    cJSON *e = cJSON_CreateObject();
    char r[2] = {ro, 0};
    cJSON_AddStringToObject(e, "t", "role");
    cJSON_AddStringToObject(e, "state", state);
    cJSON_AddNumberToObject(e, "sid", sid);
    cJSON_AddStringToObject(e, "ro", r);
    return e;
}

static cJSON *role_ev(const char *state)
{
    return role_ev_of(state, R.sid, R.ro);
}

static void role_start(int64_t now)
{
    os_mesh_station(R.id, R.name);      // resets the station's HELLO: ours follows at once
    R.hello[0] = 0;
    R.on_air = true;
    R.on_air_at = now;
    R.nblock = 0;
    role_hello(now);
    gr_app_session(R.sid);
    if (R.ro == 'z') {
        gr_boss_start(R.sid, R.bpm, R.beats, R.min, R.min_beats, R.since);
    }
    char t[96];
    snprintf(t, sizeof(t), "you are on the air as %s", role_name(R.ro));
    gr_notice(COL_PURPLE, t);
    gr_event(role_ev("session"));
    ESP_LOGI(TAG, "session %ld: %s as %s", R.sid, role_name(R.ro), R.id);
}

// Off the air; end: what to report ("owner", "battery", "busy"), or NULL when
// the server already knows (it ended it, `until`, the grace).
static void role_stop(const char *end)
{
    if (R.on_air) {
        char cur[OS_MESH_ID_LEN + 1];
        if (os_mesh_station_id(cur) && !strcmp(cur, R.id)) {
            os_mesh_station(NULL, NULL);    // only ours: a Collaborator station that took over stays
        }
        R.on_air = false;
        R.hello[0] = 0;
        gr_boss_stop();
        gr_app_session(0);
        ESP_LOGI(TAG, "session %ld off the air (%s)", R.sid, end ? end : "over");
    }
    if (end) {
        R.end = end;
        R.end_sid = R.sid;
        s_quit_sid = R.sid;
        gossip_sync_soon();
    }
    free(R.tally);
    R.tally = NULL;
    R.phase = PH_NONE;
    R.linger_until = 0;
    cJSON *e = role_ev("off");
    if (end) {
        cJSON_AddStringToObject(e, "by", end);
    }
    gr_event(e);
}

static void apply_offer(const cJSON *o, int64_t now)
{
    long sid = gr_jint(o, "sid", 1, 0x7fffffff, 0);
    const char *ro = game_jstr(o, "ro");
    if (sid && R.phase == PH_SESSION && R.linger_until && sid != R.sid) {
        // A finished boss still answering claims: the server offers this badge a new role only
        // once the fight's claims had their 90 s by its clock (ours started a sync later).
        role_stop(NULL);
    }
    if (!sid || !ro_ok(ro[0]) || ro[1] || R.phase == PH_SESSION || sid == R.ended_sid) {
        return;
    }
    if (R.phase == PH_OFFER && R.sid == sid) {
        R.offer_until = jms(o, "until");
        return;
    }
    R.phase = PH_OFFER;
    R.sid = sid;
    R.ro = ro[0];
    R.offer_until = jms(o, "until");
    R.offer_seen = now;
    R.answer = 0;
    R.misses = 0;
    if (!opted() || busy_now()) {
        R.answer = -1;                  // the setting is off, or a station or beacon has the second id
        gossip_sync_soon();
        ESP_LOGI(TAG, "offer %ld (%s) declined: %s", sid, role_name(R.ro), busy_now() ? "busy" : "not opted in");
        return;
    }
    char t[96];
    snprintf(t, sizeof(t), "you are up as %s. Open the Lab, or decline there", role_name(R.ro));
    gr_notice(COL_PURPLE, t);
    gr_event(role_ev("offer"));
    ESP_LOGI(TAG, "offer %ld: %s", sid, role_name(R.ro));
}

static void apply_ended(const cJSON *e)
{
    long sid = gr_jint(e, "sid", 1, 0x7fffffff, 0);
    if (!sid || sid == R.ended_sid) {
        return;
    }
    char ro = R.sid == sid ? R.ro : 0;
    if (R.phase == PH_SESSION && R.sid == sid && R.on_air && R.ro == 'z' && gr_boss_over()) {
        // The server ends a boss the moment its report says how it went, but the players
        // claim after they hear the end (GOSSIP.md §8): their acks need the role key on the
        // air a while longer. The server takes a fight's claims until it settles them.
        if (!R.linger_until) {
            R.linger_until = gr_ms() + BOSS_LINGER_MS;
        }
    } else if (R.phase != PH_NONE && R.sid == sid) {
        role_stop(NULL);
    }
    if (R.end && R.end_sid == sid) {
        R.end = NULL;                   // the server has it already
    }
    R.ended_sid = sid;
    R.ended_ro = ro;
    name_copy(R.ended_by, sizeof(R.ended_by), game_jstr(e, "by"));
    R.ended_stake = gr_jint(e, "stake", 0, 1000000, 0);
    R.ended_lamps = gr_jint(e, "lamps", 0, 1000000, 0);
    R.ended_at = gr_ms();
    char t[96];
    int n = snprintf(t, sizeof(t), "your %s turn ended", ro ? role_name(ro) : "role");
    if (R.ended_stake) {
        n += snprintf(t + n, sizeof(t) - n, ": +%d Stake", R.ended_stake);
    }
    if (R.ended_lamps && n > 0 && n < (int)sizeof(t)) {
        snprintf(t + n, sizeof(t) - n, ", %d !LAMPs", R.ended_lamps);
    }
    gr_notice(COL_PURPLE, t);
    cJSON *ev = role_ev_of("ended", sid, ro);
    cJSON_AddStringToObject(ev, "by", R.ended_by);
    cJSON_AddNumberToObject(ev, "stake", R.ended_stake);
    cJSON_AddNumberToObject(ev, "lamps", R.ended_lamps);
    gr_event(ev);
}

static void apply_session(const cJSON *s, int64_t now)
{
    long sid = gr_jint(s, "sid", 1, 0x7fffffff, 0);
    const char *ro = game_jstr(s, "ro"), *id = game_jstr(s, "id");
    uint8_t key[16];
    if (!sid || !ro_ok(ro[0]) || ro[1] || !gr_id_ok(id) || !hex16(game_jstr(s, "key"), key) || sid == R.ended_sid) {
        return;
    }
    if ((R.end && R.end_sid == sid) || sid == s_quit_sid) {
        return;                         // we ended it (owner, battery, busy): never back on the air
    }
    bool same = R.phase == PH_SESSION && R.sid == sid;
    if (R.phase == PH_SESSION && !same) {
        role_stop(NULL);                // another session replaces it
    }
    if (!same && busy_now()) {
        R.phase = PH_SESSION;           // so role_stop reports the right sid
        R.sid = sid;
        R.ro = ro[0];
        role_stop("busy");
        return;
    }
    R.phase = PH_SESSION;
    R.sid = sid;
    R.ro = ro[0];
    R.misses = 0;
    strlcpy(R.id, id, sizeof(R.id));
    memcpy(R.key, key, 16);
    char nm[25];
    name_copy(nm, sizeof(nm), game_jstr(s, "n"));
    if (nm[0] == '@') {
        nm[0] = ' ';                    // only an X handle starts with @ (MESH.md §1)
    }
    strlcpy(R.name, nm[0] ? nm : role_name(R.ro), sizeof(R.name));
    R.since = jms(s, "since");
    R.until = jms(s, "until");
    R.boost = gr_jint(s, "boost", 0, 9, 0);
    R.nst = 0;
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(s, "st"), *a;
    cJSON_ArrayForEach(a, st) {
        if (R.nst < ST_MAX && cJSON_IsString(a) && gr_addr_ok(a->valuestring)) {
            strlcpy(R.st[R.nst++], a->valuestring, sizeof(R.st[0]));
        }
    }
    const cJSON *boss = cJSON_GetObjectItemCaseSensitive(s, "boss");
    R.bpm = gr_jint(boss, "bpm", 30, 240, 100);
    R.beats = gr_jint(boss, "beats", 1, 2000, 300);
    R.min = gr_jint(boss, "min", 1, 160, 3);
    R.min_beats = gr_jint(boss, "minBeats", 0, 600, 60);   // missing (an older server): 60
    const char *spam = game_jstr(boss, "spam");
    strlcpy(R.spam, gr_addr_ok(spam) ? spam : "", sizeof(R.spam));
    free(R.tally);
    R.tally = NULL;
    const cJSON *tally = cJSON_GetObjectItemCaseSensitive(s, "tally");
    if (cJSON_IsObject(tally)) {
        char *tj = cJSON_PrintUnformatted(tally);
        if (tj && strlen(tj) <= TALLY_MAX) {
            R.tally = strdup(tj);
        }
        cJSON_free(tj);
    }
    if (!same) {
        role_start(now);
    } else if (R.on_air) {
        role_hello(now);
    }
}

static void apply_state(const cJSON *role, int64_t now)
{
    const cJSON *offer = cJSON_GetObjectItemCaseSensitive(role, "offer");
    const cJSON *ended = cJSON_GetObjectItemCaseSensitive(role, "ended");
    if (cJSON_IsObject(offer)) {
        apply_offer(offer, now);
    } else if (cJSON_IsObject(ended)) {
        apply_ended(ended);
    } else if (cJSON_GetObjectItemCaseSensitive(role, "key")) {
        apply_session(role, now);
    }
}

// ---------------------------------------------------------------------------
// The seam

static int near_badges(void)
{
    os_mesh_peer_t *p = malloc(20 * sizeof(*p));
    int n = p ? os_mesh_peers(p, 20) : 0, k = 0;
    for (int i = 0; i < n; i++) {
        k += p[i].kind == OS_PEER_BADGE && !p[i].bridged;
    }
    free(p);
    return k;
}

static void battery_read(int64_t now)
{
    if (s_batt_at && now - s_batt_at < BATT_EVERY_MS) {
        return;
    }
    s_batt_at = now;
    s_usb = os_battery_usb();           // the OS's one filtered reading (2.4.0)
    s_batt = os_battery_pct();
}

// Adds item under key if what is printed still fits in *room; else deletes it.
static bool add_fit(cJSON *body, const char *key, cJSON *item, int *room)
{
    char *j = item ? cJSON_PrintUnformatted(item) : NULL;
    int need = j ? (int)strlen(j) + (int)strlen(key) + 4 : 0;
    cJSON_free(j);
    if (!j || need > *room) {
        cJSON_Delete(item);
        return false;
    }
    *room -= need;
    cJSON_AddItemToObject(body, key, item);
    return true;
}

void gossip_role_sync_fill(cJSON *body, int room)
{
    if (!gr_mx || !body) {
        return;
    }
    int64_t now = gr_ms();
    int near = near_badges();
    battery_read(now);
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    s_fill_end = s_fill_no = false;
    cJSON *role = NULL;
    if (R.end) {
        role = cJSON_CreateObject();
        cJSON_AddNumberToObject(role, "sid", R.end_sid);
        cJSON_AddStringToObject(role, "end", R.end);
        s_fill_end = true;
        s_fill_end_sid = R.end_sid;
    } else if (R.phase == PH_OFFER && R.answer) {
        role = cJSON_CreateObject();
        cJSON_AddNumberToObject(role, "sid", R.sid);
        cJSON_AddBoolToObject(role, "ok", R.answer > 0);
        s_fill_no = R.answer < 0;
    } else if (R.phase == PH_SESSION) {
        role = cJSON_CreateObject();
        cJSON_AddNumberToObject(role, "sid", R.sid);
        cJSON_AddNumberToObject(role, "near", near);
        if (R.ro == 'l' && R.nblock) {
            cJSON *b = cJSON_AddArrayToObject(role, "block");
            for (int i = 0; i < R.nblock; i++) {
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "p", R.block[i].p);
                // The owner's own program: its own badge id (the server reads "by" as a badge id).
                cJSON_AddStringToObject(e, "by", R.block[i].by[0] ? R.block[i].by : s_me);
                cJSON_AddItemToArray(b, e);
            }
        }
        if (R.ro == 'z') {
            gr_boss_report(role);
        }
    }
    if (role && !add_fit(body, "role", role, &room)) {
        s_fill_end = s_fill_no = false;
    }
    if (opted()) {
        os_wifi_info_t w;
        os_wifi_get_info(&w);
        cJSON *lead = cJSON_CreateObject();
        cJSON_AddNumberToObject(lead, "batt", s_batt);
        cJSON_AddBoolToObject(lead, "usb", s_usb);
        cJSON_AddNumberToObject(lead, "wifi", w.state == OS_WIFI_CONNECTED ? w.channel : 0);
        cJSON_AddNumberToObject(lead, "mesh", os_mesh_channel());
        cJSON_AddNumberToObject(lead, "near", near);
        add_fit(body, "lead", lead, &room);
    }
    gr_app_fill(body, &room);
    xSemaphoreGive(gr_mx);
}

void gossip_role_sync_done(bool delivered, const cJSON *role)
{
    if (!gr_mx) {
        return;
    }
    int64_t now = gr_ms();
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    if (delivered) {
        s_last_sync = now;
        if (s_fill_end && R.end && R.end_sid == s_fill_end_sid) {
            R.end = NULL;
        }
        if (s_fill_no && R.phase == PH_OFFER && R.answer < 0) {
            R.phase = PH_NONE;              // declined, and the server knows
            gr_event(role_ev("declined"));
        }
    }
    gr_app_done(delivered);
    s_fill_end = s_fill_no = false;
    if (cJSON_IsObject(role)) {
        R.misses = 0;
        apply_state(role, now);
    } else if (delivered && R.phase != PH_NONE && !R.linger_until && ++R.misses >= MISSES_MAX) {
        // The server no longer lists it: it ended (an admin, a restart of ours it didn't follow).
        ESP_LOGI(TAG, "role %ld: not in the server's answers any more", R.sid);
        role_stop(NULL);
    }
    xSemaphoreGive(gr_mx);
    gr_flush();
}

bool gossip_role_fast_sync(void)
{
    if (!gr_mx) {
        return false;
    }
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    bool fast = R.phase != PH_NONE || R.end || gr_app_waiting() || gr_boss_fighting();
    xSemaphoreGive(gr_mx);
    return fast;
}

bool gossip_role_opted_in(void)
{
    return os_setting(OS_SET_LEADER) != 0;
}

void gossip_role_on_hello(const gossip_hello_t *h)
{
    if (!gr_mx || !h || !h->station || !h->ro) {
        return;
    }
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    gr_app_heard(h);
    xSemaphoreGive(gr_mx);
}

void gossip_role_setting_changed(void)
{
    if (!gr_mx) {
        return;
    }
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    if (!opted()) {
        if (R.phase == PH_OFFER && R.answer == 0) {
            R.answer = -1;                  // turning the setting off declines (GOSSIP.md §8)
        } else if (R.phase == PH_SESSION) {
            role_stop("owner");
        }
    }
    xSemaphoreGive(gr_mx);
    gossip_sync_soon();
    gr_flush();
}

void gossip_role_tick(int64_t now)
{
    if (!gr_mx) {
        return;
    }
    os_cloud_info_t *in = s_infobuf;    // OS task only (big: PSRAM)
    gossip_cfg_t *cfg = s_cfgnext;
    os_cloud_get_info(in);
    gossip_cfg(cfg);
    bool batt_low = false;
    if (R.phase == PH_SESSION) {
        battery_read(now);
        batt_low = !s_usb && s_batt >= 0 && s_batt < BATT_MIN;
    }
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    s_cfg = *cfg;                       // the Lua task reads it under gr_mx (close range, live roles)
    strlcpy(s_me, gr_id_ok(in->id) ? in->id : "", sizeof(s_me));
    drain_locked();
    if (R.phase == PH_OFFER) {
        if (R.answer == 0 && now - R.offer_seen >= OFFER_ACCEPT_MS) {
            R.answer = 1;                   // accepts by itself unless the owner declined (GOSSIP.md §8)
            gossip_sync_soon();
            ESP_LOGI(TAG, "offer %ld accepted", R.sid);
        } else if (now - R.offer_seen >= OFFER_FORGET_MS) {
            R.phase = PH_NONE;
            gr_event(role_ev("off"));
        }
    } else if (R.phase == PH_SESSION && R.on_air && R.linger_until) {
        if (now >= R.linger_until || busy_now() || !os_mesh_station_on()) {
            role_stop(NULL);                // the fight's claims had their time
        } else {
            role_hello(now);
        }
    } else if (R.phase == PH_SESSION && R.on_air) {
        int64_t t = gr_server_ms();
        if (t && R.until && t > R.until + UNTIL_SLACK_MS) {
            role_stop(NULL);                // its time is up: the server ends it too
        } else if (s_last_sync && now - s_last_sync > GRACE_MS) {
            role_stop(NULL);                // roleGraceSeconds without a sync: the server ends it
        } else if (batt_low) {
            role_stop("battery");
        } else if (busy_now() || !os_mesh_station_on()) {
            role_stop("busy");              // a Collaborator takeover or a badge beacon took the second id
        } else if (!opted() || !gr_on()) {
            role_stop("owner");
        } else {
            role_hello(now);
        }
    }
    gr_app_tick(now);
    gr_boss_tick(now);
    xSemaphoreGive(gr_mx);
    gr_flush();
}

// ---------------------------------------------------------------------------
// The gossip module's role functions (lua_gossip_role.c), any task

char *gossip_role_state_json(void)
{
    if (!gr_mx) {
        return NULL;
    }
    int64_t now = gr_ms();
    cJSON *o = NULL;
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    char ro[2] = {R.ro, 0};
    if (R.phase == PH_OFFER) {
        o = cJSON_CreateObject();
        cJSON *of = cJSON_AddObjectToObject(o, "offer");
        cJSON_AddNumberToObject(of, "sid", R.sid);
        cJSON_AddStringToObject(of, "ro", ro);
        // Times for apps are unix seconds: Lua's numbers are 32-bit (LUA_32BITS).
        cJSON_AddNumberToObject(of, "until", (double)(R.offer_until / 1000));
        if (R.answer) {
            cJSON_AddBoolToObject(of, "answer", R.answer > 0);
        } else {
            int64_t left = OFFER_ACCEPT_MS - (now - R.offer_seen);
            cJSON_AddNumberToObject(of, "left", left > 0 ? (double)((left + 999) / 1000) : 0);
        }
    } else if (R.phase == PH_SESSION) {
        o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "sid", R.sid);
        cJSON_AddStringToObject(o, "ro", ro);
        cJSON_AddStringToObject(o, "id", R.id);       // the role id is public: it is on the air
        cJSON_AddStringToObject(o, "n", R.name);
        cJSON_AddNumberToObject(o, "since", (double)(R.since / 1000));
        cJSON_AddNumberToObject(o, "until", (double)(R.until / 1000));
        int64_t t = gr_server_ms();
        if (t && R.until) {
            cJSON_AddNumberToObject(o, "left", R.until > t ? (double)((R.until - t) / 1000) : 0);
        }
        cJSON *st = cJSON_AddArrayToObject(o, "st");
        for (int i = 0; i < R.nst; i++) {
            cJSON_AddItemToArray(st, cJSON_CreateString(R.st[i]));
        }
        cJSON_AddNumberToObject(o, "boost", R.boost);
        cJSON_AddBoolToObject(o, "on_air", R.on_air);
        if (R.on_air && R.nst && R.ro != 'm') {
            // The program in its HELLO this moment (a Leader's block turns every 10 s), as update_hello() picks it.
            cJSON_AddStringToObject(o, "on_air_p",
                                    R.ro == 'l' ? R.st[((gr_ms() - R.on_air_at) / LEADER_TURN_MS) % R.nst] : R.st[0]);
        }
        if (R.ro == 'z') {
            cJSON *b = cJSON_AddObjectToObject(o, "boss");
            cJSON_AddNumberToObject(b, "bpm", R.bpm);
            cJSON_AddNumberToObject(b, "beats", R.beats);
            cJSON_AddNumberToObject(b, "min", R.min);
            cJSON_AddNumberToObject(b, "minBeats", R.min_beats);
            cJSON_AddStringToObject(b, "spam", R.spam);
        }
        if (R.ro == 'l') {
            cJSON *b = cJSON_AddArrayToObject(o, "block");
            for (int i = 0; i < R.nblock; i++) {
                cJSON *e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "p", R.block[i].p);
                cJSON_AddStringToObject(e, "by", R.block[i].by);
                cJSON_AddItemToArray(b, e);
            }
        }
        if (R.tally) {
            cJSON_AddItemToObject(o, "tally", cJSON_Parse(R.tally));
        }
    } else if (R.ended_sid && now - R.ended_at < 10 * 60 * 1000) {
        o = cJSON_CreateObject();
        cJSON *e = cJSON_AddObjectToObject(o, "ended");
        char ero[2] = {R.ended_ro, 0};
        cJSON_AddNumberToObject(e, "sid", R.ended_sid);
        cJSON_AddStringToObject(e, "ro", ero);
        cJSON_AddStringToObject(e, "by", R.ended_by);
        cJSON_AddNumberToObject(e, "stake", R.ended_stake);
        cJSON_AddNumberToObject(e, "lamps", R.ended_lamps);
    }
    xSemaphoreGive(gr_mx);
    char *j = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    return j;
}

const char *gossip_role_answer(bool yes)
{
    if (!gr_mx) {
        return "off";
    }
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    const char *err = NULL;
    if (R.phase != PH_OFFER) {
        err = "no offer";
    } else if (R.answer) {
        err = "answered";
    } else {
        R.answer = yes && opted() && !busy_now() ? 1 : -1;
    }
    xSemaphoreGive(gr_mx);
    if (!err) {
        gossip_sync_soon();
    }
    return err;
}

const char *gossip_role_end(void)
{
    if (!gr_mx) {
        return "off";
    }
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    const char *err = NULL;
    if (R.phase == PH_OFFER) {
        R.answer = -1;
        gossip_sync_soon();
    } else if (R.phase == PH_SESSION) {
        role_stop("owner");
    } else {
        err = "no role";
    }
    xSemaphoreGive(gr_mx);
    gr_kick();
    return err;
}

const char *gossip_role_block(const gossip_role_pick_t *picks, int n)
{
    if (!gr_mx) {
        return "off";
    }
    if (n < 0 || n > BLOCK_MAX) {
        return "at most 4 programs";
    }
    for (int i = 0; i < n; i++) {
        if (!gr_addr_ok(picks[i].p) || (picks[i].by[0] && !gr_id_ok(picks[i].by))) {
            return "bad program or id";
        }
    }
    xSemaphoreTake(gr_mx, portMAX_DELAY);
    const char *err = NULL;
    if (R.phase != PH_SESSION || R.ro != 'l') {
        err = "not a leader";
    } else {
        memcpy(R.block, picks, n * sizeof(*picks));
        R.nblock = n;
    }
    xSemaphoreGive(gr_mx);
    if (!err) {
        gossip_sync_soon();
    }
    return err;
}

// ---------------------------------------------------------------------------
// Test mode's stand-in for the node service's HELLO subscriber (fw-node's
// gossip.c parses every HELLO and hands role ones to gossip_role_on_hello).
// Only while test mode is on: copy on the mesh task, parse on the OS task.

typedef struct {
    char from[OS_MESH_ID_LEN + 1];
    int rssi;
    char json[];
} thello_t;

static void test_hello_parse(void *arg)
{
    thello_t *t = arg;
    gossip_hello_t h;                   // ~130 B: the OS task has the stack (test mode only)
    cJSON *j = cJSON_Parse(t->json);
    memset(&h, 0, sizeof(h));
    strlcpy(h.id, t->from, sizeof(h.id));
    h.station = !strcmp(game_jstr(j, "k"), "s");
    h.ro = game_jstr(j, "ro")[0];
    game_ascii(h.name, sizeof(h.name), game_jstr(j, "n"));
    game_ascii(h.st, sizeof(h.st), game_jstr(j, "st"));
    game_ascii(h.g, sizeof(h.g), game_jstr(j, "g"));
    h.rssi = t->rssi;
    cJSON_Delete(j);
    free(t);
    if (h.g[0]) {
        gossip_role_on_hello(&h);
    }
}

static void test_on_hello(os_mesh_type_t type, const os_mesh_peer_t *from, const char *json, void *ctx)
{
    size_t n = strlen(json);
    // While the node service runs it hands every role HELLO over itself (gossip.c's
    // subscriber): test mode only stands in for it when gossip is off.
    if (!s_test || gossip_on() || from->kind != OS_PEER_STATION || n > 232 || !strstr(json, "\"ro\"")) {
        return;
    }
    thello_t *t = malloc(sizeof(thello_t) + n + 1);
    if (!t) {
        return;
    }
    strlcpy(t->from, from->id, sizeof(t->from));
    t->rssi = from->rssi;
    memcpy(t->json, json, n + 1);
    if (!os_post(test_hello_parse, t)) {
        free(t);
    }
}

// ---------------------------------------------------------------------------
// Console: `grole` (status), and test mode's stand-ins for the sync

static void print_state(void)
{
    char *j = gossip_role_state_json();
    printf("grole state %s\n", j ? j : "null");
    free(j);
}

// Fan-in measurement (test mode): send `vote` summaries to a role id at a set
// rate, past the badge's own one-a-second limit, as many badges would. A task of
// its own so the console and the OS task stay free; it prints what it sent.
static struct {
    char to[OS_MESH_ID_LEN + 1];
    int per_s, secs;
} s_flood;
static volatile bool s_flooding;

static void flood_task(void *arg)
{
    char json[GR_MSG_MAX + 1];
    int64_t t0 = gr_ms();
    int ok = 0, fail = 0, n = s_flood.per_s * s_flood.secs;
    for (int i = 0; i < n; i++) {
        int64_t due = t0 + (int64_t)i * 1000 / s_flood.per_s, now = gr_ms();
        if (due > now) {
            vTaskDelay(pdMS_TO_TICKS(due - now) ? pdMS_TO_TICKS(due - now) : 1);
        }
        snprintf(json, sizeof(json), "{\"app\":\"" GR_APP "\",\"to\":\"%s\",\"d\":{\"t\":\"vote\",\"v\":1,\"l\":1}}",
                 s_flood.to);
        os_mesh_send(OS_MESH_APP, s_flood.to, json) == ESP_OK ? ok++ : fail++;
    }
    printf("grole flood: %d sent, %d failed in %lld ms\n", ok, fail, (long long)(gr_ms() - t0));
    s_flooding = false;
    vTaskDelete(NULL);
}

static void flood_start(const char *to, int per_s, int secs)
{
    if (s_flooding || !gr_id_ok(to) || per_s < 1 || per_s > 200 || secs < 1 || secs > 120) {
        printf("grole flood: busy or bad arguments (1-200 a second, 1-120 s)\n");
        return;
    }
    strlcpy(s_flood.to, to, sizeof(s_flood.to));
    s_flood.per_s = per_s;
    s_flood.secs = secs;
    s_flooding = true;
    if (xTaskCreate(flood_task, "grflood", 3072, NULL, 4, NULL) != pdPASS) {
        s_flooding = false;
        printf("grole flood: no task\n");
    }
}

// Console JSON: the console splits on spaces and eats double quotes, so the
// test commands take JSON written with single quotes.
static cJSON *console_json(const char *s)
{
    char *t = strdup(s);
    if (!t) {
        return NULL;
    }
    for (char *p = t; *p; p++) {
        if (*p == '\'') {
            *p = '"';
        }
    }
    cJSON *j = cJSON_Parse(t);
    free(t);
    return j;
}

// One sync round as the node service would make it: the body's role part
// printed, then the answer's RoleState (or none) handed back.
static void test_sync(const char *state, bool delivered)
{
    cJSON *body = cJSON_CreateObject();
    gossip_role_sync_fill(body, 3000);
    char *bj = cJSON_PrintUnformatted(body);
    printf("grole body %s\n", bj ? bj : "{}");
    cJSON_free(bj);
    cJSON_Delete(body);
    cJSON *role = state ? console_json(state) : NULL;
    if (state && !role) {
        printf("grole: bad JSON\n");
    }
    gossip_role_sync_done(delivered, role);
    cJSON_Delete(role);
}

static const char *pick_list(const char *arg, gossip_role_pick_t *picks, int *n)
{
    // "<addr>:<by>,<addr>:<by>" ("" = none)
    *n = 0;
    char buf[256];
    strlcpy(buf, arg, sizeof(buf));
    for (char *save = NULL, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (*n == BLOCK_MAX) {
            return "at most 4 programs";
        }
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = 0;
        }
        strlcpy(picks[*n].p, tok, sizeof(picks[0].p));
        strlcpy(picks[*n].by, colon ? colon + 1 : "", sizeof(picks[0].by));
        (*n)++;
    }
    return NULL;
}

static int cmd_grole(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    const char *err = NULL;
    if (!strcmp(sub, "status")) {
        xSemaphoreTake(gr_mx, portMAX_DELAY);
        static const char *const ph[] = {"none", "offer", "session"};
        printf("grole: %s%s sid %ld %s id %s on_air %d end %s opted_in %d test %d\n", ph[R.phase],
               R.phase == PH_OFFER ? (R.answer > 0 ? "(yes)" : R.answer < 0 ? "(no)" : "(waiting)") : "", R.sid,
               R.phase ? role_name(R.ro) : "-", R.id[0] ? R.id : "-", R.on_air, R.end ? R.end : "-",
               gossip_role_opted_in(), s_test);
        if (R.on_air) {
            printf("  hello %s\n", R.hello[0] ? R.hello : "(plain station)");
        }
        printf("  rx claim %" PRIu32 " ack %" PRIu32 " bid %" PRIu32 " vote %" PRIu32 " boss %" PRIu32
               " cure %" PRIu32 " cured %" PRIu32 ", bad %" PRIu32 ", ring drops %" PRIu32 "\n",
               gr_stats.rx[0], gr_stats.rx[1], gr_stats.rx[2], gr_stats.rx[3], gr_stats.rx[4], gr_stats.rx[5],
               gr_stats.rx[6], gr_stats.bad, gr_stats.ring_drop);
        printf("  tx badge %" PRIu32 " role %" PRIu32 " fail %" PRIu32 " queue drops %" PRIu32
               ", waiting badge %d role %d\n", gr_stats.tx_badge, gr_stats.tx_role, gr_stats.tx_fail,
               gr_stats.q_drop, s_nbq, s_nrq);
        gr_app_status();
        gr_boss_status();
        xSemaphoreGive(gr_mx);
        return 0;
    }
    if (!strcmp(sub, "stats") && argc > 2 && !strcmp(argv[2], "reset")) {
        xSemaphoreTake(gr_mx, portMAX_DELAY);
        memset(&gr_stats, 0, sizeof(gr_stats));
        gr_app_stats_reset();
        gr_boss_stats_reset();
        xSemaphoreGive(gr_mx);
        printf("grole: stats reset\n");
        return 0;
    }
    if (!strcmp(sub, "test")) {
        const char *what = argc > 2 ? argv[2] : "";
        if (!strcmp(what, "on")) {
            static bool subscribed;
            if (!subscribed) {
                subscribed = os_mesh_subscribe(OS_MESH_HELLO, test_on_hello, NULL) >= 0;
            }
            s_test = true;
            s_test_offset = argc > 3 ? strtoll(argv[3], NULL, 10) - gr_ms() : 0;
            printf("grole: test mode on (until a restart)\n");
        } else if (!strcmp(what, "off")) {
            s_test = false;
            printf("grole: test mode off\n");
        } else if (!s_test) {
            printf("grole: `grole test on [<unix ms>]` first\n");
            return 1;
        } else if (!strcmp(what, "class") && argc > 3) {
            s_test_class = argv[3][0] == '-' ? 0 : argv[3][0];
        } else if (!strcmp(what, "spam")) {
            strlcpy(s_test_spam, argc > 3 && gr_addr_ok(argv[3]) ? argv[3] : "", sizeof(s_test_spam));
        } else if (!strcmp(what, "real")) {
            memset(s_test_real, 0, sizeof(s_test_real));
            for (int i = 3, k = 0; i < argc && k < GOSSIP_CFG_ROLES; i++) {
                if (gr_id_ok(argv[i])) {
                    strlcpy(s_test_real[k++], argv[i], sizeof(s_test_real[0]));
                }
            }
        } else if (!strcmp(what, "sync")) {
            // grole test sync [<RoleState JSON, single quotes> | none | fail]
            const char *st = argc > 3 ? argv[3] : "none";
            test_sync(strcmp(st, "none") && strcmp(st, "fail") ? st : NULL, strcmp(st, "fail") != 0);
        } else if (!strcmp(what, "close") && argc > 3) {
            s_test_close = atoi(argv[3]);
            printf("grole: close range %d dBm\n", gr_close());
        } else if (!strcmp(what, "flood") && argc > 4) {
            flood_start(argv[3], atoi(argv[4]), argc > 5 ? atoi(argv[5]) : 10);
            return 0;
        } else {
            printf("grole test [on [<unix ms>]|off|class <c|->|spam [<addr>]|real <id>...|sync [<json>|none|fail]|"
                   "close <dBm>|flood <role> <per s> [<s>]]\n");
            return 1;
        }
        print_state();
        return 0;
    }
    if (!s_test) {
        printf("grole: the acting commands need test mode (`grole test on`)\n");
        return 1;
    }
    if (!strcmp(sub, "claim") && argc > 3) {
        err = gossip_role_claim(argv[2], atoi(argv[3]));
    } else if (!strcmp(sub, "bid") && argc > 3) {
        err = gossip_role_bid(argv[2], argv[3], argc > 4 && atoi(argv[4]));
    } else if (!strcmp(sub, "cure") && argc > 2) {
        err = gossip_role_cure(argv[2]);
    } else if (!strcmp(sub, "vote") && argc > 3) {
        err = gossip_role_vote(atoi(argv[2]), atoi(argv[3]));
    } else if (!strcmp(sub, "answer") && argc > 2) {
        err = gossip_role_answer(!strcmp(argv[2], "yes"));
    } else if (!strcmp(sub, "end")) {
        err = gossip_role_end();
    } else if (!strcmp(sub, "fork")) {
        err = gossip_role_fork();
    } else if (!strcmp(sub, "block")) {
        gossip_role_pick_t picks[BLOCK_MAX];
        int n;
        err = pick_list(argc > 2 ? argv[2] : "", picks, &n);
        err = err ? err : gossip_role_block(picks, n);
    } else if (!strcmp(sub, "boss")) {
        char *j = gossip_role_boss_json();
        printf("grole boss %s\n", j ? j : "null");
        free(j);
        return 0;
    } else if (!strcmp(sub, "bids")) {
        char *j = gossip_role_bids_json();
        printf("grole bids %s\n", j ? j : "null");
        free(j);
        return 0;
    } else if (!strcmp(sub, "voters")) {
        char *j = gossip_role_voters_json();
        printf("grole voters %s\n", j ? j : "null");
        free(j);
        return 0;
    } else {
        printf("grole [status|stats reset|test …|claim <role> <c>|bid <role> <addr> [fee]|cure <id>|vote <v> <l>|"
               "answer yes|no|end|fork|block <a>:<by>,…|boss|bids|voters]\n");
        return 1;
    }
    printf("grole %s: %s\n", sub, err ? err : "ok");
    return err ? 1 : 0;
}

// ---------------------------------------------------------------------------

void gossip_role_init(void)
{
    s_ring = heap_caps_calloc(RING_N, sizeof(rx_t), os_mem_bulk_caps());
    s_bq = heap_caps_calloc(QUEUE_N, sizeof(msg_t), os_mem_bulk_caps());
    s_rq = heap_caps_calloc(QUEUE_N, sizeof(msg_t), os_mem_bulk_caps());
    s_msgbuf = heap_caps_calloc(1, sizeof(msg_t), os_mem_bulk_caps());
    s_rxbuf = heap_caps_calloc(1, sizeof(rx_t), os_mem_bulk_caps());
    s_role = heap_caps_calloc(1, sizeof(role_t), os_mem_bulk_caps());
    s_cfgp = heap_caps_calloc(1, sizeof(gossip_cfg_t), os_mem_bulk_caps());
    s_cfgnext = heap_caps_calloc(1, sizeof(gossip_cfg_t), os_mem_bulk_caps());
    s_infobuf = heap_caps_calloc(1, sizeof(os_cloud_info_t), os_mem_bulk_caps());
    if (!s_ring || !s_bq || !s_rq || !s_msgbuf || !s_rxbuf || !s_role || !s_cfgp || !s_cfgnext || !s_infobuf ||
            !gr_app_init() || !gr_boss_init()) {
        ESP_LOGE(TAG, "no memory: no roles and no gossip messages on this badge");
        return;                         // gr_mx stays NULL: every entry point returns at once
    }
    gossip_cfg(&s_cfg);
    gr_mx = xSemaphoreCreateMutex();
    os_mesh_subscribe(OS_MESH_APP, on_mesh, NULL);
    os_cmd_register("grole", "grole [status|stats reset|test …|claim|bid|cure|vote|answer|end|fork|block|boss|bids|voters]",
                    cmd_grole);
}
