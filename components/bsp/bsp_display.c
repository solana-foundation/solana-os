// ILI9341 on the shared SPI bus, driven by LVGL through esp_lvgl_port.

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp.h"
#include "bsp_bus.h"

static const char *TAG = "bsp_display";
static SemaphoreHandle_t s_lcd_idle;    // see bsp_bus.h

void bsp_lcd_hold(void)
{
    if (s_lcd_idle) {
        xSemaphoreTake(s_lcd_idle, portMAX_DELAY);
    }
}

void bsp_lcd_release(void)
{
    if (s_lcd_idle) {
        xSemaphoreGive(s_lcd_idle);
    }
}

static void flush_start(lv_event_t *e)
{
    bsp_lcd_hold();
}

// Replaces esp_lvgl_port's own callback, which only calls lv_display_flush_ready()
// (esp_lcd logs a warning that it was overwritten).
static bool flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    lv_display_flush_ready(ctx);
    xSemaphoreGiveFromISR(s_lcd_idle, &woken);
    return woken == pdTRUE;
}

// The backlight pin is PWM'd by LEDC so the screen can dim (Settings >
// Brightness). 5 kHz is far above visible flicker and slow enough for the
// backlight's switch transistor; 10 bits leave fine steps at the dim end.
#define BL_MODE     LEDC_LOW_SPEED_MODE
#define BL_TIMER    LEDC_TIMER_0
#define BL_CHANNEL  LEDC_CHANNEL_0
#define BL_BITS     LEDC_TIMER_10_BIT
#define BL_MAX      ((1u << 10) - 1)

static uint8_t s_bl_pct = 100;  // the level "on" means
static bool s_bl_on;
static bool s_bl_ready;

static void bl_apply(void)
{
    if (!s_bl_ready) {
        return;
    }
    uint32_t duty = s_bl_on ? (BL_MAX * s_bl_pct + 50) / 100 : 0;
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

static void bl_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BL_MODE,
        .duty_resolution = BL_BITS,
        .timer_num = BL_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const ledc_channel_config_t ch = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = BL_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,      // dark until the panel has been initialised
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    s_bl_ready = true;
}

void bsp_backlight(bool on)
{
    s_bl_on = on;
    bl_apply();
}

void bsp_backlight_level(uint8_t pct)
{
    s_bl_pct = pct < 1 ? 1 : pct > 100 ? 100 : pct;
    bl_apply();
}

uint8_t bsp_backlight_get_level(void)
{
    return s_bl_pct;
}

lv_display_t *bsp_display_start(void)
{
    bl_init();
    ESP_ERROR_CHECK(bsp_spi_bus_init());

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = BOARD_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    // Production panels show true colours without inversion; with it, RED reads on
    // cyan and GREEN on magenta. (The prototype's panel was the other way round.)
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;     // deep object trees (the Guide's pack reveal) left ~600 B of 7 KB
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        // 2 x 7.5 KB of DMA memory (12 rows; 24 rows took 30 KB, 0.5.13). The
        // second buffer renders while the first flushes, so a full frame is
        // 20 flushes of ~3 ms at 20 MHz instead of 10 of ~6 ms.
        .buffer_size = BOARD_LCD_H_RES * 12,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .rotation = { .swap_xy = true, .mirror_x = true, .mirror_y = true },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp, NULL, TAG, "lvgl display");
    s_lcd_idle = xSemaphoreCreateBinary();
    xSemaphoreGive(s_lcd_idle);
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp));
    lv_display_add_event_cb(disp, flush_start, LV_EVENT_FLUSH_START, NULL);
    bsp_backlight(true);
    ESP_LOGI(TAG, "LCD %dx%d ready", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return disp;
}
