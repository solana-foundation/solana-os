// Host test for core/battery_calc.c (the curve and the filter). Run:
//   sh firmware/main/core/tests/battery_test.sh
#include <stdio.h>
#include <stdlib.h>
#include "../battery_calc.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void curve(void)
{
    // The table's own points.
    static const int pts[11] = {4190, 4050, 3990, 3890, 3800, 3720, 3630, 3530, 3420, 3300, 3100};
    for (int i = 0; i <= 10; i++) {
        CHECK(batt_curve_pct(pts[i]) == 100 - i * 10, "pct(%d) = %d, want %d", pts[i], batt_curve_pct(pts[i]), 100 - i * 10);
        CHECK(batt_curve_mv(100 - i * 10) == pts[i], "mv(%d) = %d", 100 - i * 10, batt_curve_mv(100 - i * 10));
    }
    CHECK(batt_curve_pct(5000) == 100 && batt_curve_pct(0) == 0 && batt_curve_pct(-5) == 0, "clamps");
    // Monotonic, and batt_curve_mv is the lowest mV that reads pct.
    int last = -1;
    for (int mv = 2900; mv <= 4400; mv++) {
        int p = batt_curve_pct(mv);
        CHECK(p >= last && p >= 0 && p <= 100, "not monotonic at %d", mv);
        last = p;
    }
    for (int p = 1; p <= 100; p++) {
        int mv = batt_curve_mv(p);
        CHECK(batt_curve_pct(mv) == p && batt_curve_pct(mv - 1) < p, "mv(%d) = %d is not where %d %% begins", p, mv, p);
    }
    // The thresholds features use.
    CHECK(batt_curve_mv(15) == 3360, "15 %% at %d", batt_curve_mv(15));
    CHECK(batt_curve_mv(70) == 3890, "70 %% at %d", batt_curve_mv(70));
}

static unsigned s_rng = 1;
static int noise(int amp)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (int)((s_rng >> 16) % (2 * amp + 1)) - amp;
}

static void filter(void)
{
    batt_filter_t f;
    batt_filter_init(&f);
    CHECK(f.shown == -1 && !f.known, "init");
    batt_filter_step(&f, -1);
    CHECK(!f.known, "a failed read is no reading");

    // On USB from boot.
    batt_filter_step(&f, 4712);
    CHECK(f.known && f.usb && f.shown == -1 && batt_filter_mv(&f) == 0, "usb");

    // Unplugged: one low reading isn't enough, two are.
    batt_filter_step(&f, 4100);
    CHECK(f.usb, "one reading under 4.3 V while on USB stays USB");
    batt_filter_step(&f, 4100);
    CHECK(!f.usb && f.shown == batt_curve_pct(4100), "unplugged: shown %d", f.shown);

    // A slow discharge 4100 -> 3400 mV over 3 h, +-15 mV noise and a 150 mV
    // sag (Wi-Fi burst) for 1 s in every 7: the number only falls, by
    // at most 1 a step, never flickers, and ends near the truth.
    int prev = f.shown, changes = 0;
    for (int s = 0; s < 3 * 3600; s++) {
        int truth = 4100 - 700 * s / (3 * 3600);
        int mv = truth + noise(15) - (s % 7 == 3 ? 150 : 0);
        batt_filter_step(&f, mv);
        CHECK(f.shown <= prev, "rose at %d s: %d -> %d", s, prev, f.shown);
        CHECK(prev - f.shown <= 1, "jumped at %d s: %d -> %d", s, prev, f.shown);
        changes += f.shown != prev;
        prev = f.shown;
    }
    int want = batt_curve_pct(3400);
    CHECK(abs(f.shown - want) <= 2, "ends at %d %%, want ~%d", f.shown, want);
    CHECK(changes <= 100 - want + 2, "%d changes", changes);

    // A load ends (screen dimmed): the voltage recovers 60 mV; the number stays.
    int held = f.shown;
    for (int s = 0; s < 600; s++) {
        batt_filter_step(&f, 3460 + noise(10));
    }
    CHECK(f.shown == held, "rose on battery: %d -> %d", held, f.shown);

    // Plugged in: USB at once; unplugged after charging: a fresh, higher number.
    batt_filter_step(&f, 4750);
    CHECK(f.usb && f.shown == -1, "plugged in");
    batt_filter_step(&f, 4150);
    batt_filter_step(&f, 4150);
    CHECK(!f.usb && f.shown == batt_curve_pct(4150), "after charging %d", f.shown);

    // Boot on battery: the first readings settle on their median, so one
    // sagging first reading doesn't pin the number low.
    batt_filter_init(&f);
    batt_filter_step(&f, 3600);             // boot surge
    for (int s = 0; s < 8; s++) {
        batt_filter_step(&f, 3900);
    }
    CHECK(f.shown == batt_curve_pct(3900), "boot settles to %d, got %d", batt_curve_pct(3900), f.shown);

    // A reading over 4.3 V on battery is impossible; a failed read keeps state.
    int before = f.shown;
    batt_filter_step(&f, -1);
    CHECK(f.shown == before && !f.usb, "failed read");
}

int main(void)
{
    curve();
    filter();
    printf("%s\n", fails ? "battery_test: FAILED" : "battery_test: ok");
    // The mapping table for the changelog: old straight line vs the curve.
    if (getenv("TABLE")) {
        printf("mV    old  new\n");
        for (int mv = 4200; mv >= 3200; mv -= 50) {
            int old = mv <= 3300 ? 0 : mv >= 4150 ? 100 : (mv - 3300) * 100 / 850;
            printf("%d  %3d  %3d\n", mv, old, batt_curve_pct(mv));
        }
    }
    return fails != 0;
}
