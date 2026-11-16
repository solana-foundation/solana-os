// Helpers for the C app regression apps (tests/c/, not shipped). They build
// without a libc (-nostdlib), straight against badge_abi.h, so each test
// sees exactly the imports it calls. Every check prints one line:
// "ok <what>" or "FAIL <what>", which run.ts counts.
#pragma once

#include "badge_abi.h"

typedef __SIZE_TYPE__ size_t;

// What clang may call for struct copies and array initialisers.
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *o = d;
    const unsigned char *i = s;
    while (n--) {
        *o++ = *i++;
    }
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *o = d;
    while (n--) {
        *o++ = (unsigned char)c;
    }
    return d;
}

size_t strlen(const char *s)
{
    const char *e = s;
    while (*e) {
        e++;
    }
    return (size_t)(e - s);
}

static unsigned slen(const char *s)
{
    unsigned n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int seq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++, b++;
    }
    return *a == *b;
}

// "<prefix><n>" into out.
static char *fmt_int(char *out, const char *prefix, int v)
{
    char *o = out, digits[12];
    int n = 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    while (*prefix) {
        *o++ = *prefix++;
    }
    if (v < 0) {
        *o++ = '-';
    }
    do {
        digits[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n) {
        *o++ = digits[--n];
    }
    *o = 0;
    return out;
}

static void say(const char *s)
{
    sos_log(s, slen(s));
}

static void say_int(const char *prefix, int v)
{
    char b[64];
    say(fmt_int(b, prefix, v));
}

static void check(int ok, const char *what)
{
    char b[120], *o = b;
    const char *p = ok ? "ok " : "FAIL ";
    while (*p) {
        *o++ = *p++;
    }
    while (*what && o < b + sizeof(b) - 1) {
        *o++ = *what++;
    }
    *o = 0;
    say(b);
}

static int G(const char *path)
{
    return sos_global(path, slen(path));
}

static void push_s(const char *s)
{
    sos_push_str(s, slen(s));
}

// t[key] = the top value
static void set_field(int t, const char *key)
{
    sos_set(t, key, slen(key));
}

// The method `name` of the object at idx, called with the object and nargs
// values already on top: obj:name(...). Leaves nres results.
static int method(int obj, const char *name, int nargs, int nres)
{
    sos_get(obj, name, slen(name));         // fn
    sos_push_value(obj);                    // self
    // move fn and self below the arguments
    for (int i = 0; i < nargs; i++) {
        sos_push_value(-2 - nargs);
        sos_remove(-3 - nargs);
    }
    return sos_call(nargs + 1, nres);
}
