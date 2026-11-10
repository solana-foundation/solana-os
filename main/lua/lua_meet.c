// mesh.meet for Lua apps (docs/LUA_API.md "Meets", docs/MESH.md "Meets"): what
// X Dex does for cards, for any app. Two badges running the same app sign one
// statement, each with its mesh key, and each queues it in the outbox with the
// *other* badge's tag, so whichever gets Wi-Fi first settles it for both.
//
//   B (asks)                                 A (its app accepts)
//     req {n nB, t, v, d dB}       ------>     on_meet(from, dB) returns dA
//                                  <------     ok {r nB, n nA, t, v, d dA, tag(kA, meet|A|T)}
//     fin {r nB, tag(kB, meet|B|T)} ----->
//
// The host builds every signed string and puts the app's id in it, so a script
// can't get a tag that counts anywhere else (X Dex, trades, relays). Everything
// here runs on the Lua task: the mesh task only queues the message (lua_net.c).
// Frames get lost, so B repeats "req", A answers a repeated "req" with the
// same "ok", and B a repeated "ok" with the same "fin".

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_mesh.h"
#include "lua_priv.h"

#define MEET_RESEND_MS  1500
#define MEET_WAIT_MS    12000
#define MEET_GAP_MS     1000        // between meets asked, and between new ones accepted
#define MEET_BOX_MAX    40          // uploads waiting for Wi-Fi: no new meets beyond
#define NONCE_LEN       16
#define STATEMENT_MAX   (48 + LUA_PKG_ID_MAX + 2 * (OS_MESH_ID_LEN + NONCE_LEN + 10 + 4 + LH_MEET_MAX))

// One statement. The ids come from the roles: `peer` is the other badge.
typedef struct {
    char peer[OS_MESH_ID_LEN + 1];
    char nr[NONCE_LEN + 1], na[NONCE_LEN + 1];  // requester's and accepter's nonces
    uint32_t tr, ta;                            // their clocks, unix seconds, 0 = unknown
    char v[3];                                  // how each hears the other: 'r'adio or 'b'ridge
    char dr[LH_MEET_MAX + 1], da[LH_MEET_MAX + 1];
} meet_t;

struct lh_meet {
    // Asking: one at a time.
    bool asking;
    meet_t out;
    int cb_ref;
    int64_t started, sent, last_ask;
    // The last ask that finished: a repeated "ok" gets the same "fin".
    bool finned;
    char fin_peer[OS_MESH_ID_LEN + 1], fin_nr[NONCE_LEN + 1], fin_tag[OS_MESH_TAG_LEN + 1];
    // The last meet accepted: a repeated "req" gets the same "ok"; "fin" brings the peer's tag.
    bool accepted, uploaded;
    meet_t in;
    char in_tag[OS_MESH_TAG_LEN + 1];
    int64_t last_accept;
};

static bool is_hex16(const char *s)
{
    if (!s || strlen(s) != NONCE_LEN) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) {
            return false;
        }
    }
    return true;
}

// A side's data: a JSON object in at most LH_MEET_MAX bytes of printable ASCII
// (the tag covers these bytes, and the server takes nothing else).
static bool data_ok(const char *d)
{
    size_t n = d ? strlen(d) : 0;
    if (n < 2 || n > LH_MEET_MAX || d[0] != '{') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (d[i] < 0x20 || d[i] > 0x7e) {
            return false;
        }
    }
    cJSON *j = cJSON_Parse(d);
    bool ok = cJSON_IsObject(j);
    cJSON_Delete(j);
    return ok;
}

static uint32_t clock_s(void)
{
    return (uint32_t)(os_cloud_time_ms() / 1000);
}

// 'b' when `id` is heard over a bridge, 'r' over the radio, 0 when it isn't a badge nearby.
static char via(const char *id)
{
    os_mesh_peer_t p;
    if (!os_mesh_peer(id, &p) || p.kind != OS_PEER_BADGE) {
        return 0;
    }
    return p.bridged ? 'b' : 'r';
}

// tag(my key, "meet|<me>|<app>|<req>|<acc>|<nr>|<na>|<tr>|<ta>|<v>|<len dr>|<dr>|<len da>|<da>")
static bool sign(const lh_t *s, const char *me, bool asked, const meet_t *m, char out[OS_MESH_TAG_LEN + 1])
{
    char *t = malloc(STATEMENT_MAX);
    if (!t) {
        out[0] = 0;
        return false;
    }
    snprintf(t, STATEMENT_MAX, "meet|%s|%s|%s|%s|%s|%s|%" PRIu32 "|%" PRIu32 "|%s|%u|%s|%u|%s", me, s->pkg.id,
             asked ? me : m->peer, asked ? m->peer : me, m->nr, m->na, m->tr, m->ta, m->v,
             (unsigned)strlen(m->dr), m->dr, (unsigned)strlen(m->da), m->da);
    bool ok = os_mesh_sign(t, out);
    free(t);
    return ok;
}

