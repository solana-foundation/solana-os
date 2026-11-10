// Bluetooth LE for the running Lua app (os_ble.h), on NimBLE. Device-only.
//
// Off until an owner starts it: nimble_port_init() brings up the controller
// and the host, nimble_port_deinit() gives their heap back, so everything
// here that lives while BLE is on is in one block (st_t) freed with it.
//
// Threads: NimBLE calls back on its host task; those callbacks update the
// state under s_mu and queue events, and the owner's wake() runs after s_mu
// is let go. The owner's calls never call NimBLE with s_mu held (some NimBLE
// calls answer through a callback before they return); enter()/leave() count
// them, so a stop from another task waits until none is inside NimBLE before
// it tears the stack down. GATT client work runs on the host task as well
// (kicked through an npl event), one procedure at a time: the MTU exchange,
// then for each operation the service and characteristic lookups it still
// needs, then the read, write or subscribe itself.
//
// Everything from the air is hostile: lengths are clamped before copying,
// names become printable ASCII, and what a peer sends is queued only while
// fewer than OS_BLE_EVENTS_MAX events wait (BLE has no flow control a peer
// must honour), so a peer can't make the badge allocate without bound.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "soc/interrupts.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os.h"
#include "os_ble.h"
#include "os_cloud.h"
#include "os_cmd.h"

static const char *TAG = "ble";

#define RING        16                      // event slots: OS_BLE_EVENTS_MAX for what peers send, the rest for OPEN/CLOSE/DONE
#define DEVS        8                       // devices a scan remembers (to merge scan responses, report once a second)
#define SVC_CACHE   4
#define CHR_CACHE   8
#define OPQ         (OS_BLE_OPS_MAX + 4)    // + writes without response
#define SYNC_MS     2000
#define CONNECT_MS  10000
#define REPORT_US   1000000

typedef enum { PH_OFF, PH_STARTING, PH_ON, PH_STOPPING } phase_t;
typedef enum { LINK_NONE, LINK_CONNECTING, LINK_OPEN } link_t;
typedef enum { OP_READ, OP_WRITE, OP_WRITE_NR, OP_SUB } op_kind_t;

typedef struct {
    os_ble_uuid_t svc, chr;
    uint32_t id;                    // 0: a write without response (no DONE)
    uint8_t kind;
    bool on;
    uint8_t *data;
    uint16_t len;
} op_t;

typedef struct {
    os_ble_uuid_t uuid;
    uint16_t start, end;
    bool missing;
} svc_c_t;

typedef struct {
    uint8_t svc;                    // index in svcc
    os_ble_uuid_t uuid;
    uint16_t val, cccd;
    uint8_t props;
    bool missing, cccd_known;
} chr_c_t;

typedef struct {
    bool used, scannable, connectable;
    ble_addr_t addr;
    int8_t rssi;
    int64_t first_us, report_us;
    char name[OS_BLE_NAME_MAX + 1];
    os_ble_uuid_t services[OS_BLE_SERVICES_MAX];
    uint8_t n_services;
    uint16_t company;
    uint8_t data[27];
    uint8_t len;
} scan_dev_t;

typedef struct {
    uint8_t own_type;

    // advertising: what the owner asked for, applied again after serve
    bool adv_set, adv_on;
    char adv_name[OS_BLE_NAME_MAX + 1];
    os_ble_uuid_t adv_svc;
    bool adv_has_svc, adv_conn;
    uint8_t adv_data[OS_BLE_ADV_MAX];
    uint8_t adv_len;
    uint16_t adv_itvl_ms;

    // scanning
    bool scan_on, scan_active, scan_has_svc;
    uint32_t scan_secs;
    uint8_t scan_duty;
    os_ble_uuid_t scan_svc;
    char scan_prefix[OS_BLE_NAME_MAX + 1];
    scan_dev_t *devs;               // DEVS, made by the first scan

    // the served service (NimBLE keeps pointers into these)
    bool served;
    uint8_t n_chrs;
    struct ble_gatt_svc_def svcs[2];
    struct ble_gatt_chr_def chrs[OS_BLE_CHARS_MAX + 1];
    ble_uuid_any_t svc_uuid, chr_uuid[OS_BLE_CHARS_MAX];
    os_ble_uuid_t chr_os[OS_BLE_CHARS_MAX];
    uint16_t val_handle[OS_BLE_CHARS_MAX];
    uint8_t (*val)[OS_BLE_DATA_MAX];    // n_chrs values, made by serve
    uint16_t val_len[OS_BLE_CHARS_MAX];
    bool subscribed[OS_BLE_CHARS_MAX];

    // the link, either way round
    link_t link;
    bool central;
    uint16_t conn;
    uint32_t link_gen;              // GATT callbacks of an older link are ignored
    os_ble_addr_t peer;
    const char *close_reason;       // set when we end it ("closed", "off")

    // GATT client
    svc_c_t svcc[SVC_CACHE];
    chr_c_t chrc[CHR_CACHE];
    uint8_t n_svcc, n_chrc;
    op_t ops[OPQ];
    uint8_t n_ops;
    bool busy;                      // a GATT procedure is out
    uint32_t next_op;
    struct ble_npl_event kick;

    // events for the owner
    os_ble_ev_t ring[RING];
    uint8_t head, count;
} st_t;

static SemaphoreHandle_t s_mu;
static StaticSemaphore_t s_mu_buf;
static st_t *S;                         // NULL while off
static volatile phase_t s_phase;
static void *s_owner;
static os_ble_wake_t s_wake;
static volatile int s_calls;            // owner calls inside NimBLE
static volatile bool s_synced;
static int s_resets;

static void lock(void) { xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mu); }

static void wake_owner(void)
{
    lock();
    os_ble_wake_t w = s_wake;
    void *c = s_owner;
    unlock();
    if (w && c) {
        w(c);
    }
}

// Under lock. Takes ev->data (freed when the event is dropped). What peers
// send waits only while fewer than OS_BLE_EVENTS_MAX events do; OPEN, CLOSE
// and DONE always find a slot (the callers bound them: one link, its CLOSE
// taken before the next, OS_BLE_OPS_MAX answers).
static bool push(os_ble_ev_t *ev)
{
    bool peer = ev->kind == OS_BLE_EV_SCAN || ev->kind == OS_BLE_EV_NOTIFY || ev->kind == OS_BLE_EV_WRITE ||
                ev->kind == OS_BLE_EV_SUBSCRIBE;
    if (!S || !s_owner || S->count >= (peer ? OS_BLE_EVENTS_MAX : RING)) {
        free(ev->data);
        ev->data = NULL;
        return false;
    }
    S->ring[(S->head + S->count) % RING] = *ev;
    S->count++;
    ev->data = NULL;
    return true;
}

static bool has_event(os_ble_ev_kind_t kind)
{
    for (int i = 0; i < S->count; i++) {
        if (S->ring[(S->head + i) % RING].kind == kind) {
            return true;
        }
    }
    return false;
}

static void drain_ring(void)
{
    while (S && S->count) {
        free(S->ring[S->head].data);
        S->head = (S->head + 1) % RING;
        S->count--;
    }
}

// The owner's calls: true while BLE is on for ctx; leave() after.
static bool enter(void *ctx)
{
    lock();
    bool ok = ctx && s_phase == PH_ON && s_owner == ctx;
    if (ok) {
        s_calls++;
    }
    unlock();
    return ok;
}

static void leave(void)
{
    lock();
    s_calls--;
    unlock();
}

// --- conversions ---------------------------------------------------------------

static void to_nimble_uuid(const os_ble_uuid_t *u, ble_uuid_any_t *out)
{
    memset(out, 0, sizeof(*out));
    if (u->len == 2) {
        out->u16.u.type = BLE_UUID_TYPE_16;
        out->u16.value = (uint16_t)(u->b[0] << 8 | u->b[1]);
    } else {
        out->u128.u.type = BLE_UUID_TYPE_128;
        for (int i = 0; i < 16; i++) {
            out->u128.value[i] = u->b[15 - i];
        }
    }
}

static bool uuid_ok(const os_ble_uuid_t *u)
{
    return u && (u->len == 2 || u->len == 16);
}

static bool uuid_eq(const os_ble_uuid_t *a, const os_ble_uuid_t *b)
{
    return a->len == b->len && !memcmp(a->b, b->b, a->len);
}

