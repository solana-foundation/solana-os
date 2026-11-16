// ILI9341 on the shared SPI bus, driven by LVGL through esp_lvgl_port.

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lvgl_port.h"
#include "display/lv_display_private.h"     // the dirty-area list, for merge_dirty()
#include "misc/lv_area_private.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp.h"
#include "bsp_bus.h"

static const char *TAG = "bsp_display";
static SemaphoreHandle_t s_lcd_idle;    // see bsp_bus.h
static esp_lcd_panel_handle_t s_panel;
static bool s_inverted;

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

// LVGL renders and flushes every dirty area on its own, and each one costs a
// walk of every object on the screen plus the panel's window commands: about
// 1 ms with 120 objects (0.2 ms of it the commands), whatever its size. Drawn
// one by one, 40 moving sprites took as long as the whole screen (46 ms) and
// 120 three times that. Here, after LVGL joined the areas that overlap, areas
// are merged as long as the pixels between them cost less than the area saved:
// sprites on one row become one strip. (LV_INV_BUF_SIZE, CMakeLists.txt, is
// 256 so that the list doesn't overflow into a full redraw before this runs.)
#define MERGE_AREA_PX   1200    // an area costs about what drawing this many pixels does
#define MERGE_PASSES    4       // each pass is n^2 / 2 cheap checks

static void merge_dirty(lv_event_t *e)
{
    lv_display_t *d = lv_event_get_user_data(e);
    for (int pass = 0, merged = 1; merged && pass < MERGE_PASSES; pass++) {
        merged = 0;
        for (uint32_t i = 0; i < d->inv_p; i++) {
            if (d->inv_area_joined[i]) {
                continue;
            }
            lv_area_t *a = &d->inv_areas[i];
            uint32_t a_px = lv_area_get_size(a);
            for (uint32_t j = i + 1; j < d->inv_p; j++) {
                if (d->inv_area_joined[j]) {
                    continue;
                }
                lv_area_t both;
                lv_area_join(&both, a, &d->inv_areas[j]);
                if (lv_area_get_size(&both) < a_px + lv_area_get_size(&d->inv_areas[j]) + MERGE_AREA_PX) {
                    *a = both;
                    a_px = lv_area_get_size(a);
                    d->inv_area_joined[j] = 1;
                    merged = 1;
                }
            }
        }
    }
}

// Replaces esp_lvgl_port's own callback, which only calls lv_display_flush_ready()
// (esp_lcd logs a warning that it was overwritten).
static volatile bool s_frame_busy;      // the transfer in flight is bsp_display_frame's, not LVGL's

static bool flush_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_frame_busy) {
        // Not LVGL's: telling it a flush is done would let it render into a
        // buffer its own transfer still reads (bsp_display_frame below).
        s_frame_busy = false;
        xSemaphoreGiveFromISR(s_lcd_idle, &woken);
        return woken == pdTRUE;
    }
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
        // ui_frame's colour buffers are in PSRAM: the DMA reads them there, as
        // 6.0.1 did on its own. Without it master's spi_master copies each one
        // into an SRAM bounce buffer (bsp_display_frame's no-copy path, 2.3.0).
        // A frame's DMA may still be reading PSRAM when another task writes the
        // flash (PSRAM is unreachable with the cache off). While an app can draw,
        // the only flash writes are NVS commits (settings, Wi-Fi, the server link,
        // Collaborator): an update holds the screen and a store-code slot is copied
        // before its app runs. Measured on a prototype (2.3.0): 1 200 frames back to
        // back under 16 NVS commits, no DMA error, hang or restart; at worst one
        // frame could show wrong pixels until the next (tests/c/t-c-fstress).
        .flags.psram_dma_direct = 1,
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
    // Production panels need inversion; without it RED reads on cyan and GREEN on
    // magenta. The prototypes' panels are the other way round (bsp.h).
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    s_inverted = true;
    s_panel = panel;
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;     // deep object trees (the Guide's pack reveal) left ~600 B of 7 KB
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        // 2 x 7.5 KB of DMA memory (12 rows; 24 rows took 30 KB, 0.5.13). The
        // second buffer renders while the first flushes, so a full frame is
        // 20 flushes of ~1.5 ms at 40 MHz instead of 10 of ~3 ms.
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
    lv_display_add_event_cb(disp, merge_dirty, LV_EVENT_RENDER_START, disp);
    bsp_backlight(true);
    ESP_LOGI(TAG, "LCD %dx%d ready", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return disp;
}

void bsp_display_invert(bool on)
{
    if (!s_panel || on == s_inverted) {
        return;
    }
    // The LVGL lock keeps the LVGL task from queueing a flush meanwhile (esp_lcd's
    // in-flight count isn't shared safely), and holding the LCD waits for the one
    // in flight and keeps the card off the bus (bsp_bus.h). The lock is recursive,
    // so callers that hold it already (app callbacks) are fine.
    lvgl_port_lock(0);
    bsp_lcd_hold();
    esp_err_t err = esp_lcd_panel_invert_color(s_panel, on);
    bsp_lcd_release();
    lvgl_port_unlock();
    if (err == ESP_OK) {
        s_inverted = on;
    } else {
        ESP_LOGW(TAG, "invert: %s", esp_err_to_name(err));
    }
}

bool bsp_display_inverted(void)
{
    return s_inverted;
}

