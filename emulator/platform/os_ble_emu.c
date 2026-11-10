// Emulator counterpart of core/os_ble.c: Bluetooth LE between browser tabs.
//
// The "air" is new BroadcastChannel("solana-os-ble"), shared by the origin's
// tabs like os_radio_emu.c's mesh: messages {from, to, data} with 12-hex
// addresses ("*" for advertisements) and a base64 frame. A "ble" task polls
// the page every 10 ms and plays the radio: advertisements at their interval,
// scans (filters, duty, one report per device a second, the name only to an
// active scan), connections (10 s to answer, supervision after 6 s of
// silence, pings every second), GATT reads, writes and notifications against
// the served service. Tabs that close say so (pagehide), and their peers see
// "lost" at once; a tab the browser froze goes "lost" by supervision.
//
// Single owner and cooperative tasks: nothing here blocks, so no locks.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "core/os_ble.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"
#include "emu.h"

static const char *TAG = "os_ble";

#define POLL_MS         10
#define CONNECT_MS      10000       // "unreachable"
#define CONNECT_RETRY   200         // a connect request again, until the device advertises
#define PING_MS         1000
#define SUPERVISION_MS  6000        // "lost": a peer that went quiet (a frozen background tab too)
#define OP_TIMEOUT_MS   5000        // "timeout"
#define REPORT_MS       1000        // one report per device a second
#define SEEN_MAX        16
#define QUEUE_CAP       16          // what peers send takes the first OS_BLE_EVENTS_MAX, OPEN/CLOSE/DONE the rest
#define FRAMES_PER_POLL 64          // the page keeps 64
#define FRAME_MAX       320

enum {
    F_ADV = 1, F_CONN, F_ACCEPT, F_DISC, F_PING, F_READ, F_READ_RSP, F_WRITE, F_WRITE_RSP, F_SUB, F_SUB_RSP,
    F_NOTIFY, F_GONE,
};
enum { ST_OK, ST_NO_CHR, ST_NOT_ALLOWED };
enum { LINK_NONE, LINK_CONNECTING, LINK_OPEN };
enum { OP_READ, OP_WRITE, OP_SUB };

EM_JS(int, ble_js_open, (const char *me), {
    if (Module.ble) return 1;
    const b = (Module.ble = { me: UTF8ToString(me), rx: [], on: false, bc: null });
    if (typeof BroadcastChannel === "undefined") return 0;
    b.bc = new BroadcastChannel("solana-os-ble");
    b.bc.onmessage = (e) => {
        const msg = e.data;
        if (!b.on || !msg || typeof msg.from !== "string" || typeof msg.data !== "string") return;
        const from = msg.from.toLowerCase();
        const to = String(msg.to || "").toLowerCase();
        if (!/^[0-9a-f]{12}$/.test(from) || from === b.me) return;
        if (to !== b.me && to !== "*") return;
        if (b.rx.length >= 64) {
            const i = b.rx.findIndex((f) => f.adv);
            b.rx.splice(i >= 0 ? i : 0, 1);
        }
        b.rx.push({ from, adv: to === "*", data: msg.data });
    };
    if (typeof addEventListener === "function") {
        addEventListener("pagehide", () => {
            if (b.on) b.bc.postMessage({ from: b.me, to: "*", data: btoa(String.fromCharCode(13)) });
        });
    }
    return 1;
});

EM_JS(void, ble_js_listen, (int on), {
    const b = Module.ble;
    if (!b) return;
    b.on = !!on;
    if (!on) b.rx.length = 0;
});

// Next frame: its length (bytes in buf, sender in from), or -1.
EM_JS(int, ble_js_take, (uint8_t *from, uint8_t *buf, int cap), {
    const b = Module.ble;
    while (b && b.rx.length) {
        const f = b.rx.shift();
        let bin;
        try { bin = atob(f.data); } catch (e) { continue; }
        if (!bin.length || bin.length > cap) continue;
        for (let i = 0; i < 6; i++) HEAPU8[from + i] = parseInt(f.from.substr(i * 2, 2), 16);
        for (let i = 0; i < bin.length; i++) HEAPU8[buf + i] = bin.charCodeAt(i);
        return bin.length;
    }
    return -1;
});

EM_JS(void, ble_js_send, (const char *to, const uint8_t *data, int len), {
    const b = Module.ble;
    if (!b || !b.bc) return;
    let s = "";
    for (let i = 0; i < len; i++) s += String.fromCharCode(HEAPU8[data + i]);
    b.bc.postMessage({ from: b.me, to: UTF8ToString(to), data: btoa(s) });
});

