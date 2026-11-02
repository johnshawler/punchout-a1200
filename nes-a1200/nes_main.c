/* nes_main.c - start-up, frame presentation (chunky -> 8 AGA bitplanes), pads, APU glue */
#include "nes.h"
#include "platform.h"
#include "nes_pal.h"

#include "nes_vid.h"
extern uint32_t nes_frames;
#ifndef GAPFROM
#define GAPFROM 0
#endif
#ifdef GAPLOG
uint16_t gaplog[16]; uint8_t gapval[16];
#endif

#ifdef HUD
extern uint8_t hudbm[4][16 + 40 * 20 + 16];
static const uint8_t font[16][5] = { {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},{2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4} };
void vid_debug_hex(unsigned x0, unsigned y0, uint32_t val, unsigned nd)
{
    for (unsigned d = 0; d < nd; d++) {
        unsigned nib = (val >> ((nd - 1 - d) * 4)) & 15;
        for (unsigned r = 0; r < 5; r++) for (unsigned b = 0; b < 3; b++) {
            unsigned x = x0 + d * 4 + b + VID_FSLACK, y = y0 + r;                                              /* the first fetched word is the (invisible) scroll slack */
            if (y >= 20 || x >= VID_FW * 8) continue;
            uint8_t m = (uint8_t)(0x80 >> (x & 7));
            for (unsigned q = 0; q < 4; q++) {
                uint8_t *ptr = &hudbm[q][16 + y * VID_FW + (x >> 3)];
                if ((font[nib][r] >> (2 - b)) & 1) *ptr |= m; else *ptr &= (uint8_t)~m;
            }
        }
    }
}

/* Diagnostics in a band below the picture: hex digits drawn into a small separate bitmap (see nes_vid.c). */
#define draw_hex(x, y, val, nd) vid_debug_hex((x), (y), (val), (nd))
static uint32_t hud_t0, hud_f0, hud_speed = 100, hud_s[8], hud_show[8], hud_tp[8], hud_tps[8], hud_vs[8], hud_vc[8];
#endif

