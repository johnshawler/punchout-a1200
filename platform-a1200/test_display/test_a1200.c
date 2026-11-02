/* Host (x86, ASAN/UBSAN) and qemu-m68k tests of ../display.c (the A1200 / AGA display module).
 *   test_a1200 replay DIR [-n N] [-t STEP] [-x PERMILLE] [-s PERMILLE]   real game dumps (64 KB VRAM + 1 KB palette RAM + 1 KB shared RAM + trailer,
 *                                                                          bank = byte 67584): dirty marks come from the VRAM difference to the previous dump
 *   test_a1200 fuzz SEED ITER      random VRAM writes / palettes / banks, incl. spurious dirty marks and rows >= 240
 *   test_a1200 kernel SEED ITER    display_c2p_group() against a bit-by-bit reference
 * After every display_frame() the planes are decoded back to palette indices and compared with VRAM (rows 0..239), the loaded 256-colour palette is
 * compared with an independent decode (tools/dump2png.py colour function) of the displayed bank, and all dirty bytes of the converted rows are checked clear. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include "platform.h"
#include "hw_video.h"
#include "display.h"

uint8_t VRAM[0x10000] __attribute__((aligned(65536)));
struct vhw VHW;
#define GUARD 4096
static uint8_t arena[GUARD + PLAT_BUF_SIZE + GUARD] __attribute__((aligned(16)));
static uint32_t pal_loaded[256];
static unsigned n_pal_loads;
void *plat_front(void) { return arena + GUARD; }
void plat_clear_buffer(void *b) { memset(b, 0, PLAT_BUF_SIZE); }
void plat_set_palette256(const uint32_t rgb[256]) { memcpy(pal_loaded, rgb, 1024); n_pal_loads++; }

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static const uint8_t T[16] = { 0x00,0x12,0x24,0x49,0x12,0x24,0x49,0x92,0x5b,0x6d,0x92,0xdb,0x7f,0x91,0xb6,0xff };
static uint32_t ref_rgb(uint8_t raw)
{
    unsigned i = raw & 3;
    return ((uint32_t)T[(((raw >> 6) & 3) << 2) | i] << 16) | ((uint32_t)T[(((raw >> 4) & 3) << 2) | i] << 8) | T[(((raw >> 2) & 3) << 2) | i];
}

static unsigned long n_checks, n_fail;
static int verify(const char *what, unsigned frame)
{
    const uint8_t *pl = (const uint8_t *)plat_front();
    unsigned bad = 0;
    for (unsigned y = 0; y < PLAT_H && bad < 5; y++)
        for (unsigned x = 0; x < 256; x++) {
            unsigned v = 0;
            for (unsigned p = 0; p < 8; p++) v |= (unsigned)((pl[y * PLAT_STRIDE + p * PLAT_PLANE_STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1) << p;
            if (v != VRAM[(y + 4) * 256 + x]) { if (bad < 5) printf("FAIL %s frame %u: pixel (%u,%u) plane value %02X VRAM %02X\n", what, frame, x, y, v, VRAM[(y + 4) * 256 + x]); bad++; }
        }
    unsigned bank = VHW.palbank & 3;
    for (unsigned i = 0; i < 256; i++)
        if (pal_loaded[i] != ref_rgb(VHW.palram[bank * 256 + i])) { if (bad < 5) printf("FAIL %s frame %u: colour %u is %06X expected %06X\n", what, frame, i, pal_loaded[i], ref_rgb(VHW.palram[bank * 256 + i])); bad++; }
    for (unsigned g = 4 * 32; g < 244 * 32; g++) if (VHW.dirty[g]) { if (bad < 5) printf("FAIL %s frame %u: dirty group %u not cleared\n", what, frame, g); bad++; }
    for (unsigned r = 0; r < 256; r++) if (VHW.dirty_row[r]) { if (bad < 5) printf("FAIL %s frame %u: dirty row %u not cleared\n", what, frame, r); bad++; }
    for (unsigned i = 0; i < GUARD; i++) if (arena[i] || arena[GUARD + PLAT_BUF_SIZE + i]) { printf("FAIL %s frame %u: write outside the buffer\n", what, frame); bad++; break; }
    n_checks++;
    if (bad) n_fail++;
    return bad;
}

static int cmpname(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int replay(const char *dir, unsigned n, unsigned step, unsigned xper, unsigned sper)
{
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return 1; }
    char **names = NULL; unsigned nn = 0; struct dirent *e;
    while ((e = readdir(d))) if (strstr(e->d_name, ".bin")) { names = realloc(names, (nn + 1) * sizeof *names); names[nn++] = strdup(e->d_name); }
    closedir(d);
    qsort(names, nn, sizeof *names, cmpname);
    memset(&VHW, 0, sizeof VHW); memset(VRAM, 0, sizeof VRAM);
    plat_clear_buffer(plat_front());
    display_init();
    static uint8_t buf[70000], prev[65536];
    uint8_t prevpal[1024]; memset(prevpal, 0, 1024); memset(prev, 0, 65536);
    unsigned used = 0, groups = 0, pallo = 0;
    for (unsigned i = 0; i < nn && (!n || used < n); i += step) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb"); if (!f) continue;
        size_t got = fread(buf, 1, sizeof buf, f); fclose(f);
        if (got < 67585) continue;
        memcpy(VRAM, buf, 65536);
        memcpy(VHW.palram, buf + 65536, 1024);
        VHW.palbank = buf[67584] & 3;
        for (unsigned g = 0; g < 8192; g++)
            if (memcmp(VRAM + g * 8, prev + g * 8, 8) || (xper && rnd() % 1000 < xper)) { VHW.dirty[g] = 1; VHW.dirty_row[g >> 5] = 1; groups++; }
        if (memcmp(VHW.palram, prevpal, 1024) || (sper && rnd() % 1000 < sper)) VHW.pal_changed = 1;
        display_frame();
        used++;
        if (verify("replay", used) && n_fail > 3) return 1;
        memcpy(prev, VRAM, 65536); memcpy(prevpal, VHW.palram, 1024);
    }
    (void)pallo;
    for (unsigned i = 0; i < nn; i++) free(names[i]);
    free(names);
    printf("replay %s: %u dumps, %u dirty groups converted, %u palette loads, %lu checks, %lu failures\n", dir, used, groups, n_pal_loads, n_checks, n_fail);
    return n_fail != 0;
}

static int fuzz(unsigned seed, unsigned iter)
{
    rng = seed * 2654435761u + 1;
    memset(&VHW, 0, sizeof VHW);
    for (unsigned i = 0; i < 65536; i++) VRAM[i] = (uint8_t)rnd();
    for (unsigned i = 0; i < 1024; i++) VHW.palram[i] = (uint8_t)rnd();
    VHW.palbank = rnd() & 3;
    plat_clear_buffer(plat_front());
    display_init();          /* VRAM is not zero: everything must be marked dirty by init */
    display_frame();
    if (verify("fuzz-init", 0)) return 1;
    for (unsigned it = 1; it <= iter; it++) {
        unsigned nw = rnd() % 400;
        for (unsigned k = 0; k < nw; k++) {
            unsigned off = rnd() & 0xFFFF;
            unsigned run = 1 + (rnd() % 4 == 0 ? rnd() % 40 : 0);
            for (unsigned j = 0; j < run && off + j < 65536; j++) { VRAM[off + j] = (uint8_t)rnd(); VHW.dirty[(off + j) >> 3] = 1; VHW.dirty_row[(off + j) >> 8] = 1; }
        }
        if (rnd() % 10 == 0) { unsigned pn = rnd() % 20; for (unsigned k = 0; k < pn; k++) VHW.palram[rnd() % 1024] = (uint8_t)rnd(); VHW.pal_changed = 1; }
        if (rnd() % 15 == 0) VHW.palbank = rnd() & 3;                       /* bank flip, flag NOT set: must be noticed */
        if (rnd() % 20 == 0) VHW.pal_changed = 1;                           /* spurious flag */
        if (rnd() % 50 == 0) display_invalidate();
        /* rows >= 240 are written too: they must not appear on the screen and must not leave dirty_row set */
        display_frame();
        /* VRAM rows >= 240 keep their group marks (documented): clear them for the check of the next frame */
        if (verify("fuzz", it)) return 1;
        for (unsigned g = 0; g < 8192; g++) if (g < 4 * 32 || g >= 244 * 32) VHW.dirty[g] = 0;
    }
    printf("fuzz seed %u: %u iterations, %lu checks, %lu failures, %u palette loads\n", seed, iter, n_checks, n_fail, n_pal_loads);
    return n_fail != 0;
}

