// mail for Lua apps (docs/LUA_API.md "mail", Solana OS 2.3.0): send a
// notification to another badge that has this app, read this app's mail on
// the badge (game/mailbox.h), and on_mail when one arrives while the app is
// open or the owner opens the app from Mailbox on one of its messages.
//
// mail.send goes through the outbox (os_outbox_add): it works offline and
// uploads after the app closed; the server decides who gets it (both owners
// have the app, caps, mutes) and the host fixes the app id in the path, so a
// script can only ever send as itself. The text rules are checked here too,
// so a script learns about a bad title at once rather than never.

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "game/mailbox.h"
#include "lua_priv.h"

#define MAIL_BOX_MAX    40          // uploads waiting: no new sends beyond (as meets)

static int64_t s_last_send;
// The message the owner opened the app on (Mailbox's "Open <App>"): handed to
// on_mail once the app's top-level chunk ran. Guarded by lh_mx.
static char s_open_app[LUA_PKG_ID_MAX + 1];
static uint32_t s_open_id;

static bool printable(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

// A badge id as the mesh gives it: 6 characters of [0-9A-Z], an emulator's starting with 'e'.
static bool badge_id_ok(const char *s, size_t n)
{
    if (n != 6) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (i == 0 && c == 'e');
        if (!ok) {
            return false;
        }
    }
    return true;
}

static int fail(lua_State *L, const char *why)
{
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}

// mail.send(to, title [, text [, data]]) -> true | nil, err
static int l_send(lua_State *L)
{
    lh_t *s = lh_get(L);
    lh_need_perm(L, LUA_PERM_MAIL, "mail");
    size_t nto, ntitle, ntext = 0;
    const char *to = luaL_checklstring(L, 1, &nto);
    const char *title = luaL_checklstring(L, 2, &ntitle);
    const char *text = luaL_optlstring(L, 3, "", &ntext);
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
    }
    if (!badge_id_ok(to, nto)) {
        return fail(L, "bad id");
    }
    if (ntitle < 1 || ntitle > MAILBOX_TITLE_MAX || !printable(title, ntitle)) {
        return fail(L, "title: 1-32 printable ASCII");
    }
    if (ntext > MAILBOX_TEXT_MAX || !printable(text, ntext)) {
        return fail(L, "text: 0-200 printable ASCII");
    }
    char data[MAILBOX_DATA_MAX + 1] = "";
    if (lua_istable(L, 4)) {
        lh_jbuf_t b = { .max = MAILBOX_DATA_MAX };
        bool ok = lh_json_encode(L, 4, &b) && b.p && b.p[0] == '{' && printable(b.p, b.len);
        if (ok) {
            memcpy(data, b.p, b.len);
            data[b.len] = 0;
        }
        free(b.p);
        if (!ok) {
            return fail(L, "data: a table of <= 128 bytes as JSON");
        }
    }
    os_cloud_info_t *in = malloc(sizeof(*in));
    if (!in) {
        return fail(L, "busy");
    }
    os_cloud_get_info(in);
    bool claimed = in->claimed;
    free(in);
    if (!claimed) {
        return fail(L, "not linked");
    }
    int64_t now = lh_now();
    if (s_last_send && now - s_last_send < LH_MAIL_GAP_MS) {
        return fail(L, "busy");
    }
    if (os_outbox_count() >= MAIL_BOX_MAX) {
        return fail(L, "full");
    }
    // No Lua errors from here on: C memory is in play.
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "to", to);
    cJSON_AddStringToObject(j, "title", title);
    cJSON_AddStringToObject(j, "text", text);
    if (data[0]) {
        cJSON_AddRawToObject(j, "data", data);
    }
    char *json = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    char path[64];
    snprintf(path, sizeof(path), "/api/device/apps/%s/mail", s->pkg.id);
    esp_err_t e = json ? os_outbox_add(path, json) : ESP_ERR_NO_MEM;
    cJSON_free(json);
    if (e != ESP_OK) {
        return fail(L, "full");
    }
    s_last_send = now;
    lua_pushboolean(L, 1);
    return 1;
}

static void push_head(lua_State *L, const mailbox_item_t *h)
{
    lua_createtable(L, 0, 8);
    lua_pushinteger(L, (lua_Integer)h->id);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, h->from);
    lua_setfield(L, -2, "from");
    if (h->by[0]) {
        lua_pushstring(L, h->by);
        lua_setfield(L, -2, "by");
    }
    lua_pushstring(L, h->title);
    lua_setfield(L, -2, "title");
    lua_pushinteger(L, (lua_Integer)h->at);
    lua_setfield(L, -2, "at");
    lua_pushboolean(L, h->read);
    lua_setfield(L, -2, "read");
}

// The full message, as mail.get and on_mail give it.
static void push_mail(lua_State *L, const mailbox_mail_t *m)
{
    push_head(L, &m->h);
    lua_pushstring(L, m->text);
    lua_setfield(L, -2, "text");
    lua_pushinteger(L, (lua_Integer)m->h.exp);
    lua_setfield(L, -2, "exp");
    if (m->data[0] && !lh_json_decode(L, m->data, strlen(m->data))) {
        lua_setfield(L, -2, "data");
    }
}

