#pragma once

// Internals shared by the Lua host's files. lua_host.h is the public side.
//
// Threads: the script runs on the "lua" task (lua_host.c), which holds the
// LVGL lock around every callback. The OS task feeds it keys, API answers
// arrive as OS-task callbacks (os_api) and mesh messages on the mesh task;
// both only queue events (lh_ev_t) and fill call records under lh_mx.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "core/os.h"
#include "core/ui.h"
#include "core/os_ble.h"
#include "vendor/lua.h"
#include "vendor/lauxlib.h"
#include "lua_host.h"
#include "lua_pkg.h"
#include "lua_xip.h"

#define LH_OBJ_MAX          128         // UI objects at a time (SRAM: ~250 B each; 64 before 2.0.0)
// SRAM for UI (2.0.0): LVGL's objects and label text stay in SRAM while a Lua
// heap in PSRAM grows, so the allocator's PSRAM reserve can't protect them.
// Every call that makes LVGL objects first checks that the SRAM left after it
// stays above LUA_HOST_SYS_RESERVE (lh_ui_room). The object cap alone isn't
// enough: one ui.list slot is up to 43 LVGL objects (ui_list_create_at).
#define LH_UI_OBJ_SRAM      512         // one object, with headroom (~250 B measured)
#define LH_UI_LIST_SRAM     (12 * 1024) // a ui.list (8 rows of 5 objects + 3) or the ui.input keyboard
#define LH_TEXT_MAX         200         // bytes per label; longer text ends in "..."
#define LH_TEXT_TOTAL       (12 * 1024) // label text per app (LVGL keeps it in SRAM; 6 KB before 2.0.0)
#define LH_QR_MAX           1           // ui.qr codes at a time: each canvas is size^2 / 8 bytes of system heap
#define LH_QR_TEXT_MAX      256         // bytes per code; more doesn't scan off this screen
#define LH_QR_MIN           25          // px: a version 1 code and its quiet zone at 1 px per module
#define LH_TIMER_MAX        8
#define LH_TIMER_MIN_MS     20
#define LH_TICK_MS          30          // on_tick period (50 before; whole 10 ms RTOS ticks)
#define LH_STORAGE_MAX      (16 * 1024) // data.json
#define LH_STORAGE_SAVE_MS  1000        // storage.set writes at most this often
#define LH_MESH_PER_S       4
#define LH_MESH_MAX         200         // bytes of JSON per mesh.send
#define LH_MEET_MAX         100         // bytes of JSON per side of a mesh.meet
#define LH_CALLS_MAX        16          // server calls waiting or in flight per app
#define LH_CALLS_SENT       3           // at once in os_api, whose queue the OS shares
#define LH_JSON_DEPTH       16          // nesting; bounds the decoder's recursion
#define LH_LED_COUNT        8
#define LH_FS_READ_MAX      LUA_PKG_FILE_MAX    // fs.read / fs.json per call: a whole file (16 KB before 2.0.0)
#define LH_FS_LINE_MAX      512         // fs.lines: longer lines come in pieces
#define LH_FS_OPEN_STEPS    500         // budget per file opened ...
#define LH_FS_BYTES_PER_STEP 4          // ... and per bytes read or compiled
#define LH_READ_CHUNK       256         // compiling from the card, a piece at a time
#define LH_WS_LATE          4           // sockets closing without a connection, per round
#define LH_BLE_LATE         8           // ble callbacks owed for the next round (closes, lost answers)
#define LH_BLE_PER_ROUND    OS_BLE_EVENTS_MAX   // ble events per round of the loop, then keys and timers

typedef enum { LH_LABEL, LH_RECT, LH_BAR, LH_GRADIENT, LH_LIST, LH_QR, LH_IMAGE, LH_CANVAS, LH_ANIM, LH_KIND_COUNT } lh_kind_t;

