#pragma once

// Background services (Solana OS 2.3.0, docs/HHGTS.md "Background services").
//
// One registry of everything that runs while its app is closed, so the owner
// can see it and stop it in the Services app (apps/services) or on the
// console (`svc`). Two kinds of entry share it:
//
// - Built-in services: Solana OS's own (the mesh, X Dex swaps, Trade invites,
//   quests' relay, the server link; Mailbox, Collaborator). Each registers a
//   static os_service_def_t at boot, before os_service_boot().
// - App services: a store app's service.lua (or a C app's on_service_start)
//   running with the `background` permission (firmware/docs/LUA_API.md
//   "service"). The Lua host owns them and shows them here through one
//   provider (os_service_set_provider), so this file never knows how they run.
//
// Threading: every callback in os_service_def_t and os_service_provider_t
// runs on the OS task (LVGL lock held), like app callbacks: never block in
// one. The functions below are OS-task only too, except os_service_notify()
// and os_service_post_stop(), which any task may call. The console's `svc`
// command hops over to the OS task to use them.
//
// Rules for a service (the registry can't enforce them all, review does):
// - stop() always works: it may not refuse, block or ask. After it returns
//   the service holds no radio (os_mesh_acquire, os_wifi_suspend), no
//   screen, no os_hold and no LED effect, and running() turns false within
//   a second (a task that ends on its own may take that long). Stopping is
//   the owner's, never the service's, decision.
// - A service never calls os_hold() and never opens os_confirm() or an
//   os_overlay over another app. A service that needs the screen
//   (OS_SVC_SCREEN, Collaborator) shows it as a hidden os_app_t it launches
//   (os_launch); HOME and CANCEL leave that app like any other, and the
//   app's on_exit calls the service's own stop (or pauses it, as its
//   contract says), so HOME always ends it.
// - A service never spends, transfers or resets: those go through
//   os_confirm(), which only a foreground app opens, on the owner's key.
// - It tells the owner things through os_service_notify(), not its own UI.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OS_SERVICE_ID_MAX       28      // "app:" + an app id (≤ 24); built-in ids are NVS keys: [a-z0-9_-], ≤ 15
#define OS_SERVICE_NAME_MAX     24      // as the Services app shows it
#define OS_SERVICE_STATUS_MAX   48      // one line: "3 unread", "Live, 12 min"
#define OS_SERVICE_BUILTIN_MAX  12      // built-in registrations

// Flags of a built-in service (os_service_def_t.flags).
#define OS_SVC_STOPPABLE    (1u << 0)   // the owner may stop it (else the Services app says why not)
#define OS_SVC_BOOT         (1u << 1)   // starts at boot until the owner turns that off (NVS "svc/<id>")
#define OS_SVC_SCREEN       (1u << 2)   // takes the screen while it runs (through its own hidden app)
#define OS_SVC_HIDDEN       (1u << 3)   // listed only while running (test hooks, rare modes)

typedef enum {
    OS_SVC_BUILTIN,         // part of Solana OS
    OS_SVC_APP,             // a store app's service (the Lua host's provider)
} os_service_kind_t;

typedef enum {
    OS_SVC_STOPPED,
    OS_SVC_RUNNING,
    OS_SVC_STOPPING,        // asked to stop, still finishing (≤ 1 s)
    OS_SVC_FAILED,          // stopped on an error or a limit; the status line says which
} os_service_state_t;

// A built-in service. Static storage: the registry keeps the pointer.
typedef struct {
    const char *id;                     // "xdex", "mailbox", "collab"
    const char *name;                   // "X Dex swaps"
    const char *symbol;                 // LV_SYMBOL_* for its row
    uint32_t accent;                    // COL_* of the feature it belongs to
    uint32_t flags;                     // OS_SVC_*
    const char *why_always;             // not stoppable: why, ≤ 79 chars ("Part of the game: no memory or radio of its own")
    os_service_state_t (*state)(void *ctx);                 // required
    void (*status)(void *ctx, char *out, size_t n);          // required: one line of printable ASCII
    int32_t (*memory)(void *ctx);                           // optional: bytes it holds now, -1 = unknown
    bool (*start)(void *ctx, const char **why);             // OS_SVC_STOPPABLE / OS_SVC_BOOT: start, or false and *why
    void (*stop)(void *ctx);                                // OS_SVC_STOPPABLE: see "stop() always works"
    int (*unread)(void *ctx);                               // optional: things waiting for the owner (status bar count)
    void *ctx;
    // Optional (2.3.0, Collaborator): an LV_SYMBOL_* the status bar shows, in
    // the service's accent, while this returns one: something the owner must
    // always be able to see (a takeover), whatever app is open. NULL = none.
    const char *(*mark)(void *ctx);
} os_service_def_t;

