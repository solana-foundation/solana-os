// The HH GtS services and the badge's own plumbing as built-in background
// services (core/os_service.h, docs/HHGTS.md "Background services"), so the
// Services app lists honestly what runs while their apps are closed.
//
// None of these can be stopped: they have no task, memory or radio of their
// own (they react to the mesh and the server link, which run anyway), and
// stopping X Dex or Trade halfway would strand the other badge mid-swap.
// The status lines read their live state on the OS task.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/os.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_mesh.h"
#include "core/os_service.h"
#include "core/os_wifi.h"
#include "core/ui.h"
#include "game.h"
#include "quests.h"
#include "trade.h"
#include "xdex.h"

#define PEERS_MAX   24

static const char *const GAME_WHY = "Part of the game: no memory or radio of its own";

static os_service_state_t on(void *ctx)
{
    return OS_SVC_RUNNING;
}

static os_service_state_t mesh_state(void *ctx)
{
    return os_mesh_ready() ? OS_SVC_RUNNING : OS_SVC_STOPPED;
}

static void mesh_status(void *ctx, char *out, size_t n)
{
    if (!os_mesh_ready()) {
        strlcpy(out, "Starting", n);
        return;
    }
    os_wifi_info_t *w = malloc(sizeof(*w));
    os_mesh_peer_t *p = malloc(PEERS_MAX * sizeof(*p));
    int badges = 0;
    int k = p ? os_mesh_peers(p, PEERS_MAX) : 0;
    for (int i = 0; i < k && i < PEERS_MAX; i++) {
        badges += p[i].kind == OS_PEER_BADGE;
    }
    if (w) {
        os_wifi_get_info(w);
    }
    if (w && w->state == OS_WIFI_CONNECTED && w->channel && w->channel != os_mesh_channel()) {
        snprintf(out, n, "Paused: Wi-Fi on ch %d", w->channel);
    } else {
        snprintf(out, n, "Ch %d, %d badge%s nearby", os_mesh_channel(), badges, badges == 1 ? "" : "s");
    }
    free(p);
    free(w);
}

static void xdex_status(void *ctx, char *out, size_t n)
{
    int k = xdex_pending_count();
    if (k > 0) {
        snprintf(out, n, "%d request%s waiting", k, k == 1 ? "" : "s");
    } else {
        strlcpy(out, "Listening for swap requests", n);
    }
}

static void trade_status(void *ctx, char *out, size_t n)
{
    const trade_view_t *t = trade_current();
    int k = trade_invite_count();
    if (t && t->phase != TRADE_IDLE && t->phase != TRADE_DONE && t->phase != TRADE_CANCELLED) {
        snprintf(out, n, "Trading with %s", t->label[0] ? t->label : t->peer);
    } else if (k > 0) {
        snprintf(out, n, "%d invite%s waiting", k, k == 1 ? "" : "s");
    } else {
        strlcpy(out, "Listening for invites", n);
    }
}

static void quests_status(void *ctx, char *out, size_t n)
{
    const quest_dialog_t *d = quests_dialog();
    if (d) {
        snprintf(out, n, "Talking to %s", d->name[0] ? d->name : d->beacon);
    } else {
        strlcpy(out, "Idle", n);
    }
}

static void cloud_status(void *ctx, char *out, size_t n)
{
    os_cloud_info_t *in = malloc(sizeof(*in));
    int queued = os_outbox_count();
    char head[32] = "Offline";
    if (in) {
        os_cloud_get_info(in);
        if (os_api_online() && in->synced_ms) {
            char ago[24];
            snprintf(head, sizeof(head), "Synced %s", ui_fmt_ago(ago, sizeof(ago), game_now_ms() - in->synced_ms));
        } else if (os_api_online()) {
            strlcpy(head, "Online", sizeof(head));
        }
        free(in);
    }
    if (queued > 0) {
        snprintf(out, n, "%s, %d to upload", head, queued);
    } else {
        strlcpy(out, head, n);
    }
}

static const os_service_def_t s_mesh = {
    .id = "mesh", .name = "Mesh radio", .symbol = LV_SYMBOL_GPS, .accent = COL_PINK,
    .why_always = "Badges find each other through it",
    .state = mesh_state, .status = mesh_status,
};
static const os_service_def_t s_xdex = {
    .id = "xdex", .name = "X Dex swaps", .symbol = LV_SYMBOL_SHUFFLE, .accent = COL_PINK,
    .why_always = GAME_WHY, .state = on, .status = xdex_status,
};
static const os_service_def_t s_trade = {
    .id = "trade", .name = "Trade invites", .symbol = LV_SYMBOL_LOOP, .accent = COL_AQUA,
    .why_always = GAME_WHY, .state = on, .status = trade_status,
};
static const os_service_def_t s_quests = {
    .id = "quests", .name = "Quest relay", .symbol = LV_SYMBOL_EYE_OPEN, .accent = COL_ORANGE,
    .why_always = GAME_WHY, .state = on, .status = quests_status,
};
static const os_service_def_t s_cloud = {
    .id = "cloud", .name = "Server link", .symbol = LV_SYMBOL_WIFI, .accent = COL_BLUE,
    .why_always = "It works only while Wi-Fi is on",
    .state = on, .status = cloud_status,
};

void game_services_init(void)
{
    os_service_register(&s_mesh);
    os_service_register(&s_xdex);
    os_service_register(&s_trade);
    os_service_register(&s_quests);
    os_service_register(&s_cloud);
}