typedef struct {
    lv_obj_t *obj;              // NULL: free slot
    ui_list_t *list;            // LH_LIST
    int items_ref, select_ref;  // LH_LIST: registry refs; LH_IMAGE, LH_CANVAS, LH_ANIM: the view in items_ref
    uint32_t order;             // creation order: the newest visible list gets Up/Down
    uint16_t gen;               // bumped when freed, so stale handles fail
    uint16_t text_len;          // LH_LABEL: bytes counted in lh_t.text_total
    uint8_t kind;
} lh_obj_t;

typedef struct {
    int id;                     // 0: free
    int fn_ref;
    uint32_t period;            // 0: once
    int64_t due;
} lh_timer_t;

// One server request (lua_net.c). The Lua task owns the list; the OS-task
// answer callback finds a record by its os_api ctx and fills it in under lh_mx.
typedef struct lh_call {
    struct lh_call *next;
    uint32_t token;             // os_confirm ctx: a number, never a pointer
    void *ctx;                  // os_api ctx while sent (os_api_token)
    int cb_ref;
    uint8_t op;
    uint8_t state;              // LH_CALL_*
    bool done;
    int status;                 // HTTP status, 0 = no answer
    char *body;
    const char *err;            // local outcome: "denied", "offline", ...
    char *method, *path, *req;  // until it's sent
} lh_call_t;

enum { LH_CALL_QUEUED, LH_CALL_CONFIRM, LH_CALL_SENT };

// The app's socket (lua_ws.c). Handles carry the id they were made with; only
// the live one's works. state, sock, call and the ticket's answer are shared
// with the OS task (the answer) and the cloud task (wakes) under lh_mx.
enum { LH_WS_NONE, LH_WS_TICKET, LH_WS_CONNECTING, LH_WS_OPEN };

typedef struct {
    uint32_t id, seq;           // the live socket (0: none), the last id given
    uint8_t state;              // LH_WS_*
    int cb_ref;
    void *call;                 // the ticket call's os_api ctx
    void *sock;                 // os_socket's ctx, once it has the socket
    char *protocols;            // "a, b" for os_socket, or NULL
    bool answered;              // the ticket's answer is in
    int status;
    char *body;
    struct {
        int ref;
        const char *reason;
    } late[LH_WS_LATE];         // closes for the next round (denied, busy, closed by the app)
    int nlate;
} lh_ws_t;

// Bluetooth (lua_ble.c). ctx is os_ble's owner ctx (an os_api_token) while
// on; woken is shared with whatever task os_ble wakes us from, under lh_mx.
// The link (central) carries the id it was made with, as ws sockets do.
enum { LH_BLE_LINK_NONE, LH_BLE_LINK_CONNECTING, LH_BLE_LINK_OPEN, LH_BLE_LINK_CLOSING };
enum { LH_BLE_OP_READ, LH_BLE_OP_WRITE, LH_BLE_OP_SUB };
enum { LH_BLE_LATE_CLOSE, LH_BLE_LATE_LOST };

typedef struct {
    void *ctx;                  // NULL: off
    bool woken;                 // a wake is queued and not yet taken
    int scan_ref, serve_ref, link_ref;
    os_ble_uuid_t scan_service; // the scan's filters, again for reports queued before a new scan
    bool scan_by_service;
    char scan_name[OS_BLE_NAME_MAX + 1];
    uint32_t link_id, link_seq; // the live link (0: none), the last id given
    uint8_t link_state;         // LH_BLE_LINK_*
    bool served, peer;          // ble.serve done; a device is connected to us
    uint8_t n_chrs;
    os_ble_uuid_t chrs[OS_BLE_CHARS_MAX];
    struct {
        uint32_t op;            // 0: free
        int ref;                // LUA_NOREF: no callback (subscribe)
        uint8_t kind;           // LH_BLE_OP_*
    } ops[OS_BLE_OPS_MAX];
    struct {
        int ref;
        uint8_t kind;           // LH_BLE_LATE_*
        const char *reason;
    } late[LH_BLE_LATE];
    int nlate;
} lh_ble_t;

