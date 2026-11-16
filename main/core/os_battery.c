// The OS's one battery reading (os_battery.h) and the `batt` console command
// with its discharge logger. Portable.
//
// The logger is for tuning the curve: `batt log on` (on USB, where the console
// is), unplug, use the badge, plug back in, `batt log`. It appends a line a
// minute (and one at every USB change) to /sdcard/battery.csv, outside
// /sdcard/hh (the owner's data), and costs nothing while off. The flag lives
// in RAM: a restart ends the run (which is how a run to empty ends anyway).

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp.h"
#include "battery_calc.h"
#include "os_battery.h"
#include "os_cmd.h"
#include "os_settings.h"
#include "os_wifi.h"

#define LOG_PATH        BSP_SD_MOUNT "/battery.csv"
#define LOG_MAX_BYTES   (512 * 1024)    // ~9 days at a line a minute: then it stops
#define LOG_HEAD        "up_s,unix,raw_mv,filt_mv,pct,usb,wifi,bright,led\n"

static batt_filter_t s_f;               // OS task only
// What other tasks read: known | usb << 1 | (pct + 1) << 2, and the two mVs.
static _Atomic uint32_t s_state;
static _Atomic int s_mv, s_raw;

// Set by the console's task, read by the OS task's tick: a late tick only
// writes its line a second later.
static _Atomic bool s_log;
static _Atomic int s_log_every = 60;    // seconds
static int64_t s_log_at;                // OS task
static bool s_logging;                  // OS task: s_log as the last tick saw it
static bool s_log_usb;                  // usb at the last line

static void publish(int raw)
{
    int pct = s_f.usb || s_f.shown < 0 ? -1 : s_f.shown;
    atomic_store(&s_raw, raw);
    atomic_store(&s_mv, batt_filter_mv(&s_f));
    atomic_store(&s_state, (uint32_t)s_f.known | (uint32_t)s_f.usb << 1 | (uint32_t)(pct + 1) << 2);
}

void os_battery_get(os_battery_info_t *out)
{
    uint32_t st = atomic_load(&s_state);
    out->known = st & 1;
    out->usb = st >> 1 & 1;
    out->pct = (int)(st >> 2) - 1;
    out->mv = atomic_load(&s_mv);
    out->raw_mv = atomic_load(&s_raw);
}

int os_battery_pct(void)
{
    os_battery_info_t b;
    os_battery_get(&b);
    return !b.known ? -1 : b.usb ? 100 : b.pct;
}

bool os_battery_usb(void)
{
    os_battery_info_t b;
    os_battery_get(&b);
    return !b.known || b.usb;
}

bool os_battery_below(int pct)
{
    os_battery_info_t b;
    os_battery_get(&b);
    return b.known && !b.usb && b.pct >= 0 && b.pct < pct;
}

void os_battery_text(char *buf, int n)
{
    os_battery_info_t b;
    os_battery_get(&b);
    if (!b.known || (!b.usb && b.pct < 0)) {
        snprintf(buf, n, "-- (no reading)");
    } else if (b.usb) {
        snprintf(buf, n, "USB (VSYS %d mV)", b.raw_mv);
    } else {
        snprintf(buf, n, "%d %%, %d mV (now %d)", b.pct, b.mv, b.raw_mv);
    }
}

// ---------------------------------------------------------------------------
// The logger

static int led_sum(void)
{
    int sum = 0;
    for (int i = 0; i < BOARD_RGB_COUNT; i++) {
        uint32_t c = bsp_rgb_get(i);
        sum += (c >> 16 & 0xFF) + (c >> 8 & 0xFF) + (c & 0xFF);
    }
    return sum;
}

static bool card_ok(void)
{
    bsp_sd_info_t sd;
    bsp_sd_get_info(&sd);
    return sd.state == BSP_SD_MOUNTED;
}

static void log_line(int64_t now_s, int raw)
{
    if (!card_ok()) {
        return;
    }
    struct stat st;
    bool fresh = stat(LOG_PATH, &st) != 0;
    if (!fresh && st.st_size > LOG_MAX_BYTES) {
        s_log = false;
        printf("batt: %s is full, logging stopped\n", LOG_PATH);
        return;
    }
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) {
        return;
    }
    if (fresh) {
        fputs(LOG_HEAD, f);
    }
    time_t t = time(NULL);
    fprintf(f, "%lld,%lld,%d,%d,%d,%d,%d,%d,%d\n", (long long)now_s, (long long)(t > 1600000000 ? t : 0), raw,
            batt_filter_mv(&s_f), s_f.usb ? -1 : s_f.shown, s_f.usb, (int)os_wifi_state(),
            os_setting(OS_SET_BRIGHTNESS), led_sum());
    fclose(f);
}

