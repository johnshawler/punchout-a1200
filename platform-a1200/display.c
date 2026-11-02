/* display.c - Zoo Keeper Amiga 1200 display module (AGA, 8 planes).  See display.h. */
#include <stdint.h>
#include "platform.h"
#include "hw_video.h"
#include "display.h"

extern uint8_t VRAM[0x10000];

#define NROWS PLAT_H
#define VROW0 4        /* first displayed VRAM row: rows 0..3 hold a colour-bar/test pattern that is outside the arcade's visible picture (monitor overscan) */
#define GROUPS_PER_ROW 32

/* arcade palette byte -> 0x00RRGGBB (tools/dump2png.py colour function: two bits per gun plus a common 2-bit intensity, resistor-weighted) */
static const uint8_t GUN[16] = { 0x00,0x12,0x24,0x49,0x12,0x24,0x49,0x92,0x5b,0x6d,0x92,0xdb,0x7f,0x91,0xb6,0xff };
static uint32_t dec24[256];
static uint32_t pal_shadow[256];
static uint8_t  cur_bank, pal_valid;

/* big-endian 32-bit load (the m68k load is native; the host tests run on little-endian machines) */
static inline uint32_t ld32(const uint8_t *p)
{
    uint32_t v = *(const uint32_t *)p;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

/* 8 chunky pixels (a = pixels 0..3, b = pixels 4..7, pixel 0 in the top byte) -> 8 plane bytes: a bit-matrix transpose in three
 * "swap" steps (4x4 blocks across the two longs, 2x2 and 1x1 blocks inside each long).  After it plane p is byte (p&3) of b (p<4) or a (p>=4)
 * counted from the least significant byte; bit (7-k) of plane byte p = bit p of pixel k. */
static inline void c2p_longs(uint32_t a, uint32_t b, uint8_t *d)
{
    uint32_t t;
    t = ((a << 4) ^ b) & 0xF0F0F0F0u; b ^= t; a ^= t >> 4;
    t = ((a >> 14) ^ a) & 0x0000CCCCu; a ^= t ^ (t << 14);
    t = ((b >> 14) ^ b) & 0x0000CCCCu; b ^= t ^ (t << 14);
    t = ((a >> 7) ^ a) & 0x00AA00AAu;  a ^= t ^ (t << 7);
    t = ((b >> 7) ^ b) & 0x00AA00AAu;  b ^= t ^ (t << 7);
    d[0 * PLAT_PLANE_STRIDE] = (uint8_t)b;
    d[1 * PLAT_PLANE_STRIDE] = (uint8_t)(b >> 8);
    d[2 * PLAT_PLANE_STRIDE] = (uint8_t)(b >> 16);
    d[3 * PLAT_PLANE_STRIDE] = (uint8_t)(b >> 24);
    d[4 * PLAT_PLANE_STRIDE] = (uint8_t)a;
    d[5 * PLAT_PLANE_STRIDE] = (uint8_t)(a >> 8);
    d[6 * PLAT_PLANE_STRIDE] = (uint8_t)(a >> 16);
    d[7 * PLAT_PLANE_STRIDE] = (uint8_t)(a >> 24);
}

void display_c2p_group(const uint8_t *src, uint8_t *dst)
{
    c2p_longs(ld32(src), ld32(src + 4), dst);
}

static void palette_update(void)
{
    unsigned bank = VHW.palbank & 3;
    if (pal_valid && !VHW.pal_changed && bank == cur_bank) return;
    VHW.pal_changed = 0;
    cur_bank = (uint8_t)bank;
    const uint8_t *pr = VHW.palram + bank * 256;
    unsigned diff = !pal_valid;
    for (unsigned i = 0; i < 256; i++) {
        uint32_t c = dec24[pr[i]];
        if (c != pal_shadow[i]) { pal_shadow[i] = c; diff = 1; }
    }
    pal_valid = 1;
    if (diff) plat_set_palette256(pal_shadow);
}

void display_init(void)
{
    for (unsigned raw = 0; raw < 256; raw++) {
        unsigned i = raw & 3;
        dec24[raw] = ((uint32_t)GUN[(((raw >> 6) & 3) << 2) | i] << 16) | ((uint32_t)GUN[(((raw >> 4) & 3) << 2) | i] << 8) | GUN[(((raw >> 2) & 3) << 2) | i];
    }
    pal_valid = 0;
    plat_clear_buffer(plat_front());
    const uint32_t *v = (const uint32_t *)VRAM;
    for (unsigned i = 0; i < (VROW0 + NROWS) * 256 / 4; i++) if (v[i]) { display_invalidate(); break; }
}

void display_invalidate(void)
{
    uint32_t *g = (uint32_t *)VHW.dirty, *r = (uint32_t *)VHW.dirty_row;
    for (unsigned i = 0; i < sizeof VHW.dirty / 4; i++) g[i] = 0x01010101u;
    for (unsigned i = 0; i < sizeof VHW.dirty_row / 4; i++) r[i] = 0x01010101u;
    VHW.dirty_any = 1;
}

uint32_t display_backlog(void) { return 0; }

void display_frame(void)
{
    palette_update();
    uint8_t *screen = (uint8_t *)plat_front();
    uint32_t *rowf = (uint32_t *)VHW.dirty_row;
    for (unsigned r4 = VROW0 / 4; r4 < (VROW0 + NROWS) / 4; r4++) {
        if (!rowf[r4]) continue;
        for (unsigned r = r4 * 4; r < r4 * 4 + 4; r++) {
            if (!VHW.dirty_row[r]) continue;
            VHW.dirty_row[r] = 0;
            const uint32_t *flags = (const uint32_t *)&VHW.dirty[r * GROUPS_PER_ROW];
            uint8_t *dflags = &VHW.dirty[r * GROUPS_PER_ROW];
            const uint8_t *src = VRAM + r * 256;
            uint8_t *dst = screen + (r - VROW0) * PLAT_STRIDE;
            for (unsigned w = 0; w < GROUPS_PER_ROW / 4; w++) {
                if (!flags[w]) continue;
                for (unsigned k = w * 4; k < w * 4 + 4; k++) {
                    if (!dflags[k]) continue;
                    dflags[k] = 0;
                    c2p_longs(ld32(src + k * 8), ld32(src + k * 8 + 4), dst + k);
                }
            }
        }
    }
    for (unsigned r = 0; r < VROW0; r++) VHW.dirty_row[r] = 0;                 /* rows that are not displayed */
    for (unsigned r = VROW0 + NROWS; r < 256; r++) VHW.dirty_row[r] = 0;
}
