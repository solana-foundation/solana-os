// files for Lua and C apps (docs/LUA_API.md "files", permission "files",
// Solana OS 2.3.0): the app's own folder on the card, hh/files/<id>/, for
// what the badge's owner copies there with a card reader (a ROM, a level
// pack, a font) and for the saves the app writes in its save/ subfolder.
//
// Bundled files (fs, lua_fs.c) are small and come from the store; these can
// be any size, so they are read a range at a time and never whole. The
// folder is the app's alone: a name is one file in it ("Tetris DX.gbc") or
// one in save/ ("save/Tetris DX.sav"), never a path, so nothing outside the
// folder can be named. The app reads everything in it and writes only in
// save/: the owner's own files are read-only to it. The folder lives under
// /sdcard/hh, so a factory reset or a new owner wipes it with everything else
// of the old owner's; removing the app leaves it (its saves outlive a reinstall).
//
// Reading and writing cost what fs does (LH_FS_OPEN_STEPS a file opened, one
// step per LH_FS_BYTES_PER_STEP bytes), and the file last read stays open
// (lh_t.files_f), so paging through a ROM seeks instead of reopening it.
// C apps read through lua_wasm.c's sos_file_read straight into their own
// memory (lh_files_read_into below): no Lua string in between.

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core/os_data.h"
#include "lua_priv.h"

#define SAVE_PREFIX     "save/"
#define LIST_MAX        128

// A file's name: printable ASCII but the characters FAT and paths reserve, not
// starting with '.' or a space (".DS_Store", "._x" files a Mac leaves), not
// ending with '.' or a space (FAT drops those). 0: not a name; 1: one of the
// owner's files; 2: a save (in save/).
int lh_files_kind(const char *p, size_t len)
{
    int kind = 1;
    if (len > 5 && !memcmp(p, SAVE_PREFIX, 5)) {
        p += 5;
        len -= 5;
        kind = 2;
    }
    if (!len || len > LH_FILES_NAME_MAX || p[0] == '.' || p[0] == ' ' || p[len - 1] == '.' || p[len - 1] == ' ') {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c > 0x7e || strchr("/\\:*?\"<>|", c)) {
            return 0;
        }
    }
    return kind;
}

// The card path of a checked name (or of the folder, for name NULL).
static void files_path(const lh_t *s, const char *name, size_t len, char *out, size_t n)
{
    char rel[24 + LUA_PKG_ID_MAX + 8 + LH_FILES_NAME_MAX];
    if (name) {
        snprintf(rel, sizeof(rel), "files/%s/%.*s", s->pkg.id, (int)len, name);
    } else {
        snprintf(rel, sizeof(rel), "files/%s", s->pkg.id);
    }
    os_data_path(out, n, rel);
}

void lh_files_close(lh_t *s)
{
    if (s->files_f) {
        fclose(s->files_f);
        s->files_f = NULL;
    }
    s->files_name[0] = 0;
}

// The folder and its save/, made at launch so the owner finds where to copy
// their files (an app's first launch shows the folder to put them in).
void lh_files_prepare(lh_t *s)
{
    if (!(s->pkg.perms & LUA_PERM_FILES)) {
        return;
    }
    char rel[24 + LUA_PKG_ID_MAX];
    snprintf(rel, sizeof(rel), "files/%s/save/x", s->pkg.id);
    os_data_mkdirs(rel);
}

// The cached file for name, opened for reading if it isn't.
static FILE *open_read(lh_t *s, const char *name, size_t len, long *size, unsigned *steps)
{
    if (s->files_f && strlen(s->files_name) == len && !memcmp(s->files_name, name, len)) {
        *size = s->files_size;
        return s->files_f;
    }
    lh_files_close(s);
    *steps += LH_FS_OPEN_STEPS;
    char path[200];
    struct stat st;
    files_path(s, name, len, path, sizeof(path));
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (f) {
        s->files_f = f;
        s->files_size = (long)st.st_size;
        memcpy(s->files_name, name, len);
        s->files_name[len] = 0;
        *size = s->files_size;
    }
    return f;
}

