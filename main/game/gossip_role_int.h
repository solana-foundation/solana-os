#pragma once

// Gossip roles, inside (game/gossip_role.h is the outside). Three files share
// this: gossip_role.c (the role identity, the sync seam, the `gossip` APP
// channel's plumbing: the mesh task's ring, the send queues and their rates,
// the console), gossip_role_app.c (claims and acks, bids, cures, roles heard)
// and gossip_role_boss.c (the fight, both sides).
//
// Locking: gr_mx guards all of it. The OS task (tick, sync, the ring's drain)
// and the Lua task (the gossip module) take it; the mesh task never does (it
// only copies into the ring under a spinlock). Code under gr_mx never calls
// out to another service that could call back: sends, on_gossip events and
// notices are queued and go out after the lock is given back (gr_flush).

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "core/os_mesh.h"
#include "gossip.h"
#include "gossip_role.h"

#define GR_TAG          "gossip_role"
#define GR_APP          "gossip"        // the reserved app id (MESH.md §6, GOSSIP.md §4)
#define GR_D_MAX        200             // an APP message's d (MESH.md §6)
#define GR_MSG_MAX      280             // the whole APP message: {"app":"gossip","to":"…","d":{…}}
#define GR_CLAIM_C_MAX  9999
#define GR_CLOSE_DEF    (-55)

extern SemaphoreHandle_t gr_mx;

// Clocks: esp_timer ms, and the server's unix ms (0 = no clock).
int64_t gr_ms(void);
int64_t gr_server_ms(void);

// What the node service says, or the console's test stand-ins (gossip_role.c
// "Test mode"): gossip on, the class letter, a held spam program, the close
// range, whether a role id is one the server lists as live.
bool gr_on(void);
char gr_class(void);
bool gr_held_spam(char out[GOSSIP_ADDR_MAX + 1]);
int gr_close(void);
bool gr_real(const char *id);
bool gr_me(char out[OS_MESH_ID_LEN + 1]);          // this badge's id

// Checks for hostile text.
bool gr_id_ok(const char *s);                       // a mesh id: 6 of [0-9A-Z], an emulator's starts with 'e'
bool gr_addr_ok(const char *s);                     // base58, 32-44 characters
bool gr_nonce_ok(const char *s);                    // 16 lowercase hex
int gr_jint(const cJSON *o, const char *key, int lo, int hi, int def);   // a whole number in range, else def

// This badge's role session as the other parts need it (under gr_mx): false
// when none is on the air.
typedef struct {
    long sid;
    char ro;
    char id[OS_MESH_ID_LEN + 1];
    uint8_t key[16];
} gr_live_t;
bool gr_role_live(gr_live_t *out);

// Queue a `gossip` APP message (under gr_mx): from the badge (to a peer id;
// one a second, two in a fight) or from the role identity (to a peer id, or
// NULL for everyone; four a second). false: the queue is full or d too big.
bool gr_send_badge(const char *to, const cJSON *d);
bool gr_send_role(const char *to, const cJSON *d);
// on_gossip(ev) for the app and services (under gr_mx; sent after it): takes ev.
void gr_event(cJSON *ev);
// A notice from the gossip service (under gr_mx; shown after it).
void gr_notice(uint32_t led, const char *text);

// gossip_role_app.c
bool gr_app_init(void);                             // false: no memory
void gr_app_rx(const char *t, const char *from, os_peer_kind_t kind, int rssi, bool to_role, const cJSON *d);
void gr_app_tick(int64_t now);
void gr_app_heard(const gossip_hello_t *h);
void gr_app_fill(cJSON *body, int *room);
void gr_app_done(bool delivered);
bool gr_app_waiting(void);                          // claims or cures in flight or waiting to upload
void gr_app_session(long sid);                      // a session began (or 0: ended): the bids go
bool gr_app_claim_auto(const char *role, int c);    // the fight's claim at the end
void gr_app_status(void);                           // console
void gr_app_stats_reset(void);

// gossip_role_boss.c
bool gr_boss_init(void);
void gr_boss_rx(const char *t, const char *from, os_peer_kind_t kind, int rssi, int64_t at, bool to_role,
                const cJSON *d);
void gr_boss_tick(int64_t now);
void gr_boss_start(long sid, int bpm, int beats, int min, int min_beats, int64_t since);   // this badge is the boss
bool gr_boss_over(void);                            // ... and its fight ended (rooted or lost)
void gr_boss_stop(void);
void gr_boss_report(cJSON *role);                   // RoleReport.boss
bool gr_boss_fighting(void);                        // a player in a fight: two messages a second, the 2 s sync
// The boss's broadcast, if one is due now (OS task, under gr_mx): the JSON d to send.
cJSON *gr_boss_due(int64_t now);
// A player's vote summary, if one is due now: d and the boss's id.
cJSON *gr_boss_vote_due(int64_t now, char to[OS_MESH_ID_LEN + 1]);
void gr_boss_status(void);                          // console
void gr_boss_stats_reset(void);

// Counters for the console and the fan-in measurement.
typedef struct {
    uint32_t rx[8];         // accepted, per kind below
    uint32_t ring_drop;     // the ring was full (the mesh task dropped it)
    uint32_t bad;           // malformed, not for us, wrong sender
    uint32_t tx_badge, tx_role, tx_fail, q_drop;
} gr_stats_t;
enum { GR_T_CLAIM, GR_T_ACK, GR_T_BID, GR_T_VOTE, GR_T_BOSS, GR_T_CURE, GR_T_CURED, GR_T_OTHER };
extern gr_stats_t gr_stats;
