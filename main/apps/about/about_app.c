// About: Solana OS version, badge identity, credits.

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "core/os.h"
#include "core/os_cloud.h"
#include "core/ui.h"
#include "core/ui_stream.h"

// The Streaming theme's About: a banner with the logo on a tile, then facts.
static void enter_stream(lv_obj_t *root, const char *id)
{
    const esp_app_desc_t *d = esp_app_get_description();
    ui_stream_glow(root, 240, 50, 190, 150, 0xE5383B);
    lv_obj_t *tile = lv_obj_create(root);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, 92, 92);
    lv_obj_set_pos(tile, OS_ROOT_W - 34 - 92, 4);
    lv_obj_set_style_radius(tile, 22, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x2A2A30), 0);
    lv_obj_set_style_bg_grad_color(tile, lv_color_hex(0x0E0E11), 0);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(0x45454C), 0);
    lv_obj_t *logo = lv_image_create(tile);
    lv_image_set_src(logo, &img_logomark_72);
    lv_image_set_scale(logo, 200);
    lv_obj_center(logo);

    lv_obj_t *tick = lv_obj_create(root);
    lv_obj_remove_style_all(tick);
    lv_obj_set_pos(tick, 14, 20);
    lv_obj_set_size(tick, 2, 9);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tick, ui_accent(), 0);
    lv_obj_t *l = ui_stream_caps_label(root, "BREAKPOINT 2026 BADGE");
    lv_obj_set_pos(l, 21, 20);
    l = ui_stream_display(root, 30, "Solana OS");
    lv_obj_set_pos(l, 14, 36);

    lv_obj_t *meta = lv_obj_create(root);
    lv_obj_remove_style_all(meta);
    lv_obj_set_pos(meta, 14, 72);
    lv_obj_set_size(meta, 200, 14);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(meta, 8, 0);
    char ver[24];
    snprintf(ver, sizeof(ver), "v%.16s", d->version);
    ver[strcspn(ver, "-+ ")] = 0;
    ui_label(meta, &stream_bold_11, COL_GREEN, ver);
    l = ui_stream_caps_label(meta, "ESP32-C5");
    lv_obj_set_style_text_letter_space(l, 0, 0);
    lv_obj_set_style_border_width(l, 1, 0);
    lv_obj_set_style_border_color(l, lv_color_white(), 0);
    lv_obj_set_style_border_opa(l, 140, 0);
    lv_obj_set_style_radius(l, 2, 0);
    lv_obj_set_style_pad_hor(l, 4, 0);
    lv_obj_set_style_pad_ver(l, 1, 0);
    l = ui_label(meta, &pearl_font_11, COL_DIM, "");
    lv_label_set_text_fmt(l, "%d apps", os_app_count);

    ui_stream_row(root, 112, "Built", d->date, 0);
    lv_obj_t *v = ui_stream_row(root, 139, "Badge", id, 0);
    lv_obj_set_style_text_font(v, &pearl_mono_9, 0);
    ui_stream_row(root, 166, "Radio", "Wi-Fi 6, 2.4 and 5 GHz, ESP-NOW", 0);
}

static void enter(lv_obj_t *root)
{
    // The server's badge ID once it has one, else the end of the MAC.
    char id[16];
    os_cloud_info_t *in = lv_malloc(sizeof(*in));
    if (in) {
        os_cloud_get_info(in);
        snprintf(id, sizeof(id), "%s", in->id);
        lv_free(in);
    } else {
        id[0] = 0;
    }
    if (!id[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(id, sizeof(id), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    }
    if (ui_stream()) {
        enter_stream(root, id);
        return;
    }
    lv_obj_t *logo = lv_image_create(root);
    lv_image_set_src(logo, &img_logomark_40);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *name = ui_label(root, &sol_font_20, COL_TEXT, "Solana OS");
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 60);

    const esp_app_desc_t *d = esp_app_get_description();
    lv_obj_t *ver = ui_label(root, &sol_mono_12, COL_DIM, "");
    lv_label_set_text_fmt(ver, "v%s  built %s\nbadge %s  " LV_SYMBOL_BULLET "  %d apps installed",
                          d->version, d->date, id, os_app_count);
    lv_obj_set_style_text_align(ver, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 88);

    ui_gradient_bar(root, 90, 126, OS_ROOT_W - 180, 3);

    lv_obj_t *blurb = ui_label(root, &sol_mono_12, COL_TEXT, "ESP32-C5 " LV_SYMBOL_BULLET " Wi-Fi 6 2.4/5 GHz "
                               LV_SYMBOL_BULLET " ESP-NOW");
    lv_obj_align(blurb, LV_ALIGN_TOP_MID, 0, 138);

    lv_obj_t *foundation = lv_image_create(root);
    lv_image_set_src(foundation, ui_light() ? &img_foundation_black : &img_foundation);
    lv_obj_align(foundation, LV_ALIGN_BOTTOM_MID, 0, -UI_HINT_H - 12);

    ui_hint(root, "CANCEL exit");
}

const os_app_t app_about = {
    .id = "about",
    .name = "About",
    .icon = &img_logomark_40,
    .accent = COL_GREEN,
    .on_enter = enter,
};