// {"app":id,"to":peer,"m":m}: an APP message with "m" for "d" (older builds ignore it). Takes m.
static esp_err_t send(const lh_t *s, const char *to, cJSON *m)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "app", s->pkg.id);
    cJSON_AddStringToObject(root, "to", to);
    cJSON_AddItemToObject(root, "m", m);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t e = json ? os_mesh_send(OS_MESH_APP, to, json) : ESP_ERR_NO_MEM;
    cJSON_free(json);
    return e;
}

static esp_err_t send_req(const lh_t *s, const meet_t *m)
{
    char v[2] = { m->v[0], 0 };
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "op", "req");
    cJSON_AddStringToObject(j, "n", m->nr);
    cJSON_AddNumberToObject(j, "t", m->tr);
    cJSON_AddStringToObject(j, "v", v);
    cJSON_AddStringToObject(j, "d", m->dr);
    return send(s, m->peer, j);
}

static void send_ok(const lh_t *s, const meet_t *m, const char *tag)
{
    char v[2] = { m->v[1], 0 };
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "op", "ok");
    cJSON_AddStringToObject(j, "r", m->nr);
    cJSON_AddStringToObject(j, "n", m->na);
    cJSON_AddNumberToObject(j, "t", m->ta);
    cJSON_AddStringToObject(j, "v", v);
    cJSON_AddStringToObject(j, "d", m->da);
    cJSON_AddStringToObject(j, "tag", tag);
    send(s, m->peer, j);
}

// "no" (tag NULL) or "fin".
static void send_end(const lh_t *s, const char *to, const char *nr, const char *tag)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "op", tag ? "fin" : "no");
    cJSON_AddStringToObject(j, "r", nr);
    if (tag) {
        cJSON_AddStringToObject(j, "tag", tag);
    }
    send(s, to, j);
}

// Queue the meet for the server with the other badge's tag (server/API.md "App meets").
static bool upload(const lh_t *s, const char *me, bool asked, const meet_t *m, const char *their_tag)
{
    char path[48 + LUA_PKG_ID_MAX];
    snprintf(path, sizeof(path), "/api/device/apps/%s/meets", s->pkg.id);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "req", asked ? me : m->peer);
    cJSON_AddStringToObject(j, "acc", asked ? m->peer : me);
    cJSON_AddStringToObject(j, "nr", m->nr);
    cJSON_AddStringToObject(j, "na", m->na);
    cJSON_AddNumberToObject(j, "tr", m->tr);
    cJSON_AddNumberToObject(j, "ta", m->ta);
    cJSON_AddStringToObject(j, "v", m->v);
    cJSON_AddStringToObject(j, "dr", m->dr);
    cJSON_AddStringToObject(j, "da", m->da);
    cJSON_AddStringToObject(j, "tag", their_tag);
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    bool ok = body && os_outbox_add(path, body) == ESP_OK;
    cJSON_free(body);
    return ok;
}

// ---------------------------------------------------------------------------
// Asking

typedef struct {
    int cb_ref;
    const char *peer, *data, *err;     // data: their JSON (accepted), else err
} asked_t;

// Protected: cb(true, {peer=, data=}) or cb(false, err).
static int asked_cb(lua_State *L)
{
    const asked_t *a = lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, a->cb_ref);
    if (a->err) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, a->err);
    } else {
        lua_pushboolean(L, 1);
        lua_createtable(L, 0, 2);
        lua_pushstring(L, a->peer);
        lua_setfield(L, -2, "peer");
        if (lh_json_decode(L, a->data, strlen(a->data))) {
            lua_createtable(L, 0, 0);
        }
        lua_setfield(L, -2, "data");
    }
    lua_call(L, 2, 0);
    return 0;
}

// The ask is over: tell the script. data: the accepter's, or NULL with err.
static void asked_done(lh_t *s, const char *data, const char *err)
{
    struct lh_meet *m = s->meet;
    asked_t a = { .cb_ref = m->cb_ref, .peer = m->out.peer, .data = data, .err = err };
    m->asking = false;
    m->cb_ref = LUA_NOREF;
    lh_callback(s, asked_cb, &a);
    if (s->L) {
        luaL_unref(s->L, LUA_REGISTRYINDEX, a.cb_ref);
    }
}