typedef struct {
    os_ble_uuid_t uuid;
    bool read, write, notify, subscribed;
    uint8_t val[OS_BLE_DATA_MAX];
    size_t len;
} chr_t;

typedef struct {
    uint32_t op;                    // 0: free
    uint8_t kind;
    int64_t deadline;
} op_t;

static bool s_ok;                   // BroadcastChannel exists
static uint8_t s_addr[6];
static void *s_owner;
static os_ble_wake_t s_wake;

static struct {
    bool on, connectable;
    char name[OS_BLE_NAME_MAX + 1];
    os_ble_uuid_t service;
    bool has_service;
    uint8_t data[OS_BLE_ADV_MAX];
    size_t len;
    uint16_t interval;
    int64_t next;
} s_adv;

static struct {
    bool on, active, by_service;
    os_ble_uuid_t service;
    char prefix[OS_BLE_NAME_MAX + 1];
    uint8_t duty;
    int64_t end;                    // 0: until stopped
    struct {
        uint8_t addr[6];
        int64_t last;
    } seen[SEEN_MAX];
} s_scan;

static struct {
    bool on;
    os_ble_uuid_t service;
    chr_t chrs[OS_BLE_CHARS_MAX];
    int n;
} s_srv;

static struct {
    uint8_t state;                  // LINK_*
    bool central;
    uint8_t peer[6];
    uint32_t id;
    int64_t started, last_rx, next_ping, next_try;
    op_t ops[OS_BLE_OPS_MAX];
    uint32_t op_seq;
} s_link;

static os_ble_ev_t s_q[QUEUE_CAP];
static int s_qhead, s_qlen;

static int64_t now_ms(void)
{
    return emu_now_us() / 1000;
}

static void hex_addr(const uint8_t *a, char out[13])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 2] = hex[a[i] >> 4];
        out[i * 2 + 1] = hex[a[i] & 15];
    }
    out[12] = 0;
}

static void send_to(const uint8_t *to, const uint8_t *frame, int len)
{
    char hex[13] = "*";
    if (to) {
        hex_addr(to, hex);
    }
    ble_js_send(hex, frame, len);
}

static bool uuid_eq(const os_ble_uuid_t *a, const os_ble_uuid_t *b)
{
    return a->len == b->len && !memcmp(a->b, b->b, a->len);
}

// ---------------------------------------------------------------------------
// Events

// false: no room; data is then freed. As on the badge (os_ble.h), what peers
// send (SCAN, NOTIFY, WRITE, SUBSCRIBE) is dropped once OS_BLE_EVENTS_MAX wait,
// so apps meet the same losses here; OPEN, CLOSE and DONE have the rest.
static bool push(os_ble_ev_t *ev)
{
    bool peer = ev->kind == OS_BLE_EV_SCAN || ev->kind == OS_BLE_EV_NOTIFY || ev->kind == OS_BLE_EV_WRITE ||
                ev->kind == OS_BLE_EV_SUBSCRIBE;
    if (!s_owner || s_qlen >= (peer ? OS_BLE_EVENTS_MAX : QUEUE_CAP)) {
        if (!peer) {
            ESP_LOGW(TAG, "event queue full: link event dropped");
        }
        free(ev->data);
        return false;
    }
    s_q[(s_qhead + s_qlen) % QUEUE_CAP] = *ev;
    s_qlen++;
    if (s_wake) {
        s_wake(s_owner);
    }
    return true;
}

static uint8_t *dup(const uint8_t *p, size_t n)
{
    if (!n) {
        return NULL;
    }
    uint8_t *d = malloc(n);
    if (d) {
        memcpy(d, p, n);
    }
    return d;
}

static void push_done(uint32_t op, bool ok, const char *reason, const uint8_t *data, size_t len)
{
    os_ble_ev_t ev = { .kind = OS_BLE_EV_DONE, .op = op, .ok = ok, .reason = reason };
    ev.data = ok ? dup(data, len) : NULL;
    ev.len = ev.data ? len : 0;
    push(&ev);
}

static void push_close(bool central, const char *reason)
{
    os_ble_ev_t ev = { .kind = OS_BLE_EV_CLOSE, .central = central, .reason = reason };
    memcpy(ev.addr.val, s_link.peer, 6);
    push(&ev);
}

