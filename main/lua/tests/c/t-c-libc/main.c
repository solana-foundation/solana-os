// A C app built like a store app (tools/cc/badge-cc: wasi-libc, a reactor):
// constructors before on_start, printf through fd_write, malloc growing
// linear memory, the clock, qsort, badge.h's typed API and a timer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "badge.h"

static int s_ctor;
static char *volatile s_keep;       // the MB escapes, so the compiler can't drop the malloc
static ui_obj s_label;

__attribute__((constructor)) static void early(void)
{
    s_ctor = 7;
}

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
}

static int cmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static void later(void *user)
{
    check(user == &s_ctor, "timer_after called back with its user pointer");
    check(ui_label_text(s_label, "timer fired") == 0, "ui_label_text");
    printf("done\n");
}

void on_start(void)
{
    check(s_ctor == 7, "constructors ran before on_start (_initialize)");
    printf("printf %d %s %.2f %x\n", 42, "str", 3.14159, 255);
    char *big = malloc(1024 * 1024);
    s_keep = big;
    check(big != NULL, "malloc 1 MB (memory.grow)");
    if (big) {
        memset(big, 0xAB, 1024 * 1024);
        check(big[1024 * 1024 - 1] == (char)0xAB, "the whole MB is usable");
    }
    int32_t used = 0, max = 0;
    check(sys_memory(&used, &max) == 0 && used > 1024 * 1024, "sys_memory counts the module's memory");
    free(s_keep);
    printf("sys_memory used %ld max %ld\n", (long)used, (long)max);
    struct timespec t;
    check(clock_gettime(CLOCK_MONOTONIC, &t) == 0 && (t.tv_sec > 0 || t.tv_nsec > 0), "clock_gettime monotonic");
    int v[] = {5, 3, 9, 1, 7};
    qsort(v, 5, sizeof v[0], cmp);
    check(v[0] == 1 && v[4] == 9, "qsort");
    s_label = ui_label(10, 10, "libc test", NULL);
    check(s_label.ref != 0, "ui_label");
    check(storage_set_int("n", 5) == 0 && storage_get_int("n", 0) == 5, "storage");
    check(timer_after(100, later, &s_ctor) >= 0, "timer_after");
    fprintf(stderr, "stderr line\n");
}