typedef struct {
    uint32_t size;              // bytes run in place, 0: compiled on the badge
    int slot;                   // flash slot, -1 if none
    bool copied;                // copied into it at this launch
} lh_code_stat_t;

typedef struct lh {
    lua_State *L;
    lua_pkg_t pkg;
    uint32_t gen;               // launch number; events carry it
    lua_xip_t code;             // the store's compiled code, mapped (lua_code.c); data NULL: compiled here
    lh_code_stat_t code_stat;   // for "lua mem"

    // Limits
    size_t mem, mem_peak;
    size_t heap_max;            // lua_host_heap_max() at launch
    bool psram;                 // the Lua heap is in PSRAM (os_mem.h)
    size_t grown;               // growth since the last system-heap check (lh_alloc)
    bool mem_hit;               // the allocator refused to go over the cap
    uint32_t steps, steps_peak;
    bool killed;                // over the instruction budget
    char kill_msg[64];          // "main.lua:12: Script took too long"
    size_t heap_before;         // SRAM free at launch
    char err[96];               // why the script stopped, "" if it didn't
    int64_t t0;                 // ms at launch

    // Task and lifecycle
    TaskHandle_t task;
    volatile bool quit;         // the OS is closing the app
    bool exit_req;              // sys.exit()
    bool ui_off;                // shutting down without the LVGL lock: UI calls do nothing
    bool failed;                // the error screen is up
    bool finished, orphan;      // task end handshake (under lh_mx)

    // UI
    lv_obj_t *area, *hint, *modal, *loading;
    ui_kb_t *kb;
    int modal_ref;              // ui.confirm / ui.input callback
    lh_obj_t objs[LH_OBJ_MAX];
    uint32_t obj_order;
    size_t text_total;          // label text bytes on screen
    void *gif_work;             // ui.anim's decoding table (lua_img.c), a userdata kept for the app's life
    int key_refs[3];            // "title", "sub", "right": row lookups that never allocate

    // LEDs, shown at the end of each callback
    uint32_t led[LH_LED_COUNT];
    bool led_dirty, led_used;

    // Timers
    lh_timer_t timers[LH_TIMER_MAX];
    int timer_seq;
    int64_t next_tick;

    // fs: the last file read stays open (lua_fs.c)
    FILE *fs_file;
    long fs_size;
    char fs_path[LUA_PKG_PATH_MAX + 1];

    // storage.*
    int store_ref;
    char *store_pending;        // encoded data.json waiting to be written
    size_t store_len;
    int64_t store_saved;

    // mesh
    int mesh_sub;
    int64_t mesh_sent[LH_MESH_PER_S];
    int scan_ref;               // mesh.scan's callback, LUA_NOREF when none waits
    struct lh_meet *meet;       // mesh.meet's handshakes (lua_meet.c), made on first use

    // Server calls
    lh_call_t *calls;

    // ws
    lh_ws_t ws;

    // ble
    lh_ble_t ble;
} lh_t;

// Figures from the last run, for "lua mem".
typedef struct {
    char id[LUA_PKG_ID_MAX + 1];
    size_t mem_peak, mem_end;   // Lua heap; mem_end is what lua_close left (0)
    uint32_t steps_peak;        // most VM instructions in one callback
    uint32_t stack_free;        // lua task stack never used, bytes
    uint32_t code_size;         // compiled code run in place, bytes (0: compiled on the badge)
    int code_slot;              // its flash slot, -1 if none
    bool code_copied;           // copied into the slot at this launch
    size_t heap_before;         // SRAM free when the app started
    char err[96];               // last error, "" if none
} lh_stats_t;

extern lh_stats_t lh_stats;

