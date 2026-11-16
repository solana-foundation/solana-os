#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "board.h"

// ---- Shared SPI bus ----
esp_err_t bsp_spi_bus_init(void);

// ---- microSD ----
#define BSP_SD_MOUNT "/sdcard"

typedef enum {
    BSP_SD_NOT_MOUNTED,
    BSP_SD_MOUNTED,
    BSP_SD_NO_CARD,     // no response to CMD0
    BSP_SD_BAD_FS,      // card works but has no FAT volume ESP-IDF can mount
    BSP_SD_ERROR,       // card responded incorrectly (reinsert it)
    BSP_SD_INCOMPATIBLE,// card obeys commands while deselected: unsafe on a shared bus
} bsp_sd_state_t;

typedef struct {
    bsp_sd_state_t state;
    esp_err_t last_err;
    char name[8];
    const char *type;   // "SDSC", "SDHC/SDXC"
    uint32_t card_mb;
    uint64_t total_bytes;
    uint64_t free_bytes;
} bsp_sd_info_t;

esp_err_t bsp_sd_mount(void);
esp_err_t bsp_sd_unmount(void);
esp_err_t bsp_sd_format(void);
void bsp_sd_get_info(bsp_sd_info_t *out);  // cheap; usage figures are cached
void bsp_sd_refresh_usage(void);
const char *bsp_sd_state_str(bsp_sd_state_t s);
void bsp_sd_print_card(void);

// ---- Display (ILI9341 + LVGL) ----
lv_display_t *bsp_display_start(void);
void bsp_backlight(bool on);
// Backlight level while on, 1..100 % PWM duty (default 100). Safe before or
// after bsp_display_start(); the level is kept across off/on.
void bsp_backlight_level(uint8_t pct);
uint8_t bsp_backlight_get_level(void);
// Colour inversion. Production panels need it (on, what bsp_display_start()
// sets); the prototypes' panels (rev 1.2 C5 modules, like HYQ5VS) show RED on
// cyan with it, and nothing on the bus tells the two apart: same ILI9341, same
// registers. The OS applies the user's setting (Settings > Invert colours).
// Any task but the LVGL task's flush path.
void bsp_display_invert(bool on);
bool bsp_display_inverted(void);

// An app's frame straight to the panel, past LVGL (Solana OS 2.3.0: C apps'
// ui_frame). w x h RGB565 pixels, `stride` pixels a row, little-endian or
// (be) big-endian as the panel takes them, scaled (nearest) to dw x dh at
// screen position (x, y) and clipped to the screen; the caller has checked
// the pixels are its to read. Hold the LVGL lock. Returns once the transfer is
// queued: it finishes in the background, holding the LCD meanwhile
// (bsp_bus.h), so the caller computes its next frame while the panel takes
// this one. The pixels are converted into a staging buffer in PSRAM first,
// except a big-endian frame of whole rows, unscaled and unclipped, in a
// 64-byte-aligned buffer of whole 64-byte lines: the DMA reads that one where
// it is, so the caller must not free or move it before the transfer is done
// (bsp_display_frame_wait). LVGL's next redraw of the area paints over the
// frame. bsp_display_frame_end() waits and frees the staging buffer.
esp_err_t bsp_display_frame(const uint16_t *px, int stride, int w, int h, int x, int y, int dw, int dh, bool be);
// Whether bsp_display_frame takes that frame from where it is (no copy), as the
// one rule its caller charges by and it draws by (the emulator copies every
// frame but charges the same).
#define BSP_FRAME_ALIGN 64              // the cache line: DMA from PSRAM needs whole lines
static inline bool bsp_display_frame_no_copy(const uint16_t *px, int stride, int w, int h, int x, int y,
                                             int dw, int dh, bool be)
{
    size_t bytes = (size_t)w * h * 2;
    return be && dw == w && dh == h && stride == w && x >= 0 && y >= 0 && x + w <= BOARD_LCD_H_RES &&
           y + h <= BOARD_LCD_V_RES && !((uintptr_t)px % BSP_FRAME_ALIGN) && !(bytes % BSP_FRAME_ALIGN);
}
int bsp_display_frame_direct(void);     // 1: the last frame went from the caller's buffer (no copy)
void bsp_display_frame_wait(void);      // the last frame's transfer is done
void bsp_display_frame_end(void);
// The last frame as it is on the panel (big-endian RGB565, w x h at x, y), for
// screenshots; NULL when there is none or none came in the last 0.5 s (the
// app stopped drawing them). Hold the LVGL lock.
const uint16_t *bsp_display_frame_last(int *x, int *y, int *w, int *h);

// ---- Buttons ----
#define BSP_BTN_PB(n)   (1u << ((n) - 1))   // n = 1..6
#define BSP_BTN_BOOT    (1u << 6)
typedef void (*bsp_btn_cb_t)(uint32_t pressed, uint32_t changed);
esp_err_t bsp_buttons_start(bsp_btn_cb_t cb);
uint32_t bsp_buttons_get(void);

// ---- RGB LEDs ----
esp_err_t bsp_rgb_init(void);
void bsp_rgb_set(int index, uint8_t r, uint8_t g, uint8_t b);
void bsp_rgb_fill(uint8_t r, uint8_t g, uint8_t b);
void bsp_rgb_show(void);
uint32_t bsp_rgb_get(int index);    // 0xRRGGBB

// ---- Battery ----
esp_err_t bsp_battery_init(void);
int bsp_battery_mv(void);           // -1 on error
int bsp_battery_percent(int mv);
