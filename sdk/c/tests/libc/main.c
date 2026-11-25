// libc probe: wasi-libc as a C app links it (string, snprintf, malloc and
// memory growth up to the 4 MB maximum, math, qsort, strtol, 64-bit integers),
// in the reactor model. It imports only WASI, so it runs on WAMR's own iwasm
// as well as on a badge:
//
//   tools/cc/badge-cc sdk/c/tests/libc -o libc.wasm
//   iwasm --interp -f run libc.wasm        # WAMR 2.4.5's release iwasm
//
// on_start (what a badge calls) runs the same checks.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "badge_abi.h"

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
__attribute__((export_name("run"))) int run(void) {
    char buf[128];
    int fails = 0;
    snprintf(buf, sizeof buf, "%d %s %.3f %x %5.1e", 42, "badge", 3.14159, 0xbeef, 12345.678);
    printf("snprintf: [%s]\n", buf);
    if (strcmp(buf, "42 badge 3.142 beef 1.2e+04") != 0) fails++;
    int *a = malloc(1000 * sizeof(int));
    for (int i = 0; i < 1000; i++) a[i] = (i * 7919) % 1000;
    qsort(a, 1000, sizeof(int), cmp);
    for (int i = 0; i < 1000; i++) if (a[i] != i) { fails++; break; }
    printf("qsort: %d %d %d\n", a[0], a[500], a[999]);
    char * volatile big = malloc(2 * 1024 * 1024);           // grows memory past the start
    if (!big) fails++; else { memset(big, 7, 2 * 1024 * 1024); if (big[2097151] != 7) fails++; }
    printf("malloc 2 MB: memory now %lu KB\n", (unsigned long)__builtin_wasm_memory_size(0) * 64);
    free(big);
    void * volatile huge = malloc(8 * 1024 * 1024);           // over the 4 MB maximum: must fail cleanly
    printf("malloc 8 MB: %s (memory %lu KB)\n", huge ? "got it (wrong)" : "NULL (right)", (unsigned long)__builtin_wasm_memory_size(0) * 64);
    if (huge) fails++;
    double s = sin(1.0), r = sqrt(2.0), p = pow(2.0, 10.5), l = log(10.0);
    float sf = sinf(0.5f), ef = expf(1.0f);
    printf("math: %.6f %.6f %.4f %.6f %.6f %.6f\n", s, r, p, l, sf, ef);
    if (fabs(s - 0.841471) > 1e-6 || fabs(r - 1.414214) > 1e-6 || fabs(l - 2.302585) > 1e-6) fails++;
    char *d = strdup("hello, badge"); char *t = strtok(d, ", ");
    printf("strtok: %s strlen %zu strstr %s\n", t, strlen("hello, badge"), strstr("hello, badge", "bad"));
    free(d); free(a);
    long long ll = 1LL << 40; printf("int64: %lld %llu\n", ll, (unsigned long long)ll * 3);
    printf("atoi %d strtol %ld strtod %g\n", atoi("123"), strtol("-ff", 0, 16), strtod("2.5e3", 0));
    printf("clock %s, rand %d\n", time(NULL) > 0 ? "set" : "zero", rand() % 100);
    printf("libc probe: %s\n", fails ? "FAIL" : "ok");
    fflush(stdout);
    return fails;
}

void on_start(void)
{
    run();
}