// Events for the Lua task.
typedef enum { LH_EV_KEY, LH_EV_WAKE, LH_EV_MESH, LH_EV_MEET, LH_EV_SCAN, LH_EV_QUIT } lh_ev_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t key;
    bool repeat;
    uint32_t gen;
    char *data;                 // LH_EV_MESH, LH_EV_MEET: "<from id>\0<json>", freed by the receiver
} lh_ev_t;

extern SemaphoreHandle_t lh_mx;     // guards call records, the mesh filter, lh_cur
extern lh_t *lh_cur;                // the running session (OS task view)

static inline lh_t *lh_get(lua_State *L)
{
    return *(lh_t **)lua_getextraspace(L);
}

int64_t lh_now(void);
bool lh_post(const lh_ev_t *ev);                // to the Lua task; false if full
void lh_need_perm(lua_State *L, uint32_t perm, const char *module);

// Runs fn(arg) protected, as one callback: a fresh instruction budget, LEDs
// shown after. fn builds the arguments and lua_call()s the script, so even
// "out of memory" while pushing them is caught. On error the app stops and
// shows the error screen (LVGL lock held); returns false.
bool lh_callback(lh_t *s, lua_CFunction fn, void *arg);
void lh_cmd_register(void);                             // lua_cmd.c

// lua_ui.c
void lh_open_ui(lua_State *L);
// Protected (inside a key callback): dialogs and the focused list take their
// keys first and may call the script. true: consumed.
bool lh_ui_key(lua_State *L, lh_t *s, os_key_t key, bool repeat);
void lh_ui_clear(lh_t *s);                              // delete every script object
void lh_ui_close_dialog(lh_t *s);                       // ui.confirm / ui.input, without calling back
void lh_ui_release(lh_t *s);                            // forget LVGL pointers (OS cleaned them)
lv_color_t lh_color(lua_State *L, int idx);
// For lua_img.c: the session (NULL while shutting down: calls do nothing),
// a new object in the pool (its handle pushed; items_ref = ref, a registry
// slot the caller reserved, freed with obj if it can't be made), the object
// behind handle 1.
lh_t *lh_ui_session(lua_State *L);
lh_obj_t *lh_ui_new_ref(lua_State *L, lh_t *s, lh_kind_t kind, lv_obj_t *obj, int ref);
lh_obj_t *lh_ui_check(lua_State *L, lh_kind_t kind, const char *what);
// Raises "<what>: the badge is low on memory" unless `need` more bytes of
// SRAM would leave LUA_HOST_SYS_RESERVE free. Before any LVGL object is made.
void lh_ui_room(lua_State *L, const char *what, size_t need);

// lua_img.c: ui.load, ui.image, ui.canvas
void lh_img_init(void);                                 // at boot, with the LVGL lock held
void lh_open_img(lua_State *L);
int lh_ui_load(lua_State *L);
int lh_ui_image(lua_State *L);
int lh_ui_canvas(lua_State *L);
int lh_ui_anim(lua_State *L);
int lh_anim_play(lua_State *L);
int lh_anim_stop(lua_State *L);
int lh_anim_playing(lua_State *L);
int lh_anim_scale(lua_State *L);
void lh_anims_run(lh_t *s);                             // Lua task, LVGL lock held: frames that are due
int64_t lh_anims_next(const lh_t *s);                   // when the next is due, or INT64_MAX
int lh_img_set(lua_State *L);
int lh_img_scale(lua_State *L);
int lh_cv_clear(lua_State *L);
int lh_cv_pixel(lua_State *L);
int lh_cv_rect(lua_State *L);
int lh_cv_line(lua_State *L);
int lh_cv_image(lua_State *L);
int lh_cv_get(lua_State *L);

// lua_sys.c: led, timer, sys, badge, json, storage, wifi
void lh_open_sys(lua_State *L);
void lh_led_flush(lh_t *s);
void lh_led_off(void);
int64_t lh_timers_next(lh_t *s);                        // earliest due time, or INT64_MAX
void lh_timers_run(lh_t *s);
bool lh_storage_load(lh_t *s);
void lh_storage_flush(lh_t *s, bool force);