/* called at the end of every emulated frame (after vid_present) */
void nes_frame_end(void)
{
#ifdef GAPLOG
    { static uint32_t lastt; static unsigned ng; extern uint16_t gaplog[16]; extern uint8_t gapval[16];
      uint32_t t = plat_tick_count; unsigned gap = (unsigned)(t - lastt); lastt = t;
      if (gap >= GAPLOG && ng < 16 && nes_frames >= GAPFROM) { gaplog[ng] = (uint16_t)(nes_frames & 0xFFFF); gapval[ng++] = (uint8_t)(gap > 255 ? 255 : gap); }
#ifdef GAPTILES
      { static uint32_t lt; extern uint32_t vid_cnt[]; uint32_t d = vid_cnt[0] - lt; lt = vid_cnt[0]; if (gap >= GAPLOG && ng <= 16 && nes_frames >= GAPFROM) gaplog[ng - 1] = (uint16_t)d; }
#endif
      }
#endif
#ifdef TIMELOG                      /* plat_tick_count at every 100th emulated frame, shown as hex when frame 16*100 is reached (needs HUD=1) */
    { static uint16_t tl[16]; static unsigned nt;
      if (nes_frames % 100 == 0 && nes_frames / 100 > nt && nt < 16) { tl[nt++] = (uint16_t)plat_tick_count; }
      if (nt == 16) {
#ifdef HUD
        for (unsigned q = 0; q < 4; q++) for (unsigned i = 0; i < sizeof hudbm[0]; i++) hudbm[q][i] = 0;
        for (unsigned k = 0; k < 8; k++) { vid_debug_hex(2 + k * 16, 0, tl[k], 3); vid_debug_hex(2 + k * 16, 6, tl[8 + k], 3); }
#endif
        for (;;) { }
      }
      return; }
#endif
#ifdef STOP_FRAME
    if (nes_frames == STOP_FRAME) {
#ifdef HUD
        for (unsigned q = 0; q < 4; q++) for (unsigned i = 0; i < sizeof hudbm[0]; i++) hudbm[q][i] = 0;
#ifdef OAMDUMP
        { extern uint8_t ppu_oam[256]; for (unsigned q = 0; q < 4; q++) for (unsigned i = 0; i < sizeof hudbm[0]; i++) hudbm[q][i] = 0;
          { extern uint32_t dbg_roots[32]; extern unsigned dbg_nroots; for (unsigned k = 0; k < 16 && k < dbg_nroots; k++) vid_debug_hex(2 + (k & 7) * 35, (k >> 3) * 6, dbg_roots[k], 8); }
          if (0) vid_debug_hex(2, 0, vid_stat[4], 8); (void)0;
          if (0) for (unsigned k = 8; k < 16; k++) vid_debug_hex(2 + (k & 7) * 35, (k >> 3) * 6, ppu_oam[(OAMDUMP + k) * 4] << 24 | ppu_oam[(OAMDUMP + k) * 4 + 1] << 16 | ppu_oam[(OAMDUMP + k) * 4 + 2] << 8 | ppu_oam[(OAMDUMP + k) * 4 + 3], 8); }
        for (;;) { }
#endif
#ifdef BADSHOW
        { extern uint16_t badlog[24]; extern unsigned nbad; for (unsigned k = 0; k < 8 && k < nbad; k++) vid_debug_hex(2 + k * 36, 0, badlog[k], 4); vid_debug_hex(2, 6, vid_stat[1], 8); vid_debug_hex(40, 6, vid_stat[6], 8); vid_debug_hex(80, 6, nbad, 8); vid_debug_hex(120, 6, plat_tick_count, 8); }
#ifdef INSTDBG
        { extern uint32_t dbg_roots[32]; extern unsigned dbg_nroots; for (unsigned q = 0; q < 4; q++) for (unsigned i = 0; i < sizeof hudbm[0]; i++) hudbm[q][i] = 0; for (unsigned k = 0; k < 16 && k < dbg_nroots; k++) vid_debug_hex(2 + (k & 7) * 35, (k >> 3) * 6, dbg_roots[k], 8); }
#endif
#else
#ifdef GAPLOG
        { for (unsigned q = 0; q < 4; q++) for (unsigned i = 0; i < sizeof hudbm[0]; i++) hudbm[q][i] = 0;
          for (unsigned k = 0; k < 8; k++) { vid_debug_hex(2 + k * 36, 0, gaplog[k], 4); vid_debug_hex(22 + k * 36, 0, gapval[k], 2); vid_debug_hex(2 + k * 36, 6, gaplog[8 + k], 4); vid_debug_hex(22 + k * 36, 6, gapval[8 + k], 2); } }
        for (;;) { }
#endif
#ifdef APUDBG
        { extern uint8_t apu_dbg(unsigned, unsigned); for (unsigned q = 0; q < 4; q++) for (unsigned k = 0; k < 6; k++) vid_debug_hex(2 + k * 10, q * 6, apu_dbg(q, k), 2); vid_debug_hex(70, 0, nes_frames, 5); }
        for (;;) { }
#endif
        vid_debug_hex(2, 0, plat_tick_count & 0xFFFF, 4); vid_debug_hex(18, 0, nes_frames, 5);
        for (unsigned q = 0; q < 8; q++) vid_debug_hex(2 + q * 20, 6, vid_cnt[q] & 0xFFFF, 4);
        for (unsigned q = 0; q < 4; q++) vid_debug_hex(62 + q * 20, 0, vid_stat[q] & 0xFFFF, 4);
#endif
#endif
        for (;;) { }
    }
#endif
#ifdef HUD
    if (plat_tick_count - hud_t0 >= 50) {
        uint32_t nf = nes_frames - hud_f0 ? nes_frames - hud_f0 : 1;
        hud_speed = (nes_frames - hud_f0) * 100 / (plat_tick_count - hud_t0);
        for (unsigned q = 0; q < 8; q++) { hud_show[q] = (nes_stat[q] - hud_s[q]) / nf; hud_s[q] = nes_stat[q]; }
        for (unsigned q = 0; q < 8; q++) { hud_vc[q] = (vid_cnt[q] - hud_vs[q]) / nf; hud_vs[q] = vid_cnt[q]; }
        for (unsigned q = 0; q < 8; q++) { hud_tps[q] = (nes_tprof[q] - hud_tp[q]) / nf; hud_tp[q] = nes_tprof[q]; }
        hud_t0 = plat_tick_count; hud_f0 = nes_frames;
    }
#ifdef HISTO
    return;
#endif
    if (nes_frames & 15) return;                       /* the digits cost real time: redraw them every 16th frame */
#ifdef SPEEDONLY                                   /* speed in per cent as decimal digits (the hex font), nothing else */
    { unsigned v = hud_speed > 999 ? 999 : hud_speed; draw_hex(2, 0, (v / 100) << 8 | ((v / 10) % 10) << 4 | (v % 10), 3); }
    return;
#endif
    draw_hex(2, 0, hud_speed, 3); draw_hex(16, 0, nes_frames & 0xFFFFF, 5);
#ifdef OAMDBG
    { extern uint8_t ppu_oam[256]; for (unsigned k = 0; k < 10; k++) { draw_hex(34 + k * 22, 0, ppu_oam[(k + 4) * 4 + 1], 2); draw_hex(34 + k * 22 + 10, 0, ppu_oam[(k + 4) * 4 + 2], 2); } }
#else
    draw_hex(40, 0, hud_show[0] & 0xFFF, 3); draw_hex(54, 0, hud_show[2] & 0xFFF, 3); draw_hex(68, 0, hud_show[6] & 0xFFF, 3);
    for (unsigned q = 0; q < 5; q++) draw_hex(86 + q * 14, 0, hud_vc[q] & 0xFFF, 3);
    draw_hex(160, 0, hud_tps[0] & 0xFFF, 3); draw_hex(174, 0, hud_tps[3] & 0xFFF, 3); draw_hex(188, 0, hud_tps[1] & 0xFFF, 3);
    draw_hex(206, 0, hud_tps[4] & 0xFFF, 3); draw_hex(220, 0, hud_tps[2] & 0xFFF, 3); draw_hex(234, 0, hud_tps[6] & 0xFFF, 3);
#endif

#endif
}

