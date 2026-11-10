// Button box: a Bluetooth LE gadget for the Solana BP26 badge (README.md).
//
// One service, 0xFFE0, with one characteristic, 0xFFE1, that a badge (or a
// phone) can read and subscribe to. Its value is two bytes:
//
//   byte 1  1 while the button is held, 0 when it is up
//   byte 2  presses since power-on, counting 0..255 and round again
//
// Every change is pushed to the connected device if it subscribed. The box
// advertises the service (so a badge can scan for 0xFFE0 only) and puts its
// name in the scan response. One connection at a time, no pairing: the badge
// does neither, and anyone in range may read a button.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "box";

#define SVC_UUID 0xFFE0
#define CHR_UUID 0xFFE1

static uint8_t s_value[2];                  // {held, presses}
static uint16_t s_chr_handle;               // filled in by ble_gatts_add_svcs
static uint8_t s_own_addr_type;

static void advertise(void);

// A device reads the characteristic: answer with the current value.
static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, s_value, sizeof s_value) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(CHR_UUID),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_chr_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect %s", ev->connect.status == 0 ? "ok" : "failed");
        if (ev->connect.status != 0) advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        advertise();                        // be found again
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "notifications %s", ev->subscribe.cur_notify ? "on" : "off");
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    // The advertisement: flags and the service, so scans filtered by 0xFFE0 find it.
    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(SVC_UUID) };
    adv.num_uuids16 = 1;
    adv.uuids16_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) { ESP_LOGE(TAG, "adv fields: %d", rc); return; }

    // The scan response: the name, for active scanners.
    struct ble_hs_adv_fields rsp = { 0 };
    const char *name = ble_svc_gap_device_name();
    rsp.name = (const uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) { ESP_LOGE(TAG, "scan response: %d", rc); return; }

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;   // connectable
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;   // discoverable
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start: %d", rc);
    else ESP_LOGI(TAG, "advertising as \"%s\"", name);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();                      // returns only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

// Polls the button every 10 ms; a change has to last two polls (debounce).
static void button_task(void *arg)
{
    const gpio_num_t pin = CONFIG_BOX_BUTTON_GPIO;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    int last = 0, seen = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int held = gpio_get_level(pin) == 0;
        if (held != seen) { seen = held; continue; }    // wait for it to settle
        if (held == last) continue;
        last = held;
        s_value[0] = held;
        if (held) s_value[1]++;
        ESP_LOGI(TAG, "button %s (%u presses)", held ? "down" : "up", s_value[1]);
        ble_gatts_chr_updated(s_chr_handle);         // notifies a subscribed device
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();       // the Bluetooth controller keeps calibration in NVS
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_services) == 0 ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_services) == 0 ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(CONFIG_BOX_NAME) == 0 ? ESP_OK : ESP_FAIL);

    nimble_port_freertos_init(host_task);
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
}
