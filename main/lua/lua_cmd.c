// "lua" console command (os_cmd, so the badge's UART console and the
// emulator's emu_console both serve it).
//
//   lua list | run <id> | rm <id> [file] | mem | sums <id> | install <id>
//   lua put <id> <file> begin|<base64>|end
//   lua recv <id> <file> <bytes> <sha256>
//   lua aot <id> | aotkey
//
// aotkey is one word: "aot key" would be "aot <id>" for an app whose id is
// "key" (ids are 3 to 24 characters of a-z, 0-9 and -).
//
// <file> is main.lua, app.json, app.wasm (a C app's module, docs/C_API.md)
// or a bundled path (docs/LUA_API.md "Console"). Writing or removing any of
// them deletes the store's compiled code (code.bin) and a C app's native
// code (app.aot), which would no longer match: the app then runs from its
// source or its app.wasm. Nothing here can write code.bin or app.aot (native
// code comes only from the store installer, checked against the store's
// SHA-256). put copies a file a line at a time: begin starts it, each
// base64 line appends to it, end checks it and moves it into place. recv
// takes a whole file in one stream after its line, a block at a time when
// the badge asks for it, so the card is never written while the UART
// receives (its buffer holds ~30 ms at 115200 baud). Tools pair it with
// sums to send only what changed. The console task's stack is small: big
// buffers are allocated.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "psa/crypto.h"
#include "core/console.h"
#include "cJSON.h"
#include "core/os_api.h"
#include "core/os_cloud.h"
#include "core/os_cmd.h"
#include "core/os_data.h"
#include "lua_aot.h"
#include "lua_install.h"
#include "lua_priv.h"

#define LIST_MAX        40
#define RECV_BLOCK      2048            // base64 characters per block, a multiple of 4
#define RECV_WAIT_MS    5000

static const char *refusal(const char *id)
{
    if (!os_data_ready()) {
        return "no microSD card";
    }
    if (lua_host_active(id)) {
        return "the app is running; close it first (open home)";
    }
    if (lua_svc_active(id)) {
        return "its background service is running; stop it first (svc stop app:<id>)";
    }
    if (lua_install_busy(id)) {
        return "the App Store is installing it";
    }
    return NULL;
}

static int cmd_list(void)
{
    lua_pkg_t *pkgs = malloc(LIST_MAX * sizeof(*pkgs));
    int n = pkgs ? lua_pkg_scan(pkgs, LIST_MAX) : 0;
    if (n < 0) {
        printf("no microSD card\n");
    } else if (n == 0) {
        printf("no apps installed\n");
    }
    for (int i = 0; i < n; i++) {
        const lua_pkg_t *p = &pkgs[i];
        char perms[128] = "";
        for (int b = 0; b < LUA_PERM_COUNT; b++) {
            if (p->perms & (1u << b)) {
                strlcat(perms, perms[0] ? "," : "", sizeof(perms));
                strlcat(perms, lua_perm_name(b), sizeof(perms));
            }
        }
        printf("%-24s %-10s %-20s %s%s%s%s\n", p->id, p->version, p->name, perms,
               p->lang == LUA_PKG_LANG_C ? "  C" : "", p->temp ? "  temp" : "", lua_pkg_expired(p) ? " (expired)" : "");
    }
    free(pkgs);
    return n < 0;
}

// The store's compiled code (a Lua app's code.bin, a C app's app.aot) no
// longer matches the app's files.
static void drop_code(const char *id)
{
    char rel[64], path[160];
    static const char *const files[] = {"code.bin", "app.aot"};
    for (int i = 0; i < 2; i++) {
        lua_pkg_rel(rel, sizeof(rel), id, files[i]);
        os_data_path(path, sizeof(path), rel);
        unlink(path);
    }
}

// Removes the file, then the folders inside files/ it leaves empty.
static bool remove_file(const char *id, const char *rel)
{
    char path[160], top[160], files[48];
    os_data_path(path, sizeof(path), rel);
    if (unlink(path) != 0) {
        return false;
    }
    snprintf(files, sizeof(files), "apps/%s/files", id);
    os_data_path(top, sizeof(top), files);
    size_t keep = strlen(top);
    for (char *slash; (slash = strrchr(path, '/')) && (size_t)(slash - path) > keep;) {
        *slash = 0;
        if (rmdir(path) != 0) {
            break;                          // not empty
        }
    }
    return true;
}

