// Ping: the mesh from C. Lists the badges in range (or bridged) every second;
// SELECT pings the one picked, RIGHT pings every badge running this app, and
// on_mesh() hears their pings. Messages are JSON objects, <= 200 bytes.
#include <stdio.h>
#include <string.h>

#include "badge.h"

#define MAX_PEERS 8

static ui_obj list, status_label;
static mesh_peer peers[MAX_PEERS];
static int npeers;
static int32_t sent;

static void refresh(void *user)
{
    (void)user;
    mesh_peer all[MAX_PEERS];
    int n = mesh_nearby(all, MAX_PEERS);
    if (n < 0) {
        ui_label_text(status_label, badge_error());
        return;
    }
    char rows[MAX_PEERS][48];
    const char *items[MAX_PEERS];
    npeers = 0;
    for (int i = 0; i < n && i < MAX_PEERS; i++) {
        if (strcmp(all[i].kind, "badge") != 0) {
            continue; // beacons are quest characters
        }
        peers[npeers] = all[i];
        snprintf(rows[npeers], sizeof rows[npeers], "%s%s", all[i].name[0] ? all[i].name : all[i].id,
                 all[i].bridged ? "  (bridged)" : "");
        items[npeers] = rows[npeers];
        npeers++;
    }
    if (!npeers) {
        items[0] = "Nobody in range yet";
    }
    ui_list_items(list, items, npeers ? npeers : 1);
}

static void ping(const char *to)
{
    char msg[64];
    snprintf(msg, sizeof msg, "{\"ping\":%ld}", (long)++sent);
    if (mesh_send(to, msg) < 0) { // "too fast: 4 messages per second", "scanning", ...
        ui_toast(badge_error());
        return;
    }
    ui_label_text(status_label, to ? "Pinged" : "Pinged everyone");
}

static void picked(void *user, int index)
{
    (void)user;
    if (index >= 0 && index < npeers) {
        ping(peers[index].id);
    }
}

void on_start(void)
{
    const char *none[] = {"Looking..."};
    list = ui_list(0, 0, UI_W, UI_H - 40, none, 1, picked, NULL);
    status_label = ui_label(8, UI_H - 36, "", &(ui_label_opts){.size = 14, .color = COL_DIM});
    ui_hint("SELECT ping   RIGHT ping all   CANCEL exit");
    timer_every(1000, refresh, NULL);
    refresh(NULL);
}

int32_t on_key(int32_t key, int32_t repeat)
{
    (void)repeat;
    if (key == KEY_RIGHT) {
        ping(NULL); // "*": every badge running ping-c
        return 1;
    }
    return key != KEY_BACK; // the list takes UP, DOWN and SELECT itself
}

static void lights_off(void *user)
{
    (void)user;
    led_off();
}

// A message from this app on another badge: frame 1 = its id, 2 = the message.
void on_mesh(void)
{
    char from[8], text[64];
    badge_str(badge_arg(1), from, sizeof from);
    int32_t n = badge_field_int(badge_arg(2), "ping", -1);
    if (n < 0) {
        return; // not a ping: anything can arrive, check it
    }
    snprintf(text, sizeof text, "Ping %ld from %s", (long)n, from);
    ui_toast(text);
    led_fill(0xEB, 0x54, 0xBC);
    timer_after(150, lights_off, NULL);
}