static void to_os_addr(const ble_addr_t *a, os_ble_addr_t *out)
{
    for (int i = 0; i < 6; i++) {
        out->val[i] = a->val[5 - i];
    }
    out->random = a->type == BLE_ADDR_RANDOM || a->type == BLE_ADDR_RANDOM_ID;
}

static void printable(char *out, size_t n, const uint8_t *in, size_t len)
{
    size_t i = 0;
    for (; i < len && i + 1 < n; i++) {
        out[i] = in[i] >= 0x20 && in[i] < 0x7f ? (char)in[i] : '?';
    }
    out[i] = '\0';
}

// --- the host --------------------------------------------------------------------

static int gap_event(struct ble_gap_event *ev, void *arg);

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    uint8_t t = BLE_OWN_ADDR_PUBLIC;
    ble_hs_id_infer_auto(0, &t);
    lock();
    if (S) {
        S->own_type = t;
    }
    unlock();
    s_synced = true;
}

static void fail_ops(const char *reason)
{
    for (int i = 0; i < S->n_ops; i++) {
        if (S->ops[i].id) {
            os_ble_ev_t e = {.kind = OS_BLE_EV_DONE, .op = S->ops[i].id, .reason = reason};
            push(&e);
        }
        free(S->ops[i].data);
    }
    S->n_ops = 0;
    S->busy = false;
    S->n_svcc = S->n_chrc = 0;
}

// The link ended (under lock): answers first, then CLOSE.
static void link_closed(const char *reason)
{
    fail_ops("lost");
    memset(S->subscribed, 0, sizeof(S->subscribed));
    os_ble_ev_t e = {.kind = OS_BLE_EV_CLOSE, .central = S->central, .addr = S->peer, .reason = reason};
    S->link = LINK_NONE;
    S->conn = BLE_HS_CONN_HANDLE_NONE;
    S->close_reason = NULL;
    S->link_gen++;
    push(&e);
}