/* ---- controller: A B Select Start Up Down Left Right, read serially from $4016 */
static uint8_t pad_state, pad_shift, pad_strobe;
#ifdef SCRIPT          /* test builds: the pad is driven by a per-frame script (same format and timing as the reference emulator run), not by real input */
static const uint16_t pad_script[][3] = {
#ifdef SCRIPTFILE
#include SCRIPTFILE
#else
#include "test/script1.inc"
#endif
    {0, 0, 0} };
static uint8_t pad_sample(void)
{
    static uint32_t lastf = 0xFFFFFFFFu; static uint8_t lastp;
    uint8_t p = 0;
    uint32_t f = nes_frames + SCRIPT;
    if (f == lastf) return lastp;
    for (unsigned i = 0; pad_script[i][1]; i++)
        if (f >= pad_script[i][0] && f < pad_script[i][1]) { unsigned m = pad_script[i][2]; p |= (uint8_t)(m & 0xFC); if (m & 0x100) p |= 0x01; if (m & 0x01) p |= 0x02; }
    lastf = f; lastp = p;
    return p;
}
#else
static uint8_t pad_sample(void)
{
    uint16_t in = plat_input(); uint8_t p = 0;
    if ((in & PIN_FIRE) || plat_key_down(0x32)) p |= 0x01;               /* fire / X = A */
    if ((in & PIN_BTN2) || plat_key_down(0x31)) p |= 0x02;               /* 2nd button / Z = B */
    if (plat_key_down(KEY_SPACE)) p |= 0x04;                             /* Select */
    if (plat_key_down(KEY_RETURN)) p |= 0x08;                            /* Start */
    if ((in & PIN_UP) || plat_key_down(KEY_UP)) p |= 0x10;
    if ((in & PIN_DOWN) || plat_key_down(KEY_DOWN)) p |= 0x20;
    if ((in & PIN_LEFT) || plat_key_down(KEY_LEFT)) p |= 0x40;
    if ((in & PIN_RIGHT) || plat_key_down(KEY_RIGHT)) p |= 0x80;
    return p;
}
#endif
void pad_write(uint32_t v) { pad_strobe = (uint8_t)(v & 1); if (pad_strobe) { pad_state = pad_sample(); pad_shift = pad_state; } }
uint8_t pad_read(void)
{
    uint8_t r;
    if (pad_strobe) { pad_state = pad_sample(); r = pad_state & 1; }
    else { r = pad_shift & 1; pad_shift = (uint8_t)((pad_shift >> 1) | 0x80); }
    return (uint8_t)(0x40 | r);
}