void lh_meet_run(lh_t *s)
{
    struct lh_meet *m = s->meet;
    if (!m || !m->asking) {
        return;
    }
    int64_t now = lh_now();
    if (now - m->started >= MEET_WAIT_MS) {
        asked_done(s, NULL, "no answer");
    } else if (now - m->sent >= MEET_RESEND_MS) {
        m->sent = now;
        send_req(s, &m->out);
    }
}

// mesh.meet(id, data, cb): true once the request is on its way (cb follows), or nil, error.
int lh_mesh_meet(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_MESH, "mesh");
    lh_t *s = lh_get(L);
    const char *to = luaL_checkstring(L, 1);
    luaL_argcheck(L, strlen(to) == OS_MESH_ID_LEN, 1, "a badge id");
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (s->ui_off) {
        return 0;       // closing: no callback can run any more
    }
    const char *err = NULL;
    char me[OS_MESH_ID_LEN + 1];
    lh_mesh_me(me);
    int64_t now = lh_now();
    struct lh_meet *m = s->meet;
    if (!m && !(m = s->meet = calloc(1, sizeof(*m)))) {
        return luaL_error(L, "not enough memory");
    }
    lh_jbuf_t d = { .max = LH_MEET_MAX };
    char v = via(to);
    if (m->asking) {
        err = "busy";
    } else if (m->last_ask && now - m->last_ask < MEET_GAP_MS) {
        err = "too fast: one meet a second";
    } else if (!me[0] || !os_mesh_ready()) {
        err = "the mesh is off";
    } else if (!v || !strcmp(to, me)) {
        err = "not nearby";
    } else if (os_outbox_count() >= MEET_BOX_MAX) {
        err = "outbox full";
    } else if (!lh_json_encode(L, 2, &d)) {
        err = strcmp(d.err, "too big") ? d.err : "data over 100 bytes";
    } else if (!data_ok(d.p)) {
        err = "data: a table of named fields, plain ASCII";
    }
    if (!err) {
        memset(&m->out, 0, sizeof(m->out));
        strlcpy(m->out.peer, to, sizeof(m->out.peer));
        os_mesh_nonce(m->out.nr);
        m->out.tr = clock_s();
        m->out.v[0] = v;
        strlcpy(m->out.dr, d.p, sizeof(m->out.dr));
        if (send_req(s, &m->out) != ESP_OK) {
            err = "the mesh is off";
        }
    }
    free(d.p);
    if (err) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushvalue(L, 3);
    m->cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    m->asking = true;
    m->started = m->sent = m->last_ask = now;
    lua_pushboolean(L, 1);
    return 1;
}

// ---------------------------------------------------------------------------
// Accepting

typedef struct {
    const char *from, *data;
    lh_jbuf_t out;
    bool accepted;
} accept_t;

// Protected: on_meet(from, data) -> a table to sign, anything else refuses.
static int accept_cb(lua_State *L)
{
    accept_t *a = lua_touserdata(L, 1);
    lua_getglobal(L, "on_meet");
    if (!lua_isfunction(L, -1)) {
        return 0;
    }
    lua_pushstring(L, a->from);
    if (lh_json_decode(L, a->data, strlen(a->data))) {
        return 0;
    }
    lua_call(L, 2, 1);
    if (lua_istable(L, -1)) {
        a->accepted = lh_json_encode(L, -1, &a->out) && data_ok(a->out.p);
    }
    return 0;
}