// mail.list() -> { {id, from, by, title, at, read}, ... } newest first
static int l_list(lua_State *L)
{
    lh_t *s = lh_get(L);
    lh_need_perm(L, LUA_PERM_MAIL, "mail");
    lh_charge_steps(L, LH_FS_OPEN_STEPS);   // as a file opened: the list may come from the card
    // On the app's heap, so an error while pushing leaks nothing.
    mailbox_item_t *items = lua_newuserdatauv(L, MAILBOX_MAX * sizeof(*items), 0);
    int n = mailbox_for_app(s->pkg.id, items, MAILBOX_MAX);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        push_head(L, &items[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// One of this app's messages, or NULL (C memory: the caller frees it).
static mailbox_mail_t *get_mine(lh_t *s, uint32_t id)
{
    mailbox_mail_t *m = malloc(sizeof(*m));
    if (m && (!mailbox_get(id, m) || strcmp(m->h.app, s->pkg.id))) {
        free(m);
        m = NULL;
    }
    return m;
}

// mail.get(id) -> {id, from, by, title, text, data, at, exp, read} | nil
static int l_get(lua_State *L)
{
    lh_t *s = lh_get(L);
    lh_need_perm(L, LUA_PERM_MAIL, "mail");
    lua_Integer id = luaL_checkinteger(L, 1);
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    mailbox_mail_t *m = lua_newuserdatauv(L, sizeof(*m), 0);
    if (id <= 0 || !mailbox_get((uint32_t)id, m) || strcmp(m->h.app, s->pkg.id)) {
        lua_pushnil(L);
        return 1;
    }
    push_mail(L, m);
    return 1;
}

void lh_open_mail(lua_State *L)
{
    static const luaL_Reg mail[] = {{"send", l_send}, {"list", l_list}, {"get", l_get}, {NULL, NULL}};
    luaL_newlib(L, mail);
    lua_setglobal(L, "mail");
}

// ---------------------------------------------------------------------------
// on_mail

// Protected: on_mail(m), m pushed from the mailbox_mail_t at 1.
static int deliver(lua_State *L)
{
    const mailbox_mail_t *m = lua_touserdata(L, 1);
    lua_getglobal(L, "on_mail");
    if (!lua_isfunction(L, -1)) {
        return 0;
    }
    push_mail(L, m);
    lua_call(L, 1, 0);
    return 0;
}

static void deliver_id(lh_t *s, uint32_t id)
{
    if (!s->L || s->failed || !(s->pkg.perms & LUA_PERM_MAIL)) {
        return;
    }
    mailbox_mail_t *m = get_mine(s, id);
    if (m) {
        lh_callback(s, deliver, m);
        free(m);
    }
}

void lh_mail_event(lh_t *s, const char *data)
{
    deliver_id(s, data ? (uint32_t)strtoul(data, NULL, 10) : 0);
}

void lh_mail_opened(lh_t *s)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    uint32_t id = !strcmp(s_open_app, s->pkg.id) ? s_open_id : 0;
    s_open_app[0] = 0;
    s_open_id = 0;
    xSemaphoreGive(lh_mx);
    if (id) {
        deliver_id(s, id);
    }
}

void lua_host_mail(const char *app, uint32_t id)
{
    char *data = malloc(12);
    if (!data || !app) {
        free(data);
        return;
    }
    snprintf(data, 12, "%" PRIu32, id);
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    lh_t *s = lh_cur;
    bool mine = s && !s->failed && !strcmp(s->pkg.id, app) && (s->pkg.perms & LUA_PERM_MAIL);
    lh_ev_t ev = { .kind = LH_EV_MAIL, .gen = s ? s->gen : 0, .data = data };
    xSemaphoreGive(lh_mx);
    if (!mine || !lh_post(&ev)) {
        free(data);
    }
}

bool lua_host_open_mail(const char *app, uint32_t id)
{
    xSemaphoreTake(lh_mx, portMAX_DELAY);
    strlcpy(s_open_app, app, sizeof(s_open_app));
    s_open_id = id;
    xSemaphoreGive(lh_mx);
    if (!lua_host_launch(app)) {
        xSemaphoreTake(lh_mx, portMAX_DELAY);
        s_open_app[0] = 0;
        s_open_id = 0;
        xSemaphoreGive(lh_mx);
        return false;
    }
    return true;
}

// The service side of on_mail (2.3.0): the same table as mail.get, as JSON
// through the services' call queue (lua_svc_call), so the app on screen
// (lua_host_mail above) never gets it twice. ≤ ~600 bytes: title 32, text
// 200, data 128 and the header fit LUA_SVC_MSG_MAX.
void lua_host_mail_service(const char *app, uint32_t id)
{
    if (!app || !app[0]) {
        return;
    }
    mailbox_mail_t *m = malloc(sizeof(*m));
    if (!m || !mailbox_get(id, m) || strcmp(m->h.app, app)) {
        free(m);
        return;
    }
    cJSON *j = cJSON_CreateObject();
    if (j) {
        cJSON_AddNumberToObject(j, "id", m->h.id);
        cJSON_AddStringToObject(j, "from", m->h.from);
        if (m->h.by[0]) {
            cJSON_AddStringToObject(j, "by", m->h.by);
        }
        cJSON_AddStringToObject(j, "title", m->h.title);
        cJSON_AddNumberToObject(j, "at", m->h.at);
        cJSON_AddBoolToObject(j, "read", m->h.read);
        cJSON_AddStringToObject(j, "text", m->text);
        cJSON_AddNumberToObject(j, "exp", m->h.exp);
        cJSON *data = m->data[0] ? cJSON_Parse(m->data) : NULL;
        if (data) {
            cJSON_AddItemToObject(j, "data", data);
        }
        char *json = cJSON_PrintUnformatted(j);
        if (json) {
            lua_svc_call(app, "on_mail", json, LUA_PERM_MAIL);
            cJSON_free(json);
        }
        cJSON_Delete(j);
    }
    free(m);
}
