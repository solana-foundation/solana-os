// The portable half of the AOT seam (lua_aot.h): whether a C app's native
// code may run. Both builds compile it, so the emulator fetches and caches
// the store's key and checks every signature exactly as a badge does; only
// running the code (lua_aot.c) is the badge's.

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"
#include "core/ed25519.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_data.h"
#include "lua_aot.h"
#include "lua_pkg.h"
#include "lua_xip.h"

#define NVS_NS      "cloud"         // the server identity's namespace (os_cloud.c drops it with the server)
#define NVS_KEY     "aotkey"

static const char s_no_key[] = "the store's key isn't on this badge yet";
static const char s_bad_sig[] = "the store's signature doesn't verify";

// The refresh (lw_aot_key_refresh): written on the OS task (the answer), read
// anywhere.
static volatile uint8_t s_refresh = LW_KEY_REFRESH_NONE;
static volatile int64_t s_refresh_us;
static volatile uint32_t s_refresh_gen;     // os_cloud_server_gen() when the refresh was asked

bool lw_aot_key(uint8_t key[32])
{
    char hex[65] = "";
    size_t len = sizeof(hex);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(h, NVS_KEY, hex, &len) == ESP_OK && lua_pkg_unhex(hex, key);
    nvs_close(h);
    return ok;
}

bool lw_aot_key_save(const char *hex)
{
    uint8_t key[32];
    nvs_handle_t h;
    if (!hex || !lua_pkg_unhex(hex, key) || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    char lower[65];
    lua_pkg_hex(lower, key, 32);
    bool ok = nvs_set_str(h, NVS_KEY, lower) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void key_drop(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY, "");        // as os_cloud.c leaves it for a new server
        nvs_commit(h);
        nvs_close(h);
    }
}

bool lw_aot_key_answer(int status, const char *body, uint32_t gen)
{
    if (gen != os_cloud_server_gen()) {
        return false;                       // asked of the server before this one
    }
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(j, "key");
    bool ok = cJSON_IsString(k) && lw_aot_key_save(k->valuestring);
    cJSON_Delete(j);
    // The server may have changed on another task between the check above
    // and the save (os_cloud bumps its number first, then clears the key):
    // whichever order the two ran in, the old server's key must not stay.
    if (ok && gen != os_cloud_server_gen()) {
        key_drop();
        ok = false;
    }
    return ok;
}

bool lw_aot_sig_ok(const uint8_t key[32], const uint8_t sha[32], const uint8_t sig[64])
{
    char msg[sizeof(LW_AOT_SIG_PREFIX) + sizeof(LW_AOT_TARGET) + 1 + 64];
    char hex[65];
    lua_pkg_hex(hex, sha, 32);
    int n = snprintf(msg, sizeof(msg), LW_AOT_SIG_PREFIX "%s|%s", LW_AOT_TARGET, hex);
    return n > 0 && (size_t)n < sizeof(msg) && ed25519_verify(key, (const uint8_t *)msg, (size_t)n, sig);
}

const char *lw_aot_refusal(const lua_pkg_t *pkg, char *path, size_t n, long *size, uint32_t *sig_us)
{
    char rel[64];
    uint8_t key[32];
    struct stat st;
    *size = -1;
    if (!pkg->aot) {
        return "";                          // none for this badge: nothing to say
    }
    if (!lw_aot_usable()) {
        return "this badge can't run native code";
    }
    lua_pkg_rel(rel, sizeof(rel), pkg->id, "app.aot");
    os_data_path(path, n, rel);
    *size = stat(path, &st) == 0 ? (long)st.st_size : -1;
    if (*size != pkg->aot_size) {
        return *size < 0 ? "app.aot isn't on the card" : "app.aot isn't the store's";
    }
    if (*size > LUA_XIP_MAX) {
        return "app.aot doesn't fit a flash slot";
    }
    if (!lw_aot_key(key)) {
        return s_no_key;
    }
    int64_t t = esp_timer_get_time();
    bool ok = lw_aot_sig_ok(key, pkg->aot_sha, pkg->aot_sig);
    if (sig_us) {
        *sig_us = (uint32_t)(esp_timer_get_time() - t);
    }
    return ok ? NULL : s_bad_sig;
}

bool lw_aot_refusal_is_key(const char *why)
{
    return why == s_no_key || why == s_bad_sig;
}

// OS task: os_api's answer. Caches what came and notes whether it changed.
static void refresh_answered(int status, const char *body, void *ctx)
{
    (void)ctx;
    if (s_refresh_gen != os_cloud_server_gen()) {
        s_refresh = LW_KEY_REFRESH_NONE;    // the old server's answer: the new one can still be asked this boot
        return;
    }
    uint8_t before[32], after[32];
    bool had = lw_aot_key(before);
    bool saved = lw_aot_key_answer(status, body, s_refresh_gen) && lw_aot_key(after);
    s_refresh_us = esp_timer_get_time();
    s_refresh = !saved ? LW_KEY_REFRESH_FAILED
                : had && !memcmp(before, after, sizeof(after)) ? LW_KEY_REFRESH_SAME : LW_KEY_REFRESH_NEW;
}

void lw_aot_key_refresh(void)
{
    if (s_refresh != LW_KEY_REFRESH_NONE || !os_api_online()) {
        return;                             // asked already this boot, or offline (a later launch may ask)
    }
    // One refresh at a time (the check above), so its server number can live
    // in one variable. "Asked" before the call: the answer runs on the OS
    // task and may come before this task goes on.
    s_refresh_gen = os_cloud_server_gen();
    s_refresh = LW_KEY_REFRESH_ASKED;
    if (os_api_call("GET", "/api/device/aotkey", NULL, 256, refresh_answered, os_api_token()) != ESP_OK) {
        s_refresh = LW_KEY_REFRESH_NONE;
    }
}

uint8_t lw_aot_key_refresh_state(int64_t *at_us)
{
    if (at_us) {
        *at_us = s_refresh_us;
    }
    return s_refresh;
}