long lh_files_read_into(lh_t *s, const char *name, size_t nlen, uint32_t off, void *dst, uint32_t len,
                        unsigned *steps, const char **err)
{
    *err = NULL;
    if (!lh_files_kind(name, nlen)) {
        *err = "not a file name";
        return -1;
    }
    long size;
    FILE *f = open_read(s, name, nlen, &size, steps);
    if (!f) {
        *err = "not found";
        return -1;
    }
    if ((uint64_t)off >= (uint64_t)size || !len) {
        return 0;                           // (compared unsigned: an offset past 2 GB isn't negative)
    }
    uint32_t n = (uint64_t)len < (uint64_t)size - off ? len : (uint32_t)((uint64_t)size - off);
    *steps += n / LH_FS_BYTES_PER_STEP;
    size_t got = fseek(f, (long)off, SEEK_SET) == 0 ? fread(dst, 1, n, f) : 0;
    if (got != n) {
        lh_files_close(s);
        *err = "couldn't read it from the microSD card";
        return -1;
    }
    return (long)n;
}

// ---------------------------------------------------------------------------
// Lua

static const char *check_name(lua_State *L, int idx, size_t *len, int *kind)
{
    const char *p = luaL_checklstring(L, idx, len);
    *kind = strlen(p) == *len ? lh_files_kind(p, *len) : 0;
    if (!*kind) {
        luaL_argerror(L, idx, "a file name in the app's folder, or save/<name>");
    }
    return p;
}

static void need(lua_State *L)
{
    lh_need_perm(L, LUA_PERM_FILES, "files");
}

static int l_dir(lua_State *L)
{
    need(L);
    lua_pushfstring(L, "hh/files/%s/", lh_get(L)->pkg.id);
    return 1;
}

static int l_size(lua_State *L)
{
    need(L);
    lh_t *s = lh_get(L);
    size_t len;
    int kind;
    const char *name = check_name(L, 1, &len, &kind);
    unsigned steps = 0;
    long size;
    FILE *f = open_read(s, name, len, &size, &steps);
    lh_charge_steps(L, steps);
    if (!f) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)size);
    return 1;
}

typedef struct {
    lh_t *s;
    const char *name;
    size_t nlen;
    uint32_t off;
    long got;
    const char *err;
} fill_t;

static size_t fill(void *ud, char *buf, size_t len)
{
    fill_t *r = ud;
    unsigned steps = 0;
    r->got = lh_files_read_into(r->s, r->name, r->nlen, r->off, buf, (uint32_t)len, &steps, &r->err);
    return r->got < 0 ? 0 : (size_t)r->got;
}

// files.read(name, offset, length): up to length bytes from offset, fewer at
// the end of the file ("" past it); nil, why when there's no such file.
static int l_read(lua_State *L)
{
    need(L);
    lh_t *s = lh_get(L);
    size_t len;
    int kind;
    const char *name = check_name(L, 1, &len, &kind);
    lua_Integer off = luaL_optinteger(L, 2, 0), n = luaL_optinteger(L, 3, LH_FILES_READ_MAX);
    luaL_argcheck(L, off >= 0, 2, "an offset from 0");
    luaL_argcheck(L, n >= 0 && n <= LH_FILES_READ_MAX, 3, "at most 256 KB per read");
    unsigned steps = 0;
    long size;
    if (!open_read(s, name, len, &size, &steps)) {
        lh_charge_steps(L, steps);
        lua_pushnil(L);
        lua_pushliteral(L, "not found");
        return 2;
    }
    long want = off >= size ? 0 : (n < size - off ? (long)n : size - (long)off);
    lh_charge_steps(L, steps + (unsigned)(want / LH_FS_BYTES_PER_STEP));
    fill_t r = { .s = s, .name = name, .nlen = len, .off = (uint32_t)off };
    // The string is filled in place, with no collection in between
    // (lua_pushfilled), so the cached file can't be swapped under it.
    lua_pushfilled(L, (size_t)want, fill, &r);
    if (r.err) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, r.err);
        return 2;
    }
    return 1;
}

static int count_saves(lh_t *s)
{
    char path[200];
    files_path(s, SAVE_PREFIX, 4, path, sizeof(path));     // .../save
    DIR *d = opendir(path);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        n += e->d_name[0] != '.';
    }
    if (d) {
        closedir(d);
    }
    return n;
}