static int cmd_rm(const char *id, const char *file)
{
    char rel[128];
    const char *why = lua_pkg_valid_id(id) ? refusal(id) : "no such app";
    if (why) {
        printf("can't remove %s: %s\n", id, why);
        return 1;
    }
    if (file) {
        if (!lua_pkg_file_rel(rel, sizeof(rel), id, file, NULL) || !remove_file(id, rel)) {
            printf("can't remove %s/%s\n", id, file);
            return 1;
        }
        drop_code(id);
        printf("removed %s/%s\n", id, file);
        return 0;
    }
    if (!lua_pkg_remove(id)) {
        printf("can't remove %s\n", id);
        return 1;
    }
    printf("removed %s\n", id);
    return 0;
}

static void print_code(const lh_code_stat_t *c)
{
    if (c->size) {
        printf("  code: the store's compiled code, %u bytes run in place (flash slot %d%s)\n", (unsigned)c->size,
               c->slot, c->copied ? ", copied in at launch" : "");
    } else {
        printf("  code: main.lua compiled on the badge\n");
    }
}

// A C app's heap is its Lua state's and WAMR's together (module, instance,
// linear memory), so the "Lua heap" figures count both.
static void print_wasm(const lh_wasm_stat_t *w)
{
    if (!w->c) {
        return;
    }
    printf("  C app: %s, %u bytes; linear memory %u bytes (peak %u)\n", w->aot ? "app.aot (native)"
           : "app.wasm (interpreted)", (unsigned)w->module_size, (unsigned)w->mem_size, (unsigned)w->mem_peak);
    if (w->frames) {
        printf("  ui_frame: %u frames, %u of them sent without a copy\n", (unsigned)w->frames,
               (unsigned)w->frames_direct);
    }
    if (w->aot) {
        printf("  run in place from flash slot %d%s; launch: signature %u us, slot %u us\n"
               "  most native time in one callback %u us (cap %d ms)\n", w->slot,
               w->copied ? " (copied in at launch)" : "", (unsigned)w->sig_us, (unsigned)w->map_us,
               (unsigned)w->native_peak_us, LW_AOT_CALLBACK_MS);
    } else {
        printf("  most wasm instructions in one callback %u (cap %d)\n", (unsigned)w->steps_peak,
               LUA_HOST_WASM_STEPS_MAX);
    }
}

static int cmd_mem(void)
{
    lh_t *s = lh_cur;
    if (s && s->L) {
        printf("running %s: %u bytes of %s heap (peak %u, cap %u, in %s)\n", s->pkg.id, (unsigned)s->mem,
               s->wasm_stat.c ? "app" : "Lua", (unsigned)s->mem_peak, (unsigned)s->heap_max,
               s->psram ? "PSRAM" : "SRAM");
        if (s->wasm_stat.c) {
            print_wasm(&s->wasm_stat);
        } else {
            print_code(&s->code_stat);
        }
    }
    if (!lh_stats.id[0]) {
        printf("no app has finished yet\n");
        return 0;
    }
    bool c = lh_stats.wasm.c;
    printf("last run %s: %s heap peak %u bytes (cap %u), %u left after close\n"
           "  most %s in one callback %u (cap %d)\n"
           "  lua task stack never used %u bytes\n"
           "  SRAM free at launch %u\n"
           "  last error: %s\n",
           lh_stats.id, c ? "app" : "Lua", (unsigned)lh_stats.mem_peak, (unsigned)lua_host_heap_max(),
           (unsigned)lh_stats.mem_end, c ? "host steps (copies, output, fs, crypto, canvas)" : "instructions",
           (unsigned)lh_stats.steps_peak, LUA_HOST_STEPS_MAX, (unsigned)lh_stats.stack_free,
           (unsigned)lh_stats.heap_before, lh_stats.err[0] ? lh_stats.err : "none");
    if (lh_stats.wasm.c) {
        print_wasm(&lh_stats.wasm);
    } else {
        print_code(&(lh_code_stat_t){ .size = lh_stats.code_size, .slot = lh_stats.code_slot,
                                      .copied = lh_stats.code_copied });
    }
    return 0;
}

