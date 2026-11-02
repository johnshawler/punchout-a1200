/* host + qemu-m68k test: expand every PCM sound of sound/out_full/sounds.bin with the Amiga decoder (lz4.h, same as snd.c) and compare with the reference samples.
 * usage: lz4test sounds.bin REFDIR   (REFDIR/cXX.raw = the exact samples, written by `python3 sound/tools/build_sounds_full.py --dump-ref`)  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sounds.h"
#include "lz4.h"
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); static uint8_t blob[2000000]; size_t n = fread(blob, 1, sizeof blob, f); fclose(f);
    int bad = 0, cnt = 0;
    for (unsigned i = 1; i < SND_NDEFS; i++) {
        const snd_def_t *d = &snd_defs[i];
        if (d->kind != SND_KIND_PCM) continue;
        char name[512]; unsigned code = 0;
        for (code = 0; code < 256; code++) if (snd_cmd_def[code] == i) break;
        snprintf(name, sizeof name, "%s/c%02X.raw", argv[2], code);
        FILE *r = fopen(name, "rb"); if (!r) { printf("missing %s\n", name); bad++; continue; }
        static uint8_t ref[200000], out[200100]; size_t rn = fread(ref, 1, sizeof ref, r); fclose(r);
        memset(out, 0xA5, sizeof out);
        lz4_expand(blob + d->off, d->clen, out);
        if (d->delta) { int8_t a = 0; for (uint32_t k = 0; k < d->rlen; k++) { a = (int8_t)(a + (int8_t)out[k]); out[k] = (uint8_t)a; } }
        int ok = rn == d->rlen && !memcmp(out, ref, rn) && out[rn] == 0xA5 && out[rn + 1] == 0xA5 && out[rn + 2] == 0xA5 && out[rn + 3] == 0xA5;
        if (!ok) { bad++; printf("FAIL %02X (len %zu vs %u)\n", code, rn, (unsigned)d->rlen); }
        cnt++;
    }
    (void)n; printf("%d PCM sounds expanded, %d failures\n", cnt, bad);
    return bad != 0;
}