// The controller restarted: links, advertising and scans are gone.
static void on_reset(int reason)
{
    bool w = false;
    lock();
    s_resets++;
    if (S) {
        if (S->link != LINK_NONE) {
            link_closed("lost");
            w = true;
        }
        S->adv_on = S->scan_on = false;
    }
    unlock();
    s_synced = false;
    if (w) {
        wake_owner();
    }
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// --- advertising -------------------------------------------------------------------

// "Badge <id>", or "Badge" before the badge has an id.
static void default_name(char *out, size_t n)
{
    os_cloud_info_t *ci = malloc(sizeof(*ci));
    if (ci) {
        os_cloud_get_info(ci);
    }
    snprintf(out, n, ci && ci->id[0] ? "Badge %s" : "Badge", ci ? ci->id : "");
    free(ci);
}

// Applies S->adv_* (the owner's call, or serve putting it back). Not under lock.
static int adv_apply(void)
{
    lock();
    char name[OS_BLE_NAME_MAX + 1];
    strcpy(name, S->adv_name);
    os_ble_uuid_t svc = S->adv_svc;
    bool has_svc = S->adv_has_svc, conn = S->adv_conn;
    uint8_t mfg[2 + OS_BLE_ADV_MAX] = {0xff, 0xff};
    uint8_t len = S->adv_len;
    memcpy(mfg + 2, S->adv_data, len);
    uint16_t itvl = S->adv_itvl_ms;
    uint8_t own = S->own_type;
    unlock();

    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    ble_svc_gap_device_name_set(name);      // what a connected device reads as the name

    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    ble_uuid_any_t u;
    if (has_svc) {
        to_nimble_uuid(&svc, &u);
        if (svc.len == 2) {
            f.uuids16 = &u.u16;
            f.num_uuids16 = 1;
            f.uuids16_is_complete = 1;
        } else {
            f.uuids128 = &u.u128;
            f.num_uuids128 = 1;
            f.uuids128_is_complete = 1;
        }
    }
    if (len) {
        f.mfg_data = mfg;
        f.mfg_data_len = 2 + len;
    }
    int rc = ble_gap_adv_set_fields(&f);
    if (rc == 0) {
        struct ble_hs_adv_fields r = {0};
        r.name = (const uint8_t *)name;
        r.name_len = strlen(name);
        r.name_is_complete = 1;
        rc = ble_gap_adv_rsp_set_fields(&r);
    }
    if (rc == 0) {
        struct ble_gap_adv_params p = {0};
        p.conn_mode = conn ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
        p.disc_mode = BLE_GAP_DISC_MODE_GEN;       // non-connectable: scannable, so the name is there
        p.itvl_min = p.itvl_max = (uint16_t)(itvl * 1000u / 625u);
        rc = ble_gap_adv_start(own, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    }
    lock();
    S->adv_on = rc == 0;
    unlock();
    return rc;
}

esp_err_t os_ble_advertise(void *ctx, const os_ble_adv_t *adv)
{
    char name[OS_BLE_NAME_MAX + 1];
    if (adv) {
        size_t max = !adv->service ? OS_BLE_ADV_MAX : adv->service->len == 2 ? OS_BLE_ADV_MAX - 4 : OS_BLE_ADV_MAX - 18;
        if ((adv->name && strlen(adv->name) > OS_BLE_NAME_MAX) || adv->len > max || (adv->len && !adv->data)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (adv->service && !uuid_ok(adv->service)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (adv->name) {
            printable(name, sizeof(name), (const uint8_t *)adv->name, strlen(adv->name));
        } else {
            default_name(name, sizeof(name));
        }
    }
    if (!enter(ctx)) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = 0;
    if (!adv) {
        lock();
        S->adv_set = S->adv_on = false;
        unlock();
        rc = ble_gap_adv_stop();
        rc = rc == BLE_HS_EALREADY ? 0 : rc;
    } else {
        lock();
        bool busy = adv->connectable && S->link != LINK_NONE;
        if (!busy) {
            S->adv_set = true;
            strcpy(S->adv_name, name);
            S->adv_has_svc = adv->service != NULL;
            if (adv->service) {
                S->adv_svc = *adv->service;
            }
            S->adv_conn = adv->connectable;
            S->adv_len = adv->len;
            memcpy(S->adv_data, adv->data, adv->len);
            uint16_t ms = adv->interval_ms ? adv->interval_ms : 100;
            S->adv_itvl_ms = ms < 20 ? 20 : ms > 10000 ? 10000 : ms;
        }
        unlock();
        rc = busy ? BLE_HS_EBUSY : adv_apply();
    }
    leave();
    if (rc) {
        ESP_LOGW(TAG, "advertise: %d", rc);
    }
    return rc == 0 ? ESP_OK : rc == BLE_HS_EBUSY || rc == BLE_HS_ENOMEM ? ESP_ERR_INVALID_STATE : ESP_FAIL;
}

// --- scanning --------------------------------------------------------------------------

typedef struct {
    const uint8_t *name;
    uint8_t name_len;
    os_ble_uuid_t services[OS_BLE_SERVICES_MAX];
    uint8_t n_services;
    bool has_mfg;
    uint16_t company;
    const uint8_t *mfg;
    uint8_t mfg_len;
} ad_t;

static void add_service(os_ble_uuid_t *list, uint8_t *n, const os_ble_uuid_t *u)
{
    for (int i = 0; i < *n; i++) {
        if (uuid_eq(&list[i], u)) {
            return;
        }
    }
    if (*n < OS_BLE_SERVICES_MAX) {
        list[(*n)++] = *u;
    }
}

// AD structures: length, type, value; a length past the end ends it.
static void parse_ad(const uint8_t *p, int n, ad_t *ad)
{
    memset(ad, 0, sizeof(*ad));
    for (int i = 0; i + 1 < n;) {
        int l = p[i];
        if (l == 0 || i + 1 + l > n) {
            break;
        }
        uint8_t type = p[i + 1];
        const uint8_t *v = p + i + 2;
        int vl = l - 1;
        if (type == 0x08 || type == 0x09) {
            if (type == 0x09 || !ad->name) {
                ad->name = v;
                ad->name_len = vl > OS_BLE_NAME_MAX ? OS_BLE_NAME_MAX : vl;
            }
        } else if (type == 0x02 || type == 0x03) {
            for (int k = 0; k + 2 <= vl; k += 2) {
                os_ble_uuid_t u = {.len = 2, .b = {v[k + 1], v[k]}};
                add_service(ad->services, &ad->n_services, &u);
            }
        } else if (type == 0x06 || type == 0x07) {
            for (int k = 0; k + 16 <= vl; k += 16) {
                os_ble_uuid_t u = {.len = 16};
                for (int j = 0; j < 16; j++) {
                    u.b[j] = v[k + 15 - j];
                }
                add_service(ad->services, &ad->n_services, &u);
            }
        } else if (type == 0xff && vl >= 2 && !ad->has_mfg) {
            ad->has_mfg = true;
            ad->company = v[0] | v[1] << 8;
            ad->mfg = v + 2;
            ad->mfg_len = vl - 2;
        }
        i += 1 + l;
    }
}

// Under lock. One advertising report: merged into the device's entry, which
// is reported at most once a second, with its name when an active scan can
// still get one (its scan response, or a second of waiting for it).
static bool on_disc(const struct ble_gap_disc_desc *d)
{
    if (!S->scan_on || !S->devs) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    ad_t ad;
    parse_ad(d->data, d->length_data > 31 ? 31 : d->length_data, &ad);
    bool rsp = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;

    scan_dev_t *dev = NULL, *oldest = NULL;
    for (int i = 0; i < DEVS; i++) {
        scan_dev_t *x = &S->devs[i];
        if (x->used && !ble_addr_cmp(&x->addr, &d->addr)) {
            dev = x;
            break;
        }
        if (!oldest || !x->used || (oldest->used && x->report_us < oldest->report_us)) {
            oldest = x;
        }
    }
    if (!dev) {
        dev = oldest;
        memset(dev, 0, sizeof(*dev));
        dev->used = true;
        dev->addr = d->addr;
        dev->first_us = now;
        dev->report_us = now - REPORT_US;
        dev->scannable = true;
    }
    if (!rsp) {
        dev->connectable = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                           d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
        dev->scannable = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                         d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND;
        dev->n_services = 0;
        dev->company = 0;
        dev->len = 0;
        if (ad.has_mfg) {
            dev->company = ad.company;
            dev->len = ad.mfg_len > sizeof(dev->data) ? sizeof(dev->data) : ad.mfg_len;
            memcpy(dev->data, ad.mfg, dev->len);
        }
    }
    for (int i = 0; i < ad.n_services; i++) {
        add_service(dev->services, &dev->n_services, &ad.services[i]);
    }
    if (ad.name) {
        printable(dev->name, sizeof(dev->name), ad.name, ad.name_len);
    }
    dev->rssi = d->rssi;

    if (S->scan_has_svc) {
        bool hit = false;
        for (int i = 0; i < dev->n_services && !hit; i++) {
            hit = uuid_eq(&dev->services[i], &S->scan_svc);
        }
        if (!hit) {
            return false;
        }
    }
    if (S->scan_prefix[0] && strncmp(dev->name, S->scan_prefix, strlen(S->scan_prefix))) {
        return false;
    }
    bool ready = !S->scan_active || rsp || !dev->scannable || dev->name[0] || now - dev->first_us >= REPORT_US;
    if (!ready || now - dev->report_us < REPORT_US || S->count >= OS_BLE_EVENTS_MAX) {
        return false;
    }
    dev->report_us = now;
    os_ble_ev_t e = {.kind = OS_BLE_EV_SCAN, .rssi = dev->rssi, .connectable = dev->connectable,
                     .n_services = dev->n_services, .company = dev->company};
    to_os_addr(&dev->addr, &e.addr);
    strcpy(e.name, dev->name);
    memcpy(e.services, dev->services, sizeof(e.services));
    if (dev->len) {
        e.data = malloc(dev->len);
        if (!e.data) {
            return false;
        }
        memcpy(e.data, dev->data, dev->len);
        e.len = dev->len;
    }
    return push(&e);
}

static int disc_start(void)
{
    lock();
    struct ble_gap_disc_params dp = {0};
    dp.itvl = 160;                                      // 100 ms
    dp.window = (uint16_t)(160 * S->scan_duty / 100);
    dp.window = dp.window < 4 ? 4 : dp.window;
    dp.passive = !S->scan_active;
    int32_t ms = S->scan_secs ? (int32_t)S->scan_secs * 1000 : BLE_HS_FOREVER;
    uint8_t own = S->own_type;
    memset(S->devs, 0, DEVS * sizeof(scan_dev_t));
    S->scan_on = true;
    unlock();
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    int rc = ble_gap_disc(own, ms, &dp, gap_event, NULL);
    if (rc) {
        lock();
        S->scan_on = false;
        unlock();
    }
    return rc;
}

esp_err_t os_ble_scan(void *ctx, const os_ble_scan_t *scan)
{
    if (scan && ((scan->service && !uuid_ok(scan->service)) ||
                 (scan->name_prefix && strlen(scan->name_prefix) > OS_BLE_NAME_MAX))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!enter(ctx)) {
        return ESP_ERR_INVALID_STATE;
    }
    int rc = 0;
    lock();
    bool connecting = S->link == LINK_CONNECTING;
    if (scan && !connecting && !S->devs) {
        S->devs = calloc(DEVS, sizeof(scan_dev_t));
    }
    bool nomem = scan && !connecting && !S->devs;
    if (scan && !connecting && !nomem) {
        S->scan_active = scan->active;
        S->scan_has_svc = scan->service != NULL;
        if (scan->service) {
            S->scan_svc = *scan->service;
        }
        printable(S->scan_prefix, sizeof(S->scan_prefix), (const uint8_t *)(scan->name_prefix ? scan->name_prefix : ""),
                  scan->name_prefix ? strlen(scan->name_prefix) : 0);
        S->scan_secs = scan->seconds > 86400 ? 86400 : scan->seconds;
        uint8_t duty = scan->duty ? scan->duty : 30;
        S->scan_duty = duty < 10 ? 10 : duty > 100 ? 100 : duty;
    }
    if (!scan) {
        S->scan_on = false;
    }
    unlock();
    if (!scan) {
        rc = ble_gap_disc_cancel();
        rc = rc == BLE_HS_EALREADY ? 0 : rc;
    } else if (connecting) {
        rc = BLE_HS_EBUSY;
    } else if (nomem) {
        rc = BLE_HS_ENOMEM;
    } else {
        rc = disc_start();
    }
    leave();
    if (rc) {
        ESP_LOGW(TAG, "scan: %d", rc);
    }
    return rc == 0 ? ESP_OK : rc == BLE_HS_EBUSY ? ESP_ERR_INVALID_STATE : rc == BLE_HS_ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
}

// --- being connected to (peripheral) ----------------------------------------------------

static int chr_index(uint16_t handle)
{
    for (int i = 0; i < S->n_chrs; i++) {
        if (S->val_handle[i] == handle) {
            return i;
        }
    }
    return -1;
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int i = (int)(intptr_t)arg, rc = 0;
    bool w = false;
    lock();
    if (!S || !S->served || i < 0 || i >= S->n_chrs) {
        rc = BLE_ATT_ERR_UNLIKELY;
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        rc = os_mbuf_append(ctxt->om, S->val[i], S->val_len[i]) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > OS_BLE_DATA_MAX) {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        } else {
            os_mbuf_copydata(ctxt->om, 0, len, S->val[i]);
            S->val_len[i] = len;
            if (S->count < OS_BLE_EVENTS_MAX) {
                os_ble_ev_t e = {.kind = OS_BLE_EV_WRITE, .chr = S->chr_os[i], .len = len};
                e.data = len ? malloc(len) : NULL;
                if (e.data || !len) {
                    if (len) {
                        memcpy(e.data, S->val[i], len);
                    }
                    w = push(&e);
                }
            }
        }
    } else {
        rc = BLE_ATT_ERR_UNLIKELY;
    }
    unlock();
    if (w) {
        wake_owner();
    }
    return rc;
}

esp_err_t os_ble_serve(void *ctx, const os_ble_uuid_t *service, const os_ble_chr_t *chrs, int n)
{
    if (!uuid_ok(service) || !chrs || n < 1 || n > OS_BLE_CHARS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < n; i++) {
        if (!uuid_ok(&chrs[i].uuid)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!enter(ctx)) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    bool busy = S->served || S->link != LINK_NONE;
    bool adv = S->adv_on, scan = S->scan_on;
    void *val = busy ? NULL : calloc(n, OS_BLE_DATA_MAX);
    bool nomem = !busy && !val;
    busy = busy || nomem;
    if (!busy) {
        S->val = val;
        S->served = true;
        S->n_chrs = n;
        memset(S->svcs, 0, sizeof(S->svcs));
        memset(S->chrs, 0, sizeof(S->chrs));
        to_nimble_uuid(service, &S->svc_uuid);
        for (int i = 0; i < n; i++) {
            to_nimble_uuid(&chrs[i].uuid, &S->chr_uuid[i]);
            S->chr_os[i] = chrs[i].uuid;
            S->chrs[i].uuid = &S->chr_uuid[i].u;
            S->chrs[i].access_cb = chr_access;
            S->chrs[i].arg = (void *)(intptr_t)i;
            S->chrs[i].val_handle = &S->val_handle[i];
            S->chrs[i].flags = (chrs[i].read ? BLE_GATT_CHR_F_READ : 0) |
                               (chrs[i].write ? BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP : 0) |
                               (chrs[i].notify ? BLE_GATT_CHR_F_NOTIFY : 0);
        }
        S->svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
        S->svcs[0].uuid = &S->svc_uuid.u;
        S->svcs[0].characteristics = S->chrs;
    }
    unlock();
    int rc = nomem ? BLE_HS_ENOMEM : BLE_HS_EBUSY;
    if (!busy) {
        // NimBLE takes services only while nothing advertises or scans:
        // pause both, rebuild the table (GAP and GATT services first), resume.
        if (adv) {
            ble_gap_adv_stop();
        }
        if (scan) {
            ble_gap_disc_cancel();
        }
        rc = ble_gatts_reset();
        if (rc == 0) {
            ble_svc_gap_deinit();           // frees the name init() allocates again
            ble_svc_gap_init();
            ble_svc_gatt_init();
            rc = ble_gatts_count_cfg(S->svcs);
        }
        if (rc == 0) {
            rc = ble_gatts_add_svcs(S->svcs);
        }
        if (rc == 0) {
            rc = ble_gatts_start();
        }
        if (rc) {
            lock();
            S->served = false;
            free(S->val);
            S->val = NULL;
            unlock();
        }
        if (adv) {
            adv_apply();
        } else {
            char name[OS_BLE_NAME_MAX + 1];
            lock();
            bool set = S->adv_set;
            strcpy(name, S->adv_name);
            unlock();
            if (!set) {
                default_name(name, sizeof(name));
            }
            ble_svc_gap_device_name_set(name);      // init() put NimBLE's default back
        }
        if (scan) {
            disc_start();
        }
    }
    leave();
    if (rc && rc != BLE_HS_EBUSY) {
        ESP_LOGW(TAG, "serve: %d", rc);
    }
    return rc == 0 ? ESP_OK : rc == BLE_HS_EBUSY ? ESP_ERR_INVALID_STATE : rc == BLE_HS_ENOMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
}

esp_err_t os_ble_set(void *ctx, const os_ble_uuid_t *chr, const uint8_t *data, size_t len, bool notify)
{
    if (len > OS_BLE_DATA_MAX || (len && !data) || !uuid_ok(chr)) {
        return len > OS_BLE_DATA_MAX ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
    }
    if (!enter(ctx)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    uint16_t conn = 0, handle = 0;
    lock();
    int i = -1;
    for (int k = 0; S->served && k < S->n_chrs; k++) {
        if (uuid_eq(&S->chr_os[k], chr)) {
            i = k;
        }
    }
    if (i < 0) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        memcpy(S->val[i], data, len);
        S->val_len[i] = len;
        if (notify && (S->link != LINK_OPEN || S->central)) {
            err = ESP_ERR_INVALID_STATE;
        } else if (notify && !S->subscribed[i]) {
            err = ESP_ERR_NOT_ALLOWED;
        }
        conn = S->conn;
        handle = S->val_handle[i];
    }
    unlock();
    if (err == ESP_OK && notify) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        int rc = om ? ble_gatts_notify_custom(conn, handle, om) : BLE_HS_ENOMEM;
        err = rc == 0 ? ESP_OK : rc == BLE_HS_ENOMEM ? ESP_ERR_NO_MEM : rc == BLE_HS_ENOTCONN ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    leave();
    return err;
}

// --- connecting (central) -----------------------------------------------------------------

static const char *gatt_reason(int status)
{
    if (status == BLE_HS_ETIMEOUT) {
        return "timeout";
    }
    if (status == BLE_HS_ENOTCONN) {
        return "lost";
    }
    if (status == BLE_HS_EDONE || status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND)) {
        return "no such characteristic";
    }
    return "not allowed";
}

static void kick(void)
{
    if (S) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &S->kick);
    }
}

// Under lock: the head operation is answered.
static bool op_done(bool ok, const char *reason, uint8_t *data, size_t len)
{
    bool w = false;
    if (S->n_ops) {
        op_t *op = &S->ops[0];
        if (op->id) {
            os_ble_ev_t e = {.kind = OS_BLE_EV_DONE, .op = op->id, .ok = ok, .reason = ok ? NULL : reason,
                             .chr = op->chr, .data = data, .len = len};
            w = push(&e);
            data = NULL;
        }
        free(op->data);
        S->n_ops--;
        memmove(&S->ops[0], &S->ops[1], S->n_ops * sizeof(op_t));
    }
    free(data);
    S->busy = false;
    return w;
}

static bool gen_ok(void *arg)
{
    return S && S->link == LINK_OPEN && S->central && S->link_gen == (uint32_t)(uintptr_t)arg;
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    lock();
    if (gen_ok(arg)) {
        S->busy = false;
        kick();
    }
    unlock();
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg)
{
    bool w = false;
    lock();
    if (gen_ok(arg) && S->n_ops) {
        svc_c_t *c = &S->svcc[S->n_svcc ? S->n_svcc - 1 : 0];
        if (error->status == 0 && svc) {
            if (!c->start) {
                c->start = svc->start_handle;
                c->end = svc->end_handle;
            }
        } else {
            if (error->status == BLE_HS_EDONE || error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND)) {
                c->missing = !c->start;
                S->busy = false;
            } else {
                S->n_svcc--;            // look again next time
                w = op_done(false, gatt_reason(error->status), NULL, 0);
            }
            kick();
        }
    }
    unlock();
    if (w) {
        wake_owner();
    }
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    bool w = false;
    lock();
    if (gen_ok(arg) && S->n_ops) {
        chr_c_t *c = &S->chrc[S->n_chrc ? S->n_chrc - 1 : 0];
        if (error->status == 0 && chr) {
            if (!c->val) {
                c->val = chr->val_handle;
                c->props = chr->properties;
            }
        } else {
            if (error->status == BLE_HS_EDONE || error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND)) {
                c->missing = !c->val;
                S->busy = false;
            } else {
                S->n_chrc--;
                w = op_done(false, gatt_reason(error->status), NULL, 0);
            }
            kick();
        }
    }
    unlock();
    if (w) {
        wake_owner();
    }
    return 0;
}

