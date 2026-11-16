#pragma once

// Mailbox (Solana OS 2.3.0; the parent repo's docs/HHGTS.md "Mailbox",
// server/API.md "Mailbox"): the owner's notifications from Solana, apps'
// servers and other badges' apps. A built-in background service (`mailbox`,
// core/os_service.h: stoppable, starts at boot): while it runs it collects
// the owner's mail whenever a hello says some waits (`mail` > 0), which is
// also how it collects when Wi-Fi comes up (a hello is the first thing the
// badge does online). Collecting is GET /api/device/mail pages and one ack,
// through os_api (the cloud task).
//
// Storage: /sdcard/hh/mail/index.json (the list) and mail/<id>.json (one
// message as the server sent it). RAM keeps the list (in PSRAM) and loads a
// message's text on first read. Without a card it keeps the last few in RAM
// until a restart. A wipe (new owner, factory reset) empties it.
//
// Threads: the service's state machine runs on the OS task (game tick, os_api
// callbacks). The list is shared with the Lua task (mail.list / mail.get) and
// the console under a mutex, so every function here may be called from any
// task, except where it says OS task.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAILBOX_MAX         50          // on the card (HHGTS.md)
#define MAILBOX_RAM_MAX     10          // without a card
#define MAILBOX_TITLE_MAX   32          // server/API.md "Text rules"
#define MAILBOX_TEXT_MAX    200
#define MAILBOX_DATA_MAX    128
#define MAILBOX_LABEL_MAX   24          // an app's name, an owner label
#define MAILBOX_APP_MAX     24          // app ids (lua_pkg.h LUA_PKG_ID_MAX)

typedef struct {
    uint32_t id;
    uint32_t at, exp;                   // unix seconds
    bool read;
    char app[MAILBOX_APP_MAX + 1];      // the app it belongs to, "" = none (Solana)
    char an[MAILBOX_LABEL_MAX + 1];     // the sender as listed: "Solana", the app's name
    char from[8];                       // a badge id, "server" or "solana"
    char by[MAILBOX_LABEL_MAX + 1];     // badge mail: the sender's owner label, else ""
    char title[MAILBOX_TITLE_MAX + 1];
} mailbox_item_t;

typedef struct {
    mailbox_item_t h;
    char text[MAILBOX_TEXT_MAX + 1];
    char data[MAILBOX_DATA_MAX + 1];    // compact JSON object, "" = none
} mailbox_mail_t;

// game_init(): the service (os_service_register), the `mail` console command.
void mailbox_init(void);
void mailbox_tick(int64_t now);         // OS task, from the game tick

bool mailbox_running(void);             // the service is on (Services)
int mailbox_count(void);
int mailbox_unread(void);
// The i-th message, newest first: false past the end.
bool mailbox_item(int i, mailbox_item_t *out);
// One message in full (reads the card the first time): false if it's gone.
bool mailbox_get(uint32_t id, mailbox_mail_t *out);
// The app's messages, newest first, up to max (mail.list): how many.
int mailbox_for_app(const char *app, mailbox_item_t *out, int max);
void mailbox_mark_read(uint32_t id);
void mailbox_delete(uint32_t id);
int mailbox_delete_read(void);          // how many went

// Collect now (the Mailbox app's RIGHT, `mail sync`): false when it can't
// (off, offline, unclaimed, already collecting). OS task.
bool mailbox_sync(void);
bool mailbox_syncing(void);
// When the last collection ended well (unix ms, 0 = not this boot), and
// whether the last one failed (offline mid-sync, a 5xx).
int64_t mailbox_synced_at(void);
bool mailbox_failed(void);
