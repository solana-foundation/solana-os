#pragma once

// Solana OS app manager.
//
// One app runs at a time. The OS owns the status bar and hands the app a root
// container. Every app callback runs on the OS task with the LVGL lock held,
// so apps may touch LVGL freely there, and must not block for long. Work that
// blocks (scans, SD I/O) belongs in the app's own task or driver callbacks;
// on_tick then renders the results.

#include <stdbool.h>
#include "lvgl.h"
#include "os_keys.h"
#include "theme.h"

#define OS_TICK_MS      100

typedef struct {
    const char *id;                 // short name for the console: "open wifi"
    const char *name;               // launcher / status bar title
    const char *symbol;             // LV_SYMBOL_* launcher icon, or NULL to use icon
    const lv_image_dsc_t *icon;
    uint32_t accent;
    void (*on_enter)(lv_obj_t *root);
    void (*on_exit)(void);          // stop tasks/radios/timers, free memory
    bool (*on_key)(os_key_t key, bool repeat);  // false = not handled (Back then exits)
    void (*on_tick)(void);          // every OS_TICK_MS
    void (*on_button)(uint32_t bit); // optional: raw presses instead of keys (calibration)
    bool (*visible)(void);          // optional: in the launcher only while true (Beacon); "open <id>" works anyway
} os_app_t;

// apps/registry.c
extern const os_app_t *const os_apps[];
extern const int os_app_count;
extern const os_app_t os_launcher;
extern const os_app_t os_calibrate;     // hidden system app
extern const os_app_t app_update;       // hidden: Settings > Software update (apps/update)
extern const os_app_t app_themes;       // hidden: Settings > Theme (apps/themes); the store's Themes app is Lua

void os_start(void);
void os_splash(void);

// Safe from any task. Takes effect after the current callback returns.
void os_launch(const os_app_t *app);
void os_launch_id(const char *id);
void os_inject_key(os_key_t key);

// Run fn(arg) on the OS task with the LVGL lock held, after the current
// callback. Safe from any task (mesh handlers, API callbacks, timers). Returns
// false if the queue is full; then fn is not called.
bool os_post(void (*fn)(void *arg), void *arg);
void os_toast(const char *fmt, ...);         // any task
// Calls cb once, on the OS task, the first time the battery sense says USB
// power (over 4.3 V, the status bar's rule; also when there is no battery
// sense), now if it already does. One callback at a time; any task.
void os_on_usb_power(void (*cb)(void));
const os_app_t *os_current(void);

// OS-drawn confirm dialog over the current app: SELECT = yes, CANCEL = no.
// The app (or a Lua script) never sees those keys, so it can't answer for
// the user: spending !LAMPs, transfers, factory reset. done(yes, ctx) runs on
// the OS task. Leaving the app (HOME, os_launch) answers no before the app's
// on_exit. If a confirm is already open, done(false) runs right away. Safe
// from any task (the text is copied).
typedef void (*os_confirm_cb_t)(bool yes, void *ctx);
void os_confirm(const char *text, os_confirm_cb_t done, void *ctx);
bool os_confirm_open(void);
// Close the open confirm if it was opened with `done` (its question no longer
// applies): done(false, ctx) runs as if CANCEL was pressed. OS task.
void os_confirm_dismiss(os_confirm_cb_t done);

// An OS-drawn panel over the current app (the bridge code, core/os_bridge.c),
// so an app, Lua ones included, can show it without being left. While open it
// gets every key but HOME (on_key returning false closes it) and a tick every
// OS_TICK_MS after the app's. Leaving the app, HOME and a theme change close
// it; on_close deletes its objects. A confirm dialog still goes on top. OS task.
typedef struct {
    bool (*on_key)(os_key_t key, bool repeat);
    void (*on_tick)(void);
    void (*on_close)(void);
} os_overlay_t;
void os_overlay_open(const os_overlay_t *o);    // closes another one first
void os_overlay_close(void);
const os_overlay_t *os_overlay(void);           // the open one, or NULL

// Keep the current app on screen (a firmware update writing the flash): while
// held, HOME toasts `why` instead of going home and os_launch() is ignored.
// NULL releases it. OS task.
void os_hold(const char *why);

// Rebuild the UI in the theme OS_SET_THEME names (OS task, LVGL locked; the
// Themes app, the "theme" console command).
void os_theme_apply(void);
// The status bar, for a home layout that draws in it (the Streaming theme's
// tabs): delete what you add when your layout goes.
lv_obj_t *os_status_bar(void);
// Hide the bar's own contents while a home layout draws its own header in it
// (a design with no system bar). Call it in the layout's enter(); the OS shows
// them again when any app is entered.
void os_status_bare(bool on);
// UI_BAR_SLIM: the title's text and colour (a COL_*, 0 = text) until the next
// app is entered, e.g. the focused item's name.
void os_status_title(const char *text, uint32_t color);
// Switch to the theme with this id and save the choice (any task; it happens
// after the current callback). False if no theme has that id.
bool os_theme_set(int id);

// Set the backlight from the Brightness setting (any task; at boot and when
// Settings changes it).
void os_brightness_apply(void);

// Set the panel's colour inversion from the Invert colours setting (any task
// but the LVGL task; at boot and when Settings changes it). See bsp.h.
void os_invert_apply(void);

// Screenshot of the live screen over the console, see tools/badge.py.
void os_screenshot(void);
