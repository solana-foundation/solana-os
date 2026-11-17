// Wi-Fi at boot (docs/HHGTS.md "Badge behaviour"): with a saved network the
// badge tries to join it for 30 s, then turns Wi-Fi off again, so a badge away
// from its network neither keeps scanning nor keeps the radio off the mesh
// channel. With older saved networks too, a most recent one that isn't found
// (it gave up, or 30 s went by) makes way for the most recent older one a scan
// finds, which gets its own 30 s. Portable: only the os_wifi API (and
// os_mesh_held), from a task that ends once it has joined or given up; it
// outlives the boot only while waiting for a mesh app to close.

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "os_mesh.h"
#include "os_wifi.h"

static const char *TAG = "wifi_boot";

#define BOOT_JOIN_MS    30000
#define SCAN_MS         15000       // bound on the dual-band scan for an older network
#define SCAN_MAX        32

static uint32_t s_mine;     // os_wifi_changes() right after this task's own last call

static bool past(TickType_t start, int ms)
{
    return xTaskGetTickCount() - start >= pdMS_TO_TICKS(ms);
}

// Someone else (the user, a Lua app's wifi.on()/off(), a bridge) drove Wi-Fi
// since: what happens next is theirs, even if Wi-Fi is off again.
static bool taken(void)
{
    return os_wifi_changes() != s_mine;
}

// With Wi-Fi off, a mesh app opening doesn't pause it: scanning or joining then
// would take the app's radio. Go on once it closes, as a resume would. False:
// someone else drove Wi-Fi meanwhile.
static bool wait_for_mesh_apps(void)
{
    while (!taken() && os_mesh_held()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return !taken();
}

// The most recent saved network isn't here: Wi-Fi off (the driver doesn't scan
// while it joins), scan both bands and join the most recent older saved network
// in range. True: joining it, and ssid/pass now name it. False: Wi-Fi stays off
// (none in range), or someone else drove Wi-Fi meanwhile.
static bool join_older(char *ssid, char *pass)
{
    os_wifi_set_enabled(false);
    s_mine = os_wifi_changes();
    wifi_ap_record_t *recs = malloc(SCAN_MAX * sizeof(*recs));      // the task's stack is small
    if (!recs) {
        return false;
    }
    TickType_t start = xTaskGetTickCount();
    bool scanning = false;
    int n = 0;
    while (!scanning && !past(start, SCAN_MS) && !taken()) {
        if (os_mesh_held()) {
            if (!wait_for_mesh_apps()) {
                free(recs);
                return false;
            }
            start = xTaskGetTickCount();
        }
        scanning = os_wifi_scan_start() == ESP_OK;
        if (!scanning) {
            vTaskDelay(pdMS_TO_TICKS(250));     // another scan runs, or the radio is lent out
        }
    }
    // Even if someone takes over meanwhile, let the scan end and read it: that
    // frees the results os_wifi keeps for whoever started the scan.
    while (scanning && !os_wifi_scan_done() && !past(start, SCAN_MS)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (scanning && os_wifi_scan_done()) {
        n = os_wifi_scan_results(recs, SCAN_MAX);
        ESP_LOGI(TAG, "scan: %d access points in %d ms", n, (int)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));
    } else if (!taken()) {
        ESP_LOGW(TAG, "no scan within %d s", SCAN_MS / 1000);
    }
    if (taken()) {
        free(recs);
        return false;
    }
    char s[33], p[65];
    bool found = false;
    for (int i = 1; !found && os_wifi_saved_get(i, s, sizeof(s), p, sizeof(p)); i++) {
        for (int j = 0; j < n && !found; j++) {
            found = !strcmp((const char *)recs[j].ssid, s);
        }
    }
    free(recs);
    if (!found) {
        ESP_LOGI(TAG, "no older saved network in range: Wi-Fi off");
        return false;
    }
    if (!wait_for_mesh_apps()) {
        return false;
    }
    strlcpy(ssid, s, 33);
    strlcpy(pass, p, 65);
    ESP_LOGI(TAG, "looking for '%s'", ssid);
    os_wifi_connect(ssid, pass);
    return true;
}

static void boot_join_task(void *arg)
{
    char ssid[33] = "", pass[65] = "";
    os_wifi_saved_get(0, ssid, sizeof(ssid), pass, sizeof(pass));
    bool older = os_wifi_saved_count() > 1;
    os_wifi_info_t in;
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        os_wifi_get_info(&in);
        if (in.state == OS_WIFI_CONNECTED || !in.enabled || strcmp(in.ssid, ssid)) {
            break;          // joined, or the user turned it off or picked another network
        }
        bool late = past(start, BOOT_JOIN_MS);
        if (older && in.state != OS_WIFI_SUSPENDED && (late || in.state == OS_WIFI_FAILED)) {
            older = false;
            ESP_LOGI(TAG, "'%s' not found: looking for an older saved network", ssid);
            if (!join_older(ssid, pass)) {
                break;
            }
            start = xTaskGetTickCount();
            continue;
        }
        if (late && in.state != OS_WIFI_SUSPENDED) {
            ESP_LOGI(TAG, "'%s' not found in %d s: Wi-Fi off", ssid, BOOT_JOIN_MS / 1000);
            os_wifi_set_enabled(false);
            break;
        }
        if (late) {
            break;          // lent to a mesh app: resuming rejoins, as after any join
        }
        // Gave up early: keep listening until 30 s, unless a mesh app opened
        // (it doesn't pause a Wi-Fi that gave up, so a retry would take its radio).
        if (in.state == OS_WIFI_FAILED && !os_mesh_held()) {
            os_wifi_connect(ssid, pass);
        }
    }
    vTaskDelete(NULL);
}

void os_wifi_boot(void)
{
    char ssid[33];
    if (!os_wifi_saved_ssid(ssid, sizeof(ssid))) {
        return;             // nothing saved: Wi-Fi stays off until the user joins one
    }
    ESP_LOGI(TAG, "looking for '%s'", ssid);
    os_wifi_set_enabled(true);
    if (xTaskCreate(boot_join_task, "wifi_boot", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no task: Wi-Fi stays on");
    }
}
