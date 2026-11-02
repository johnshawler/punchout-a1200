/* LZ4 block decoder for the sound data (sound/tools/lz4pack.py).  Unaligned 32-bit accesses are legal on the 68020; the host test (a1200/test/lz4test.c) compiles
 * this file on a little-endian machine where the long copies are just as correct (they copy bytes in order). */
#ifndef LZ4_H
#define LZ4_H
#include <stdint.h>
/* LZ4 block decoder (unaligned long accesses are legal on the 68020) */
static inline void lz4_expand(const uint8_t *s, uint32_t clen, uint8_t *d)
{
    const uint8_t *e = s + clen;
    for (;;) {
        unsigned tok = *s++;
        uint32_t ll = tok >> 4, ml;
        if (ll == 15) { unsigned b; do { b = *s++; ll += b; } while (b == 255); }
        while (ll >= 4) { *(uint32_t *)d = *(const uint32_t *)s; d += 4; s += 4; ll -= 4; }
        while (ll--) *d++ = *s++;
        if (s >= e) break;
        {
            uint32_t off = (uint32_t)s[0] | ((uint32_t)s[1] << 8);
            const uint8_t *m;
            s += 2; ml = tok & 15;
            if (ml == 15) { unsigned b; do { b = *s++; ml += b; } while (b == 255); }
            ml += 4; m = d - off;
            if (off == 1) {                                         /* run of one byte */
                uint32_t v = *m; v |= v << 8; v |= v << 16;
                while (ml >= 4) { *(uint32_t *)d = v; d += 4; ml -= 4; }
                while (ml--) *d++ = (uint8_t)v;
            } else if (off >= 4) {
                while (ml >= 4) { *(uint32_t *)d = *(const uint32_t *)m; d += 4; m += 4; ml -= 4; }
                while (ml--) *d++ = *m++;
            } else {
                while (ml--) *d++ = *m++;
            }
        }
    }
}

#endif