// The .put file in place of the real one, once app.json is checked.
static const char *finish_file(const char *id, const char *file, const char *part, const char *path)
{
    if (!strcmp(file, "app.json")) {
        lua_pkg_t pkg;
        FILE *f = fopen(part, "rb");
        char *buf = malloc(LUA_PKG_JSON_MAX + 1);
        size_t n = f && buf ? fread(buf, 1, LUA_PKG_JSON_MAX, f) : 0;
        if (f) {
            fclose(f);
        }
        bool ok = buf && (buf[n] = 0, lua_pkg_parse(buf, id, &pkg));
        free(buf);
        if (!ok) {
            return "app.json is not a manifest for this app";
        }
    }
    unlink(path);                           // FatFs rename() won't replace a file
    if (rename(part, path) != 0) {
        return "can't write it to the card";
    }
    drop_code(id);
    return NULL;
}

static int cmd_put(const char *id, const char *file, const char *arg)
{
    char rel[128], part[176], path[160];
    size_t max;
    if (!lua_pkg_file_rel(rel, sizeof(rel), id, file, &max)) {
        printf("usage: lua put <id> <main.lua|app.json|app.wasm|bundled path> begin|<base64>|end\n");
        return 1;
    }
    const char *why = refusal(id);
    if (why) {
        printf("%s\n", why);
        return 1;
    }
    os_data_path(path, sizeof(path), rel);
    snprintf(part, sizeof(part), "%s.put", path);
    if (!strcmp(arg, "begin")) {
        os_data_mkdirs(rel);
        FILE *f = fopen(part, "wb");
        if (!f) {
            printf("can't create %s\n", part);
            return 1;
        }
        fclose(f);
        return 0;
    }
    struct stat st;
    if (stat(part, &st) != 0) {
        printf("run 'lua put %s %s begin' first\n", id, file);
        return 1;
    }
    if (!strcmp(arg, "end")) {
        why = finish_file(id, file, part, path);
        if (why) {
            printf("%s\n", why);
            unlink(part);
            return 1;
        }
        printf("%s/%s: %ld bytes\n", id, file, (long)st.st_size);
        return 0;
    }
    unsigned char bin[96];
    size_t n = 0;
    if (mbedtls_base64_decode(bin, sizeof(bin), &n, (const unsigned char *)arg, strlen(arg)) != 0) {
        printf("bad base64 (at most 128 characters per line)\n");
        return 1;
    }
    if (st.st_size + n > max) {
        printf("%s is limited to %u bytes\n", file, (unsigned)max);
        return 1;
    }
    FILE *f = fopen(part, "ab");
    bool ok = f && fwrite(bin, 1, n, f) == n;
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    if (!ok) {
        printf("write failed\n");
    }
    return !ok;
}

// ---------------------------------------------------------------------------
// recv

static bool parse_sha(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        char c = hex[i] | 0x20;             // either case
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) {
            return false;
        }
        out[i / 2] = i % 2 ? (out[i / 2] | v) : v << 4;
    }
    return true;
}

// Fills buf with exactly n base64 characters from the stream (line breaks
// and spaces skipped). NULL, or why it couldn't.
static const char *read_block(char *buf, size_t n)
{
    size_t have = 0;
    while (have < n) {
        int got = console_stream_read(buf + have, n - have, RECV_WAIT_MS);
        if (got < 0) {
            return "this console takes no stream";
        }
        if (got == 0) {
            return "timeout";
        }
        char *p = buf + have;
        int kept = 0;
        for (int i = 0; i < got; i++) {
            if (p[i] != '\r' && p[i] != '\n' && p[i] != ' ' && p[i] != '\t') {
                p[kept++] = p[i];
            }
        }
        have += kept;
    }
    return NULL;
}

// "files/<name>": a file of the app's own folder, hh/files/<id>/ (LUA_API.md
// "files"), the way a card reader would put it there: a game the owner
// copies to an emulator, say. rel and its size limit, or false.
#define OWNER_FILE_MAX  (32L * 1024 * 1024)