static int kernel(unsigned seed, unsigned iter)
{
    rng = seed * 2654435761u + 7;
    static uint8_t out[8 * PLAT_PLANE_STRIDE + 64] __attribute__((aligned(4)));
    uint8_t src[16] __attribute__((aligned(4)));
    for (unsigned it = 0; it < iter; it++) {
        for (int i = 0; i < 8; i++) src[i] = (uint8_t)rnd();
        if (it % 7 == 0) { uint8_t v = (uint8_t)rnd(); for (int i = 0; i < 8; i++) src[i] = v; }
        if (it % 11 == 0) { for (int i = 0; i < 8; i++) src[i] = (uint8_t)(rnd() & 1 ? 0xFF : 0); }
        memset(out, 0x5A, sizeof out);
        unsigned off = 3 + it % 5;
        display_c2p_group(src, out + off);
        for (unsigned p = 0; p < 8; p++)
            for (unsigned k = 0; k < 8; k++)
                if (((out[off + p * PLAT_PLANE_STRIDE] >> (7 - k)) & 1) != ((src[k] >> p) & 1)) { printf("FAIL kernel it %u plane %u pixel %u\n", it, p, k); return 1; }
        for (unsigned i = 0; i < sizeof out; i++) {
            int in = 0; for (unsigned p = 0; p < 8; p++) if (i == off + p * PLAT_PLANE_STRIDE) in = 1;
            if (!in && out[i] != 0x5A) { printf("FAIL kernel it %u: stray write at %u\n", it, i); return 1; }
        }
    }
    printf("kernel seed %u: %u groups ok\n", seed, iter);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "replay")) {
        unsigned n = 0, t = 1, x = 0, s = 0;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "-n")) n = atoi(argv[i + 1]);
            else if (!strcmp(argv[i], "-t")) t = atoi(argv[i + 1]);
            else if (!strcmp(argv[i], "-x")) x = atoi(argv[i + 1]);
            else if (!strcmp(argv[i], "-s")) s = atoi(argv[i + 1]);
        }
        return replay(argv[2], n, t ? t : 1, x, s);
    }
    if (argc >= 4 && !strcmp(argv[1], "fuzz")) return fuzz(atoi(argv[2]), atoi(argv[3]));
    if (argc >= 4 && !strcmp(argv[1], "kernel")) return kernel(atoi(argv[2]), atoi(argv[3]));
    fprintf(stderr, "usage: replay DIR [-n N] [-t STEP] [-x PERMILLE] [-s PERMILLE] | fuzz SEED ITER | kernel SEED ITER\n");
    return 2;
}