static void end_link(const char *reason)
{
    if (s_link.state == LINK_NONE) {
        return;
    }
    if (s_link.central) {
        for (int i = 0; i < OS_BLE_OPS_MAX; i++) {
            if (s_link.ops[i].op) {
                push_done(s_link.ops[i].op, false, "lost", NULL, 0);
                s_link.ops[i].op = 0;
            }
        }
    } else {
        for (int i = 0; i < s_srv.n; i++) {
            s_srv.chrs[i].subscribed = false;
        }
    }
    bool central = s_link.central;
    s_link.state = LINK_NONE;
    push_close(central, reason);
}

// ---------------------------------------------------------------------------
// Frames

typedef struct {
    uint8_t b[FRAME_MAX];
    int n;
} wr_t;

static void w8(wr_t *w, uint8_t v)
{
    if (w->n < FRAME_MAX) {
        w->b[w->n++] = v;
    }
}

static void w32(wr_t *w, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        w8(w, (uint8_t)(v >> (24 - i * 8)));
    }
}

static void wbytes(wr_t *w, const uint8_t *p, size_t n)
{
    w8(w, (uint8_t)n);
    for (size_t i = 0; i < n; i++) {
        w8(w, p[i]);
    }
}

static void wuuid(wr_t *w, const os_ble_uuid_t *u)
{
    wbytes(w, u->b, u->len);
}

typedef struct {
    const uint8_t *b;
    int n, o;
    bool bad;
} rd_t;

static uint8_t r8(rd_t *r)
{
    if (r->o >= r->n) {
        r->bad = true;
        return 0;
    }
    return r->b[r->o++];
}

static uint32_t r32(rd_t *r)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = v << 8 | r8(r);
    }
    return v;
}

// A length-prefixed run of at most max bytes; NULL (and bad) when it doesn't fit.
static const uint8_t *rbytes(rd_t *r, size_t max, size_t *len)
{
    size_t n = r8(r);
    if (r->bad || n > max || r->o + (int)n > r->n) {
        r->bad = true;
        *len = 0;
        return NULL;
    }
    const uint8_t *p = r->b + r->o;
    r->o += (int)n;
    *len = n;
    return p;
}

static bool ruuid(rd_t *r, os_ble_uuid_t *u)
{
    size_t n;
    const uint8_t *p = rbytes(r, 16, &n);
    if (!p || (n != 2 && n != 16)) {
        r->bad = true;
        return false;
    }
    u->len = (uint8_t)n;
    memcpy(u->b, p, n);
    return true;
}

static void send_link(uint8_t type)
{
    wr_t w = { .n = 0 };
    w8(&w, type);
    w32(&w, s_link.id);
    send_to(s_link.peer, w.b, w.n);
}

static void send_adv(void)
{
    wr_t w = { .n = 0 };
    w8(&w, F_ADV);
    w8(&w, s_adv.connectable ? 1 : 0);
    wbytes(&w, (const uint8_t *)s_adv.name, strlen(s_adv.name));
    if (s_adv.has_service) {
        wuuid(&w, &s_adv.service);
    } else {
        w8(&w, 0);
    }
    wbytes(&w, s_adv.data, s_adv.len);
    send_to(NULL, w.b, w.n);
}

static int fake_rssi(const uint8_t *a)
{
    return -45 - (a[5] % 30) - (int)(esp_random() % 5);
}

static void on_adv(const uint8_t *from, rd_t *r, int64_t now)
{
    if (!s_scan.on) {
        return;
    }
    bool conn = r8(r) & 1;
    size_t nlen, dlen, slen;
    const uint8_t *name = rbytes(r, OS_BLE_NAME_MAX, &nlen);
    const uint8_t *svc = rbytes(r, 16, &slen);
    const uint8_t *data = rbytes(r, OS_BLE_ADV_MAX, &dlen);
    if (r->bad || (slen != 0 && slen != 2 && slen != 16)) {
        return;
    }
    (void)name;
    os_ble_uuid_t u = { .len = (uint8_t)slen };
    if (slen) {
        memcpy(u.b, svc, slen);
    }
    if (s_scan.by_service && (!slen || !uuid_eq(&u, &s_scan.service))) {
        return;
    }
    char text[OS_BLE_NAME_MAX + 1] = "";
    if (s_scan.active) {                    // the name is in the scan response
        for (size_t i = 0; i < nlen; i++) {
            text[i] = name[i] >= 0x20 && name[i] <= 0x7E ? (char)name[i] : '?';
        }
        text[nlen] = 0;
    }
    if (s_scan.prefix[0] && strncmp(text, s_scan.prefix, strlen(s_scan.prefix))) {
        return;
    }
    if ((int)(esp_random() % 100) >= s_scan.duty) {
        return;                             // the radio wasn't listening
    }
    int slot = -1, oldest = 0;
    for (int i = 0; i < SEEN_MAX; i++) {
        if (!memcmp(s_scan.seen[i].addr, from, 6)) {
            slot = i;
            break;
        }
        if (s_scan.seen[i].last < s_scan.seen[oldest].last) {
            oldest = i;
        }
    }
    if (slot >= 0 && now - s_scan.seen[slot].last < REPORT_MS) {
        return;
    }
    os_ble_ev_t ev = { .kind = OS_BLE_EV_SCAN, .rssi = (int8_t)fake_rssi(from), .connectable = conn };
    memcpy(ev.addr.val, from, 6);
    strlcpy(ev.name, text, sizeof(ev.name));
    if (slen) {
        ev.services[0] = u;
        ev.n_services = 1;
    }
    if (dlen) {
        ev.company = 0xFFFF;
        ev.data = dup(data, dlen);
        ev.len = ev.data ? dlen : 0;
    }
    if (push(&ev)) {
        slot = slot >= 0 ? slot : oldest;
        memcpy(s_scan.seen[slot].addr, from, 6);
        s_scan.seen[slot].last = now;
    }
}

