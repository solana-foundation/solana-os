// Host driver for ed25519_test.py: reads lines "v <pub> <sig> <msg>" or
// "c <point>" (hex; an empty message is "-") and prints 1 or 0 for each.
#include <stdio.h>
#include <string.h>
#include "core/ed25519.h"

static size_t unhex(const char *h, uint8_t *out, size_t max)
{
    size_t n = 0;
    if (!strcmp(h, "-")) {
        return 0;
    }
    while (h[0] && h[1] && n < max) {
        unsigned b;
        sscanf(h, "%2x", &b);
        out[n++] = (uint8_t)b;
        h += 2;
    }
    return n;
}

int main(void)
{
    static char line[70000];
    static uint8_t msg[32768];
    char a[8], b[200], c[200], d[66000];
    while (fgets(line, sizeof(line), stdin)) {
        uint8_t pub[32], sig[64];
        if (sscanf(line, "%7s %199s %199s %65999s", a, b, c, d) == 4 && a[0] == 'v') {
            unhex(b, pub, 32);
            unhex(c, sig, 64);
            size_t n = unhex(d, msg, sizeof(msg));
            printf("%d\n", ed25519_verify(pub, msg, n, sig));
        } else if (sscanf(line, "%7s %199s", a, b) == 2 && a[0] == 'c') {
            unhex(b, pub, 32);
            printf("%d\n", ed25519_on_curve(pub));
        }
    }
    return 0;
}
