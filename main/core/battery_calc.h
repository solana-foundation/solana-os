// Battery percentage from the battery sense: the curve and the filter, pure C
// (no ESP-IDF, no FreeRTOS), so core/tests/battery_test.c runs them on the
// host. os_battery.c owns the one filter state the whole OS reads.
//
// What the sense reads (KiCad board file): ADC1/GPIO1 sits on a 22k/22k
// divider from VSYS, behind the power path, not on the cell. On USB, VSYS is
// VBUS through a BAT60A Schottky (~4.6-4.9 V), so the cell can't be seen at
// all; on battery it is the cell less the drop across two AO3401A switches,
// the DW03D protection FETs and the cell's own resistance, so it sags with
// load. The MCP73831's STAT pin drives the red/blue LEDs only: the firmware
// has no charge-status signal.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BATT_USB_MV         4300    // VSYS over this: USB power (a cell never reads it)
#define BATT_MEDIAN_N       9       // one-second readings the median is taken over
#define BATT_EMA_SECONDS    60      // the slow average's time constant, in readings
#define BATT_UNPLUG_N       2       // readings at or under BATT_USB_MV before "on battery"

// Percent (0..100) of a single LiPo cell at mV, interpolated in a table
// (battery_calc.c names its source). Pure.
int batt_curve_pct(int mv);

// The inverse, for reports: the lowest mV that reads pct. Pure.
int batt_curve_mv(int pct);

typedef struct {
    int16_t ring[BATT_MEDIAN_N];    // the last readings on battery
    uint8_t n, head;
    int32_t ema16;                  // the slow average, mV x 16
    int16_t shown;                  // percent shown, -1 = not yet
    uint8_t below;                  // readings at or under BATT_USB_MV while still "USB"
    bool usb;                       // VSYS says USB power
    bool known;                     // a reading has been made
    bool seeded;                    // ema16 holds a value for this stretch on battery
} batt_filter_t;

void batt_filter_init(batt_filter_t *f);

// One reading, once a second (mv < 0: the sense failed; the state is kept).
// On USB: shown = -1 and the stretch on battery ends. On battery: the median
// of the last readings drops load spikes, the slow average follows it, and
// the percentage shown only ever falls until the next time on USB.
void batt_filter_step(batt_filter_t *f, int mv);

// The filtered battery mV (0 while unknown or on USB).
int batt_filter_mv(const batt_filter_t *f);