static bool owner_file_rel(char *rel, size_t n, const char *id, const char *file)
{
    if (strncmp(file, "files/", 6) || !lua_pkg_valid_id(id) || !lh_files_kind(file + 6, strlen(file + 6))) {
        return false;
    }
    snprintf(rel, n, "files/%s/%s", id, file + 6);
    return true;
}

static const char *receive(const char *id, const char *file, long bytes, const uint8_t want[32])
{
    char rel[128], path[160], part[176];
    size_t max;
    bool owner = owner_file_rel(rel, sizeof(rel), id, file);
    if (owner) {
        max = OWNER_FILE_MAX;
    } else {
        lua_pkg_file_rel(rel, sizeof(rel), id, file, &max);
    }
    if (bytes > (long)max) {
        return "too big";
    }
    const char *why = refusal(id);
    if (why) {
        return why;
    }
    if (console_stream_read(part, 0, 0) < 0) {
        return "this console takes no stream";
    }
    os_data_mkdirs(rel);
    os_data_path(path, sizeof(path), rel);
    snprintf(part, sizeof(part), "%s.put", path);
    char *b64 = malloc(RECV_BLOCK);
    unsigned char *bin = malloc(RECV_BLOCK / 4 * 3);
    FILE *f = b64 && bin ? fopen(part, "wb") : NULL;
    if (!f) {
        free(b64);
        free(bin);
        return b64 && bin ? "can't write to the card" : "not enough memory";
    }
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&sha, PSA_ALG_SHA_256);
    size_t total = (bytes + 2) / 3 * 4, done = 0;
    long written = 0;
    printf("recv: ready %d\n", RECV_BLOCK);
    fflush(stdout);
    while (!why && done < total) {
        size_t n = total - done < RECV_BLOCK ? total - done : RECV_BLOCK, out = 0;
        why = read_block(b64, n);
        if (!why && mbedtls_base64_decode(bin, RECV_BLOCK / 4 * 3, &out, (unsigned char *)b64, n) != 0) {
            why = "bad base64";
        }
        if (!why && fwrite(bin, 1, out, f) != out) {
            why = "can't write to the card";
        }
        if (!why) {
            psa_hash_update(&sha, bin, out);
            written += out;
            done += n;
            if (done < total) {
                printf("recv: more %d\n", RECV_BLOCK);
                fflush(stdout);
            }
        }
    }
    uint8_t got[32];
    size_t sha_len;
    psa_hash_finish(&sha, got, 32, &sha_len);
    psa_hash_abort(&sha);
    free(b64);
    free(bin);
    if (fclose(f) != 0 && !why) {
        why = "can't write to the card";
    }
    if (!why && written != bytes) {
        why = "wrong size";
    }
    if (!why && memcmp(got, want, 32)) {
        why = "the SHA-256 doesn't match";
    }
    if (!why && owner) {
        unlink(path);
        why = rename(part, path) == 0 ? NULL : "can't write it to the card";
    } else if (!why) {
        why = finish_file(id, file, part, path);
    }
    if (why) {
        unlink(part);
    }
    return why;
}

static int cmd_recv(const char *id, const char *file, const char *bytes, const char *sha)
{
    char rel[128];
    uint8_t want[32];
    char *end;
    long n = strtol(bytes, &end, 10);
    if ((!lua_pkg_file_rel(rel, sizeof(rel), id, file, NULL) && !owner_file_rel(rel, sizeof(rel), id, file)) ||
        end == bytes || *end || n < 0 || !parse_sha(sha, want)) {
        printf("usage: lua recv <id> <main.lua|app.json|app.wasm|bundled path|files/<name>> <bytes> <sha256>\n");
        return 1;
    }
    const char *why = receive(id, file, n, want);
    if (why) {
        printf("recv: error %s\n", why);
        return 1;
    }
    printf("recv: ok %ld\n", n);
    return 0;
}

// ---------------------------------------------------------------------------
// sums: one command at a time on the console, so the paths can be static.

static char s_rel[128], s_path[160], s_sub[LUA_PKG_PATH_MAX + 1];