#ifdef BGTEST
/* Background scroll test: alternating solid / blank tile columns scrolled one pixel at a time (SCROLLHOLD ticks each); the position of the stripes in the
 * screenshot shows whether the Copper scroll arithmetic is right for every X. */
extern void vid_nt_changed(unsigned bank, unsigned off, unsigned oldv, unsigned newv);
extern uint32_t ppu_oam_gen;
extern uint8_t nes_chr_reg[4];
#ifndef SCROLLHOLD
#define SCROLLHOLD 25
#endif
void main(void)
{
    nes_init();
    for (unsigned i = 0; i < 4; i++) nes_chr_reg[i] = 4;
    for (unsigned b = 0; b < 2; b++)
        for (unsigned i = 0; i < 960; i++) { unsigned nv = ((i & 1) ? 0xFF : 0xFA); unsigned ov = ppu_nt[b][i]; ppu_nt[b][i] = (uint8_t)nv; vid_nt_changed(b, i, ov, nv); }
    for (unsigned i = 0; i < 32; i++) ppu_pal[i] = (uint8_t)(i & 3 ? 0x16 + (i & 3) : 0x21);
    for (unsigned x = 0;; x = (x + 1) & 255) {
        Region r = { 0, 240, (uint16_t)((x >> 3) & 31), (uint8_t)(x & 7), 0x18, 0, 0x90 };
        ppu_ctrl = 0x90; ppu_mask = 0x18; ppu_oam_gen++;
        for (unsigned i = 0; i < 256; i += 4) ppu_oam[i] = 0xF0;
        vid_present(&r, 1);
#ifdef HUD
        vid_debug_hex(2, 0, x, 3);
#endif
        uint32_t t = plat_tick_count + SCROLLHOLD;
        while ((int32_t)(plat_tick_count - t) < 0) { }
    }
}
#elif defined(SPRDUMP)
/* Sprite test: no game runs; the sprite tables (and CHR bank registers, palette) of reference-emulator frames are presented one after the other, each for
 * SPRHOLD ticks, with nothing but sprites on screen, so the screen can be compared with an exact rendering of the same tables. */
extern uint32_t ppu_oam_gen;
extern void vid_test_slat0(unsigned v);
extern uint8_t nes_chr_reg[4];
static const struct { uint8_t oam[256], ctrl, mask, regs[4], l0, pal[32]; uint16_t frame; } snaps[] = {
#include "test/sprdump.inc"
};
#ifndef SPRK0
#define SPRK0 0
#define SPRK1 (sizeof snaps / sizeof snaps[0])
#endif
#ifndef SPRHOLD
#define SPRHOLD 50
#endif
void main(void)
{
    nes_init();
    for (;;)
        for (unsigned k = SPRK0; k < SPRK1; k++) {
            Region r = { 0, 240, 0, 0, 0x10, 0, snaps[k].ctrl };
            for (unsigned i = 0; i < 256; i++) ppu_oam[i] = snaps[k].oam[i];
            for (unsigned i = 0; i < 32; i++) ppu_pal[i] = snaps[k].pal[i];
            for (unsigned i = 0; i < 4; i++) nes_chr_reg[i] = snaps[k].regs[i];
            ppu_ctrl = snaps[k].ctrl; ppu_mask = 0x10; ppu_oam_gen++;
            vid_test_slat0(!snaps[k].l0);
            vid_present(&r, 1);
#ifdef HUD
            vid_debug_hex(2, 0, k, 3); vid_debug_hex(20, 0, snaps[k].frame, 4);
#ifdef INSTDBG
            { extern uint32_t dbg_roots[32], dbg_x[32]; extern unsigned dbg_nroots;
              for (unsigned q = 0; q < 8 && q < dbg_nroots; q++) { vid_debug_hex(2 + q * 35, 6, dbg_roots[q], 8); }
              for (unsigned q = 0; q < 12 && q < dbg_nroots; q++) { vid_debug_hex(44 + q * 20, 0, dbg_x[q], 4); } }
#endif
#endif
            uint32_t t = plat_tick_count + SPRHOLD;
            while ((int32_t)(plat_tick_count - t) < 0) { }
        }
}
#else
void main(void)
{
    nes_init();
    nes_run((uint32_t)(nes_prg[0x1FFFC] | nes_prg[0x1FFFD] << 8));
}
#endif

