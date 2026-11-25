// Battery percentage: the curve and the filter (battery_calc.h). Pure C.

#include <string.h>
#include "battery_calc.h"

// A single LiPo / Li-ion cell, 100 % to 0 % in steps of 10 %: Meshtastic's
// default OCV_ARRAY (meshtastic/firmware, src/power.h, GPL-3.0; the numbers
// only), the curve its firmware uses for one-cell LiPo boards unless a board
// supplies its own. It is flat from ~3.7 to ~3.9 V and steep at both ends,
// which a straight line from 3.3 to 4.15 V (Solana OS before 2.4.0) is not.
// Tune it from a discharge log (`batt log`, firmware/CHANGELOG.md 2.4.0).
static const int16_t k_curve[11] = {4190, 4050, 3990, 3890, 3800, 3720, 3630, 3530, 3420, 3300, 3100};

int batt_curve_pct(int mv)
{
    if (mv >= k_curve[0]) {
        return 100;
    }
    if (mv <= k_curve[10]) {
        return 0;
    }
    int i = 1;
    while (mv < k_curve[i]) {
        i++;
    }
    // k_curve[i] <= mv < k_curve[i - 1]: between (10 - i) * 10 and (11 - i) * 10 %.
    int lo = k_curve[i], hi = k_curve[i - 1];
    return (10 - i) * 10 + (mv - lo) * 10 / (hi - lo);
}

int batt_curve_mv(int pct)
{
    if (pct >= 100) {
        return k_curve[0];
    }
    if (pct <= 0) {
        return k_curve[10];
    }
    int i = 10 - pct / 10;                  // segment from k_curve[i] (pct / 10 * 10 %)
    int lo = k_curve[i], hi = k_curve[i - 1];
    int frac = pct % 10;
    return lo + ((hi - lo) * frac + 9) / 10; // rounded up: the lowest mV that reads pct
}

void batt_filter_init(batt_filter_t *f)
{
    memset(f, 0, sizeof(*f));
    f->shown = -1;
}

static int median(const batt_filter_t *f)
{
    int16_t v[BATT_MEDIAN_N];
    int n = f->n;
    memcpy(v, f->ring, sizeof(v[0]) * n);
    for (int i = 1; i < n; i++) {           // n <= 9: insertion sort
        int16_t x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static void battery_stretch_end(batt_filter_t *f)
{
    f->n = f->head = 0;
    f->seeded = false;
    f->shown = -1;
}

void batt_filter_step(batt_filter_t *f, int mv)
{
    if (mv < 0) {
        return;                             // a failed read changes nothing
    }
    bool first = !f->known;
    f->known = true;
    if (mv > BATT_USB_MV) {
        // USB power: VSYS is VBUS, the cell is out of sight (and charging, if
        // there is one). Nothing to show but "USB".
        f->usb = true;
        f->below = 0;
        battery_stretch_end(f);
        return;
    }
    if (f->usb && !first) {
        // One low reading on USB can be the plug's own dip: on battery after
        // BATT_UNPLUG_N in a row.
        if (++f->below < BATT_UNPLUG_N) {
            return;
        }
    }
    f->usb = false;
    f->below = 0;

    f->ring[f->head] = (int16_t)mv;
    f->head = (f->head + 1) % BATT_MEDIAN_N;
    if (f->n < BATT_MEDIAN_N) {
        f->n++;
    }
    int med = median(f);
    if (!f->seeded || f->n < BATT_MEDIAN_N) {
        // Settling (boot, or just unplugged): follow the median of what we have.
        f->ema16 = med * 16;
        f->seeded = true;
        f->shown = (int16_t)batt_curve_pct(med);
        return;
    }
    f->ema16 += (med * 16 - f->ema16) / BATT_EMA_SECONDS;
    int pct = batt_curve_pct(f->ema16 / 16);
    if (pct < f->shown) {
        f->shown = (int16_t)pct;            // on battery the number only falls
    }
}

int batt_filter_mv(const batt_filter_t *f)
{
    return f->known && !f->usb && f->seeded ? (int)(f->ema16 / 16) : 0;
}