static chr_t *served(const os_ble_uuid_t *svc, const os_ble_uuid_t *chr)
{
    if (!s_srv.on || (svc && !uuid_eq(svc, &s_srv.service))) {
        return NULL;
    }
    for (int i = 0; i < s_srv.n; i++) {
        if (uuid_eq(&s_srv.chrs[i].uuid, chr)) {
            return &s_srv.chrs[i];
        }
    }
    return NULL;
}

static op_t *find_op(uint32_t op)
{
    for (int i = 0; i < OS_BLE_OPS_MAX; i++) {
        if (op && s_link.ops[i].op == op) {
            return &s_link.ops[i];
        }
    }
    return NULL;
}

static const char *status_reason(uint8_t st)
{
    return st == ST_NO_CHR ? "no such characteristic" : "not allowed";
}

// A frame on the link: from the peer, with the link's id.
static void on_link_frame(uint8_t type, rd_t *r)
{
    wr_t w = { .n = 0 };
    os_ble_uuid_t svc, chr;
    size_t len;
    const uint8_t *data;
    switch (type) {
    case F_DISC:
        end_link("peer closed");
        break;
    case F_PING:
        break;
    case F_READ: {
        uint32_t op = r32(r);
        if (!ruuid(r, &svc) || !ruuid(r, &chr) || s_link.central) {
            return;
        }
        chr_t *c = served(&svc, &chr);
        uint8_t st = !c ? ST_NO_CHR : !c->read ? ST_NOT_ALLOWED : ST_OK;
        w8(&w, F_READ_RSP);
        w32(&w, s_link.id);
        w32(&w, op);
        w8(&w, st);
        wbytes(&w, c ? c->val : NULL, st == ST_OK ? c->len : 0);
        send_to(s_link.peer, w.b, w.n);
        break;
    }
    case F_WRITE: {
        uint32_t op = r32(r);
        bool resp = r8(r);
        ruuid(r, &svc);
        ruuid(r, &chr);
        data = rbytes(r, OS_BLE_DATA_MAX, &len);
        if (r->bad || s_link.central) {
            return;
        }
        chr_t *c = served(&svc, &chr);
        uint8_t st = !c ? ST_NO_CHR : !c->write ? ST_NOT_ALLOWED : ST_OK;
        if (st == ST_OK) {
            memcpy(c->val, data, len);
            c->len = len;
            os_ble_ev_t ev = { .kind = OS_BLE_EV_WRITE, .chr = chr, .data = dup(data, len), .len = len };
            ev.len = ev.data ? len : 0;
            push(&ev);
        }
        if (resp) {
            w8(&w, F_WRITE_RSP);
            w32(&w, s_link.id);
            w32(&w, op);
            w8(&w, st);
            send_to(s_link.peer, w.b, w.n);
        }
        break;
    }
    case F_SUB: {
        uint32_t op = r32(r);
        bool on = r8(r);
        if (!ruuid(r, &svc) || !ruuid(r, &chr) || s_link.central) {
            return;
        }
        chr_t *c = served(&svc, &chr);
        uint8_t st = !c ? ST_NO_CHR : !c->notify ? ST_NOT_ALLOWED : ST_OK;
        if (st == ST_OK && c->subscribed != on) {
            c->subscribed = on;
            os_ble_ev_t ev = { .kind = OS_BLE_EV_SUBSCRIBE, .chr = chr, .ok = on };
            push(&ev);
        }
        w8(&w, F_SUB_RSP);
        w32(&w, s_link.id);
        w32(&w, op);
        w8(&w, st);
        send_to(s_link.peer, w.b, w.n);
        break;
    }
    case F_READ_RSP:
    case F_WRITE_RSP:
    case F_SUB_RSP: {
        uint32_t id = r32(r);
        uint8_t st = r8(r);
        data = type == F_READ_RSP ? rbytes(r, OS_BLE_DATA_MAX, &len) : NULL;
        op_t *o = s_link.central && !r->bad ? find_op(id) : NULL;
        if (o) {
            o->op = 0;
            push_done(id, st == ST_OK, st == ST_OK ? NULL : status_reason(st), data, data ? len : 0);
        }
        break;
    }
    case F_NOTIFY:
        ruuid(r, &chr);
        data = rbytes(r, OS_BLE_DATA_MAX, &len);
        if (r->bad || !s_link.central) {
            return;
        }
        {
            os_ble_ev_t ev = { .kind = OS_BLE_EV_NOTIFY, .chr = chr, .data = dup(data, len), .len = len };
            ev.len = ev.data ? len : 0;
            push(&ev);
        }
        break;
    }
}

