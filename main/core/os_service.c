// Background services registry, see os_service.h.
//
// Small on purpose: a table of built-in definitions, one provider for app
// services, boot flags in NVS "svc", the `svc` console command and the
// notice rate limit. Everything but os_service_notify() and
// os_service_post_stop() runs on the OS task, so the table needs no lock.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "core/os.h"
#include "core/os_cmd.h"
#include "core/os_service.h"
#include "game/game.h"

static const char *TAG = "svc";

#define NVS_NS          "svc"
#define BOOT_DELAY_MS   3000            // after os_start: the launcher draws first
#define NOTICE_GAP_MS   10000           // one notice per service every 10 s
#define NOTICE_SLOTS    8
#define NOTICE_LEN      80

static const os_service_def_t *s_defs[OS_SERVICE_BUILTIN_MAX];
static int s_ndefs;
static const os_service_provider_t *s_prov;

// ---------------------------------------------------------------------------
// Boot flags: NVS "svc/<id>" = 1 on, 0 off; missing = the definition's default.

static bool boot_flag(const os_service_def_t *d)
{
    uint8_t v = (d->flags & OS_SVC_BOOT) ? 1 : 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, d->id, &v);
        nvs_close(h);
    }
    return v != 0;
}

static void boot_flag_set(const os_service_def_t *d, bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, d->id, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------------------------------------------------------------------
// Registry

static bool id_ok(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n > 15 || id[0] == '_') {
        return false;
    }
    for (const char *p = id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

static const os_service_def_t *def_find(const char *id)
{
    for (int i = 0; i < s_ndefs; i++) {
        if (!strcmp(s_defs[i]->id, id)) {
            return s_defs[i];
        }
    }
    return NULL;
}

bool os_service_register(const os_service_def_t *d)
{
    if (!d || !id_ok(d->id) || !d->state || !d->status || def_find(d->id) || s_ndefs >= OS_SERVICE_BUILTIN_MAX
        || ((d->flags & (OS_SVC_STOPPABLE | OS_SVC_BOOT)) && (!d->start || !d->stop))) {
        ESP_LOGE(TAG, "can't register %s", d && d->id ? d->id : "?");
        return false;
    }
    s_defs[s_ndefs++] = d;
    return true;
}

void os_service_set_provider(const os_service_provider_t *p)
{
    s_prov = p;
}

static bool listed(const os_service_def_t *d)
{
    return !(d->flags & OS_SVC_HIDDEN) || d->state(d->ctx) != OS_SVC_STOPPED;
}

static void view_of(const os_service_def_t *d, os_service_view_t *v)
{
    memset(v, 0, sizeof(*v));
    strlcpy(v->id, d->id, sizeof(v->id));
    strlcpy(v->name, d->name, sizeof(v->name));
    v->symbol = d->symbol;
    v->accent = d->accent;
    v->kind = OS_SVC_BUILTIN;
    v->state = d->state(d->ctx);
    char line[OS_SERVICE_STATUS_MAX + 1] = "";
    d->status(d->ctx, line, sizeof(line));
    game_ascii(v->status, sizeof(v->status), line);
    v->stoppable = (d->flags & OS_SVC_STOPPABLE) != 0;
    if (!v->stoppable) {
        strlcpy(v->why_always, d->why_always ? d->why_always : "Part of Solana OS", sizeof(v->why_always));
    }
    v->boot = v->stoppable ? boot_flag(d) : true;
    v->screen = (d->flags & OS_SVC_SCREEN) != 0;
    v->memory = d->memory ? d->memory(d->ctx) : -1;
    v->unread = d->unread && v->state != OS_SVC_STOPPED ? d->unread(d->ctx) : 0;
}

static int listed_defs(void)
{
    int n = 0;
    for (int i = 0; i < s_ndefs; i++) {
        n += listed(s_defs[i]);
    }
    return n;
}

int os_service_count(void)
{
    return listed_defs() + (s_prov ? s_prov->count() : 0);
}

// App services first (they're what the owner chose), then Solana OS's own.
bool os_service_get(int i, os_service_view_t *out)
{
    int napp = s_prov ? s_prov->count() : 0;
    if (i < 0) {
        return false;
    }
    if (i < napp) {
        if (!s_prov->get(i, out)) {
            return false;
        }
        out->kind = OS_SVC_APP;
        out->stoppable = true;
        return true;
    }
    i -= napp;
    for (int k = 0; k < s_ndefs; k++) {
        if (listed(s_defs[k]) && i-- == 0) {
            view_of(s_defs[k], out);
            return true;
        }
    }
    return false;
}

bool os_service_find(const char *id, os_service_view_t *out)
{
    const os_service_def_t *d = id ? def_find(id) : NULL;
    if (d) {
        view_of(d, out);
        return true;
    }
    int n = s_prov && id ? s_prov->count() : 0;
    for (int i = 0; i < n; i++) {
        if (s_prov->get(i, out) && !strcmp(out->id, id)) {
            out->kind = OS_SVC_APP;
            out->stoppable = true;
            return true;
        }
    }
    return false;
}

static bool is_app_id(const char *id)
{
    return id && !strncmp(id, "app:", 4);
}

bool os_service_start(const char *id, const char **why)
{
    static const char *dummy;
    why = why ? why : &dummy;
    *why = "";
    if (is_app_id(id)) {
        if (!s_prov) {
            *why = "Not found";
            return false;
        }
        return s_prov->start(id, why);
    }
    const os_service_def_t *d = id ? def_find(id) : NULL;
    if (!d) {
        *why = "Not found";
        return false;
    }
    if (!(d->flags & OS_SVC_STOPPABLE)) {
        *why = d->why_always ? d->why_always : "Part of Solana OS";
        return false;
    }
    if (d->state(d->ctx) == OS_SVC_RUNNING) {
        boot_flag_set(d, true);
        return true;
    }
    if (!d->start(d->ctx, why)) {
        if (!*why || !**why) {
            *why = "It didn't start";
        }
        return false;
    }
    boot_flag_set(d, true);
    ESP_LOGI(TAG, "started %s", d->id);
    return true;
}

bool os_service_stop(const char *id, const char **why)
{
    static const char *dummy;
    why = why ? why : &dummy;
    *why = "";
    if (is_app_id(id)) {
        if (!s_prov) {
            *why = "Not found";
            return false;
        }
        s_prov->stop(id);
        return true;
    }
    const os_service_def_t *d = id ? def_find(id) : NULL;
    if (!d) {
        *why = "Not found";
        return false;
    }
    if (!(d->flags & OS_SVC_STOPPABLE)) {
        *why = d->why_always ? d->why_always : "Part of Solana OS";
        return false;
    }
    boot_flag_set(d, false);            // what the owner stopped stays stopped
    if (d->state(d->ctx) != OS_SVC_STOPPED) {
        d->stop(d->ctx);
    }
    ESP_LOGI(TAG, "stopped %s", d->id);
    return true;
}

bool os_service_set_boot(const char *id, bool on, const char **why)
{
    static const char *dummy;
    why = why ? why : &dummy;
    *why = "";
    if (is_app_id(id)) {
        if (!s_prov || !s_prov->set_boot(id, on)) {
            *why = "Not found";
            return false;
        }
        return true;
    }
    const os_service_def_t *d = id ? def_find(id) : NULL;
    if (!d) {
        *why = "Not found";
        return false;
    }
    if (!(d->flags & OS_SVC_STOPPABLE)) {
        *why = d->why_always ? d->why_always : "Part of Solana OS";
        return false;
    }
    boot_flag_set(d, on);
    return true;
}

void os_service_stop_apps(const char *why)
{
    if (s_prov && s_prov->stop_all) {
        s_prov->stop_all(why);
    }
}

static void post_stop(void *arg)
{
    os_service_stop(arg, NULL);
    free(arg);
}

void os_service_post_stop(const char *id)
{
    char *copy = id ? strdup(id) : NULL;
    if (copy && !os_post(post_stop, copy)) {
        free(copy);
    }
}

int os_service_unread(void)
{
    int n = 0;
    for (int i = 0; i < s_ndefs; i++) {
        const os_service_def_t *d = s_defs[i];
        if (d->unread && d->state(d->ctx) != OS_SVC_STOPPED) {
            n += d->unread(d->ctx);
        }
    }
    return n;
}

const char *os_service_mark(uint32_t *accent)
{
    for (int i = 0; i < s_ndefs; i++) {
        const os_service_def_t *d = s_defs[i];
        const char *m = d->mark && d->state(d->ctx) != OS_SVC_STOPPED ? d->mark(d->ctx) : NULL;
        if (m) {
            if (accent) {
                *accent = d->accent;
            }
            return m;
        }
    }
    return NULL;
}

int os_service_running(int32_t *memory)
{
    int n = 0;
    int32_t mem = 0;
    int total = os_service_count();
    os_service_view_t *v = malloc(sizeof(*v));
    for (int i = 0; v && i < total; i++) {
        if (os_service_get(i, v) && v->state == OS_SVC_RUNNING) {
            n++;
            mem += v->memory > 0 ? v->memory : 0;
        }
    }
    free(v);
    if (memory) {
        *memory = mem;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Notices: rate-limited on the caller's task, shown on the OS task.

static portMUX_TYPE s_notice_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    char id[OS_SERVICE_ID_MAX + 1];
    int64_t at;
} s_notice[NOTICE_SLOTS];

typedef struct {
    char id[OS_SERVICE_ID_MAX + 1];
    uint32_t led;
    char text[NOTICE_LEN + 1];
} notice_t;

static void notice_show(void *arg)
{
    notice_t *n = arg;
    os_service_view_t *v = malloc(sizeof(*v));
    const char *name = v && os_service_find(n->id, v) ? v->name : n->id;
    os_toast("%s: %s", name, n->text);
    if (n->led) {
        game_led_flash(n->led);
    }
    free(v);
    free(n);
}

bool os_service_notify(const char *id, uint32_t led, const char *fmt, ...)
{
    if (!id || !fmt) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    bool ok = false;
    portENTER_CRITICAL(&s_notice_mux);
    int slot = -1, oldest = 0;
    for (int i = 0; i < NOTICE_SLOTS; i++) {
        if (!strcmp(s_notice[i].id, id)) {
            slot = i;
            break;
        }
        if (s_notice[i].at < s_notice[oldest].at) {
            oldest = i;
        }
    }
    if (slot < 0) {
        slot = oldest;
        strlcpy(s_notice[slot].id, id, sizeof(s_notice[slot].id));
        s_notice[slot].at = 0;
    }
    if (!s_notice[slot].at || now - s_notice[slot].at >= NOTICE_GAP_MS) {
        s_notice[slot].at = now;
        ok = true;
    }
    portEXIT_CRITICAL(&s_notice_mux);
    if (!ok) {
        return false;
    }
    notice_t *n = malloc(sizeof(*n));
    if (!n) {
        return false;
    }
    strlcpy(n->id, id, sizeof(n->id));
    n->led = led;
    char raw[NOTICE_LEN + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(raw, sizeof(raw), fmt, ap);
    va_end(ap);
    game_ascii(n->text, sizeof(n->text), raw);
    if (!os_post(notice_show, n)) {
        free(n);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Boot

static void boot_start(void *arg)
{
    for (int i = 0; i < s_ndefs; i++) {
        const os_service_def_t *d = s_defs[i];
        if ((d->flags & OS_SVC_STOPPABLE) && boot_flag(d) && d->state(d->ctx) == OS_SVC_STOPPED) {
            const char *why = "";
            bool ok = d->start(d->ctx, &why);
            ESP_LOGI(TAG, "boot: %s %s%s", d->id, ok ? "started" : "didn't start: ", ok ? "" : why);
        }
    }
}

static esp_timer_handle_t s_boot_timer;

static void boot_timer(void *arg)
{
    if (!os_post(boot_start, NULL)) {
        esp_timer_start_once(s_boot_timer, 500 * 1000);     // queue full: try again shortly
    }
}

// ---------------------------------------------------------------------------
// Console: `svc` hands its line to the OS task, like `xdex` (the emulator's
// console isn't a task that could wait for it).

static const char *state_text(os_service_state_t s)
{
    switch (s) {
    case OS_SVC_RUNNING:  return "on";
    case OS_SVC_STOPPING: return "stopping";
    case OS_SVC_FAILED:   return "failed";
    default:              return "off";
    }
}

static void cmd_list(void)
{
    os_service_view_t *v = malloc(sizeof(*v));
    if (!v) {
        return;
    }
    int n = os_service_count();
    for (int i = 0; i < n; i++) {
        if (!os_service_get(i, v)) {
            continue;
        }
        char mem[16] = "-";
        if (v->memory >= 0) {
            snprintf(mem, sizeof(mem), "%ld B", (long)v->memory);
        }
        printf("%-12s %-8s %-4s %-5s %-10s %s%s\n", v->id, state_text(v->state),
               v->stoppable ? (v->boot ? "boot" : "-") : "os", v->kind == OS_SVC_APP ? "app" : "built", mem,
               v->status, v->unread ? " (unread)" : "");
    }
    int32_t m;
    int run = os_service_running(&m);
    printf("%d services, %d running (%ld B known), %d unread\n", n, run, (long)m, os_service_unread());
    free(v);
}

static void cmd_test(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (on) {
            nvs_set_u8(h, "_test", 1);
        } else {
            nvs_erase_key(h, "_test");
            nvs_erase_key(h, "test");       // its boot flag: leave nothing behind
        }
        nvs_commit(h);
        nvs_close(h);
    }
    printf("test service %s after a restart\n", on ? "registered" : "gone");
}

static void cmd_run(void *arg)
{
    char *line = arg;
    char *argv[4] = {0};
    int argc = 0;
    for (char *t = strtok(line, " "); t && argc < 4; t = strtok(NULL, " ")) {
        argv[argc++] = t;
    }
    const char *why = "";
    const char *op = argc > 0 ? argv[0] : "list";
    if (!strcmp(op, "list")) {
        cmd_list();
    } else if ((!strcmp(op, "start") || !strcmp(op, "stop")) && argc == 2) {
        bool ok = !strcmp(op, "start") ? os_service_start(argv[1], &why) : os_service_stop(argv[1], &why);
        printf("svc: %s %s: %s\n", op, argv[1], ok ? "ok" : why);
    } else if (!strcmp(op, "boot") && argc == 3) {
        bool ok = os_service_set_boot(argv[1], !strcmp(argv[2], "on"), &why);
        printf("svc: boot %s %s: %s\n", argv[1], argv[2], ok ? "ok" : why);
    } else if (!strcmp(op, "test") && argc == 2) {
        cmd_test(!strcmp(argv[1], "on"));
    } else {
        printf("usage: svc [list|start <id>|stop <id>|boot <id> on|off|test on|off]\n");
    }
    free(line);
}

static int cmd_svc(int argc, char **argv)
{
    char *line = malloc(96);
    if (!line) {
        return 1;
    }
    line[0] = 0;
    for (int i = 1; i < argc && i < 5; i++) {
        strlcat(line, argv[i], 96);
        strlcat(line, " ", 96);
    }
    if (!os_post(cmd_run, line)) {
        free(line);
        printf("svc: busy, try again\n");
        return 1;
    }
    return 0;
}

void os_service_test_register(void);        // os_service_test.c

void os_service_boot(void)
{
    uint8_t test = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "_test", &test);
        nvs_close(h);
    }
    if (test) {
        os_service_test_register();
    }
    os_cmd_register("svc", "svc [list|start <id>|stop <id>|boot <id> on|off|test on|off]", cmd_svc);
    const esp_timer_create_args_t args = { .callback = boot_timer, .name = "svc_boot" };
    if (esp_timer_create(&args, &s_boot_timer) == ESP_OK) {
        esp_timer_start_once(s_boot_timer, BOOT_DELAY_MS * 1000);
    }
}