// Descriptors after the value, up to the next characteristic: its CCCD.
static int dsc_cb(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    bool w = false;
    lock();
    if (gen_ok(arg) && S->n_ops) {
        chr_c_t *c = NULL;
        for (int i = 0; i < S->n_chrc; i++) {
            if (S->chrc[i].val == chr_val) {
                c = &S->chrc[i];
            }
        }
        if (error->status == 0 && dsc) {
            if (c && !c->cccd_known) {
                if (ble_uuid_u16(&dsc->uuid.u) == BLE_ATT_UUID_CHARACTERISTIC) {
                    c->cccd_known = true;        // the next characteristic: none
                } else if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
                    c->cccd = dsc->handle;
                    c->cccd_known = true;
                }
            }
        } else {
            if (error->status == BLE_HS_EDONE || error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_ATTR_NOT_FOUND)) {
                if (c) {
                    c->cccd_known = true;
                }
                S->busy = false;
            } else {
                w = op_done(false, gatt_reason(error->status), NULL, 0);
            }
            kick();
        }
    }
    unlock();
    if (w) {
        wake_owner();
    }
    return 0;
}

static int attr_cb(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    bool w = false;
    lock();
    if (gen_ok(arg) && S->n_ops) {
        if (error->status == 0) {
            uint8_t *data = NULL;
            size_t len = 0;
            if (S->ops[0].kind == OP_READ && attr && attr->om) {
                len = OS_MBUF_PKTLEN(attr->om);
                len = len > OS_BLE_DATA_MAX ? OS_BLE_DATA_MAX : len;
                data = len ? malloc(len) : NULL;
                if (data) {
                    os_mbuf_copydata(attr->om, 0, len, data);
                } else {
                    len = 0;
                }
            }
            w = op_done(true, NULL, data, len);
        } else {
            w = op_done(false, gatt_reason(error->status), NULL, 0);
        }
        kick();
    }
    unlock();
    if (w) {
        wake_owner();
    }
    return 0;
}

