#pragma once

// Collaborator (Solana OS 2.3.0): the owner lends the badge to the event's
// server. Contracts: the parent repo's docs/HHGTS.md "Collaborator" (what the
// owner sees and agrees to), server/API.md "Collaborator" (the sync), and
// docs/MESH.md "Collaborator stations" (the air).
//
// A built-in background service (core/os_service.h, id "collab",
// OS_SVC_STOPPABLE | OS_SVC_BOOT | OS_SVC_SCREEN). It starts only with the
// owner's consent (NVS "collab/ok", given in the Collaborator app through
// os_confirm), and then at every boot until turned off. While on it syncs with
// POST /api/device/collab/sync: every 30 s while live, every 1-2 s while the
// server has taken it over. A takeover sends APP messages from the station id
// the server gives (os_mesh_station), forwards what it hears for that station,
// and, when the owner lends the screen, shows the server's screen through the
// hidden app collab_screen (apps/collab), whose exit (HOME, CANCEL) ends the
// takeover. The badge decides nothing economic: the server's clock does.
//
// OS task only, except the mesh handler (which copies and queues).

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "core/os.h"
#include "core/os_mesh.h"

typedef enum {
    COLLAB_OFF,             // the service isn't running
    COLLAB_WAITING,         // on, but not reaching the server (no Wi-Fi, no answer yet)
    COLLAB_LIVE,
    COLLAB_TAKEN,
} collab_phase_t;

typedef enum {
    COLLAB_SHOW_IDLE,
    COLLAB_SHOW_CARD,
    COLLAB_SHOW_QR,
    COLLAB_SHOW_IMAGE,
    COLLAB_SHOW_COLOR,
    COLLAB_SHOW_COUNT,
} collab_show_kind_t;

// What the takeover screen shows (API.md CollabShow), as the last sync gave it.
typedef struct {
    collab_show_kind_t k;
    char title[33];
    char body[241];
    char text[161];                 // qr
    uint32_t col, bg;               // 0xRRGGBB; bg 0 = the theme's
    bool has_col, has_bg;
    int64_t to;                     // count: server ms
    const lv_image_dsc_t *img;      // image, once downloaded and decoded (NULL meanwhile)
    bool img_failed;                // no card, or it didn't decode: the title alone
} collab_show_t;

typedef struct {
    collab_phase_t phase;
    bool lend;                      // the owner lends the screen
    bool screen;                    // the session uses the screen
    bool earns;
    char why[64];                   // why it earns nothing, or the last error
    int live_min, earned, cap;      // today, from the server
    int rate_live, rate_minutes, rate_take;
    int minutes, pending;           // the session's
    char station[OS_MESH_ID_LEN + 1];
    int64_t live_since;             // game_now_ms() when it went live (0 = not live)
    int64_t synced_ms;              // the last answer
    int sent, heard;                // this session's messages
    uint32_t rev;                   // anything above changed
    uint32_t show_rev;              // the screen's content changed
} collab_info_t;

void collab_init(void);             // game_init: registers the service, `collab`, the mesh handler

const collab_info_t *collab_info(void);
const collab_show_t *collab_show(void);
bool collab_consent(void);          // the owner said yes (and hasn't turned it off since)

// The Collaborator app's actions (the owner's).
void collab_turn_on(void);          // after the owner's yes in os_confirm: consent + start
void collab_turn_off(void);         // stop (forfeits a takeover), consent gone
void collab_set_lend(bool lend);
void collab_end_takeover(void);     // forfeits it; no takeover for 10 minutes

// The hidden takeover app (apps/collab/collab_app.c).
extern const os_app_t app_collab_screen;
void collab_screen_entered(void);   // its on_enter: the LED pattern starts
void collab_screen_left(void);      // its on_exit: the owner left it unless we closed it; the LEDs stop
