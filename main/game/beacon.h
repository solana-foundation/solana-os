#pragma once

// Badge beacons (docs/MESH.md "Badge beacons", server/API.md "Badge beacons").
// A badge a QuestMaker paired (console `beacon pair`, which the site's flasher
// and emulator run) plays a quest beacon while its Beacon app is open: it says
// the beacon's hello to the server, broadcasts the character's HELLO under the
// beacon id (os_mesh_beacon), relays badges' RELAYs to the server over its own
// Wi-Fi and shows the scan code. Nothing of its owner's changes.
//
// Runs on the OS task like the other game services (mesh RELAYs hop over).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BEACON_NONE,            // this badge isn't a beacon
    BEACON_CHECKING,        // the app is open, no answer from the server yet
    BEACON_PENDING,         // waiting for a QuestMaker to adopt the pairing code
    BEACON_ASLEEP,          // adopted, no character
    BEACON_ON_AIR,          // playing its character (silent while offline)
} beacon_phase_t;

typedef struct {
    beacon_phase_t phase;
    char id[7];             // beacon id, "" = none
    char code[8];           // pairing code while pending
    char ch[17];            // character id, name, title and accent
    char name[25];
    char title[33];
    uint32_t color;
    int mesh;               // the server's mesh channel, 0 = not told yet
    int visits;             // relays answered since the app opened
    int busy;               // turned away (busy, stale nonce, server unreachable)
    int64_t last_visit_ms;  // game_now_ms() of the last one, 0 = none
    char error[48];         // the last hello's failure, "" = none
    uint32_t rev;           // changes when anything but the counters does
} beacon_info_t;

void beacon_init(void);                     // from game_init()
void beacon_tick(int64_t now);              // game tick
void beacon_cloud(const char *id);          // game tick: hello's beacon id ("" = none)

bool beacon_listed(void);                   // this badge is a beacon: the launcher lists the app
const beacon_info_t *beacon_info(void);

// The Beacon app's on_enter / on_exit. Serving survives a restart: a badge
// that restarts with the app open opens it again.
void beacon_serve(bool on);
void beacon_sync(void);                     // say hello now

// The scan code's link for the current 30 s window (MESH.md "Scan codes"),
// false until the server gave a scan key.
bool beacon_scan_url(char *out, size_t n);