// Host task: the next step of the head operation.
static void kick_fn(struct ble_npl_event *ev)
{
    bool w = false;
    lock();
    if (s_phase != PH_ON || !S || S->link != LINK_OPEN || !S->central || S->busy || !S->n_ops) {
        unlock();
        return;
    }
    op_t *op = &S->ops[0];
    void *gen = (void *)(uintptr_t)S->link_gen;
    uint16_t conn = S->conn;
    int rc = 0, si = -1, ci = -1;
    for (int i = 0; i < S->n_svcc; i++) {
        if (uuid_eq(&S->svcc[i].uuid, &op->svc)) {
            si = i;
        }
    }
    if (si >= 0 && S->svcc[si].missing) {
        w = op_done(false, "no such characteristic", NULL, 0);
        kick();
        unlock();
        goto out;
    }
    if (si < 0) {
        if (S->n_svcc == SVC_CACHE) {
            S->n_svcc = 0;          // forget them all (and their characteristics)
            S->n_chrc = 0;
        }
        svc_c_t *c = &S->svcc[S->n_svcc++];
        memset(c, 0, sizeof(*c));
        c->uuid = op->svc;
        ble_uuid_any_t u;
        to_nimble_uuid(&op->svc, &u);
        S->busy = true;
        unlock();
        rc = ble_gattc_disc_svc_by_uuid(conn, &u.u, svc_cb, gen);
        goto started;
    }
    for (int i = 0; i < S->n_chrc; i++) {
        if (S->chrc[i].svc == si && uuid_eq(&S->chrc[i].uuid, &op->chr)) {
            ci = i;
        }
    }
    if (ci >= 0 && S->chrc[ci].missing) {
        w = op_done(false, "no such characteristic", NULL, 0);
        kick();
        unlock();
        goto out;
    }
    if (ci < 0) {
        if (S->n_chrc == CHR_CACHE) {
            S->n_chrc = 0;
        }
        chr_c_t *c = &S->chrc[S->n_chrc++];
        memset(c, 0, sizeof(*c));
        c->svc = si;
        c->uuid = op->chr;
        ble_uuid_any_t u;
        to_nimble_uuid(&op->chr, &u);
        uint16_t start = S->svcc[si].start, end = S->svcc[si].end;
        S->busy = true;
        unlock();
        rc = ble_gattc_disc_chrs_by_uuid(conn, start, end, &u.u, chr_cb, gen);
        goto started;
    }
    chr_c_t *c = &S->chrc[ci];
    if (op->kind == OP_SUB && !c->cccd_known) {
        uint16_t val = c->val, end = S->svcc[si].end;
        if (val >= end) {
            c->cccd_known = true;           // nothing after the value: no CCCD
        } else {
            S->busy = true;
            unlock();
            // NimBLE takes the value's handle and looks after it.
            rc = ble_gattc_disc_all_dscs(conn, val, end, dsc_cb, gen);
            goto started;
        }
    }
    // The operation itself. Its data is the op's; copied out before unlocking.
    uint8_t kind = op->kind, buf[OS_BLE_DATA_MAX];
    uint16_t len = op->len, handle = c->val;
    if (len) {
        memcpy(buf, op->data, len);
    }
    if (kind == OP_SUB) {
        uint16_t v = !op->on ? 0 : (c->props & BLE_GATT_CHR_PROP_NOTIFY) ? 1 : (c->props & BLE_GATT_CHR_PROP_INDICATE) ? 2 : 0xffff;
        if (!c->cccd || v == 0xffff) {
            w = op_done(false, "not allowed", NULL, 0);
            kick();
            unlock();
            goto out;
        }
        buf[0] = v & 0xff;
        buf[1] = v >> 8;
        len = 2;
        handle = c->cccd;
    }
    S->busy = true;
    unlock();
    if (kind == OP_READ) {
        rc = ble_gattc_read(conn, handle, attr_cb, gen);
    } else if (kind == OP_WRITE_NR) {
        rc = ble_gattc_write_no_rsp_flat(conn, handle, buf, len);
        lock();
        if (gen_ok(gen) && S->n_ops) {
            op_done(rc == 0, gatt_reason(rc), NULL, 0);
            kick();
        }
        unlock();
        rc = 0;
    } else if (len + 3 > ble_att_mtu(conn)) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
        rc = om ? ble_gattc_write_long(conn, handle, 0, om, attr_cb, gen) : BLE_HS_ENOMEM;
    } else {
        rc = ble_gattc_write_flat(conn, handle, buf, len, attr_cb, gen);
    }
started:
    if (rc) {
        lock();
        if (gen_ok(gen) && S->n_ops) {
            w = op_done(false, gatt_reason(rc), NULL, 0);
            kick();
        }
        unlock();
    }
out:
    if (w) {
        wake_owner();
    }
}