static int sum_file(const char *id, const char *file)
{
    uint8_t sha[32];
    char hex[65];
    long size;
    if (!lua_pkg_file_rel(s_rel, sizeof(s_rel), id, file, NULL)) {
        return 0;                           // not a name the app could have shipped
    }
    os_data_path(s_path, sizeof(s_path), s_rel);
    if (!lua_pkg_sha256_file(s_path, sha, &size)) {
        return 0;
    }
    lua_pkg_hex(hex, sha, sizeof(sha));
    printf("%s %ld %s\n", hex, size, file);
    return 1;
}

// The files under files/<s_sub>, depth first.
static int sum_dir(const char *id, int depth)
{
    snprintf(s_rel, sizeof(s_rel), "apps/%s/files%s%s", id, s_sub[0] ? "/" : "", s_sub);
    os_data_path(s_path, sizeof(s_path), s_rel);
    DIR *d = opendir(s_path);
    if (!d) {
        return 0;
    }
    int n = 0;
    size_t base = strlen(s_sub);
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || base + 1 + len > LUA_PKG_PATH_MAX) {
            continue;
        }
        bool dir = e->d_type == DT_DIR;
        char *at = s_sub + base;
        if (base) {
            *at++ = '/';
        }
        memcpy(at, e->d_name, len + 1);
        if (!dir) {
            n += sum_file(id, s_sub);
        } else if (depth < LUA_PKG_PATH_DEPTH) {
            n += sum_dir(id, depth + 1);
        }
        s_sub[base] = 0;
    }
    closedir(d);
    return n;
}

static int cmd_sums(const char *id)
{
    if (!lua_pkg_valid_id(id)) {
        printf("usage: lua sums <id>\n");
        return 1;
    }
    if (!os_data_ready()) {
        printf("sums: no microSD card\n");
        return 1;
    }
    int n = sum_file(id, "main.lua") + sum_file(id, "app.json") + sum_file(id, "app.wasm");
    s_sub[0] = 0;
    n += sum_dir(id, 1);
    printf("sums: %d files\n", n);
    return 0;
}

static int cmd_install(const char *id)
{
    esp_err_t err = lua_install_start(id, NULL);
    if (err != ESP_OK) {
        printf("can't install %s: %s\n", id, err == ESP_ERR_INVALID_STATE ? "another install is running"
               : err == ESP_ERR_NOT_FOUND ? "no microSD card" : esp_err_to_name(err));
        return 1;
    }
    printf("installing %s from the App Store (the log and a toast tell how it went)\n", id);
    return 0;
}

// ---------------------------------------------------------------------------
// aot: why a C app runs app.aot or app.wasm (lua_aot.h). Reads and verifies,
// never writes app.aot: native code comes only from the store installer.

// `lua aotkey`: one request counts at a time, and everything about it lives
// on the OS task (the command posts there, and os_api's answers run there).
// So the server number a request was sent under can't be replaced under its
// answer (a second `lua aotkey` after `server <url>` would otherwise lend its
// number to the first one's answer, and the old store's key would be cached),
// and nothing is allocated: os_api drops an answer when the OS queue stays
// full, which would leak a request's record.
static void *s_aotkey_ctx;          // the request whose answer counts; NULL: none
static uint32_t s_aotkey_gen;       // os_cloud_server_gen() when it was sent

static void aot_key_answered(int status, const char *body, void *ctx)
{
    if (ctx != s_aotkey_ctx) {
        return;                     // a later `lua aotkey` took its place
    }
    s_aotkey_ctx = NULL;
    uint8_t key[32];
    char hex[9];
    if (s_aotkey_gen != os_cloud_server_gen()) {
        printf("aotkey: the server changed meanwhile; ask again\n");
    } else if (lw_aot_key_answer(status, body, s_aotkey_gen) && lw_aot_key(key)) {
        lua_pkg_hex(hex, key, 4);
        printf("aotkey: cached %s...\n", hex);
    } else {
        printf("aotkey: the server sent no key (HTTP %d)\n", status);
    }
}