// lua_fs.c: fs and require
void lh_open_fs(lua_State *L);
void lh_fs_close(lh_t *s);                              // the cached file, when the app stops
void lh_fs_check_path(lua_State *L, int idx, char out[LUA_PKG_PATH_MAX + 1]);  // a bundled file's path argument
FILE *lh_fs_fopen(lh_t *s, const char *rel, long *size);  // a file of the caller's own (not the cache); NULL: none

// lua_code.c: the store's compiled code, run in place (code.bin)
#define LH_CODE_NONE    (-1)
bool lh_code_open(lh_t *s);         // lua task, before the state; false: compile the source
void lh_code_close(lh_t *s);        // after lua_close
// Loads the chunk compiled from rel ("main.lua", "lib/quiz.lua"): LUA_OK and
// the function pushed, a lua_load error and its message, or LH_CODE_NONE
// (nothing pushed) when the code has no such chunk.
int lh_code_load(lua_State *L, lh_t *s, const char *rel);

// lua_ws.c: the app's socket
void lh_open_ws(lua_State *L);
void lh_ws_run(lh_t *s);                                // Lua task: closes and the socket's events
bool lh_ws_busy(const lh_t *s);                         // from ws.open until its close: no server calls
void lh_ws_stop(lh_t *s);                               // any task: the app stops, the socket goes
void lh_ws_free(lh_t *s);                               // after the Lua task ended

// lua_ble.c: Bluetooth LE (os_ble)
void lh_open_ble(lua_State *L);
void lh_ble_init(lh_t *s);                              // at launch: no refs yet
void lh_ble_run(lh_t *s);                               // Lua task: owed callbacks and os_ble's events
void lh_ble_stop(lh_t *s);                              // any task: the app stops, Bluetooth goes off

// lua_crypto.c: crypto (SHA-256, Ed25519 checks) and solana (base58, program-derived addresses)
void lh_open_crypto(lua_State *L);

// lua_net.c: mesh, lamps, items, profile, quest, http
void lh_open_net(lua_State *L);
void lh_calls_run(lh_t *s);                             // answer finished calls
void lh_net_stop(lh_t *s);                              // OS task, after the Lua task ended
void lh_calls_free(lh_t *s);
void lh_mesh_event(lh_t *s, char *data);
void lh_mesh_scan_run(lh_t *s);                         // mesh.scan's callback once its results wait
void lh_mesh_end_app(void);                             // stop the app's scan and channel (any task)
void lh_mesh_start(lh_t *s);                            // OS task
void lh_mesh_mute(void);                                // stop queueing messages
void lh_mesh_stop(void);                                // OS task
void lh_mesh_me(char out[7]);                           // this badge's id when the app started, "" without one

// lua_meet.c: mesh.meet, a statement two badges sign (docs/MESH.md "Meets")
int lh_mesh_meet(lua_State *L);
void lh_meet_event(lh_t *s, const char *data);          // Lua task: a meet message, "<from id>\0<json>"
void lh_meet_run(lh_t *s);                              // Lua task: repeats and the timeout of an ask
void lh_meet_free(lh_t *s);                             // after the Lua task ended

// lua_json.c: JSON <-> Lua. Encoding never raises Lua errors (it may run
// with C memory to free); decoding may (out of memory).
typedef struct {
    char *p;
    size_t len, cap, max;
    const char *err;
} lh_jbuf_t;

bool lh_json_encode(lua_State *L, int idx, lh_jbuf_t *b);   // b->p is malloc'd; free it
const char *lh_json_decode(lua_State *L, const char *s, size_t len);  // NULL: value pushed
void lh_push_jbuf(lua_State *L, lh_jbuf_t *b);      // push b as a string and free it
void lh_open_json(lua_State *L);