static esp_err_t queue_op(void *ctx, op_kind_t kind, const os_ble_uuid_t *svc, const os_ble_uuid_t *chr,
                          const uint8_t *data, size_t len, bool on, uint32_t *op)
{
    if (op) {
        *op = 0;
    }
    if (len > OS_BLE_DATA_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!uuid_ok(svc) || !uuid_ok(chr) || (len && !data)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!enter(ctx)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    lock();
    int waiting = 0, nr = 0;
    for (int i = 0; i < S->n_ops; i++) {
        waiting += S->ops[i].id != 0;
        nr += S->ops[i].id == 0;
    }
    for (int i = 0; i < S->count; i++) {
        waiting += S->ring[(S->head + i) % RING].kind == OS_BLE_EV_DONE;
    }
    if (S->link != LINK_OPEN || !S->central) {
        err = ESP_ERR_INVALID_STATE;
    } else if (kind == OP_WRITE_NR ? nr >= OPQ - OS_BLE_OPS_MAX : waiting >= OS_BLE_OPS_MAX) {
        err = ESP_ERR_NO_MEM;
    } else {
        op_t *o = &S->ops[S->n_ops];
        memset(o, 0, sizeof(*o));
        o->data = len ? malloc(len) : NULL;
        if (len && !o->data) {
            err = ESP_ERR_NO_MEM;
        } else {
            if (len) {
                memcpy(o->data, data, len);
            }
            o->len = len;
            o->kind = kind;
            o->svc = *svc;
            o->chr = *chr;
            o->on = on;
            if (kind != OP_WRITE_NR) {
                o->id = ++S->next_op ? S->next_op : ++S->next_op;
            }
            if (op) {
                *op = o->id;
            }
            S->n_ops++;
            kick();
        }
    }
    unlock();
    leave();
    return err;
}

esp_err_t os_ble_read(void *ctx, const os_ble_uuid_t *service, const os_ble_uuid_t *chr, uint32_t *op)
{
    return queue_op(ctx, OP_READ, service, chr, NULL, 0, false, op);
}

esp_err_t os_ble_write(void *ctx, const os_ble_uuid_t *service, const os_ble_uuid_t *chr, const uint8_t *data,
                       size_t len, bool response, uint32_t *op)
{
    return queue_op(ctx, response ? OP_WRITE : OP_WRITE_NR, service, chr, data, len, false, op);
}

esp_err_t os_ble_subscribe(void *ctx, const os_ble_uuid_t *service, const os_ble_uuid_t *chr, bool on, uint32_t *op)
{
    return queue_op(ctx, OP_SUB, service, chr, NULL, 0, on, op);
}

esp_err_t os_ble_connect(void *ctx, const os_ble_addr_t *addr)
{
    if (!addr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!enter(ctx)) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    // One link, and its CLOSE taken before the next, so link events stay bounded.
    bool busy = S->link != LINK_NONE || has_event(OS_BLE_EV_CLOSE);
    bool adv = S->adv_on && S->adv_conn, scan = S->scan_on;
    uint8_t own = S->own_type;
    if (!busy) {
        S->link = LINK_CONNECTING;
        S->central = true;
        S->peer = *addr;
        S->close_reason = NULL;
        S->scan_on = false;
        if (adv) {
            S->adv_on = false;      // a device connecting to us meanwhile would take the one link
        }
    }
    unlock();
    int rc = BLE_HS_EBUSY;
    if (!busy) {
        if (scan) {
            ble_gap_disc_cancel();
        }
        if (adv) {
            ble_gap_adv_stop();
        }
        ble_addr_t a = {.type = addr->random ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC};
        for (int i = 0; i < 6; i++) {
            a.val[i] = addr->val[5 - i];
        }
        rc = ble_gap_connect(own, &a, CONNECT_MS, NULL, gap_event, (void *)1);
        if (rc) {
            lock();
            S->link = LINK_NONE;
            unlock();
        }
    }
    leave();
    if (rc && rc != BLE_HS_EBUSY) {
        ESP_LOGW(TAG, "connect: %d", rc);
    }
    return rc == 0 ? ESP_OK : rc == BLE_HS_EBUSY || rc == BLE_HS_EALREADY || rc == BLE_HS_ENOMEM ? ESP_ERR_INVALID_STATE
                                                                                                    : ESP_FAIL;
}

void os_ble_disconnect(void *ctx)
{
    if (!enter(ctx)) {
        return;
    }
    lock();
    link_t link = S->link;
    uint16_t conn = S->conn;
    if (link != LINK_NONE) {
        S->close_reason = "closed";
    }
    unlock();
    if (link == LINK_CONNECTING) {
        ble_gap_conn_cancel();
    } else if (link == LINK_OPEN) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    leave();
}

// --- GAP events (host task) ----------------------------------------------------------------

static const char *disc_reason(int reason, bool central)
{
    if (reason == BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM) || reason == BLE_HS_HCI_ERR(BLE_ERR_RD_CONN_TERM_RESRCS) ||
        reason == BLE_HS_HCI_ERR(BLE_ERR_RD_CONN_TERM_PWROFF)) {
        return "peer closed";
    }
    if (reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_ESTABLISHMENT) && central) {
        return "unreachable";
    }
    return "lost";
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    bool w = false, mtu = false;
    uint16_t conn = 0;
    uint32_t gen = 0;
    lock();
    if (!S) {
        unlock();
        return 0;
    }
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        w = on_disc(&ev->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        S->scan_on = false;
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        S->adv_on = false;
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (arg) {      // ours: ble_gap_connect
            if (ev->connect.status == 0 && S->link == LINK_CONNECTING) {
                S->link = LINK_OPEN;
                S->conn = ev->connect.conn_handle;
                S->link_gen++;
                S->busy = true;     // the MTU exchange goes first
                S->n_svcc = S->n_chrc = 0;
                mtu = true;
                conn = S->conn;
                gen = S->link_gen;
                os_ble_ev_t e = {.kind = OS_BLE_EV_OPEN, .central = true, .addr = S->peer};
                w = push(&e);
            } else if (ev->connect.status == 0) {
                ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            } else if (S->link == LINK_CONNECTING) {
                link_closed(S->close_reason ? S->close_reason : "unreachable");
                w = true;
            }
        } else if (ev->connect.status == 0) {      // a device connected to our advertisement
            S->adv_on = false;
            struct ble_gap_conn_desc d;
            if (S->link != LINK_NONE || ble_gap_conn_find(ev->connect.conn_handle, &d)) {
                ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            S->link = LINK_OPEN;
            S->central = false;
            S->conn = ev->connect.conn_handle;
            S->link_gen++;
            S->close_reason = NULL;
            to_os_addr(&d.peer_ota_addr, &S->peer);
            memset(S->subscribed, 0, sizeof(S->subscribed));
            os_ble_ev_t e = {.kind = OS_BLE_EV_OPEN, .central = false, .addr = S->peer};
            w = push(&e);
        } else {
            S->adv_on = false;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if (S->link != LINK_NONE && ev->disconnect.conn.conn_handle == S->conn) {
            link_closed(S->close_reason ? S->close_reason : disc_reason(ev->disconnect.reason, S->central));
            w = true;
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (S->link == LINK_OPEN && S->central && ev->notify_rx.conn_handle == S->conn &&
            S->count < OS_BLE_EVENTS_MAX) {
            for (int i = 0; i < S->n_chrc; i++) {
                if (S->chrc[i].val && S->chrc[i].val == ev->notify_rx.attr_handle) {
                    size_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
                    len = len > OS_BLE_DATA_MAX ? OS_BLE_DATA_MAX : len;
                    os_ble_ev_t e = {.kind = OS_BLE_EV_NOTIFY, .chr = S->chrc[i].uuid, .len = len};
                    e.data = len ? malloc(len) : NULL;
                    if (len && !e.data) {
                        break;
                    }
                    if (len) {
                        os_mbuf_copydata(ev->notify_rx.om, 0, len, e.data);
                    }
                    w = push(&e);
                    break;
                }
            }
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        int i = chr_index(ev->subscribe.attr_handle);
        if (i >= 0 && S->link == LINK_OPEN && !S->central && ev->subscribe.reason != BLE_GAP_SUBSCRIBE_REASON_TERM) {
            bool on = ev->subscribe.cur_notify || ev->subscribe.cur_indicate;
            if (on != S->subscribed[i]) {
                S->subscribed[i] = on;
                os_ble_ev_t e = {.kind = OS_BLE_EV_SUBSCRIBE, .chr = S->chr_os[i], .ok = on};
                w = push(&e);
            }
        }
        break;
    }
    default:
        break;
    }
    unlock();
    if (mtu && ble_gattc_exchange_mtu(conn, mtu_cb, (void *)(uintptr_t)gen)) {
        lock();
        if (gen_ok((void *)(uintptr_t)gen)) {
            S->busy = false;
            kick();
        }
        unlock();
    }
    if (w) {
        wake_owner();
    }
    return 0;
}

// --- on and off --------------------------------------------------------------------------------

bool os_ble_available(void)
{
    return true;
}

bool os_ble_active(void)
{
    return s_phase != PH_OFF;
}

// Blocking: stops the host (it ends a link first), frees all of it.
static void shutdown(void)
{
    for (int i = 0; i < 500 && s_calls; i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    lock();
    if (S && S->link != LINK_NONE) {
        S->close_reason = "off";
    }
    unlock();
    int rc = nimble_port_stop();
    if (rc) {
        ESP_LOGW(TAG, "stop: %d", rc);
    }
    if (S) {
        ble_npl_event_deinit(&S->kick);
    }
    nimble_port_deinit();
    lock();
    if (S) {
        drain_ring();
        for (int i = 0; i < S->n_ops; i++) {
            free(S->ops[i].data);
        }
        free(S->devs);
        free(S->val);
        free(S);
        S = NULL;
    }
    s_synced = false;
    s_phase = PH_OFF;
    unlock();
}

static void stop_task(void *arg)
{
    shutdown();
    vTaskDelete(NULL);
}

esp_err_t os_ble_start(os_ble_wake_t wake, void *ctx)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < 150 && s_phase == PH_STOPPING; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    lock();
    if (s_phase != PH_OFF) {
        bool same = s_phase == PH_ON && s_owner == ctx;
        unlock();
        return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_DEFAULT) < OS_BLE_MIN_FREE) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    s_phase = PH_STARTING;
    unlock();

    st_t *st = calloc(1, sizeof(*st));
    esp_err_t err = st ? nimble_port_init() : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init: %s", esp_err_to_name(err));
        free(st);
        lock();
        s_phase = PH_OFF;
        unlock();
        return err == ESP_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    st->conn = BLE_HS_CONN_HANDLE_NONE;
    st->own_type = BLE_OWN_ADDR_PUBLIC;
    ble_npl_event_init(&st->kick, kick_fn, NULL);
    lock();
    S = st;
    unlock();
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    char name[OS_BLE_NAME_MAX + 1];
    default_name(name, sizeof(name));
    ble_svc_gap_device_name_set(name);
    s_synced = false;
    nimble_port_freertos_init(host_task);
    for (int i = 0; i < SYNC_MS / 10 && !s_synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s_synced) {
        ESP_LOGW(TAG, "the host did not sync");
        lock();
        s_phase = PH_STOPPING;
        unlock();
        shutdown();
        return ESP_FAIL;
    }
    lock();
    s_owner = ctx;
    s_wake = wake;
    s_phase = PH_ON;
    unlock();
    return ESP_OK;
}

void os_ble_stop(void *ctx)
{
    lock();
    if (!ctx || s_phase != PH_ON || s_owner != ctx) {
        unlock();
        return;
    }
    s_owner = NULL;
    s_wake = NULL;
    drain_ring();
    s_phase = PH_STOPPING;
    unlock();
    // The host waits for a link to end: not on the caller's task (it may be
    // the OS task), unless there is no memory for one.
    if (xTaskCreate(stop_task, "ble_off", 3072, NULL, 5, NULL) != pdPASS) {
        shutdown();
    }
}

bool os_ble_address(os_ble_addr_t *out)
{
    lock();
    bool on = s_phase == PH_ON && S;
    uint8_t own = on ? S->own_type : 0;
    if (on) {
        s_calls++;
    }
    unlock();
    if (!on) {
        return false;
    }
    ble_addr_t a = {.type = own == BLE_OWN_ADDR_RANDOM ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC};
    bool ok = ble_hs_id_copy_addr(a.type, a.val, NULL) == 0;
    if (ok && out) {
        to_os_addr(&a, out);
    }
    leave();
    return ok;
}

bool os_ble_next(void *ctx, os_ble_ev_t *ev)
{
    bool ok = false;
    lock();
    if (ctx && s_owner == ctx && S && S->count) {
        *ev = S->ring[S->head];
        S->head = (S->head + 1) % RING;
        S->count--;
        ok = true;
    }
    unlock();
    return ok;
}

// --- console: "ble" -------------------------------------------------------------------------------

static char s_cmd_ctx;              // the console's own ctx
static volatile bool s_cmd_posted;

static void fmt_uuid(char *out, const os_ble_uuid_t *u)
{
    const uint8_t *b = u->b;
    if (u->len == 2) {
        sprintf(out, "%02x%02x", b[0], b[1]);
    } else {
        sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
                b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }
}

static void fmt_addr(char *out, const os_ble_addr_t *a)
{
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", a->val[0], a->val[1], a->val[2], a->val[3], a->val[4], a->val[5]);
}

static void print_hex(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        printf("%02x", d[i]);
    }
}