// What the Services app and the console see of one entry, either kind.
typedef struct {
    char id[OS_SERVICE_ID_MAX + 1];     // built-in: its id; app service: "app:<app id>"
    char name[OS_SERVICE_NAME_MAX + 1];
    char status[OS_SERVICE_STATUS_MAX + 1];
    char why_always[80];                // not stoppable: why ("" for app services)
    const char *symbol;
    uint32_t accent;
    os_service_kind_t kind;
    os_service_state_t state;
    bool stoppable;
    bool boot;                          // starts at boot
    bool screen;                        // OS_SVC_SCREEN
    int32_t memory;                     // bytes, -1 = unknown
    int unread;
    char app[25];                       // OS_SVC_APP: the app's id (≤ 24), "" for built-ins
} os_service_view_t;

// The Lua host's view of app services (part 2 of the feature fills it in).
// Every function runs on the OS task; ids are the provider's own.
typedef struct {
    int (*count)(void);
    bool (*get)(int i, os_service_view_t *out);
    bool (*start)(const char *id, const char **why);    // the owner's start: no question asked (they pressed it)
    void (*stop)(const char *id);                       // the owner's stop: stops it and turns off boot
    bool (*set_boot)(const char *id, bool on);
    // Optional (2.3.0, part 2): the system stops every app service (a
    // software update): they keep their start-at-boot. why: their status.
    void (*stop_all)(const char *why);
} os_service_provider_t;

// Register a built-in service (any time before os_service_boot(); OS task
// after). false when the table is full or the id is taken or invalid.
bool os_service_register(const os_service_def_t *def);

// One provider (the Lua host); NULL removes it. Before os_service_boot().
void os_service_set_provider(const os_service_provider_t *p);

// After os_start(), before console_start(): registers `svc` and, on the OS
// task a few seconds later, starts the built-in services whose boot flag is
// on (NVS "svc/<id>": 1 on, 0 off, missing = OS_SVC_BOOT). App services start
// at boot from their own provider.
void os_service_boot(void);

// Everything the registry lists, built-ins first in registration order, then
// the provider's. i in [0, count).
int os_service_count(void);
bool os_service_get(int i, os_service_view_t *out);
bool os_service_find(const char *id, os_service_view_t *out);

// The owner's actions (the Services app, the console). start of a built-in
// also turns its boot flag on, stop turns it off: what the owner stopped
// stays stopped after a restart. false and *why when it can't ("Part of
// Solana OS", "Not found", the service's own reason).
bool os_service_start(const char *id, const char **why);
bool os_service_stop(const char *id, const char **why);
bool os_service_set_boot(const char *id, bool on, const char **why);

// OS task: stop every app service for the system's own reason (the software
// update before its download): unlike the owner's stop they start again at
// the next boot. why: the status they show ("Stopped: software update").
void os_service_stop_apps(const char *why);

// From any task: stop a service as if the owner had (Collaborator's server
// asking to end, a low-battery rule). Runs on the OS task after the current
// callback.
void os_service_post_stop(const char *id);

// The one way a background service tells the owner something (any task):
// a toast "<name>: <text>" (printable ASCII, cut to fit) and, with led set
// (0xRRGGBB, 0 = none), a short LED flash at OS_LED_MAX. Rate-limited per
// service id: one every 10 s, more are dropped (false). Counts that wait
// (Mailbox's unread) go through def.unread, which the status bar sums.
bool os_service_notify(const char *id, uint32_t led, const char *fmt, ...);

// Sum of every service's unread(): the status bar shows it (a bell and the
// count) while above 0. OS task.
int os_service_unread(void);

// The first running built-in service's mark (def.mark) and its accent, or
// NULL: the status bar shows it. OS task.
const char *os_service_mark(uint32_t *accent);

// Running services (either kind) and the running ones' known memory, for
// the status line and `heap`. OS task.
int os_service_running(int32_t *memory);
