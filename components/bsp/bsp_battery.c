// Battery sense on GPIO1 (ADC1, 12 dB): a 22k/22k divider from VSYS, so the
// pin reads half of VSYS. VSYS is VBUS through a Schottky on USB (~4.6-4.9 V)
// and the cell through the power path otherwise (main/core/battery_calc.h
// says what that means for the percentage, which the OS works out, not the
// BSP).

#include <stdlib.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp.h"

static const char *TAG = "bsp_battery";

#define BURST       16                  // readings per bsp_battery_mv()
#define TRIM        4                   // dropped at each end before averaging

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static SemaphoreHandle_t s_mx;          // the OS task and the console both read

esp_err_t bsp_battery_init(void)
{
    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BOARD_BAT_ADC_GPIO, &unit, &s_chan), TAG, "io");
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = unit };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "unit");
    adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg), TAG, "chan");
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali), TAG, "cali");
    s_mx = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mx, ESP_ERR_NO_MEM, TAG, "mutex");
    return ESP_OK;
}

int bsp_battery_read(int *out, int n)
{
    if (!s_adc || !s_mx) {
        return -1;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int got = 0;
    for (; got < n; got++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK ||
                adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
            break;
        }
        out[got] = mv * BOARD_BAT_DIVIDER;
    }
    xSemaphoreGive(s_mx);
    return got == n ? n : -1;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

int bsp_battery_mv(void)
{
    int v[BURST];
    if (bsp_battery_read(v, BURST) != BURST) {
        return -1;
    }
    qsort(v, BURST, sizeof(v[0]), cmp_int);
    int sum = 0;
    for (int i = TRIM; i < BURST - TRIM; i++) {
        sum += v[i];
    }
    return sum / (BURST - 2 * TRIM);
}