void os_battery_tick(void)
{
    int raw = bsp_battery_mv();
    batt_filter_step(&s_f, raw);
    publish(raw);
    bool start = s_log && !s_logging;
    s_logging = s_log;
    if (s_logging) {
        int64_t now_s = esp_timer_get_time() / 1000000;
        if (start || now_s - s_log_at >= s_log_every || s_f.usb != s_log_usb) {
            s_log_at = now_s;
            s_log_usb = s_f.usb;
            log_line(now_s, raw);
        }
    }
}

// ---------------------------------------------------------------------------
// `batt`

static void print_state(void)
{
    os_battery_info_t b;
    os_battery_get(&b);
    if (!b.known) {
        printf("battery: no reading (sense %d mV)\n", b.raw_mv);
    } else if (b.usb) {
        printf("battery: USB power (VSYS %d mV): the cell can't be read while plugged in\n", b.raw_mv);
    } else {
        printf("battery: %d %% (filtered %d mV, last reading %d mV; %d %% begins at %d mV)\n", b.pct, b.mv,
               b.raw_mv, b.pct, batt_curve_mv(b.pct));
    }
    printf("log: %s, every %d s, %s\n", s_log ? "on" : "off", s_log_every, LOG_PATH);
}

typedef struct {
    int n, min, max;
    double sum, sq;
} stat_t;

static void stat_add(stat_t *s, int v)
{
    if (!s->n || v < s->min) {
        s->min = v;
    }
    if (!s->n || v > s->max) {
        s->max = v;
    }
    s->n++;
    s->sum += v;
    s->sq += (double)v * v;
}

static void stat_print(const char *what, const stat_t *s)
{
    double mean = s->n ? s->sum / s->n : 0;
    double var = s->n ? s->sq / s->n - mean * mean : 0;
    printf("%-9s n=%d mean=%.1f sd=%.1f min=%d max=%d (spread %d mV)\n", what, s->n, mean, var > 0 ? sqrt(var) : 0,
           s->min, s->max, s->max - s->min);
}

// `batt noise [s]`: single readings and bsp_battery_mv() bursts every 100 ms.
static void noise(int seconds)
{
    stat_t single = {0}, burst = {0};
    for (int k = 0; k < seconds * 10; k++) {   // counted, not timed: the emulator's console doesn't wait in vTaskDelay
        int v[16];
        if (bsp_battery_read(v, 16) == 16) {
            for (int i = 0; i < 16; i++) {
                stat_add(&single, v[i]);
            }
        }
        int mv = bsp_battery_mv();
        if (mv >= 0) {
            stat_add(&burst, mv);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    stat_print("single", &single);
    stat_print("burst", &burst);
}

static void log_print(int last)
{
    FILE *f = fopen(LOG_PATH, "r");
    if (!f) {
        printf("no %s\n", LOG_PATH);
        return;
    }
    // Count lines, then print the last `last` (0: all).
    int lines = 0, c;
    while ((c = fgetc(f)) != EOF) {
        lines += c == '\n';
    }
    int skip = last > 0 && lines > last + 1 ? lines - last : 1;     // keep the header out of the skip
    rewind(f);
    char line[96];
    int i = 0;
    while (fgets(line, sizeof(line), f)) {
        if (i == 0 || i >= skip) {
            fputs(line, stdout);
        }
        i++;
    }
    fclose(f);
}

static int cmd_batt(int argc, char **argv)
{
    if (argc < 2) {
        print_state();
        return 0;
    }
    if (!strcmp(argv[1], "noise")) {
        int s = argc > 2 ? atoi(argv[2]) : 10;
        noise(s < 1 ? 1 : s > 120 ? 120 : s);
        return 0;
    }
    if (!strcmp(argv[1], "log")) {
        if (argc < 3) {
            log_print(0);
        } else if (!strcmp(argv[2], "on")) {
            int every = argc > 3 ? atoi(argv[3]) : 60;
            s_log_every = every < 5 ? 5 : every > 3600 ? 3600 : every;
            s_log = true;           // the OS task's next tick writes the first line
            printf("logging every %d s to %s (%s)\n", s_log_every, LOG_PATH, card_ok() ? "card ready" : "NO CARD");
        } else if (!strcmp(argv[2], "off")) {
            s_log = false;
            printf("logging off\n");
        } else if (!strcmp(argv[2], "tail")) {
            log_print(argc > 3 ? atoi(argv[3]) : 20);
        } else if (!strcmp(argv[2], "clear")) {
            printf("%s\n", remove(LOG_PATH) == 0 ? "removed" : "nothing to remove");
        } else {
            printf("batt log [on [seconds]|off|tail [n]|clear]\n");
        }
        return 0;
    }
    printf("batt [noise [s]|log [on [seconds]|off|tail [n]|clear]]\n");
    return 1;
}

void os_battery_init(void)
{
    batt_filter_init(&s_f);
    os_battery_tick();
    os_cmd_register("batt", "batt [noise [s]|log [on [s]|off|tail [n]|clear]] - battery reading, noise, discharge log",
                    cmd_batt);
}