static void on_req(lh_t *s, const char *me, const char *from, const cJSON *j)
{
    struct lh_meet *m = s->meet;
    const char *n = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "n"));
    const char *d = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "d"));
    const char *pv = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "v"));
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "t");
    char v = via(from);
    if (!is_hex16(n) || !data_ok(d) || !pv || (strcmp(pv, "r") && strcmp(pv, "b")) || !cJSON_IsNumber(t)
            || t->valuedouble < 0 || t->valuedouble > UINT32_MAX || !v) {
        return;
    }
    if (m->accepted && !strcmp(m->in.peer, from) && !strcmp(m->in.nr, n)) {
        send_ok(s, &m->in, m->in_tag);          // our "ok" got lost
        return;
    }
    int64_t now = lh_now();
    if (m->last_accept && now - m->last_accept < MEET_GAP_MS) {
        return;                                 // it asks again
    }
    m->last_accept = now;
    accept_t a = { .from = from, .data = d, .out = { .max = LH_MEET_MAX } };
    bool alive = lh_callback(s, accept_cb, &a);
    if (alive && a.accepted) {
        meet_t *in = &m->in;
        memset(in, 0, sizeof(*in));
        strlcpy(in->peer, from, sizeof(in->peer));
        strlcpy(in->nr, n, sizeof(in->nr));
        os_mesh_nonce(in->na);
        in->tr = (uint32_t)t->valuedouble;
        in->ta = clock_s();
        in->v[0] = pv[0];
        in->v[1] = v;
        strlcpy(in->dr, d, sizeof(in->dr));
        strlcpy(in->da, a.out.p, sizeof(in->da));
        m->accepted = sign(s, me, false, in, m->in_tag);
        m->uploaded = false;
        if (m->accepted) {
            send_ok(s, in, m->in_tag);
        }
    } else if (alive) {
        send_end(s, from, n, NULL);
    }
    free(a.out.p);
}

static void on_ok(lh_t *s, const char *me, const char *from, const cJSON *j)
{
    struct lh_meet *m = s->meet;
    const char *r = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "r"));
    if (!r) {
        return;
    }
    if (m->finned && !strcmp(m->fin_peer, from) && !strcmp(m->fin_nr, r)) {
        send_end(s, from, r, m->fin_tag);       // our "fin" got lost
        return;
    }
    const char *n = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "n"));
    const char *d = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "d"));
    const char *pv = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "v"));
    const char *tag = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "tag"));
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "t");
    meet_t *out = &m->out;
    if (!m->asking || strcmp(out->peer, from) || strcmp(out->nr, r) || !is_hex16(n) || !is_hex16(tag) || !data_ok(d)
            || !pv || (strcmp(pv, "r") && strcmp(pv, "b")) || !cJSON_IsNumber(t) || t->valuedouble < 0
            || t->valuedouble > UINT32_MAX) {
        return;
    }
    strlcpy(out->na, n, sizeof(out->na));
    out->ta = (uint32_t)t->valuedouble;
    out->v[1] = pv[0];
    strlcpy(out->da, d, sizeof(out->da));
    // The server checks the accepter's tag (we can't: it's under their key). Ours
    // lets them upload the meet too.
    char mine[OS_MESH_TAG_LEN + 1];
    if (!sign(s, me, true, out, mine) || !upload(s, me, true, out, tag)) {
        asked_done(s, NULL, "outbox full");
        return;
    }
    m->finned = true;
    strlcpy(m->fin_peer, from, sizeof(m->fin_peer));
    strlcpy(m->fin_nr, r, sizeof(m->fin_nr));
    strlcpy(m->fin_tag, mine, sizeof(m->fin_tag));
    send_end(s, from, r, mine);
    asked_done(s, out->da, NULL);
}

// A meet message for this app from the mesh (lua_net.c queued it): "<from id>\0<json of m>".
void lh_meet_event(lh_t *s, const char *data)
{
    const char *from = data, *json = data + strlen(data) + 1;
    char me[OS_MESH_ID_LEN + 1];
    lh_mesh_me(me);
    if (!me[0] || !strcmp(from, me) || (!s->meet && !(s->meet = calloc(1, sizeof(struct lh_meet))))) {
        return;
    }
    struct lh_meet *m = s->meet;
    cJSON *j = cJSON_Parse(json);
    const char *op = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "op"));
    const char *r = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "r"));
    if (!op) {
        // not ours to read
    } else if (!strcmp(op, "req")) {
        on_req(s, me, from, j);
    } else if (!strcmp(op, "ok")) {
        on_ok(s, me, from, j);
    } else if (!strcmp(op, "no")) {
        if (m->asking && r && !strcmp(m->out.peer, from) && !strcmp(m->out.nr, r)) {
            asked_done(s, NULL, "refused");
        }
    } else if (!strcmp(op, "fin")) {
        const char *tag = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "tag"));
        if (m->accepted && !m->uploaded && r && is_hex16(tag) && !strcmp(m->in.peer, from) && !strcmp(m->in.nr, r)) {
            // The requester queued it too; whichever upload arrives first stores it.
            m->uploaded = true;
            upload(s, me, false, &m->in, tag);
        }
    }
    cJSON_Delete(j);
}

void lh_meet_free(lh_t *s)
{
    free(s->meet);
    s->meet = NULL;
}
