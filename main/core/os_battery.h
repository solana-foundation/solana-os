#pragma once

// The battery as the whole OS sees it (2.4.0): one filtered reading and one
// percentage, made once a second on the OS task (the status bar's tick) and
// read by everything else: the status bar, HW Debug, the console, the
// updater's 70 % rule, Collaborator's, Gossip's and app services' 15 % rules,
// Lua/C sys.battery(). The curve and the filter are core/battery_calc.c.
// Portable: the emulator's bsp_battery_emu.c feeds it.

#include <stdbool.h>

typedef struct {
    bool known;     // a reading has been made (false: no battery sense)
    bool usb;       // USB power: the cell can't be read, pct is meaningless
    int pct;        // percent shown on battery (only falls until the next USB), -1 while unknown or on USB
    int mv;         // the filtered battery mV the percentage comes from (0 while unknown or on USB)
    int raw_mv;     // the last single reading of VSYS (bsp_battery_mv), -1 on error
} os_battery_info_t;

// One line for HW Debug and the consoles' `info`: "4012 mV, 82 %",
// "USB (VSYS 4712 mV)", "-- (no reading)".
void os_battery_text(char *buf, int n);

// After bsp_battery_init(): the first reading (so consumers at boot have one)
// and the `batt` console command.
void os_battery_init(void);

// OS task, once a second.
void os_battery_tick(void);

// Any task.
void os_battery_get(os_battery_info_t *out);

// Percent for rules and reports: 100 on USB power, -1 unknown. Any task.
int os_battery_pct(void);

// USB power, or no battery sense at all (the console's and the status bar's
// rule: a badge that can't read its battery is treated as plugged in). Any task.
bool os_battery_usb(void);

// The battery is known, not on USB power, and shows under pct. Any task.
bool os_battery_below(int pct);