static void on_frame(const uint8_t *from, const uint8_t *buf, int n, int64_t now)
{
    rd_t r = { .b = buf, .n = n };
    uint8_t type = r8(&r);
    if (type == F_ADV) {
        on_adv(from, &r, now);
        return;
    }
    bool with_peer = s_link.state != LINK_NONE && !memcmp(s_link.peer, from, 6);
    if (type == F_GONE) {
        if (with_peer && s_link.state == LINK_OPEN) {
            end_link("lost");
        }
        return;
    }
    uint32_t id = r32(&r);
    if (r.bad) {
        return;
    }
    if (type == F_CONN) {
        if (s_link.state == LINK_NONE && s_adv.on && s_adv.connectable) {
            s_link = (typeof(s_link)){ .state = LINK_OPEN, .central = false, .id = id, .last_rx = now,
                                       .next_ping = now + PING_MS };
            memcpy(s_link.peer, from, 6);
            s_adv.on = false;               // advertising stops while a device is connected
            send_link(F_ACCEPT);
            os_ble_ev_t ev = { .kind = OS_BLE_EV_OPEN, .central = false };
            memcpy(ev.addr.val, from, 6);
            push(&ev);
        } else if (with_peer && s_link.id == id && !s_link.central) {
            send_link(F_ACCEPT);            // our accept crossed a retry
        }
        return;
    }
    if (type == F_ACCEPT) {
        if (with_peer && s_link.id == id && s_link.state == LINK_CONNECTING) {
            s_link.state = LINK_OPEN;
            s_link.last_rx = now;
            s_link.next_ping = now + PING_MS;
            os_ble_ev_t ev = { .kind = OS_BLE_EV_OPEN, .central = true };
            memcpy(ev.addr.val, from, 6);
            push(&ev);
        } else if (!(with_peer && s_link.id == id)) {
            wr_t w = { .n = 0 };            // we gave up on it: hang up so the peer does too
            w8(&w, F_DISC);
            w32(&w, id);
            send_to(from, w.b, w.n);
        }
        return;
    }
    if (!with_peer || s_link.id != id || s_link.state != LINK_OPEN) {
        return;
    }
    s_link.last_rx = now;
    on_link_frame(type, &r);
}

static void timers(int64_t now)
{
    if (s_scan.on && s_scan.end && now >= s_scan.end) {
        s_scan.on = false;
    }
    bool peripheral = s_link.state != LINK_NONE && !s_link.central;
    if (s_adv.on && !peripheral && now >= s_adv.next) {
        send_adv();
        s_adv.next = now + s_adv.interval;
    }
    if (s_link.state == LINK_CONNECTING) {
        if (now - s_link.started >= CONNECT_MS) {
            end_link("unreachable");
        } else if (now >= s_link.next_try) {
            send_link(F_CONN);
            s_link.next_try = now + CONNECT_RETRY;
        }
    } else if (s_link.state == LINK_OPEN) {
        if (now - s_link.last_rx >= SUPERVISION_MS) {
            end_link("lost");
            return;
        }
        if (now >= s_link.next_ping) {
            send_link(F_PING);
            s_link.next_ping = now + PING_MS;
        }
        for (int i = 0; i < OS_BLE_OPS_MAX; i++) {
            if (s_link.ops[i].op && now >= s_link.ops[i].deadline) {
                uint32_t op = s_link.ops[i].op;
                s_link.ops[i].op = 0;
                push_done(op, false, "timeout", NULL, 0);
            }
        }
    }
}

