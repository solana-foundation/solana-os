// Wi-Fi at boot (docs/HHGTS.md "Badge behaviour"): with a saved network the
// badge tries to join it for 30 s, then turns Wi-Fi off again, so a badge away
// from its network neither keeps scanning nor keeps the radio off the mesh
// channel. With older saved networks too, a most recent one that isn't found
// (it gave up, or 30 s went by) makes way for the most recent older one a scan
// finds, which gets its own 30 s. Nothing it does happens while the radio is
// lent out (a mesh app is open, or Wi-Fi is suspended), and lent time doesn't
// count toward those 30 s. Portable: only the os_wifi API (and os_mesh_held),
// from a task that ends once it has joined or given up; it outlives the boot
// only while the radio stays lent out.

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

static uint32_t s_mine;     // os_wifi_changes() right after boot's own last call

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

// The radio is lent out: a mesh app is open (with Wi-Fi off or given up, its
// acquire didn't pause Wi-Fi), or Wi-Fi is suspended (a mesh app, or the
// ESP-NOW app, which suspends it directly). Scanning or joining now would take it.
static bool lent(void)
{
    return os_mesh_held() || os_wifi_state() == OS_WIFI_SUSPENDED;
}

// Go on once the radio is back, as a resume would. False: someone else drove
// Wi-Fi meanwhile. *waited (if given) is set when it had to wait.
static bool wait_for_mesh_apps(bool *waited)
{
    bool logged = false;
    while (!taken() && lent()) {
        if (!logged) {
            ESP_LOGI(TAG, "radio lent out: waiting for it");
            logged = true;
        }
        if (waited) {
            *waited = true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return !taken();
}

// Join once no mesh app is open, then check again: os_mesh_acquire counts an app
// and reads Wi-Fi under one lock, so an app that opened around the join either
// saw it (and pauses it) or shows here. Then back off (the join is still
// scanning the mesh channel) and join again once it closes. False: someone else
// drove Wi-Fi meanwhile. *waited: a mesh app had to close first.
static bool join(const char *ssid, const char *pass, bool *waited)
{
    *waited = false;
    for (;;) {
        if (!wait_for_mesh_apps(waited)) {
            return false;
        }
        os_wifi_connect(ssid, pass);
        s_mine = os_wifi_changes();
        if (!os_mesh_held()) {
            return true;
        }
        os_wifi_set_enabled(false);
        s_mine = os_wifi_changes();
    }
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
        ESP_LOGW(TAG, "no memory for a scan: Wi-Fi off");
        return false;
    }
    TickType_t start = xTaskGetTickCount();
    bool scanning = false;
    int n = 0;
    while (!scanning && !past(start, SCAN_MS) && !taken()) {
        if (lent()) {
            if (!wait_for_mesh_apps(NULL)) {
                free(recs);
                return false;
            }
            start = xTaskGetTickCount();
        }
        scanning = os_wifi_scan_start() == ESP_OK;
        if (!scanning) {
            vTaskDelay(pdMS_TO_TICKS(250));     // another scan runs
        }
    }
    TickType_t scan_t0 = xTaskGetTickCount();
    // Even if someone takes over meanwhile, let the scan end and read it: that
    // frees the results os_wifi keeps for whoever started the scan.
    while (scanning && !os_wifi_scan_done() && !past(start, SCAN_MS)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (scanning && os_wifi_scan_done()) {
        n = os_wifi_scan_results(recs, SCAN_MAX);
        ESP_LOGI(TAG, "scan: %d access points in %d ms", n, (int)((xTaskGetTickCount() - scan_t0) * portTICK_PERIOD_MS));
    } else if (!taken()) {
        ESP_LOGW(TAG, "no scan within %d s: Wi-Fi off", SCAN_MS / 1000);
        free(recs);
        return false;
    }
    if (taken()) {
        free(recs);
        return false;
    }
    // The most recent saved one in range other than the network that didn't
    // join (by name: the list may have changed meanwhile).
    char s[33], p[65];
    bool found = false;
    for (int i = 0; !found && os_wifi_saved_get(i, s, sizeof(s), p, sizeof(p)); i++) {
        for (int j = 0; strcmp(s, ssid) && j < n && !found; j++) {
            found = !strcmp((const char *)recs[j].ssid, s);
        }
    }
    free(recs);
    if (!found) {
        ESP_LOGI(TAG, "no older saved network in range: Wi-Fi off");
        return false;
    }
    strlcpy(ssid, s, 33);
    strlcpy(pass, p, 65);
    ESP_LOGI(TAG, "looking for '%s'", ssid);
    bool waited;
    return join(ssid, pass, &waited);
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
        if (in.state == OS_WIFI_CONNECTED || taken() || !in.enabled || strcmp(in.ssid, ssid)) {
            break;          // joined, or someone else drove Wi-Fi (off, on again, another network)
        }
        if (in.state == OS_WIFI_SUSPENDED) {
            start = xTaskGetTickCount();    // lent out: the resume rejoins, with 30 s of its own
            continue;
        }
        bool late = past(start, BOOT_JOIN_MS);
        if (older && (late || in.state == OS_WIFI_FAILED)) {
            older = false;
            ESP_LOGI(TAG, "'%s' didn't join: looking for an older saved network", ssid);
            if (!join_older(ssid, pass)) {
                break;
            }
            start = xTaskGetTickCount();
            continue;
        }
        if (late) {
            ESP_LOGI(TAG, "'%s' didn't join in %d s: Wi-Fi off", ssid, BOOT_JOIN_MS / 1000);
            os_wifi_set_enabled(false);
            break;
        }
        // Gave up early: keep listening until 30 s. A mesh app doesn't pause a
        // Wi-Fi that gave up, so the retry waits for the radio (as a resume would
        // rejoin), and the 30 s start again then.
        if (in.state == OS_WIFI_FAILED) {
            bool waited;
            if (!join(ssid, pass, &waited)) {
                break;
            }
            if (waited) {
                start = xTaskGetTickCount();
            }
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
    s_mine = os_wifi_changes();
    if (xTaskCreate(boot_join_task, "wifi_boot", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no task: Wi-Fi stays on");
    }
}