#if defined(HUD) && defined(PROFILE) && defined(HISTO)
/* PC histogram over the whole code (translated code included): HISTO_BINS bars drawn into the band every 4096 samples, scaled to the highest. */
extern char _g0[], _g8[];
#define HISTO_BINS 280
static uint32_t hbin[HISTO_BINS], hn, hmax;
void plat_prof_pc(uint32_t pc)
{
    uint32_t lo = (uint32_t)_g0, hi = (uint32_t)_g8;
    if (pc >= lo && pc < hi) { uint32_t b = (pc - lo) / ((hi - lo) / HISTO_BINS + 1); hbin[b]++; }
    else hbin[HISTO_BINS - 1]++;
    if (++hn < 2048) return;
    hn = 0; hmax = 1;
    for (unsigned b = 0; b < HISTO_BINS; b++) if (hbin[b] > hmax) hmax = hbin[b];
    for (unsigned q = 0; q < 4; q++) for (unsigned i = 0; i < sizeof hudbm[0]; i++) hudbm[q][i] = 0;
    for (unsigned b = 0; b < HISTO_BINS; b++) {
        unsigned hgt = hbin[b] * 11 / hmax;
        for (unsigned y = 0; y < hgt; y++) {
            unsigned x = b + VID_FSLACK, yy = 10 - y;
            for (unsigned q = 0; q < 4; q++) hudbm[q][16 + yy * VID_FW + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
        }
    }
    if (hmax > 60000) for (unsigned b = 0; b < HISTO_BINS; b++) hbin[b] >>= 1;
}
#elif defined(HUD) && defined(PROFILE)
/* PC sampler: samples attributed to code groups (link order, see link.ld): 0 platform+ISR, 1 rt glue, 2 core, 3 ppu, 4 interp, 5 present/pads,
 * 6 translated code, 7 other text, 8 rest (data, stack).  Shares in per mille, hex, drawn in the band every 64 samples. */
extern char _g0[], _g1[], _g2[], _g3[], _g4[], _g5[], _g6[], _g7[], _g8[];
static uint32_t gcount[10], nsamp, subcount[16];
static const uint32_t pf_addr[] = {
#include "prof_syms.inc"
    0xFFFFFFFFu };
void plat_prof_pc(uint32_t pc)
{
    unsigned g = 8;
    if (pc >= (uint32_t)_g0 && pc < (uint32_t)_g1) g = 0; else if (pc < (uint32_t)_g2) g = 1; else if (pc < (uint32_t)_g3) g = 2;
    else if (pc < (uint32_t)_g4) g = 3; else if (pc < (uint32_t)_g5) g = 4; else if (pc < (uint32_t)_g6) g = 5;
    else if (pc < (uint32_t)_g7) g = 6; else if (pc < (uint32_t)_g8) g = 7;
    gcount[g]++;
    if (g == 3) { unsigned s = 0; while (pf_addr[s + 1] != 0xFFFFFFFFu && pc >= pf_addr[s + 1]) s++; if (s < 16) subcount[s]++; }
    if ((++nsamp & 63) != 0) return;
    for (unsigned k = 0; k < 9; k++) draw_hex(2 + k * 14, 6, gcount[k] * 1000 / nsamp, 3);
    for (unsigned k = 0; k < 10; k++) draw_hex(130 + k * 12, 6, subcount[k] * 1000 / nsamp, 3);
    if (nsamp >= 8192) { for (unsigned k = 0; k < 9; k++) gcount[k] = 0; for (unsigned k = 0; k < 16; k++) subcount[k] = 0; nsamp = 0; }
}
#endif