// OS task.
static void aot_key_ask(void *arg)
{
    (void)arg;
    if (s_aotkey_ctx) {
        os_api_cancel(s_aotkey_ctx);    // an earlier one still on its way: only the newest counts
    }
    s_aotkey_ctx = os_api_token();
    s_aotkey_gen = os_cloud_server_gen();
    esp_err_t err = os_api_call("GET", "/api/device/aotkey", NULL, 256, aot_key_answered, s_aotkey_ctx);
    if (err != ESP_OK) {
        s_aotkey_ctx = NULL;
        printf("aotkey: %s\n", err == ESP_ERR_INVALID_STATE ? "offline (Wi-Fi on, and a registered badge)"
               : "the server queue is full; try again");
        return;
    }
    printf("aotkey: asking the server\n");
}

static int cmd_aot_key(void)
{
    if (!os_post(aot_key_ask, NULL)) {
        printf("aotkey: the badge is busy; try again\n");
        return 1;
    }
    return 0;
}

static int cmd_aot(const char *id)
{
    char rel[64], path[160], hex[65];
    lua_pkg_t *pkg = malloc(sizeof(*pkg));  // ~400 bytes: the console's stack is small
    if (!lua_pkg_valid_id(id) || !os_data_ready() || !pkg || !lua_pkg_load(id, pkg)) {
        printf("aot %s: %s\n", id, !os_data_ready() ? "no microSD card" : !pkg ? "not enough memory"
               : "no such app installed");
        free(pkg);
        return 1;
    }
    if (pkg->lang != LUA_PKG_LANG_C) {
        printf("aot %s: a Lua app (native code is for C apps)\n", id);
        free(pkg);
        return 1;
    }
    // The manifest's code.aot as the store wrote it, whatever its target
    // (lua_pkg_parse keeps only this build's).
    lua_pkg_rel(rel, sizeof(rel), id, "app.json");
    char *json = os_data_read(rel, LUA_PKG_JSON_MAX, NULL);
    cJSON *m = json ? cJSON_Parse(json) : NULL;
    free(json);
    const cJSON *aot = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "code"), "aot");
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(aot, "target");
    const cJSON *sig = cJSON_GetObjectItemCaseSensitive(aot, "sig");
    char shown[48];
    lua_pkg_plain(shown, sizeof(shown), cJSON_IsString(target) ? target->valuestring : "");
    bool ours = cJSON_IsString(target) && !strcmp(target->valuestring, LW_AOT_TARGET);
    printf("aot %s (%s):\n  this build's target: %s\n", id, pkg->version, LW_AOT_TARGET);
    if (!aot) {
        printf("  manifest's target:   none (the store sent no native code)\n");
    } else {
        printf("  manifest's target:   %s (%s)\n", shown[0] ? shown : "?", ours ? "this build's" : "another build's");
    }
    uint8_t key[32];
    bool has_key = lw_aot_key(key);
    if (has_key) {
        lua_pkg_hex(hex, key, 4);
        printf("  store's key:         cached, %s...\n", hex);
    } else {
        printf("  store's key:         not cached (lua aotkey fetches it)\n");
    }
    // A launch the cached key couldn't vouch for asks the server again, once
    // per boot (lua_aot.h).
    int64_t at;
    uint8_t refresh = lw_aot_key_refresh_state(&at);
    if (refresh != LW_KEY_REFRESH_NONE) {
        long ago = refresh == LW_KEY_REFRESH_ASKED ? 0 : (long)((esp_timer_get_time() - at) / 1000000);
        printf("  key refresh:         %s", refresh == LW_KEY_REFRESH_ASKED ? "asked the server, no answer yet"
               : refresh == LW_KEY_REFRESH_NEW ? "refreshed: the server sent a new key"
               : refresh == LW_KEY_REFRESH_SAME ? "refreshed: the server sent the same key"
               : "the server sent no key");
        if (refresh != LW_KEY_REFRESH_ASKED) {
            printf(" %ld s ago", ago);
        }
        printf(" (after a launch without a key that verified)\n");
    }
    const char *verdict = !aot ? "none" : !ours ? "not checked (another target)"
                          : !pkg->aot ? (cJSON_IsString(sig) ? "unreadable in app.json" : "none in app.json")
                          : !has_key ? "not checked (no key)"
                          : lw_aot_sig_ok(key, pkg->aot_sha, pkg->aot_sig) ? "verifies" : "doesn't verify";
    printf("  signature:           %s\n", verdict);
    cJSON_Delete(m);
    // The file itself: at launch its bytes are hashed again through the slot
    // (lua_xip.c), so a file that isn't what was signed never runs.
    long size = -1;
    uint8_t sha[32];
    lua_pkg_rel(rel, sizeof(rel), id, "app.aot");
    os_data_path(path, sizeof(path), rel);
    bool hashed = lua_pkg_sha256_file(path, sha, &size);
    bool same = hashed && pkg->aot && size == pkg->aot_size && !memcmp(sha, pkg->aot_sha, 32);
    if (hashed) {
        printf("  app.aot:             %ld bytes on the card, %s; %s\n", size,
               size <= LUA_XIP_MAX ? "fits a flash slot" : "too big for a flash slot",
               !pkg->aot ? "nothing signed to compare" : same ? "the signed SHA-256" : "NOT the signed SHA-256");
    } else if (pkg->aot) {
        printf("  app.aot:             not on the card; the manifest says %ld bytes, %s\n", (long)pkg->aot_size,
               pkg->aot_size <= LUA_XIP_MAX ? "fits a flash slot" : "too big for a flash slot");
    } else {
        printf("  app.aot:             not on the card\n");
    }
    const char *no = lw_aot_refusal(pkg, path, sizeof(path), &size, NULL);
    struct stat st;
    lua_pkg_rel(rel, sizeof(rel), id, "app.wasm");
    os_data_path(path, sizeof(path), rel);
    bool wasm = stat(path, &st) == 0;
    if (!no && !same) {
        // A slot found by the signed SHA-256 is used without reading the card
        // (lua_xip.c), and hashed: what runs is the signed code either way.
        printf("  runs:                app.aot's signed code if a flash slot still holds it (checked at launch); "
               "else %s: the file on the card isn't what the store signed\n",
               wasm ? "app.wasm, interpreted" : "nothing (no app.wasm)");
    } else if (!no) {
        printf("  runs:                app.aot, native code run in place\n");
    } else if (wasm) {
        printf("  runs:                app.wasm, interpreted: %s\n",
               no[0] ? no : !aot ? "no native code from the store" : "no signed native code for this build");
    } else {
        printf("  runs:                nothing: %s, and there is no app.wasm\n", no[0] ? no : "no native code");
    }
    free(pkg);
    return 0;
}