static void ble_task(void *arg)
{
    uint8_t from[6], buf[FRAME_MAX];
    for (;;) {
        if (s_owner) {
            int64_t now = now_ms();
            int n;
            // Every frame, as a radio would: a slow app loses what peers send
            // (push), not the link's pings.
            for (int k = 0; s_owner && k < FRAMES_PER_POLL && (n = ble_js_take(from, buf, sizeof(buf))) > 0; k++) {
                on_frame(from, buf, n, now);
            }
            if (s_owner) {
                timers(now);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(s_owner ? POLL_MS : 100));
    }
}

// ---------------------------------------------------------------------------
// os_ble.h

static void reset(void)
{
    for (int i = 0; i < s_qlen; i++) {
        free(s_q[(s_qhead + i) % QUEUE_CAP].data);
    }
    s_qhead = s_qlen = 0;
    memset(&s_adv, 0, sizeof(s_adv));
    memset(&s_scan, 0, sizeof(s_scan));
    memset(&s_srv, 0, sizeof(s_srv));
    uint32_t seq = s_link.op_seq;
    memset(&s_link, 0, sizeof(s_link));
    s_link.op_seq = seq;
}

bool os_ble_available(void)
{
    return s_ok;
}

esp_err_t os_ble_start(os_ble_wake_t wake, void *ctx)
{
    if (!s_ok) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_owner) {
        return s_owner == ctx ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_DEFAULT) < OS_BLE_MIN_FREE) {
        return ESP_ERR_NO_MEM;
    }
    reset();
    s_owner = ctx;
    s_wake = wake;
    ble_js_listen(1);
    ESP_LOGI(TAG, "on");
    return ESP_OK;
}

void os_ble_stop(void *ctx)
{
    if (!ctx || s_owner != ctx) {
        return;
    }
    if (s_link.state != LINK_NONE) {
        send_link(F_DISC);
    }
    s_owner = NULL;
    s_wake = NULL;
    reset();
    ble_js_listen(0);
    ESP_LOGI(TAG, "off");
}

bool os_ble_active(void)
{
    return s_owner != NULL;
}

bool os_ble_address(os_ble_addr_t *out)
{
    if (!s_owner) {
        return false;
    }
    memcpy(out->val, s_addr, 6);
    out->random = false;
    return true;
}

esp_err_t os_ble_advertise(void *ctx, const os_ble_adv_t *adv)
{
    if (!ctx || ctx != s_owner) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!adv) {
        s_adv.on = false;
        return ESP_OK;
    }
    size_t max = !adv->service ? OS_BLE_ADV_MAX : adv->service->len == 2 ? OS_BLE_ADV_MAX - 4 : OS_BLE_ADV_MAX - 18;
    if ((adv->name && strlen(adv->name) > OS_BLE_NAME_MAX) || adv->len > max) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (adv->interval_ms && (adv->interval_ms < 20 || adv->interval_ms > 10000)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adv->connectable && s_link.state != LINK_NONE) {
        return ESP_ERR_INVALID_STATE;       // one link: nobody may connect meanwhile
    }
    if (adv->name) {
        strlcpy(s_adv.name, adv->name, sizeof(s_adv.name));
    } else {
        os_cloud_info_t *in = malloc(sizeof(*in));
        if (in) {
            os_cloud_get_info(in);
        }
        snprintf(s_adv.name, sizeof(s_adv.name), in && in->id[0] ? "Badge %s" : "Badge", in ? in->id : "");
        free(in);
    }
    s_adv.has_service = adv->service != NULL;
    if (adv->service) {
        s_adv.service = *adv->service;
    }
    if (adv->len) {
        memcpy(s_adv.data, adv->data, adv->len);
    }
    s_adv.len = adv->len;
    s_adv.connectable = adv->connectable;
    s_adv.interval = adv->interval_ms ? adv->interval_ms : 100;
    s_adv.on = true;
    s_adv.next = 0;
    return ESP_OK;
}

