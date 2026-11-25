// Emulated battery: a 1000 mAh LiPo that starts fairly full and drains slowly
// while the page is open, with a little ADC noise. Plausible, not measured.
// The percentage is core/os_battery.c's, as on the badge.

#include "esp_random.h"
#include "esp_timer.h"
#include "bsp.h"

static bool s_ready;

esp_err_t bsp_battery_init(void)
{
    s_ready = true;
    return ESP_OK;
}

static int one(void)
{
    int minutes = (int)(esp_timer_get_time() / 60000000LL);
    int mv = 4040 - minutes * 3;            // ~180 mV per hour of use
    if (mv < 3620) {
        mv = 3620;
    }
    return mv + (int)(esp_random() % 9) - 4;
}

int bsp_battery_read(int *out, int n)
{
    if (!s_ready) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        out[i] = one();
    }
    return n;
}

int bsp_battery_mv(void)
{
    return s_ready ? one() : -1;
}