// ---------------------------------------------------------------------------

static int cmd_lua(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "list";
    if (!strcmp(sub, "list")) {
        return cmd_list();
    } else if (!strcmp(sub, "run") && argc == 3) {
        return !lua_host_launch(argv[2]);
    } else if (!strcmp(sub, "rm") && (argc == 3 || argc == 4)) {
        return cmd_rm(argv[2], argc == 4 ? argv[3] : NULL);
    } else if (!strcmp(sub, "mem")) {
        return cmd_mem();
    } else if (!strcmp(sub, "put") && argc == 5) {
        return cmd_put(argv[2], argv[3], argv[4]);
    } else if (!strcmp(sub, "recv") && argc == 6) {
        return cmd_recv(argv[2], argv[3], argv[4], argv[5]);
    } else if (!strcmp(sub, "sums") && argc == 3) {
        return cmd_sums(argv[2]);
    } else if (!strcmp(sub, "install") && argc == 3) {
        return cmd_install(argv[2]);
    } else if (!strcmp(sub, "aotkey") && argc == 2) {
        return cmd_aot_key();
    } else if (!strcmp(sub, "aot") && argc == 3) {
        return cmd_aot(argv[2]);
    }
    printf("usage: lua [list|run <id>|rm <id> [file]|mem|sums <id>|install <id>|put <id> <file> begin|<base64>|end"
           "|recv <id> <file> <bytes> <sha256>|aot <id>|aotkey]\n");
    return 1;
}

void lh_cmd_register(void)
{
    os_cmd_register("lua", "lua [list|run|rm <id> [file]|mem|sums|install <id>|put <id> <file> ...|recv <id> <file> <bytes> <sha256>|aot <id>|aotkey]",
                    cmd_lua);
}