// files.write(save/<name>, offset, data): writes data at offset (at most the
// file's size: no holes), making the file if it isn't there. true, or nil, why.
static int l_write(lua_State *L)
{
    need(L);
    lh_t *s = lh_get(L);
    size_t len, dlen;
    int kind;
    const char *name = check_name(L, 1, &len, &kind);
    lua_Integer off = luaL_checkinteger(L, 2);
    const char *data = luaL_checklstring(L, 3, &dlen);
    luaL_argcheck(L, kind == 2, 1, "the app writes only in its save/ folder");
    luaL_argcheck(L, dlen <= LH_FILES_READ_MAX, 3, "at most 256 KB per write");
    char path[200];
    struct stat st;
    files_path(s, name, len, path, sizeof(path));
    long size = stat(path, &st) == 0 ? (long)st.st_size : -1;
    lh_charge_steps(L, LH_FS_OPEN_STEPS + (unsigned)(dlen / LH_FS_BYTES_PER_STEP));
    const char *why = NULL;
    if (off < 0 || off > (size < 0 ? 0 : size)) {
        why = "the offset is past the end of the file";
    } else if ((uint64_t)off + dlen > LH_FILES_SAVE_MAX) {
        why = "a save file is at most 1 MB";
    } else if (size < 0 && count_saves(s) >= LH_FILES_SAVES_MAX) {
        why = "at most 32 files in save/";
    }
    if (!why) {
        lh_files_close(s);                  // it may be the cached one
        char dir[24 + LUA_PKG_ID_MAX];
        snprintf(dir, sizeof(dir), "files/%s/save/x", s->pkg.id);
        os_data_mkdirs(dir);
        FILE *f = fopen(path, size < 0 ? "wb" : "r+b");
        bool ok = f && fseek(f, (long)off, SEEK_SET) == 0 && fwrite(data, 1, dlen, f) == dlen;
        if (f && fclose(f) != 0) {
            ok = false;
        }
        why = ok ? NULL : "couldn't write it to the microSD card";
    }
    if (why) {
        lua_pushnil(L);
        lua_pushstring(L, why);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_remove(lua_State *L)
{
    need(L);
    lh_t *s = lh_get(L);
    size_t len;
    int kind;
    const char *name = check_name(L, 1, &len, &kind);
    luaL_argcheck(L, kind == 2, 1, "the app removes only its own saves (save/)");
    char path[200];
    files_path(s, name, len, path, sizeof(path));
    lh_files_close(s);
    lh_charge_steps(L, LH_FS_OPEN_STEPS);
    lua_pushboolean(L, unlink(path) == 0);
    return 1;
}

typedef struct {
    char name[LH_FILES_NAME_MAX + 6];
    long size;
} list_row_t;

// One folder's regular files into rows (sub: NULL for the top, "save"). The
// rows are a userdata made before the directory is opened, so nothing that
// can raise runs while it is open.
static int list_dir(lh_t *s, const char *sub, list_row_t *rows, int n)
{
    char path[200], file[200 + 256];
    files_path(s, sub, sub ? strlen(sub) : 0, path, sizeof(path));
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && n < LIST_MAX && (e = readdir(d))) {
        char full[LH_FILES_NAME_MAX + 6];
        int k = snprintf(full, sizeof(full), "%s%s", sub ? SAVE_PREFIX : "", e->d_name);
        if (k <= 0 || (size_t)k >= sizeof(full) || !lh_files_kind(full, (size_t)k)) {
            continue;
        }
        struct stat st;
        snprintf(file, sizeof(file), "%s/%s", path, e->d_name);
        if (stat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        memcpy(rows[n].name, full, (size_t)k + 1);
        rows[n++].size = (long)st.st_size;
    }
    if (d) {
        closedir(d);
    }
    return n;
}

// files.list(): the owner's files, then the saves ("save/<name>"), at most 128.
static int l_list(lua_State *L)
{
    need(L);
    lh_t *s = lh_get(L);
    lh_charge_steps(L, 2 * LH_FS_OPEN_STEPS);
    list_row_t *rows = lua_newuserdatauv(L, sizeof(list_row_t) * LIST_MAX, 0);
    int n = list_dir(s, NULL, rows, 0);
    n = list_dir(s, "save", rows, n);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, rows[i].name);
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, rows[i].size);
        lua_setfield(L, -2, "size");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

void lh_open_files(lua_State *L)
{
    static const luaL_Reg fns[] = {
        {"dir", l_dir}, {"list", l_list}, {"size", l_size}, {"read", l_read},
        {"write", l_write}, {"remove", l_remove}, {NULL, NULL},
    };
    luaL_newlib(L, fns);
    lua_setglobal(L, "files");
}