esp_err_t os_ble_scan(void *ctx, const os_ble_scan_t *scan)
{
    if (!ctx || ctx != s_owner) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!scan) {
        s_scan.on = false;
        return ESP_OK;
    }
    if (s_link.state == LINK_CONNECTING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (scan->name_prefix && strlen(scan->name_prefix) > OS_BLE_NAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(&s_scan, 0, sizeof(s_scan));
    s_scan.by_service = scan->service != NULL;
    if (scan->service) {
        s_scan.service = *scan->service;
    }
    if (scan->name_prefix) {
        strlcpy(s_scan.prefix, scan->name_prefix, sizeof(s_scan.prefix));
    }
    s_scan.active = scan->active;
    s_scan.duty = !scan->duty ? 30 : scan->duty < 10 ? 10 : scan->duty > 100 ? 100 : scan->duty;
    s_scan.end = scan->seconds ? now_ms() + (int64_t)scan->seconds * 1000 : 0;
    for (int i = 0; i < SEEN_MAX; i++) {
        s_scan.seen[i].last = INT64_MIN / 2;
    }
    s_scan.on = true;
    return ESP_OK;
}

esp_err_t os_ble_serve(void *ctx, const os_ble_uuid_t *service, const os_ble_chr_t *chrs, int n)
{
    if (!ctx || ctx != s_owner || s_srv.on || s_link.state != LINK_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!service || !chrs || n < 1 || n > OS_BLE_CHARS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    s_srv.service = *service;
    for (int i = 0; i < n; i++) {
        s_srv.chrs[i] = (chr_t){ .uuid = chrs[i].uuid, .read = chrs[i].read, .write = chrs[i].write,
                                 .notify = chrs[i].notify };
    }
    s_srv.n = n;
    s_srv.on = true;
    return ESP_OK;
}

esp_err_t os_ble_set(void *ctx, const os_ble_uuid_t *chr, const uint8_t *data, size_t len, bool notify)
{
    if (!ctx || ctx != s_owner) {
        return ESP_ERR_INVALID_STATE;
    }
    chr_t *c = served(NULL, chr);
    if (!c) {
        return ESP_ERR_NOT_FOUND;
    }
    if (len > OS_BLE_DATA_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (len) {
        memcpy(c->val, data, len);
    }
    c->len = len;
    if (!notify) {
        return ESP_OK;
    }
    if (s_link.state != LINK_OPEN || s_link.central) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!c->subscribed) {
        return ESP_ERR_NOT_ALLOWED;
    }
    wr_t w = { .n = 0 };
    w8(&w, F_NOTIFY);
    w32(&w, s_link.id);
    wuuid(&w, chr);
    wbytes(&w, data, len);
    send_to(s_link.peer, w.b, w.n);
    return ESP_OK;
}

// One link, and the last one's CLOSE taken before the next, as on the badge.
static bool close_waiting(void)
{
    for (int i = 0; i < s_qlen; i++) {
        if (s_q[(s_qhead + i) % QUEUE_CAP].kind == OS_BLE_EV_CLOSE) {
            return true;
        }
    }
    return false;
}

esp_err_t os_ble_connect(void *ctx, const os_ble_addr_t *addr)
{
    if (!ctx || ctx != s_owner || s_link.state != LINK_NONE || close_waiting()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_scan.on = false;                      // a scan ends while the badge connects
    if (s_adv.connectable) {
        s_adv.on = false;                   // a device connecting to us meanwhile would take the one link
    }
    uint32_t seq = s_link.op_seq;
    int64_t now = now_ms();
    s_link = (typeof(s_link)){ .state = LINK_CONNECTING, .central = true, .started = now, .next_try = now,
                               .op_seq = seq };
    memcpy(s_link.peer, addr->val, 6);
    do {
        s_link.id = esp_random();
    } while (!s_link.id);
    return ESP_OK;
}

static esp_err_t start_op(void *ctx, uint8_t kind, bool answered, uint32_t *op)
{
    *op = 0;
    if (!ctx || ctx != s_owner || s_link.state != LINK_OPEN || !s_link.central) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!answered) {
        return ESP_OK;
    }
    for (int i = 0; i < OS_BLE_OPS_MAX; i++) {
        if (!s_link.ops[i].op) {
            do {
                *op = ++s_link.op_seq;
            } while (!*op);
            s_link.ops[i] = (op_t){ .op = *op, .kind = kind, .deadline = now_ms() + OP_TIMEOUT_MS };
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static void op_frame(wr_t *w, uint8_t type, uint32_t op)
{
    w8(w, type);
    w32(w, s_link.id);
    w32(w, op);
}

esp_err_t os_ble_read(void *ctx, const os_ble_uuid_t *service, const os_ble_uuid_t *chr, uint32_t *op)
{
    esp_err_t err = start_op(ctx, OP_READ, true, op);
    if (err == ESP_OK) {
        wr_t w = { .n = 0 };
        op_frame(&w, F_READ, *op);
        wuuid(&w, service);
        wuuid(&w, chr);
        send_to(s_link.peer, w.b, w.n);
    }
    return err;
}

esp_err_t os_ble_write(void *ctx, const os_ble_uuid_t *service, const os_ble_uuid_t *chr, const uint8_t *data,
                       size_t len, bool response, uint32_t *op)
{
    if (len > OS_BLE_DATA_MAX) {
        *op = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = start_op(ctx, OP_WRITE, response, op);
    if (err == ESP_OK) {
        wr_t w = { .n = 0 };
        op_frame(&w, F_WRITE, *op);
        w8(&w, response ? 1 : 0);
        wuuid(&w, service);
        wuuid(&w, chr);
        wbytes(&w, data, len);
        send_to(s_link.peer, w.b, w.n);
    }
    return err;
}

esp_err_t os_ble_subscribe(void *ctx, const os_ble_uuid_t *service, const os_ble_uuid_t *chr, bool on, uint32_t *op)
{
    esp_err_t err = start_op(ctx, OP_SUB, true, op);
    if (err == ESP_OK) {
        wr_t w = { .n = 0 };
        op_frame(&w, F_SUB, *op);
        w8(&w, on ? 1 : 0);
        wuuid(&w, service);
        wuuid(&w, chr);
        send_to(s_link.peer, w.b, w.n);
    }
    return err;
}

void os_ble_disconnect(void *ctx)
{
    if (!ctx || ctx != s_owner || s_link.state == LINK_NONE) {
        return;
    }
    send_link(F_DISC);
    end_link("closed");
}

bool os_ble_next(void *ctx, os_ble_ev_t *ev)
{
    if (!ctx || ctx != s_owner || !s_qlen) {
        return false;
    }
    *ev = s_q[s_qhead];
    s_qhead = (s_qhead + 1) % QUEUE_CAP;
    s_qlen--;
    return true;
}

// ---------------------------------------------------------------------------
// Console

static void print_uuid(const os_ble_uuid_t *u)
{
    for (int i = 0; i < u->len; i++) {
        printf("%s%02x", u->len == 16 && (i == 4 || i == 6 || i == 8 || i == 10) ? "-" : "", u->b[i]);
    }
}

static int cmd_ble(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "status")) {
        printf("usage: ble [status] (the emulator's Bluetooth is a Lua app's: ble.on() in the app)\n");
        return 1;
    }
    char a[13];
    hex_addr(s_addr, a);
    printf("ble: %s, address %c%c:%c%c:%c%c:%c%c:%c%c:%c%c (emulated: tabs of this browser)\n",
           s_owner ? "on" : "off", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
    if (!s_owner) {
        return 0;
    }
    if (s_adv.on) {
        printf("advertising: \"%s\"%s, every %u ms, %u bytes", s_adv.name, s_adv.connectable ? ", connectable" : "",
               (unsigned)s_adv.interval, (unsigned)s_adv.len);
        if (s_adv.has_service) {
            printf(", service ");
            print_uuid(&s_adv.service);
        }
        printf("\n");
    }
    if (s_scan.on) {
        printf("scanning: %s, duty %u%%", s_scan.active ? "active" : "passive", (unsigned)s_scan.duty);
        if (s_scan.by_service) {
            printf(", service ");
            print_uuid(&s_scan.service);
        }
        if (s_scan.prefix[0]) {
            printf(", name \"%s\"", s_scan.prefix);
        }
        printf("\n");
    }
    if (s_srv.on) {
        printf("serving ");
        print_uuid(&s_srv.service);
        printf(": %d characteristic%s\n", s_srv.n, s_srv.n == 1 ? "" : "s");
    }
    if (s_link.state != LINK_NONE) {
        hex_addr(s_link.peer, a);
        printf("link: %s %s %s\n", s_link.state == LINK_OPEN ? "open" : "connecting", s_link.central ? "to" : "from",
               a);
    }
    printf("events waiting: %d\n", s_qlen);
    return 0;
}

void os_ble_init(void)
{
    memcpy(s_addr, emu_mac(), 6);
    s_addr[5] += 2;                         // an ESP32's Bluetooth address: its base MAC + 2
    char me[13];
    hex_addr(s_addr, me);
    s_ok = ble_js_open(me);
    if (!s_ok) {
        ESP_LOGW(TAG, "no BroadcastChannel in this browser: no Bluetooth");
    }
    xTaskCreate(ble_task, "ble", 4096, NULL, 22, NULL);
    os_cmd_register("ble", "ble [status]", cmd_ble);
}