// ---------------------------------------------------------------------------
// Frames past LVGL (bsp.h)
//
// LVGL's own path for a full app frame is a canvas: the app's pixels copied
// into the canvas, then rendered strip by strip into the 2 x 7.5 KB draw
// buffers (through the canvas's image decoder) and flushed, all on the LVGL
// task after the app's callback has returned, so the app waits for every bit
// of it. Here the pixels are byte-swapped (the panel takes RGB565 big-endian)
// and scaled into one staging buffer, and the SPI DMA reads it from PSRAM
// (SOC_PSRAM_DMA_CAPABLE; the cache is written back first) while the app goes
// on. The staging buffer is kept from the first frame until
// bsp_display_frame_end(): a frame is at most the screen, 150 KB of PSRAM.

#define FRAME_ALIGN 64                  // the cache line: DMA from PSRAM needs whole lines

static uint16_t *s_stage;               // big-endian pixels of the last frame
static size_t s_stage_cap;
static int s_fx, s_fy, s_fw, s_fh;      // where the last frame is
static int64_t s_frame_us;              // when it was drawn
static int s_direct;                    // it went from the caller's buffer
static uint16_t s_xmap[BOARD_LCD_H_RES];

static inline uint16_t swap16(uint16_t v)
{
    return (uint16_t)(v << 8 | v >> 8);
}

int bsp_display_frame_direct(void)
{
    return s_direct;
}

esp_err_t bsp_display_frame(const uint16_t *px, int stride, int w, int h, int x, int y, int dw, int dh, bool be)
{
    if (!s_panel || !px || w <= 0 || h <= 0 || dw <= 0 || dh <= 0 || stride < w) {
        return ESP_ERR_INVALID_ARG;
    }
    // Clip the destination to the screen; sx/sy step through the source.
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + dw > BOARD_LCD_H_RES ? BOARD_LCD_H_RES : x + dw;
    int y1 = y + dh > BOARD_LCD_V_RES ? BOARD_LCD_V_RES : y + dh;
    if (x0 >= x1 || y0 >= y1) {
        return ESP_OK;
    }
    int cw = x1 - x0, ch = y1 - y0;
    size_t bytes = (size_t)cw * ch * 2;
    size_t need = (bytes + FRAME_ALIGN - 1) & ~(size_t)(FRAME_ALIGN - 1);
    bool same = dw == w && dh == h;
    bsp_lcd_hold();                     // the last frame (or LVGL's flush) is done with the buffer
    if (be && same && stride == w && cw == w && ch == h && !((uintptr_t)px % FRAME_ALIGN) && !(bytes % FRAME_ALIGN)) {
        // Sent from where it is: the cache written back, the DMA reads it.
        esp_cache_msync((void *)px, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        s_direct = 1;
        s_fw = 0;                       // nothing kept for screenshots
        s_frame_busy = true;
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, px);
        if (err != ESP_OK) {
            s_frame_busy = false;
            bsp_lcd_release();
        }
        return err;
    }
    s_direct = 0;
    if (need > s_stage_cap) {
        heap_caps_free(s_stage);
        s_stage = heap_caps_aligned_alloc(FRAME_ALIGN, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_stage_cap = s_stage ? need : 0;
        if (!s_stage) {
            bsp_lcd_release();
            return ESP_ERR_NO_MEM;
        }
    }
    if (!same) {
        for (int i = 0; i < cw; i++) {
            s_xmap[i] = (uint16_t)((int64_t)(x0 - x + i) * w / dw);
        }
    }
    uint16_t *o = s_stage;
    for (int j = 0; j < ch; j++) {
        int sy = same ? y0 - y + j : (int)((int64_t)(y0 - y + j) * h / dh);
        const uint16_t *row = px + (size_t)sy * stride;
        if (same) {
            row += x0 - x;
            int i = 0;
            if (be) {
                memcpy(o, row, (size_t)cw * 2);
                i = cw;
            } else if (!((uintptr_t)row & 3) && !((uintptr_t)o & 3)) {
                const uint32_t *r32 = (const uint32_t *)row;
                uint32_t *o32 = (uint32_t *)o;
                for (; i + 1 < cw; i += 2) {
                    uint32_t v = *r32++;
                    *o32++ = (v & 0x00ff00ffu) << 8 | (v >> 8 & 0x00ff00ffu);
                }
            }
            for (; i < cw; i++) {
                o[i] = swap16(row[i]);
            }
        } else {
            for (int i = 0; i < cw; i++) {
                uint16_t v = row[s_xmap[i]];
                o[i] = be ? v : swap16(v);
            }
        }
        o += cw;
    }
    esp_cache_msync(s_stage, need, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    s_fx = x0, s_fy = y0, s_fw = cw, s_fh = ch;
    s_frame_us = esp_timer_get_time();
    s_frame_busy = true;                // flush_done gives the LCD back once it is on the panel
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, s_stage);
    if (err != ESP_OK) {
        s_frame_busy = false;
        bsp_lcd_release();
    }
    return err;
}

void bsp_display_frame_wait(void)
{
    bsp_lcd_hold();
    bsp_lcd_release();
}

void bsp_display_frame_end(void)
{
    bsp_lcd_hold();
    heap_caps_free(s_stage);
    s_stage = NULL;
    s_stage_cap = 0;
    s_fw = s_fh = 0;
    bsp_lcd_release();
}

const uint16_t *bsp_display_frame_last(int *x, int *y, int *w, int *h)
{
    if (!s_stage || !s_fw || esp_timer_get_time() - s_frame_us > 500000) {
        return NULL;                    // none, or the app stopped drawing: LVGL has the area again
    }
    *x = s_fx, *y = s_fy, *w = s_fw, *h = s_fh;
    return s_stage;
}
