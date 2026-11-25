#pragma once

// Gossip, the node service (Solana OS 2.4.0; the parent repo's docs/GOSSIP.md
// §3-§7, docs/MESH.md "Gossip"). Builder: fw-node. The badge as a player:
// it hears other badges' and roles' HELLO `st`/`g` fields, accumulates
// exposure, makes exposure records, puts its own active program and tag into
// its HELLO (os_mesh_hello_extra), syncs with POST /api/device/gossip/sync on
// the cloud task (os_api), keeps the owner's state on the card
// (/sdcard/hh/gossip/) and is the built-in background service `gossip`.
//
// Threads: the state machine runs on the OS task (game tick, os_api
// callbacks, os_post from the mesh task). Functions marked "any task" copy
// out under the service's own lock, for the Lua task (lua_gossip*.c) and the
// console.
//
// SEAM (foundation commit): the declarations between the "seam" lines are
// shared with game/gossip_role.c (builder fw-roles) and lua/lua_gossip*.c.
// Their signatures are fixed; fill in the bodies in gossip.c. Add anything
// else (more getters for lua_gossip.c, console) below the seam.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

// ---- seam ------------------------------------------------------------------

#define GOSSIP_ADDR_MAX     44      // a base58 address of 32 bytes
#define GOSSIP_G_MAX        40      // a HELLO's "g": "<c><b>.<w>.<tag>"
#define GOSSIP_CFG_ROLES    8       // GOSSIP.md §7 Cfg.roles
#define GOSSIP_CFG_SPAM     8       // GOSSIP.md §7 Cfg.spam

// The genome of a program (GOSSIP.md §3), decoded from G = SHA-256(address).
typedef struct {
    uint8_t g[32];              // G itself
    uint8_t hue;                // G[0]
    uint8_t r, gr, b;           // the colour formula's RGB (also the LED colour)
    uint8_t body, eyes;         // 0..7
    uint8_t marks, mark_seed;   // 0..7, 0..31
    uint8_t traits[3];          // trait numbers (0..5, GOSSIP.md §3 "Traits")
    uint8_t ntraits;            // 2 or 3
    uint8_t vir, hard, stab;    // 1..10
    uint8_t rarity;             // 0 common, 1 rare, 2 epic, 3 legendary
} gossip_genome_t;

// What the last sync's Cfg says (GOSSIP.md §7), with the badge's defaults
// before the first one.
typedef struct {
    int thr, decay;
    int wt_rssi[4], wt[4];      // rssiWeight thresholds (dBm) and weights
    int offpeak_from, offpeak_to;   // UTC hours [from, to); -1 = none
    int close;                  // the close range for claims (dBm), -55 (GOSSIP.md §7 Cfg `close`)
    int name_nodes;             // adoptions before a program can be named (Cfg `nameNodes`, 50)
    int nroles;
    struct { char id[8]; char ro; } roles[GOSSIP_CFG_ROLES];   // live role ids
    int nspam;
    char spam[GOSSIP_CFG_SPAM][GOSSIP_ADDR_MAX + 1];           // live spam programs
} gossip_cfg_t;

// One HELLO with a `g` field as the node service heard it (its OS_MESH_HELLO
// subscriber parses the raw JSON; os_mesh's on_hello doesn't). Handed to the
// role service for `heard` and the boss radar.
typedef struct {
    char id[8];                 // the sender (a badge or a station id from the peer table)
    bool station;               // k:"s"
    char ro;                    // a role HELLO's "ro" ('l', 'm', 'a', 'z'), 0 for none
    char name[25];              // "n", printable ASCII
    char st[GOSSIP_ADDR_MAX + 1];
    char g[GOSSIP_G_MAX + 1];
    int rssi;
} gossip_hello_t;

// game_init(): service registration, mesh subscription, console. game tick:
// gossip_tick(now) (OS task).
void gossip_init(void);
void gossip_tick(int64_t now);
// Settings > Gossip changed OS_SET_GOSSIP (OS task).
void gossip_setting_changed(void);

// Gossip is on (setting, service), the badge is claimed and the service runs.
// Any task.
bool gossip_on(void);
// The server's clock (unix ms) as of the last sync, advanced by esp_timer;
// 0 until a sync answered since boot ("without a clock"). Any task.
int64_t gossip_server_ms(void);
// The node's class letter from the last Node ('r','v','i','b'; 0 none). Any task.
char gossip_class(void);
// A copy of the current Cfg. Any task.
void gossip_cfg(gossip_cfg_t *out);
// The first program this node holds (a slot) that Cfg lists as spam: true and
// its address, else false. Any task.
bool gossip_held_spam(char out[GOSSIP_ADDR_MAX + 1]);
// Decodes an address (base58, 32 bytes) into its genome: false when it isn't
// one. Pure, any task (one SHA-256).
bool gossip_genome(const char *addr, gossip_genome_t *out);
// Queue an Action (GOSSIP.md §7, the JSON object without "n": the service
// numbers it), sent with the next sync, which it asks for soon. false: the
// queue (4) is full, gossip is off or the JSON is not an object. Any task.
bool gossip_action(const char *json);
// Ask for a sync soon (≥ 5 s after the last, GOSSIP.md §5). Any task.
void gossip_sync_soon(void);

// ---- end of seam -----------------------------------------------------------

// ---- the node service's own (fw-node) --------------------------------------
// For lua/lua_gossip.c and the console. Each returns compact JSON in memory
// the caller frees (free()), built under the service's lock, or NULL. Any task.

// gossip.me(): the last Node plus {id, on, synced, incubating}; NULL while
// unclaimed (or out of memory).
char *gossip_me_json(void);
// gossip.programs(): {"slots":[...],"inbox":[...]}, each a program object
// (gossip_program_json's) or false.
char *gossip_programs_json(void);
// gossip.program(a): the record and its genome; NULL when no record is held.
char *gossip_program_json(const char *addr);
// gossip.genome(a): the decoded genome alone; NULL for an invalid address.
char *gossip_genome_json(const char *addr);
// gossip.nearby(): ≤ 20 badges and roles heard, strongest first.
char *gossip_nearby_json(void);
// gossip.hearing(): ≤ 8 programs being heard.
char *gossip_hearing_json(void);
// gossip.lead(on): sets OS_SET_LEADER on the OS task and tells the role service.
void gossip_set_lead(bool on);
// A Gossip notice (toast + LED, os_service_notify). While this badge is in a boss
// fight (voting, or its own boss) it is held, and the last three are shown after the
// fight with a count of the rest. OS task. True when shown or held.
bool gossip_notice(uint32_t col, const char *text);
// The rarity's name: "common", "rare", "epic", "legendary".
const char *gossip_rarity_name(int rarity);