static void cmd_print(void *arg)
{
    static const char *kinds[] = {"scan", "open", "close", "notify", "write", "subscribe", "done"};
    os_ble_ev_t e;
    s_cmd_posted = false;
    for (int n = 0; n < RING && os_ble_next(&s_cmd_ctx, &e); n++) {
        char a[18], u[37];
        fmt_addr(a, &e.addr);
        printf("ble: %s", kinds[e.kind]);
        switch (e.kind) {
        case OS_BLE_EV_SCAN:
            printf(" %s%s rssi %d%s name \"%s\"", a, e.addr.random ? " random" : "", e.rssi,
                   e.connectable ? " connectable" : "", e.name);
            for (int i = 0; i < e.n_services; i++) {
                fmt_uuid(u, &e.services[i]);
                printf(" svc %s", u);
            }
            if (e.company || e.len) {
                printf(" company %04x data ", e.company);
                print_hex(e.data, e.len);
            }
            break;
        case OS_BLE_EV_OPEN:
            printf(" %s %s", e.central ? "central to" : "peripheral, from", a);
            break;
        case OS_BLE_EV_CLOSE:
            printf(" %s \"%s\"", e.central ? "central" : "peripheral", e.reason);
            break;
        case OS_BLE_EV_SUBSCRIBE:
            fmt_uuid(u, &e.chr);
            printf(" %s %s", u, e.ok ? "on" : "off");
            break;
        case OS_BLE_EV_DONE:
            printf(" op %u %s", (unsigned)e.op, e.ok ? "ok" : e.reason);
            if (e.ok && e.len) {
                printf(" data ");
                print_hex(e.data, e.len);
            }
            break;
        default:
            fmt_uuid(u, &e.chr);
            printf(" %s %u bytes ", u, (unsigned)e.len);
            print_hex(e.data, e.len);
            break;
        }
        printf("\n");
        free(e.data);
    }
}

static void cmd_wake(void *ctx)
{
    if (!s_cmd_posted) {
        s_cmd_posted = true;
        if (!os_post(cmd_print, NULL)) {
            s_cmd_posted = false;
        }
    }
}

static bool parse_uuid(const char *s, os_ble_uuid_t *u)
{
    char hex[33];
    int n = 0;
    for (; *s && n < 33; s++) {
        if (*s == '-') {
            continue;
        }
        hex[n++] = *s;
    }
    if (*s || (n != 4 && n != 32)) {
        return false;
    }
    u->len = n / 2;
    for (int i = 0; i < u->len; i++) {
        unsigned v;
        char two[3] = {hex[2 * i], hex[2 * i + 1], 0};
        if (sscanf(two, "%2x", &v) != 1) {
            return false;
        }
        u->b[i] = v;
    }
    return true;
}

// "0a0b..." or "t:text" -> bytes; -1 when it isn't either.
static int parse_data(const char *s, uint8_t *out, int max)
{
    if (!strncmp(s, "t:", 2)) {
        int n = strlen(s + 2);
        if (n > max) {
            return -1;
        }
        memcpy(out, s + 2, n);
        return n;
    }
    int n = strlen(s);
    if (n % 2 || n / 2 > max) {
        return -1;
    }
    for (int i = 0; i < n / 2; i++) {
        unsigned v;
        char two[3] = {s[2 * i], s[2 * i + 1], 0};
        if (sscanf(two, "%2x", &v) != 1) {
            return -1;
        }
        out[i] = v;
    }
    return n / 2;
}

