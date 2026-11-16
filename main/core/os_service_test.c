// A test-only built-in service (os_service.h): `svc test on` and a restart
// register it, `svc test off` and a restart remove it. It exercises start,
// stop, start-at-boot, the status line, memory, notices (one every 30 s,
// with an LED flash) and the status bar's unread count. Nothing else uses it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "core/os_service.h"
#include "core/theme.h"
#include "lvgl.h"

#define TEST_MEM    4096

static esp_timer_handle_t s_timer;
static volatile int s_beats;
static int s_notices;
static void *s_mem;

static void beat(void *arg)
{
    int b = ++s_beats;
    if (b % 30 == 0 && os_service_notify("test", COL_PURPLE, "beat %d", b)) {
        s_notices++;        // read on the OS task: a torn int is harmless in a test
    }
}

static os_service_state_t state(void *ctx)
{
    return s_mem ? OS_SVC_RUNNING : OS_SVC_STOPPED;
}

static void status(void *ctx, char *out, size_t n)
{
    if (s_mem) {
        snprintf(out, n, "Beat %d, %d notice%s", s_beats, s_notices, s_notices == 1 ? "" : "s");
    } else {
        strlcpy(out, "Stopped", n);
    }
}

static int32_t memory(void *ctx)
{
    return s_mem ? TEST_MEM : 0;
}

static bool start(void *ctx, const char **why)
{
    if (!s_timer) {
        const esp_timer_create_args_t a = { .callback = beat, .name = "svc_test" };
        if (esp_timer_create(&a, &s_timer) != ESP_OK) {
            *why = "No timer";
            return false;
        }
    }
    s_mem = malloc(TEST_MEM);
    if (!s_mem) {
        *why = "Out of memory";
        return false;
    }
    memset(s_mem, 0xA5, TEST_MEM);
    s_beats = 0;
    s_notices = 0;
    esp_timer_start_periodic(s_timer, 1000 * 1000);
    return true;
}

static void stop(void *ctx)
{
    esp_timer_stop(s_timer);
    free(s_mem);
    s_mem = NULL;
    s_notices = 0;
}

static int unread(void *ctx)
{
    return s_notices;
}

static const os_service_def_t s_def = {
    .id = "test", .name = "Test service", .symbol = LV_SYMBOL_SETTINGS, .accent = COL_YELLOW,
    .flags = OS_SVC_STOPPABLE | OS_SVC_BOOT,
    .state = state, .status = status, .memory = memory, .start = start, .stop = stop, .unread = unread,
};

void os_service_test_register(void)
{
    os_service_register(&s_def);
}
