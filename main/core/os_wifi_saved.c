// Saved Wi-Fi networks, most recent first, in NVS namespace "wifi". The newest
// keeps the keys a single saved network always had ("ssid", "pass"), so a badge
// updating from a build that saved one network keeps it, a downgrade still finds
// the newest, and os_wifi_saved_ssid() keeps meaning "the network to join".
// Older ones are "ssid1".."ssid4" / "pass1".."pass4". Portable: only the NVS
// API, so the badge (os_wifi.c) and the emulator (os_wifi_emu.c) share it.
// Each step reads one network at a time: os_wifi.c saves from the Wi-Fi event
// task, whose stack is small.

#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "os_wifi.h"

#define NVS_NS  "wifi"

typedef struct {
    char ssid[33];
    char pass[65];
} net_t;

static void key(char *out, const char *base, int i)
{
    if (i) {
        snprintf(out, 16, "%s%d", base, i);
    } else {
        strlcpy(out, base, 16);
    }
}

static bool load(nvs_handle_t h, int i, net_t *n)
{
    char k[16];
    size_t len = sizeof(n->ssid);
    key(k, "ssid", i);
    if (nvs_get_str(h, k, n->ssid, &len) != ESP_OK || !n->ssid[0]) {
        n->ssid[0] = 0;
        return false;
    }
    len = sizeof(n->pass);
    key(k, "pass", i);
    if (nvs_get_str(h, k, n->pass, &len) != ESP_OK) {
        n->pass[0] = 0;
    }
    return true;
}

static void store(nvs_handle_t h, int i, const net_t *n)
{
    char k[16];
    key(k, "ssid", i);
    nvs_set_str(h, k, n->ssid);
    key(k, "pass", i);
    nvs_set_str(h, k, n->pass);
}

static void erase(nvs_handle_t h, int i)
{
    char k[16];
    key(k, "ssid", i);
    nvs_erase_key(h, k);
    key(k, "pass", i);
    nvs_erase_key(h, k);
}

// Slot of `ssid`, or -1. The list ends at the first empty slot.
static int index_of(nvs_handle_t h, const char *ssid, net_t *tmp)
{
    for (int i = 0; i < OS_WIFI_SAVED_MAX && load(h, i, tmp); i++) {
        if (!strcmp(tmp->ssid, ssid)) {
            return i;
        }
    }
    return -1;
}

int os_wifi_saved_count(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    net_t n;
    int i = 0;
    while (i < OS_WIFI_SAVED_MAX && load(h, i, &n)) {
        i++;
    }
    nvs_close(h);
    return i;
}

bool os_wifi_saved_get(int i, char *ssid, size_t ssid_n, char *pass, size_t pass_n)
{
    nvs_handle_t h;
    if (i < 0 || i >= OS_WIFI_SAVED_MAX || nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    net_t n;
    bool ok = true;
    for (int j = 0; ok && j <= i; j++) {
        ok = load(h, j, &n);            // a slot past a gap isn't in the list
    }
    nvs_close(h);
    if (!ok) {
        return false;
    }
    if (ssid) {
        strlcpy(ssid, n.ssid, ssid_n);
    }
    if (pass) {
        strlcpy(pass, n.pass, pass_n);
    }
    return true;
}

bool os_wifi_saved_find(const char *ssid, char *pass, size_t pass_n)
{
    nvs_handle_t h;
    if (!ssid || !ssid[0] || nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    net_t n;
    bool found = index_of(h, ssid, &n) >= 0;
    nvs_close(h);
    if (found && pass) {
        strlcpy(pass, n.pass, pass_n);
    }
    return found;
}

bool os_wifi_saved_ssid(char *out, size_t n)
{
    return os_wifi_saved_get(0, out, n, NULL, 0);
}

bool os_wifi_saved_pass(char *out, size_t n)
{
    char ssid[33];
    return os_wifi_saved_get(0, ssid, sizeof(ssid), out, n);
}

void os_wifi_saved_put(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (!ssid || !ssid[0] || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    net_t n;
    int at = index_of(h, ssid, &n);
    if (at < 0) {
        // New: everything moves down one, the oldest drops off the end.
        at = 0;
        while (at < OS_WIFI_SAVED_MAX - 1 && load(h, at, &n)) {
            at++;
        }
    }
    for (int i = at; i > 0; i--) {
        load(h, i - 1, &n);
        store(h, i, &n);
    }
    strlcpy(n.ssid, ssid, sizeof(n.ssid));
    strlcpy(n.pass, pass ? pass : "", sizeof(n.pass));
    store(h, 0, &n);
    nvs_commit(h);
    nvs_close(h);
}

bool os_wifi_saved_remove(const char *ssid)
{
    nvs_handle_t h;
    if (!ssid || !ssid[0] || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    net_t n;
    bool removed = false;
    int at;
    while ((at = index_of(h, ssid, &n)) >= 0) {
        // The ones after it move up one; the last slot empties.
        int i = at;
        while (i + 1 < OS_WIFI_SAVED_MAX && load(h, i + 1, &n)) {
            store(h, i, &n);
            i++;
        }
        erase(h, i);
        removed = true;
    }
    nvs_commit(h);
    nvs_close(h);
    return removed;
}