static void heap_line(const char *what)
{
    printf("ble: heap %-10s %6u free, %6u min, %6u largest\n", what, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

static void cmd_off(void)
{
    os_ble_stop(&s_cmd_ctx);
    for (int i = 0; i < 300 && s_phase != PH_OFF; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static const char *err_name(esp_err_t e)
{
    return e == ESP_OK ? "ok" : esp_err_to_name(e);
}

static int cmd_ble(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    bool mine = s_owner == &s_cmd_ctx;
    if (!strcmp(sub, "status")) {
        os_ble_addr_t a;
        char as[18] = "-";
        if (os_ble_address(&a)) {
            fmt_addr(as, &a);
        }
        lock();
        printf("ble: %s, owner %s, address %s", s_phase == PH_OFF ? "off" : s_phase == PH_ON ? "on" : "busy",
               !s_owner ? "none" : mine ? "console" : "app", as);
        if (S) {
            printf(", adv %s, scan %s, served %s, link %s%s, %d events, %d ops, %d resets", S->adv_on ? "on" : "off",
                   S->scan_on ? "on" : "off", S->served ? "yes" : "no",
                   S->link == LINK_NONE ? "none" : S->link == LINK_CONNECTING ? "connecting" : "open",
                   S->link == LINK_OPEN ? (S->central ? " (central)" : " (peripheral)") : "", S->count, S->n_ops, s_resets);
        }
        unlock();
        printf("\n");
        heap_line("now");
        static const char *tasks[] = {"nimble_host", "ble_ll_task"};
        for (int i = 0; i < 2; i++) {
            TaskHandle_t t = xTaskGetHandle(tasks[i]);
            if (t) {
                printf("ble: %-12s %u B of stack never used\n", tasks[i], (unsigned)uxTaskGetStackHighWaterMark(t));
            }
        }
        return 0;
    }
    if (s_owner && !mine) {
        printf("ble: in use by an app\n");
        return 1;
    }
    if (!strcmp(sub, "on")) {
        int64_t t0 = esp_timer_get_time();
        esp_err_t e = os_ble_start(cmd_wake, &s_cmd_ctx);
        printf("ble: on: %s, %lld ms\n", err_name(e), (long long)((esp_timer_get_time() - t0) / 1000));
        heap_line("on");
        return e != ESP_OK;
    }
    if (!strcmp(sub, "off")) {
        int64_t t0 = esp_timer_get_time();
        cmd_off();
        printf("ble: off, %lld ms\n", (long long)((esp_timer_get_time() - t0) / 1000));
        heap_line("off");
        return 0;
    }
    if (!strcmp(sub, "cycle")) {        // ble cycle <n>: on and off n times, heap after each
        int n = argc > 2 ? atoi(argv[2]) : 20;
        heap_line("before");
        for (int i = 0; i < n; i++) {
            esp_err_t e = os_ble_start(cmd_wake, &s_cmd_ctx);
            if (e != ESP_OK) {
                printf("ble: cycle %d: on: %s\n", i + 1, err_name(e));
                return 1;
            }
            size_t on = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
            cmd_off();
            vTaskDelay(pdMS_TO_TICKS(50));      // the idle task frees the host task's stack
            printf("ble: cycle %2d: on %u free; off %u free, %u largest\n", i + 1, (unsigned)on,
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        }
        heap_line("after");
        return 0;
    }
    if (!mine) {
        printf("ble: off (ble on first)\n");
        return 1;
    }
    void *c = &s_cmd_ctx;
    uint8_t buf[OS_BLE_DATA_MAX];
    os_ble_uuid_t u1, u2;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    uint32_t op = 0;
    if (!strcmp(sub, "adv")) {
        // ble adv [name|-] [svc|-] [hex|-] [conn] [interval ms] | ble adv off
        if (argc > 2 && !strcmp(argv[2], "off")) {
            e = os_ble_advertise(c, NULL);
        } else {
            os_ble_adv_t a = {0};
            int n = 0;
            if (argc > 2 && strcmp(argv[2], "-")) {
                a.name = argv[2];
            }
            if (argc > 3 && strcmp(argv[3], "-")) {
                if (!parse_uuid(argv[3], &u1)) {
                    goto usage;
                }
                a.service = &u1;
            }
            if (argc > 4 && strcmp(argv[4], "-") && (n = parse_data(argv[4], buf, sizeof(buf))) < 0) {
                goto usage;
            }
            a.data = buf;
            a.len = n;
            a.connectable = argc > 5 && !strcmp(argv[5], "conn");
            a.interval_ms = argc > 6 ? atoi(argv[6]) : 0;
            e = os_ble_advertise(c, &a);
        }
        printf("ble: adv: %s\n", err_name(e));
        heap_line("adv");
    } else if (!strcmp(sub, "scan")) {
        // ble scan [secs] [active|passive] [svc|-] [duty] [name prefix] | ble scan off
        if (argc > 2 && !strcmp(argv[2], "off")) {
            e = os_ble_scan(c, NULL);
        } else {
            os_ble_scan_t s = {.seconds = argc > 2 ? atoi(argv[2]) : 10, .active = argc > 3 && !strcmp(argv[3], "active")};
            if (argc > 4 && strcmp(argv[4], "-")) {
                if (!parse_uuid(argv[4], &u1)) {
                    goto usage;
                }
                s.service = &u1;
            }
            s.duty = argc > 5 ? atoi(argv[5]) : 0;
            s.name_prefix = argc > 6 ? argv[6] : NULL;
            e = os_ble_scan(c, &s);
        }
        printf("ble: scan: %s\n", err_name(e));
    } else if (!strcmp(sub, "serve") && argc > 3) {
        // ble serve <svc> <chr>[:rwn] ...
        os_ble_chr_t ch[OS_BLE_CHARS_MAX] = {0};
        int n = 0;
        if (!parse_uuid(argv[2], &u1)) {
            goto usage;
        }
        for (int i = 3; i < argc && n < OS_BLE_CHARS_MAX; i++, n++) {
            char tmp[48];
            strlcpy(tmp, argv[i], sizeof(tmp));
            char *flags = strchr(tmp, ':');
            if (flags) {
                *flags++ = 0;
            }
            if (!parse_uuid(tmp, &ch[n].uuid)) {
                goto usage;
            }
            ch[n].read = !flags || strchr(flags, 'r');
            ch[n].write = !flags || strchr(flags, 'w');
            ch[n].notify = !flags || strchr(flags, 'n');
        }
        e = os_ble_serve(c, &u1, ch, n);
        printf("ble: serve: %s\n", err_name(e));
        heap_line("served");
    } else if ((!strcmp(sub, "set") || !strcmp(sub, "notify")) && argc > 3) {
        int n = parse_data(argv[3], buf, sizeof(buf));
        if (!parse_uuid(argv[2], &u1) || n < 0) {
            goto usage;
        }
        e = os_ble_set(c, &u1, buf, n, !strcmp(sub, "notify"));
        printf("ble: %s: %s\n", sub, err_name(e));
    } else if (!strcmp(sub, "connect") && argc > 2) {
        os_ble_addr_t a = {.random = argc > 3 && !strcmp(argv[3], "random")};
        unsigned v[6];
        if (sscanf(argv[2], "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
            goto usage;
        }
        for (int i = 0; i < 6; i++) {
            a.val[i] = v[i];
        }
        e = os_ble_connect(c, &a);
        printf("ble: connect: %s\n", err_name(e));
    } else if ((!strcmp(sub, "read") || !strcmp(sub, "write") || !strcmp(sub, "sub")) && argc > 3) {
        // ble read <svc> <chr> | ble write <svc> <chr> <hex|t:text> [nr] | ble sub <svc> <chr> [off]
        if (!parse_uuid(argv[2], &u1) || !parse_uuid(argv[3], &u2)) {
            goto usage;
        }
        if (!strcmp(sub, "read")) {
            e = os_ble_read(c, &u1, &u2, &op);
        } else if (!strcmp(sub, "sub")) {
            e = os_ble_subscribe(c, &u1, &u2, !(argc > 4 && !strcmp(argv[4], "off")), &op);
        } else {
            int n = argc > 4 ? parse_data(argv[4], buf, sizeof(buf)) : -1;
            if (n < 0) {
                goto usage;
            }
            e = os_ble_write(c, &u1, &u2, buf, n, !(argc > 5 && !strcmp(argv[5], "nr")), &op);
        }
        printf("ble: %s: %s, op %u\n", sub, err_name(e), (unsigned)op);
    } else if (!strcmp(sub, "drop")) {
        os_ble_disconnect(c);
        printf("ble: drop\n");
        return 0;
    } else {
        goto usage;
    }
    return e != ESP_OK;
usage:
    printf("ble status|on|off|cycle [n]\n"
           "    adv [name|-] [svc|-] [hex|t:text|-] [conn] [ms] | adv off\n"
           "    scan [secs] [active] [svc|-] [duty] [prefix] | scan off\n"
           "    serve <svc> <chr>[:rwn]... | set <chr> <data> | notify <chr> <data>\n"
           "    connect <aa:bb:..> [random] | read <svc> <chr> | write <svc> <chr> <data> [nr]\n"
           "    sub <svc> <chr> [off] | drop\n");
    return 1;
}

static void spare_isr(void *arg)
{
}

void os_ble_init(void)
{
    s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
    // Bluetooth on and off once, now: the first start makes what ESP-IDF
    // keeps for good (the controller's two interrupt descriptors, a libc
    // lock: ~130 B). Made at boot, they sit with the boot's own allocations
    // instead of splitting the largest free block the first time an app
    // turns Bluetooth on (measured: 116 -> 104 KB). About 20 ms.
    //
    // Which interrupt lines the controller gets depends on what else holds one
    // at that moment, and ESP-IDF makes a line's descriptor (16 B, never freed)
    // the first time the line is used. The console's UART (it starts a second
    // after a boot on USB) and anything else that takes a line later push the
    // controller onto lines it hasn't had: two new descriptors, made while an
    // app and Bluetooth fill the heap, which then stay in the middle of it
    // (measured: an app that turned Bluetooth on left the largest free block
    // at 59 KB of 145 KB free, for good). So hold a few spare lines while
    // Bluetooth starts here: every line the controller can end up on later has
    // its descriptor from now. The sources are peripherals Solana OS doesn't
    // use; the lines stay disabled and are given back at once.
    static const int spare[] = { ETS_PCNT_INTR_SOURCE, ETS_TWAI0_INTR_SOURCE, ETS_TWAI1_INTR_SOURCE,
                                 ETS_PARL_IO_RX_INTR_SOURCE, ETS_PARL_IO_TX_INTR_SOURCE, ETS_MCPWM0_INTR_SOURCE };
    intr_handle_t held[sizeof(spare) / sizeof(spare[0])] = { 0 };
    for (size_t i = 0; i < sizeof(spare) / sizeof(spare[0]); i++) {
        if (esp_intr_alloc(spare[i], ESP_INTR_FLAG_INTRDISABLED, spare_isr, NULL, &held[i]) != ESP_OK) {
            held[i] = NULL;
        }
    }
    static char boot;
    if (os_ble_start(NULL, &boot) == ESP_OK) {
        lock();
        s_owner = NULL;
        s_phase = PH_STOPPING;
        unlock();
        shutdown();
    }
    for (size_t i = 0; i < sizeof(spare) / sizeof(spare[0]); i++) {
        if (held[i]) {
            esp_intr_free(held[i]);
        }
    }
    os_cmd_register("ble", "ble [status|on|off|cycle|adv|scan|serve|set|notify|connect|read|write|sub|drop] - Bluetooth LE",
                    cmd_ble);
}
