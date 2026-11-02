/* nes_vid.c - native Amiga video back end for the NES PPU model.
 *
 * Instead of re-creating the 256x240 picture every frame, the nametables live in persistent PLANAR bitmaps (4 bitplanes = colour index
 * palette*4+pixel, 0 = background) that are redrawn only for tiles the game actually changed.  The display hardware does the scrolling: every
 * stretch of scanlines the PPU displayed under one register state (a Region) becomes a few Copper moves (bitplane pointers + BPLCON1 fine scroll).
 * Sprites are AGA hardware sprites: NES sprite palette p -> sprite pair p (colour registers 16+4p+1..3), streams rebuilt once per frame.
 *
 * Bitmap layout: the logical 2x2 nametable space (512x480 px) as a torus, plus 256 px of horizontal wrap padding: 96 bytes per line, 480 lines,
 * 4 planes.  Because the cartridge changes the mirroring (the mapper) during a frame, there is one torus per mirroring mode, maintained lazily:
 * a torus is only brought up to date when a region of the frame being presented uses it.
 *
 * MMC2: which 4 KB CHR bank a background tile is drawn from depends on the tile sequence left to right (tiles $FD/$FE flip the latch).  It is
 * resolved per nametable row (start state = end state of the previous row); see compute_row().  That is an approximation of the PPU's fetch order
 * (it ignores horizontal scroll and sprite fetches except the 8x16 dummy fetch) and is checked against the exact renderer by comparing frames.
 */
#include "nes.h"
#include "nes_vid.h"
#include "platform.h"
#include "nes_pal.h"

#define STRIDE 104
#ifndef BGADJ
#define BGADJ (-1)
#endif
#define ROWS   480
#define PLANE  (STRIDE * ROWS)
#define PADB   16                                   /* guard bytes in front of every plane (the fetch may start 2 bytes before the line) */
#define NONE   0xFFFFFFFFu
#define NEVER  0xFFFFFFFEu

/* NV bitmaps per mirroring mode ("variants"): the game animates by switching CHR banks, so the same nametable looks different from frame to frame; a variant
 * is a complete bitmap for one setting of the two background bank registers, so switching back and forth costs nothing once both have been drawn. */
#ifndef NV
#define NV 3
#endif
static uint8_t torus0[NV][4][PADB + STRIDE * 240 + PADB] __attribute__((section(".bss.chip"), aligned(4)));      /* mode 0: 64 columns (+32 repeated) x 240 lines */
static uint8_t torus1[NV][4][PADB + STRIDE * 480 + PADB] __attribute__((section(".bss.chip"), aligned(4)));      /* mode 1: 32 columns (x2) x 480 lines */
static inline uint8_t *tor(unsigned m, unsigned v, unsigned k) { return m ? &torus1[v][k][PADB] : &torus0[v][k][PADB]; }
static uint16_t var_key[2][NV];
static uint32_t var_use[2][NV], var_clock;
static unsigned cur_var[2], draw_v;
/* MMC2 bank resolution, per mirroring mode m and "ring" (the row of tiles the fetch walks along: mode 0 = both nametables side by side, one ring;
 * mode 1 = each physical nametable is its own 32-tile ring): CHR register used by a tile on scanline 0 of its tile row / on scanlines 1..7 */
static uint8_t tile_reg[2][2][960] __attribute__((aligned(4))), tile_reg1[2][2][960] __attribute__((aligned(4)));
static uint8_t row_regs[2][2][30], row_start[2][2][30], row_end[2][2][30], row_flip[2][2][30];
static uint32_t chain_dirty[2][2];                  /* [mode][ring] rows whose resolution must be recomputed */
static unsigned vctrl, vmask;                        /* PPUCTRL / PPUMASK as they were while the picture was drawn (not as they are in vblank) */

static uint32_t dcol[2][NV][2][30];                 /* [mode][variant][physical nt][tile row]: columns to compare/redraw */
static uint32_t drow[2][NV][2];                     /* rows of dcol that are not empty */
static uint8_t bgtxt[8][32];                         /* the replacement for the repeating MIKE TYSON background of the intro screens: 8 rows x 32 tile slices */
static uint16_t chr_h[8192];                         /* hash of the 16 bytes of every CHR tile: tiles with equal pixels compare equal across banks */
static uint32_t drawn[2][NV][2][960];               /* [mode][variant][physical nt][tile] key as drawn */
static inline void mark_tile(unsigned m, unsigned b, unsigned r, uint32_t cols) { if (!cols) return; for (unsigned v = 0; v < NV; v++) { dcol[m][v][b][r] |= cols; drow[m][v][b] |= 1u << r; } }
static uint8_t blank34[PADB + 40] __attribute__((section(".bss.chip"), aligned(4)));
static unsigned bgtable_cur;
uint32_t vid_stat[8], vid_cnt[8];            /* vid_cnt (per frame in the HUD): 0 tiles drawn, 1 rows resolved, 2 nametable writes, 3 regions, 4 sprite rebuilds, 5 tiles compared */

/* ------------------------------------------------------------------ tiles */
static inline unsigned phys(unsigned mode, unsigned n) { return mode ? (n >> 1) : (n & 1); }     /* mode 1 = horizontal mirroring */

/* the four plane bytes of the 8 rows of a tile (plane 0/1 = pixel bits, plane 2/3 = palette bits where the pixel is not transparent), written to up to 3 places */
static void draw_rows(uint8_t *d, const uint8_t *chr0, const uint8_t *chr, unsigned pal)
{
    for (unsigned i = 0; i < 8; i++) {
        const uint8_t *cc = i ? chr : chr0;
        unsigned lo = cc[i], hi = cc[8 + i], mm = lo | hi;
        d[i * 4] = (uint8_t)lo; d[i * 4 + 1] = (uint8_t)hi; d[i * 4 + 2] = (pal & 1) ? (uint8_t)mm : 0; d[i * 4 + 3] = (pal & 2) ? (uint8_t)mm : 0;
    }
}
static void put_rows(unsigned m, unsigned tx, unsigned ty, const uint8_t *d)
{
    uint8_t *p0 = tor(m, draw_v, 0) + ty * 8 * STRIDE + tx, *p1 = tor(m, draw_v, 1) + ty * 8 * STRIDE + tx;
    uint8_t *p2 = tor(m, draw_v, 2) + ty * 8 * STRIDE + tx, *p3 = tor(m, draw_v, 3) + ty * 8 * STRIDE + tx;
    for (unsigned i = 0; i < 8; i++, d += 4) {
        *p0 = d[0]; *p1 = d[1]; *p2 = d[2]; *p3 = d[3];
        p0 += STRIDE; p1 += STRIDE; p2 += STRIDE; p3 += STRIDE;
    }
}

/* The intro screens repeat "MIKE TYSON" as a diagonal pattern of tiles $F3..$F9,$FB (CHR bank 7) at nametable position (c - r) mod 8.  The pixels of those
 * tiles are replaced by slice (c - r) mod 32 of a 256 px wide text strip (nothing else about the tiles changes).  Returns slice + 1, or 0 for other tiles. */
static const char bg_text[] = "ONLY AMIGA MAKES IT POSSIBLE";
static const uint8_t bg_font[][8] = {            /* 5x7 glyphs: A B E G I K L M N O P S T Y, rows top to bottom (bit 4 = left pixel) */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11,0}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E,0}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F,0}, {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F,0},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E,0}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11,0}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F,0}, {0x11,0x1B,0x15,0x15,0x11,0x11,0x11,0},
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11,0}, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E,0}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,0}, {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E,0},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04,0}, {0x11,0x11,0x0A,0x04,0x04,0x04,0x04,0} };
static const char bg_glyph[] = "ABEGIKLMNOPSTY";
static void bg_text_init(void)
{
    for (unsigned ch = 0; bg_text[ch]; ch++) {
        const uint8_t *g = 0;
        for (unsigned i = 0; bg_glyph[i]; i++) if (bg_glyph[i] == bg_text[ch]) g = bg_font[i];
        if (!g) continue;
        for (unsigned row = 0; row < 7; row++)             /* one tile per character: 5 pixels at columns 1..5 */
            bgtxt[row + 1][ch] = (uint8_t)(g[row] << 2);
    }
}
static inline unsigned bgov(unsigned b, unsigned r, unsigned c, unsigned tile, unsigned bank)
{
    if (bank != 7 || tile < 0xF3 || tile > 0xFB || tile == 0xFA) return 0;
    unsigned idx = tile == 0xFB ? 0 : tile - 0xF2;
    if (((c - r) & 7) != idx) return 0;
    if (tile == 0xFB) {                                    /* $FB is also the plain portrait fill: only a pattern tile next to the pattern tiles counts */
        const uint8_t *row = &ppu_nt[b][r * 32];
        if (!((c > 0 && row[c - 1] == 0xF9) || (c < 31 && row[c + 1] == 0xF3))) return 0;
    }
    return ((c - r) & 31) + 1;
}

static void draw_tile(unsigned m, unsigned b, unsigned r, unsigned c, unsigned tile, unsigned pal, unsigned bank0, unsigned bank)
{
    const uint8_t *chr = nes_chr + bank * 4096u + tile * 16u, *chr0 = nes_chr + bank0 * 4096u + tile * 16u;
    /* mode 0 (vertical mirroring): the torus is both nametables side by side (64 columns) x 240 lines; NT0 is repeated at columns 64..95 (the fetch
     * runs past the right edge).  mode 1 (horizontal mirroring): 32 columns repeated twice x 480 lines (NT0 above NT1). */
    uint8_t d[32];
    draw_rows(d, chr0, chr, pal);
    { unsigned ov = bgov(b, r, c, tile, bank);
      if (ov) for (unsigned i = 0; i < 8; i++) { unsigned lo = bgtxt[i][ov - 1]; d[i * 4] = (uint8_t)lo; d[i * 4 + 1] = 0xFF; d[i * 4 + 2] = (pal & 1) ? 0xFF : 0; d[i * 4 + 3] = (pal & 2) ? 0xFF : 0; } }
    if (m == 0) {
        put_rows(m, b * 32 + c, r, d);
        if (!b) put_rows(m, c + 64, r, d);
        else if (c < 4) put_rows(m, c + 96, r, d);
    } else {
        put_rows(m, c, b * 30 + r, d);
        put_rows(m, c + 32, b * 30 + r, d);
        if (c < 4) put_rows(m, c + 64, b * 30 + r, d);
    }
    vid_cnt[0]++;
}

/* The PPU fetches the 34 tiles of a scanline in order starting at the coarse X of the scroll; a tile $FD/$FE flips the latch after its own fetch.
 * Scanline 0 of a tile row starts from the state the previous row left (row_start); the 7 others start from the state this row itself leaves
 * (the last flip among the 34 fetched tiles), so tiles in front of the first flip tile can use another bank on scanline 0.  Tiles outside the
 * 32 shown are resolved by simply carrying the state on around the ring. */
static uint8_t nt_nflip[2][30];                      /* number of $FD/$FE tiles in each nametable row (kept by vid_nt_changed) */

/* CHR register of every tile of one ring row for scanline 0 of the tile row (tile_reg) and for scanlines 1..7 (tile_reg1); only registers that
 * changed mark tiles for redrawing.  Rows without a flip tile ($FD/$FE) take one register (the common case); the others walk the 34 tiles the PPU
 * fetches, starting at window column W. */
static void compute_row(unsigned m, unsigned ring, unsigned r, unsigned W, unsigned ctl)
{
    unsigned sz = (ctl >> 5) & 1, t1 = (ctl >> 4) & 1, len = m ? 32 : 64, st0 = row_start[m][ring][r], fl = t1 ? 2 : 1, rA = t1 ? 2 : 0, mask = 0;
    unsigned nflip = m ? nt_nflip[ring][r] : nt_nflip[0][r] + nt_nflip[1][r];
    uint32_t chg[2] = {0, 0};
    if (!nflip) {
        unsigned reg = (st0 & fl) ? rA : rA + 1;
        uint32_t v4 = reg * 0x01010101u;
        for (unsigned bk = 0; bk < (m ? 1u : 2u); bk++) {
            unsigned b2 = m ? ring : bk;
            uint32_t *p0 = (uint32_t *)&tile_reg[m][b2][r * 32], *p1 = (uint32_t *)&tile_reg1[m][b2][r * 32];
            unsigned diff = 0;
            for (unsigned q = 0; q < 8; q++) { diff |= (p0[q] ^ v4) | (p1[q] ^ v4); p0[q] = v4; p1[q] = v4; }
            if (diff) chg[b2] = 0xFFFFFFFFu;
        }
        mask = 1u << reg;
        row_end[m][ring][r] = (uint8_t)st0; row_flip[m][ring][r] = 0;
    } else {
        const uint8_t *nt0 = ppu_nt[m ? ring : 0] + r * 32, *nt1 = ppu_nt[m ? ring : 1] + r * 32;
        unsigned st1 = 0, n = len > 34 ? len : 34;
        for (unsigned pass = 0; pass < 2; pass++) {
            uint8_t *tr = pass ? &tile_reg1[m][0][0] : &tile_reg[m][0][0];
            unsigned st = pass ? st1 : st0;
            for (unsigned i = 0; i < n; i++) {
                unsigned L = (W + i) & (len - 1), col = L & 31, t = (L < 32 ? nt0 : nt1)[col];
                if (i < len) {
                    unsigned reg = (st & fl) ? rA : rA + 1, bank = m ? ring : (L >> 5);
                    uint8_t *p = &tr[bank * 960 + r * 32 + col];
                    if (*p != reg) { *p = (uint8_t)reg; chg[bank] |= 1u << col; }
                    mask |= 1u << reg;
                }
                if (t == 0xFD) st |= fl; else if (t == 0xFE) st &= ~fl;
                if (!pass && i == 33) { st1 = st; row_end[m][ring][r] = (uint8_t)st; }
            }
            if (!pass) { if (sz) st1 &= ~2u; }                               /* the sprite fetches in between (8x16: dummy fetch of tile $FF) */
        }
        row_flip[m][ring][r] = 1;
    }
    vid_cnt[1]++;
    row_regs[m][ring][r] = (uint8_t)mask;
    if (m) mark_tile(1, ring, r, chg[ring]); else { mark_tile(0, 0, r, chg[0]); mark_tile(0, 1, r, chg[1]); }
}

/* The window (first coarse X the PPU fetches from) can differ between tile rows of one frame; each row is resolved for the region that shows most of it. */
static uint8_t rwin[2][2][30], rbest[2][2][30], row_w[2][2][30], rctl[2][2][30], row_ctl[2][2][30];
static inline unsigned win_of(unsigned v0, unsigned m) { return m ? (v0 & 31) : (((v0 >> 10) & 1) * 32 + (v0 & 31)); }

static void collect_rows(unsigned m, unsigned v0, unsigned y0, unsigned y1, unsigned ctl)
{
    unsigned cy = (v0 >> 5) & 31, fy = (v0 >> 12) & 7, g = m ? (v0 >> 11) & 1 : 0, W = win_of(v0, m), lines = y1 - y0, first;
    if (cy >= 30) { cy = 29; fy = 7; }
    first = 8 - fy;
    while (lines) {
        unsigned take = first < lines ? first : lines;
        if (take > rbest[m][g][cy]) { rbest[m][g][cy] = (uint8_t)take; rwin[m][g][cy] = (uint8_t)W; rctl[m][g][cy] = (uint8_t)ctl; }
        lines -= take; first = 8;
        if (++cy == 30) { cy = 0; if (m) g ^= 1; }
    }
}

static void resolve_mode(unsigned m, unsigned Wdef, unsigned Cdef, int partial)
{
    unsigned rings = m ? 2 : 1;
    for (unsigned g = 0; g < rings; g++)
        for (unsigned r = 0; r < 30; r++) {
            if (!rbest[m][g][r]) { if (partial) { rwin[m][g][r] = row_w[m][g][r]; rctl[m][g][r] = row_ctl[m][g][r]; } else { rwin[m][g][r] = (uint8_t)Wdef; rctl[m][g][r] = (uint8_t)Cdef; } }
            if (((rctl[m][g][r] ^ row_ctl[m][g][r]) & 0x30) || (row_flip[m][g][r] && rwin[m][g][r] != row_w[m][g][r])) { chain_dirty[m][g] |= 1u << r; }
        }
    if (!chain_dirty[m][0] && !chain_dirty[m][rings - 1]) return;            /* nothing changed: no row can have a new start state either */
    for (unsigned pass = 0; pass < 2; pass++) {
        unsigned pg = rings - 1, pr = 29;                                   /* the row fetched just before: the last row of the last ring, cyclically */
        for (unsigned g = 0; g < rings; g++)
            for (unsigned r = 0; r < 30; pg = g, pr = r, r++) {
                unsigned st = row_end[m][pg][pr];
                if (rctl[m][g][r] & 0x20) st &= ~2u;                            /* 8x16 sprites: the unused-slot dummy fetch leaves latch 1 at $FE */
                
                if (((chain_dirty[m][g] >> r) & 1) || st != row_start[m][g][r]) {
                    row_start[m][g][r] = (uint8_t)st; row_w[m][g][r] = rwin[m][g][r]; row_ctl[m][g][r] = rctl[m][g][r];
                    compute_row(m, g, r, rwin[m][g][r], rctl[m][g][r]);
                    chain_dirty[m][g] &= ~(1u << r);
                }
            }
    }
}

static void resolve_regions_inner(const Region *r, int n, unsigned open_v, unsigned open_mir, int have_open, int open_y0, unsigned open_ctrl, int open_y1)
{
    unsigned seen = 0, wdef[2] = {0, 0}, cdef[2] = {0, 0};
    if (have_open) {
        /* Mid-frame (the sprite-0 hit is being timed): the windows and controls of the rows are only guessed, so keep what the last complete frame
         * decided and only follow nametable changes.  The complete picture re-resolves everything at vblank. */
        for (unsigned m = 0; m < 2; m++) {
            unsigned rings = m ? 2 : 1;
            if (!(chain_dirty[m][0] | chain_dirty[m][rings - 1])) continue;
            for (unsigned g = 0; g < rings; g++) for (unsigned q = 0; q < 30; q++) { rwin[m][g][q] = row_w[m][g][q]; rctl[m][g][q] = row_ctl[m][g][q]; }
            resolve_mode(m, 0, 0, 1);
        }
        (void)r; (void)n; (void)open_v; (void)open_mir; (void)open_y0; (void)open_ctrl; (void)open_y1;
        return;
    }
    for (int i = 0; i < n; i++) if (r[i].mask & 0x08) {
        unsigned m = r[i].mir & 1;
        if (!(seen & (1u << m))) { seen |= 1u << m; wdef[m] = win_of(r[i].v0, m); cdef[m] = r[i].pad; for (unsigned g = 0; g < 2; g++) for (unsigned q = 0; q < 30; q++) rbest[m][g][q] = 0; }
        collect_rows(m, r[i].v0, r[i].y0, r[i].y1, r[i].pad);
    }
    for (unsigned m = 0; m < 2; m++) if (seen & (1u << m)) resolve_mode(m, wdef[m], cdef[m], 0);
}
static void resolve_regions(const Region *r, int n, unsigned open_v, unsigned open_mir, int have_open, int open_y0, unsigned open_ctrl, int open_y1)
{
    TPS(t0);
    resolve_regions_inner(r, n, open_v, open_mir, have_open, open_y0, open_ctrl, open_y1);
    TPE(t0, 4);
}

/* choose the variant for the current background bank registers (evicting the least recently used one) */
static void select_variant(unsigned m)
{
    unsigned t1 = (vctrl >> 4) & 1;
    uint16_t key = (uint16_t)(t1 ? (nes_chr_reg[2] | nes_chr_reg[3] << 5) : (nes_chr_reg[0] | nes_chr_reg[1] << 5));
    unsigned best = 0;
    var_clock++;
    for (unsigned v = 0; v < NV; v++) {
        if (var_key[m][v] == key) { cur_var[m] = v; var_use[m][v] = var_clock; return; }
        if (var_use[m][v] < var_use[m][best]) best = v;
    }
    /* a new variant starts as a copy of the most recently used valid one (bitmap and "as drawn" keys), so only the tiles whose bank/tile/palette differ get
     * redrawn: the full redraw of 2 x 960 tiles into chip RAM took ~250 ms (the hitch at the first punch of a fight) */
    unsigned src = NV;
    for (unsigned v = 0; v < NV; v++) if (var_key[m][v] != 0xFFFF && v != best && (src == NV || var_use[m][v] > var_use[m][src])) src = v;
    var_key[m][best] = key; var_use[m][best] = var_clock; cur_var[m] = best;
    if (src != NV) {
#ifndef NOCOPY
        for (unsigned k = 0; k < 4; k++) {
            uint32_t *d = (uint32_t *)(tor(m, best, k) - PADB), *s = (uint32_t *)(tor(m, src, k) - PADB);
            unsigned n = (m ? sizeof torus1[0][0] : sizeof torus0[0][0]) / 4;
            for (unsigned i = 0; i < n; i++) d[i] = s[i];
        }
#endif
        for (unsigned b = 0; b < 2; b++) { drow[m][best][b] = 0x3FFFFFFFu; for (unsigned r = 0; r < 30; r++) dcol[m][best][b][r] = 0xFFFFFFFFu; for (unsigned i = 0; i < 960; i++) drawn[m][best][b][i] = drawn[m][src][b][i]; }
        return;
    }
    for (unsigned b = 0; b < 2; b++) { drow[m][best][b] = 0x3FFFFFFFu; for (unsigned r = 0; r < 30; r++) dcol[m][best][b][r] = 0xFFFFFFFFu; for (unsigned i = 0; i < 960; i++) drawn[m][best][b][i] = 0xFFFFFFFFu; }
}

static void process_draw(unsigned m)
{
    unsigned v = cur_var[m];
    draw_v = v;
    for (unsigned b = 0; b < 2; b++) {
        uint32_t rows = drow[m][v][b];
        if (!rows) continue;
        drow[m][v][b] = 0;
        while (rows) {
            unsigned r = (unsigned)__builtin_ctz(rows); rows &= rows - 1;
            uint32_t cols = dcol[m][v][b][r];
            dcol[m][v][b][r] = 0;
            const uint8_t *nt = ppu_nt[b] + r * 32, *at = ppu_nt[b] + 0x3C0 + (r >> 2) * 8;
            unsigned rs = (r & 2) ? 4 : 0;
            uint32_t *dk = &drawn[m][v][b][r * 32];
            while (cols) {
                unsigned c = (unsigned)__builtin_ctz(cols); cols &= cols - 1;
                unsigned tile = nt[c], pal = (at[c >> 2] >> (rs | ((c & 2) ? 2 : 0))) & 3;
                unsigned bank0 = nes_chr_reg[tile_reg[m][b][r * 32 + c]], bank = nes_chr_reg[tile_reg1[m][b][r * 32 + c]];
                uint32_t key = ((uint32_t)chr_h[bank0 * 256 + tile] * 0x9E3779B1u) ^ ((uint32_t)chr_h[bank * 256 + tile] * 0x85EBCA77u) ^ (pal * 0xC2B2AE3Du); { unsigned ov = bgov(b, r, c, tile, bank); if (ov) key ^= ov * 0x2545F491u; } if (key == 0xFFFFFFFFu) key = 0;
                if (dk[c] != key) { dk[c] = key; draw_tile(m, b, r, c, tile, pal, bank0, bank); }
            }
        }
    }
}

void vid_nt_changed(unsigned bank, unsigned off, unsigned oldv, unsigned newv)
{
    vid_cnt[2]++;
    if (off < 0x3C0) {
        unsigned r = off >> 5, c = off & 31;
        nt_nflip[bank][r] = (uint8_t)(nt_nflip[bank][r] + ((newv == 0xFD || newv == 0xFE) ? 1 : 0) - ((oldv == 0xFD || oldv == 0xFE) ? 1 : 0));
        /* the bank a tile is drawn from depends only on the flip tiles ($FD/$FE) around it, never on its own value: a change between two other
         * tiles needs a redraw of that tile and nothing else */
        if ((oldv >= 0xFD && oldv <= 0xFE) || (newv >= 0xFD && newv <= 0xFE)) { chain_dirty[0][0] |= 1u << r; chain_dirty[1][bank] |= 1u << r; }
        { uint32_t cols = 1u << c;                 /* an $FB tile next to the MIKE TYSON pattern counts as part of it (see bgov) */
          if ((oldv == 0xF9 || newv == 0xF9) && c < 31) cols |= 1u << (c + 1);
          if ((oldv == 0xF3 || newv == 0xF3) && c > 0) cols |= 1u << (c - 1);
          mark_tile(0, bank, r, cols); mark_tile(1, bank, r, cols); }
    } else {
        unsigned a = off - 0x3C0, r0 = (a >> 3) * 4;
        uint32_t cols = 0xFu << ((a & 7) * 4);
        for (unsigned r = r0; r < r0 + 4 && r < 30; r++) { mark_tile(0, bank, r, cols); mark_tile(1, bank, r, cols); }
    }
}

void vid_chr_changed(void) { }

void vid_ctrl_changed(void) { for (unsigned a = 0; a < 2; a++) for (unsigned b = 0; b < 2; b++) chain_dirty[a][b] = 0x3FFFFFFFu; }

/* ---------------------------------------------------------------- sprites
 * AGA sprites, 32 pixels wide (FMODE SPR32).  NES sprite palette p -> channels 2p and 2p+1 (colour registers 16+4p+1..3).  A channel's stream is a
 * list of "instances" (control long + two longs per line: plane 0, plane 1); NES sprites of one palette that lie within a 32 pixel window and
 * touch or overlap vertically are merged into one instance, so a line can show 2 x 32 = 64 pixels per palette (the NES shows at most 64 per line). */
static uint8_t rev8[256];
#define SLONGS 1500
static uint32_t sprite_stream[2][8][SLONGS + 4] __attribute__((section(".bss.chip"), aligned(8)));
static uint8_t spr_init_done;
#define HBASE 0xA1
#ifndef SW
#define SW 32                                       /* sprite width in pixels */
#endif
#define CTLL (SW == 64 ? 4 : SW == 32 ? 2 : 1)       /* longs in the control block: one fetch unit each for POS and CTL (the word sits at the start of the unit) */
#define LPL (SW / 16)                               /* longs per line */
#define PLL (SW / 32 ? SW / 32 : 1)                 /* longs per plane and line */

/* MMC2 latch 0 as the sprite fetches leave it: sprites are fetched line by line, 8 per line in OAM order; a sprite tile $FD/$FE in pattern
 * table 0 flips the latch, which selects the 4 KB bank the following sprites (also on the next lines) take their pattern from.  Without such a
 * tile (the usual case) every sprite uses the bank of the current latch state. */
static uint8_t spr_bank[64][16] __attribute__((aligned(4))), spr_uni[64], slat0, spr_flat;     /* spr_uni: one bank for all rows of the sprite */
static unsigned spr_flat_bank, spr_flat_bank1;
#ifdef SPRDUMP
void vid_test_slat0(unsigned v) { slat0 = (uint8_t)v; }
#endif
static inline unsigned spr_bank_of(unsigned i, unsigned r, unsigned table) { return spr_flat ? (table ? spr_flat_bank1 : spr_flat_bank) : spr_bank[i][r]; }
extern uint32_t ppu_oam_gen;
static uint32_t banks_gen = ~0u, banks_key, banks_l0_end;
static void spr_banks(unsigned sz16, unsigned sptab, int commit)
{
    unsigned h = sz16 ? 16 : 8, l0 = slat0;
    uint32_t key = sz16 | sptab << 1 | slat0 << 2 | nes_chr_reg[0] << 8 | nes_chr_reg[1] << 13 | nes_chr_reg[2] << 18 | nes_chr_reg[3] << 23;
    if (banks_gen == ppu_oam_gen && banks_key == key) { if (commit) slat0 = (uint8_t)banks_l0_end; return; }
    banks_gen = ppu_oam_gen; banks_key = key; banks_l0_end = l0;
    /* the flip events: sprite rows whose tile is $FD/$FE in pattern table 0, in fetch order (line, then OAM index) */
    uint32_t ek[48]; uint8_t et[48], sb[49]; unsigned ne = 0;
    for (unsigned i = 0; i < 64 && ne < 47; i++) {
        unsigned y = ppu_oam[i * 4], tile = ppu_oam[i * 4 + 1];
        if (y >= 0xEF || tile < 0xFC) continue;
        if (sz16 ? (tile & 1) : sptab) continue;
        unsigned attr = ppu_oam[i * 4 + 2];
        for (unsigned r = 0; r < h && ne < 47; r++) {
            unsigned line = y + 1 + r, rr = (attr & 0x80) ? h - 1 - r : r, t2 = sz16 ? (tile & 0xFE) + (rr >= 8) : tile;
            if (line >= 240) break;
            if (t2 == 0xFD || t2 == 0xFE) {
                uint32_t k = line << 6 | i; unsigned j = ne++;
                while (j > 0 && ek[j - 1] > k) { ek[j] = ek[j - 1]; et[j] = et[j - 1]; j--; }
                ek[j] = k; et[j] = (uint8_t)(t2 == 0xFD);
            }
        }
    }
    if (!ne) { spr_flat = 1; for (unsigned i = 0; i < 64; i++) spr_uni[i] = 1; spr_flat_bank = nes_chr_reg[l0 ? 0 : 1]; spr_flat_bank1 = nes_chr_reg[3]; if (commit) slat0 = (uint8_t)l0; return; }
    spr_flat = 0;
    sb[0] = (uint8_t)l0;
    for (unsigned e = 0; e < ne; e++) sb[e + 1] = et[e];                    /* the state after event e is just the type of the last flip */
    for (unsigned i = 0; i < 64; i++) {
        unsigned y = ppu_oam[i * 4], tile = ppu_oam[i * 4 + 1];
        if (y >= 0xEF) continue;
        unsigned table = sz16 ? (tile & 1) : sptab, e = 0;
        if (((y + 1) << 6 | i) > ek[ne - 1]) {                                  /* starts after the last flip: one bank for all rows (the usual case) */
            uint32_t b4 = nes_chr_reg[table ? 3 : (sb[ne] ? 0 : 1)] * 0x01010101u;
            uint32_t *d = (uint32_t *)spr_bank[i]; d[0] = b4; d[1] = b4; d[2] = b4; d[3] = b4;
            spr_uni[i] = 1;
            continue;
        }
        unsigned first = 0xFF;
        spr_uni[i] = 1;
        for (unsigned r = 0; r < h; r++) {
            unsigned line = y + 1 + r;
            if (line >= 240) break;
            uint32_t k = line << 6 | i;
            while (e < ne && ek[e] < k) e++;
            unsigned b = nes_chr_reg[table ? 3 : (sb[e] ? 0 : 1)];
            spr_bank[i][r] = (uint8_t)b;
            if (first == 0xFF) first = b; else if (b != first) spr_uni[i] = 0;
        }
    }
    banks_l0_end = sb[ne];
    if (commit) slat0 = (uint8_t)banks_l0_end;
}
/* Hardware sprite pair q shows NES sprites of palette pair_pal[q]; pairs below bplcon2_n are in front of the playfield (NES priority bit 0),
 * the others behind it (bit 1).  The (palette, priority) combinations used in a frame are packed into the 4 pairs. */
static uint8_t pair_pal[4], pair_n_front, pair_used, pu_snap[2], pair_final[4], pf_snap[2][4];
static uint8_t pair_cur[4] = {0xFF, 0xFF, 0xFF, 0xFF};      /* palette each sprite pair's colour registers hold at the end of the last frame (after its Copper events) */
uint32_t dbg_roots[32], dbg_x[32]; unsigned dbg_nroots;
uint16_t badlog[24]; unsigned nbad;
#define BADLOG(k) { extern uint32_t nes_frames; if (nbad < 24 && (nbad == 0 || (badlog[nbad - 1] & 0x7FFF) != (nes_frames & 0x7FFF))) badlog[nbad++] = (uint16_t)((nes_frames & 0x7FFF) | (k) << 15); }
#define EVMAX 40
#ifndef GAP
#define GAP 1
#endif
static unsigned ev_n;                                     /* Copper colour events of this frame's sprite set: (line, pair, palette) in line order */
static uint16_t ev_line[EVMAX];
static uint8_t ev_pair[EVMAX], ev_pal[EVMAX];
static uint8_t ev_snap_pair[2][EVMAX], ev_snap_pal[2][EVMAX], ev_snap_n[2];
static uint16_t ev_snap_line[2][EVMAX];
static uint32_t bplcon2_val;
static uint32_t *sp_wp[8], *sp_base[8];
static unsigned sp_bot[8];
static unsigned vid_sz16, vid_sptab;

static void inst_control(uint32_t *ip, unsigned top, unsigned bot, unsigned x0)
{
    unsigned vs = plat_vstart + top, ve = plat_vstart + bot, hs = HBASE + x0;
    uint32_t pos = ((vs & 0xFF) << 8) | ((hs >> 1) & 0xFF), ctl = ((ve & 0xFF) << 8) | ((vs >> 8) << 2) | ((ve >> 8) << 1) | (hs & 1);
#if SW == 64
    ip[0] = pos << 16; ip[1] = 0; ip[2] = ctl << 16; ip[3] = 0;
#elif SW == 32
    ip[0] = pos << 16; ip[1] = ctl << 16;
#else
    ip[0] = pos << 16 | ctl;
#endif
}

/* Window instances.  The picture is cut into 32 pixel wide windows (x / 32); a hardware sprite instance covers one window for a stretch of lines and shows
 * every NES sprite of one (palette, priority) combination that overlaps the window (a sprite can lie in two windows).  Its plane data is composed line by
 * line with pixel exact shifts, the sprite with the lowest OAM index winning where they overlap, so stacked sprites (a boxer is rows of 8x8 sprites) and
 * sprites at odd positions cost one channel together instead of one each, and channels are only reused after a gap line. */
typedef struct { uint16_t top, bot; uint8_t x, oi, combo, nr, tile, attr, bank, uni, row[16][2]; } SSp;     /* a sprite with its (flipped) pattern rows, nr = lines shown */
static SSp ssp[64];
typedef struct { uint16_t top, bot, xhi; uint8_t combo, x0, key, first, n, xlo; } Inst;      /* xlo..xhi: horizontal extent of the sprites */      /* x0: left edge of the 32 pixel window */
static Inst insts[96];
static uint8_t ilist[256];                                /* sprite indexes of the instances, lowest OAM index last */

typedef struct { uint32_t key; uint8_t d[16][2]; } RowC;
static RowC rowc[512];
__attribute__((noinline)) static void sprite_rows(SSp *s, unsigned tile, unsigned attr)
{
    unsigned h = vid_sz16 ? 16 : 8, vf = attr & 0x80, hf = attr & 0x40, table = vid_sz16 ? (tile & 1) : vid_sptab;
    const uint8_t *tb = nes_chr + (vid_sz16 ? (tile & 0xFE) : tile) * 16u;
    unsigned b0 = spr_bank_of(s->oi, 0, table), uni = spr_uni[s->oi];
    RowC *rc = &rowc[(tile * 2u + (vf ? 1u : 0) + (hf ? 256u : 0) + b0 * 37u) & 511];
    uint32_t key = tile | (vf ? 0x100u : 0) | (hf ? 0x200u : 0) | b0 << 10 | (uint32_t)vid_sz16 << 16 | 0x80000000u;
#ifdef NOICACHE
    uni = 0;
#endif
    if (uni && rc->key == key) { for (unsigned r = 0; r < s->nr; r++) { s->row[r][0] = rc->d[r][0]; s->row[r][1] = rc->d[r][1]; } return; }
    for (unsigned r = 0; r < h; r++) {                                              /* all rows (the cache entry must be complete) */
        unsigned rr = vf ? h - 1 - r : r;
        unsigned bank = uni ? b0 : (r < s->nr ? spr_bank_of(s->oi, r, table) : b0);
        const uint8_t *c = tb + bank * 4096u + ((rr & 8) << 1) + (rr & 7);
        unsigned lo = c[0], hi = c[8];
        if (hf) { lo = rev8[lo]; hi = rev8[hi]; }
        if (uni) { rc->d[r][0] = (uint8_t)lo; rc->d[r][1] = (uint8_t)hi; }
        if (r < s->nr) { s->row[r][0] = (uint8_t)lo; s->row[r][1] = (uint8_t)hi; }
    }
    if (uni) rc->key = key;
}

/* Composed instances are cached: a character moves as a whole and mostly keeps its pose from frame to frame, so the plane data of an instance only depends
 * on which tiles (and flips, banks) sit at which offsets inside the window; the position goes into the control words.  The key is two 32 bit hashes. */
#define ICN 128
#define ICL 120                                                           /* longest instance (lines) that is cached */
#ifndef IPOOL
#define IPOOL 6144                                                        /* pool of plane data (longs), written as a ring */
#endif
typedef struct { uint32_t s1, s2, pos, nl; } ICEnt;
static ICEnt icache[ICN];
static uint32_t ipool[IPOOL], ihead;

__attribute__((noinline)) static void emit_inst(unsigned ch, const Inst *I)
{
    uint32_t *ip = sp_wp[ch], *lp = ip + CTLL;
    unsigned X0 = I->x0, nl = I->bot - I->top, cacheable = nl <= ICL;
    uint32_t s1 = 0x9E3779B9u ^ vid_sz16 ^ nl << 8, s2 = 0x85EBCA6Bu + nl;
    for (unsigned k = 0; k < I->n; k++) {
        const SSp *s = &ssp[ilist[I->first + k]];
        uint32_t v = (uint32_t)s->tile | (uint32_t)(s->attr & 0xC0) << 2 | (uint32_t)s->bank << 10 | (uint32_t)(s->x - X0) << 16,
                 v2 = (uint32_t)(s->top - I->top) | (uint32_t)s->nr << 8;
        cacheable &= s->uni;
        s1 = (s1 << 5 | s1 >> 27) ^ v; s1 = (s1 ^ v2 << 3) * 0x9E3779B1u;
        s2 = (s2 + v) * 0x01000193u; s2 = (s2 ^ v2) * 0x01000193u; s2 ^= s2 >> 15;
    }
#ifdef NOICACHE
    cacheable = 0;
#endif
    ICEnt *ic = &icache[(s1 ^ s2 >> 7) % ICN];
    if (cacheable && ic->s1 == s1 && ic->s2 == s2 && ic->nl == nl && ihead - ic->pos <= IPOOL - ICL * 2 - 8) {
        const uint32_t *d = &ipool[ic->pos % IPOOL];
        for (unsigned n = nl * 2; n; n--) *lp++ = *d++;
        vid_cnt[5]++;
    } else {
        uint32_t q0[240], q1[240];
        for (unsigned l = 0; l < nl; l++) { q0[l] = 0; q1[l] = 0; }
        for (unsigned k = 0; k < I->n; k++) {                                   /* highest OAM index first: the lowest ends up on top */
            SSp *s = &ssp[ilist[I->first + k]];
            sprite_rows(s, s->tile, s->attr);
            unsigned sh = 24 - (s->x - X0), l = s->top - I->top;                  /* 0 <= sh <= 24: the window starts at the leftmost sprite */
            for (unsigned r = 0; r < s->nr; r++, l++) {
                uint32_t v0 = (uint32_t)s->row[r][0] << sh, v1 = (uint32_t)s->row[r][1] << sh, m = v0 | v1;
                q0[l] = (q0[l] & ~m) | v0; q1[l] = (q1[l] & ~m) | v1;
            }
        }
        for (unsigned l = 0; l < nl; l++) { lp[0] = q0[l]; lp[1] = q1[l]; lp += LPL; }
        if (cacheable) {
            if (ihead % IPOOL + nl * 2 > IPOOL) ihead += IPOOL - ihead % IPOOL;      /* no wrapping inside an entry */
            uint32_t *d = &ipool[ihead % IPOOL];
            for (unsigned l = 0; l < nl; l++) { d[0] = q0[l]; d[1] = q1[l]; d += 2; }
            ic->s1 = s1; ic->s2 = s2; ic->nl = nl; ic->pos = ihead; ihead += nl * 2;
        }
        vid_cnt[7]++;
    }
    inst_control(ip, I->top, I->bot, X0);
    sp_wp[ch] = lp; sp_bot[ch] = I->bot;
}

/* Sprites whose pattern rows are all transparent (the game's filler tile $FF, the MMC2 trigger tiles $FD/$FE, ...) need no channel at all. */
static uint8_t blank_cache[32 * 256];                /* 0 unknown, 1 blank, 2 has pixels; indexed by 4 KB bank and tile */
static unsigned tile_blank(unsigned bank, unsigned tile)
{
    uint8_t *c = &blank_cache[(bank & 31) * 256 + tile];
    if (!*c) {
        const uint8_t *p = nes_chr + bank * 4096u + tile * 16u;
        unsigned a = 0;
        for (unsigned i = 0; i < 16; i++) a |= p[i];
        *c = a ? 2 : 1;
    }
    return *c == 1;
}
__attribute__((noinline)) static unsigned sprite_blank(unsigned i, unsigned tile, unsigned top, unsigned sz16, unsigned sptab)
{
    unsigned h = sz16 ? 16 : 8;
    if (spr_uni[i]) {
        unsigned table = sz16 ? (tile & 1) : sptab, bank = spr_bank_of(i, 0, table);
        return sz16 ? tile_blank(bank, tile & 0xFE) && tile_blank(bank, (tile & 0xFE) + 1) : tile_blank(bank, tile);
    }
    for (unsigned r = 0; r < h && top + r < 240; r++) {
        unsigned table = sz16 ? (tile & 1) : sptab, bank = spr_bank_of(i, r, table);
        if (sz16) { if (!tile_blank(bank, tile & 0xFE) || !tile_blank(bank, (tile & 0xFE) + 1)) return 0; r = 15; }
        else if (!tile_blank(bank, tile)) return 0;
    }
    return 1;
}

static uint8_t oam_snap[2][256] __attribute__((aligned(4))), pair_snap[2][4], nf_snap[2], snap_ok[2];
static uint32_t key_snap[2], bplcon2_snap[2], placed_snap[2];

__attribute__((noinline)) static void build_sprites(unsigned set)
{
    unsigned sz16 = (vctrl & 0x20) != 0, h = sz16 ? 16 : 8, placed = 0, sptab = (vctrl >> 3) & 1;
    if (!spr_init_done) { for (unsigned i = 0; i < 256; i++) { unsigned r = 0; for (unsigned b = 0; b < 8; b++) if (i & (1u << b)) r |= 0x80u >> b; rev8[i] = (uint8_t)r; } spr_init_done = 1; }
    vid_sz16 = sz16; vid_sptab = sptab;
    uint32_t key = nes_chr_reg[0] | nes_chr_reg[1] << 5 | nes_chr_reg[2] << 10 | nes_chr_reg[3] << 15 | (vctrl & 0x28) << 17 | (vmask & 0x10) << 20 | (uint32_t)slat0 << 28;
    {   /* same sprites as when this buffer was built: reuse it */
        const uint32_t *o = (const uint32_t *)ppu_oam, *c = (const uint32_t *)oam_snap[set];
        unsigned same = snap_ok[set] && key_snap[set] == key;
        for (unsigned i = 0; same && i < 64; i++) same = o[i] == c[i];
        if (same) {
            for (unsigned q = 0; q < 4; q++) pair_pal[q] = pair_snap[set][q];
            pair_n_front = nf_snap[set]; bplcon2_val = bplcon2_snap[set]; vid_stat[2] = placed_snap[set];
            pair_used = pu_snap[set]; for (unsigned q = 0; q < 4; q++) pair_final[q] = pf_snap[set][q];
            ev_n = ev_snap_n[set]; for (unsigned e = 0; e < ev_n; e++) { ev_line[e] = ev_snap_line[set][e]; ev_pair[e] = ev_snap_pair[set][e]; ev_pal[e] = ev_snap_pal[set][e]; }
            return;
        }
    }
    vid_cnt[4]++;
    { TPS(t0); spr_banks(sz16, sptab, 1); TPE(t0, 5); }
#ifdef SPRDBG
    vid_stat[4] = spr_bank[9][0] | (unsigned)spr_flat << 8 | (unsigned)slat0 << 16 | sptab << 24; vid_stat[5] = (unsigned)sz16 | (vctrl & 0xFF) << 8;
#endif
    unsigned n = 0;
    TPS(tpg);
#ifdef NOSPR
    if (0)
#else
    if (vmask & 0x10)
#endif
        for (unsigned i = 0; i < 64; i++) {
            uint32_t w = ((const uint32_t *)ppu_oam)[i];
            unsigned y = w >> 24, tile = (w >> 16) & 255, attr = (w >> 8) & 255;
            if (y >= 0xEF) continue;
            if (!sz16 && spr_uni[i]) {                                           /* the common case: one bank, tile known to be blank or not */
                unsigned bank = spr_bank_of(i, 0, sptab), bc = blank_cache[(bank & 31) * 256 + tile];
                if (bc == 1 || (bc == 0 && tile_blank(bank, tile))) continue;
            } else if (sprite_blank(i, tile, y + 1, sz16, sptab)) continue;
            if (i == 0 && (attr & 0x20) && (w & 255) >= 240) continue;            /* the sprite-0 marker behind the playfield at the right edge: costs a channel, shows nothing */
            SSp *s = &ssp[n++];
            s->top = (uint16_t)(y + 1); s->nr = (uint8_t)(240 - s->top < h ? 240 - s->top : h); s->bot = (uint16_t)(s->top + s->nr);
            s->x = (uint8_t)w; s->oi = (uint8_t)i; s->combo = (uint8_t)(((attr >> 5) & 1) * 4 + (attr & 3));
            s->tile = (uint8_t)tile; s->attr = (uint8_t)attr; s->uni = spr_uni[i];
            s->bank = (uint8_t)spr_bank_of(i, 0, sz16 ? (tile & 1) : sptab);
        }
    TPE(tpg, 4);
    TPS(tpa);
    for (unsigned c = 0; c < 8; c++) { sp_base[c] = sp_wp[c] = sprite_stream[set][c]; sp_bot[c] = 0; }
    /* 1. instances: per (combination, window) the sprites sorted by first line, cut where a line is not covered */
    unsigned nruns = 0, nil = 0;
    {
        uint8_t byx[64], tmp[64], cl[8][64], ncl[8] = { 0 }, inst_of[64], fillc[96];
        {   /* sprites by x (counting sort), then split by combination */
            uint8_t cnt[260] __attribute__((aligned(4)));
            for (unsigned q = 0; q < 260; q += 4) *(uint32_t *)&cnt[q] = 0;
            for (unsigned k = 0; k < n; k++) cnt[ssp[k].x + 1]++;
            for (unsigned q = 1; q < 257; q++) cnt[q] = (uint8_t)(cnt[q] + cnt[q - 1]);
            for (unsigned k = 0; k < n; k++) byx[cnt[ssp[k].x]++] = (uint8_t)k;
            for (unsigned k = 0; k < n; k++) { unsigned c = ssp[byx[k]].combo; cl[c][ncl[c]++] = byx[k]; }
        }
        for (unsigned combo = 0; combo < 8; combo++) {
            const uint8_t *bx = cl[combo]; unsigned nx = ncl[combo];
            for (unsigned g = 0; g < nx;) {                                         /* windows: from the leftmost sprite, 32 pixels wide */
                unsigned x0 = ssp[bx[g]].x, nt = 0, ge = g;
                while (ge < nx && ssp[bx[ge]].x <= x0 + 24) {
                    unsigned j = nt++;
                    while (j > 0 && ssp[tmp[j - 1]].top > ssp[bx[ge]].top) { tmp[j] = tmp[j - 1]; j--; }
                    tmp[j] = bx[ge]; ge++;
                }
                g = ge;
                for (unsigned a = 0; a < nt;) {                                     /* cut where a line is not covered */
                    unsigned b = a, end = ssp[tmp[a]].bot;
                    while (b + 1 < nt && ssp[tmp[b + 1]].top <= end) { b++; if (ssp[tmp[b]].bot > end) end = ssp[tmp[b]].bot; }
                    Inst *I = &insts[nruns]; 
                    I->top = ssp[tmp[a]].top; I->bot = (uint16_t)end; I->combo = (uint8_t)combo; I->x0 = (uint8_t)x0; I->first = (uint8_t)nil; I->n = (uint8_t)(b - a + 1);
                    for (unsigned q = a; q <= b; q++) inst_of[tmp[q]] = (uint8_t)nruns;
                    fillc[nruns] = 0; nil += I->n; nruns++;
                    a = b + 1;
                }
            }
        }
        for (unsigned k = n; k-- > 0;) {                                              /* highest OAM index first, so that the lowest wins in the composition */
            unsigned q = inst_of[k]; ilist[insts[q].first + fillc[q]++] = (uint8_t)k;
        }
        for (unsigned q = 0; q < nruns; q++) {
            Inst *I = &insts[q];
            I->key = ssp[ilist[I->first + I->n - 1]].oi;
            unsigned lo = 255, hi = 0;
            for (unsigned k = 0; k < I->n; k++) { unsigned x = ssp[ilist[I->first + k]].x; if (x < lo) lo = x; if (x > hi) hi = x; }
            I->xlo = (uint8_t)lo; I->xhi = (uint16_t)(hi + 8);
        }
    }
    TPE(tpa, 7);
    /* 2. assign the instances to channels.  A pair of channels shows one NES sprite palette at a time, but the Copper can change the three colours between
     * two instances (an "event" at the first line of the new one), so a pair serves any palette as long as the instances of its two channels that are
     * on screen at the same time use the same one.  Likewise the priority against the playfield (NES priority bit): the pairs below BPLCON2's count are in
     * front of it, and that count is changed by an event when the instances on screen allow it.  Where instances overlap they must stack in OAM order
     * (lower channel = in front).  Channel 7 is not used: its DMA slot does not get served with this bitplane fetch. */
    /* Phase A: channels, in NES priority order (lowest OAM index first).  An instance goes on the lowest (front instances) or highest (back instances) channel
     * where nothing else of that channel is within a gap line of it, the other channel of its pair shows the same palette while they overlap in time,
     * overlapping instances in the same window stack in OAM order (lower channel = in front), and front pairs are below back pairs wherever they overlap. */
    int8_t ich[96]; unsigned chlongs[8] = { 0 };
    uint8_t pl[96]; uint16_t plt[96], plb[96]; unsigned npl = 0;                  /* the placed instances: index, first line, end */
    uint8_t korder[96]; unsigned nko = 0;
    /* who must be in front of whom: instances of one class (front / back) whose sprites really overlap, the lower OAM index in front; then a topological order
     * (ties: lower OAM index first) so that an instance is placed after every instance that has to be above it */
    static uint8_t relm[96][96];                                              /* relm[a][b]: a in front of b */
    for (unsigned a = 0; a < nruns; a++) { ich[a] = -1; for (unsigned b = 0; b < nruns; b++) relm[a][b] = 0; }
    for (unsigned a = 0; a < nruns; a++)
        for (unsigned b = a + 1; b < nruns; b++) {
            const Inst *A = &insts[a], *B = &insts[b];
            if (A->combo == B->combo || (A->combo >> 2) != (B->combo >> 2)) continue;
            if (!(A->top < B->bot && B->top < A->bot) || !(A->xlo < B->xhi && B->xlo < A->xhi)) continue;
            for (unsigned p = 0; p < A->n; p++) {
                const SSp *sa = &ssp[ilist[A->first + p]];
                for (unsigned q = 0; q < B->n; q++) {
                    const SSp *sb = &ssp[ilist[B->first + q]];
                    if (sa->x < sb->x + 8 && sb->x < sa->x + 8 && sa->top < sb->bot && sb->top < sa->bot) { if (sa->oi < sb->oi) relm[a][b] = 1; else relm[b][a] = 1; }
                }
            }
        }
    {
        uint8_t used[96] = { 0 };
        for (unsigned k = 0; k < nruns; k++) {
            int best = -1, bestfree = 0;
            for (unsigned i = 0; i < nruns; i++) {
                if (used[i]) continue;
                unsigned fr = 1;
                for (unsigned j = 0; j < nruns && fr; j++) if (!used[j] && j != i && relm[j][i]) fr = 0;
                if (best < 0 || (fr > (unsigned)bestfree) || (fr == (unsigned)bestfree && ((insts[i].combo >> 2) < (insts[best].combo >> 2) || ((insts[i].combo >> 2) == (insts[best].combo >> 2) && insts[i].key < insts[best].key)))) { best = (int)i; bestfree = (int)fr; }
            }
            used[best] = 1; korder[nko++] = (uint8_t)best;
        }
    }
    TPS(tpb);
    for (unsigned ko = 0; ko < nko; ko++) {
        unsigned ii = korder[ko];
        const Inst *R = &insts[ii];
        unsigned pal = R->combo & 3, back = R->combo >> 2;
        unsigned base = 0, order = 0;                                             /* channels ruled out: always / only by the OAM order rule (bit c = channel c) */
        if (R->bot - R->top > 0) {
            for (unsigned e = 0; e < npl; e++) {
                if (!(plt[e] < R->bot + GAP && R->top < plb[e] + GAP)) continue;      /* not even a gap line apart */
                unsigned jj = pl[e];
                const Inst *Q = &insts[jj];
                unsigned qc = (unsigned)ich[jj], qp = qc >> 1;
                base |= 1u << qc;                                                     /* busy (touching counts: the channel needs a gap line) */
                if (!(Q->top < R->bot && R->top < Q->bot)) continue;                  /* but not on screen together */
                if ((Q->combo & 3) != pal) base |= 1u << (qc ^ 1);                    /* the other channel of its pair has to show its palette */
                if ((Q->combo >> 2) != back) {                                        /* front pairs below back pairs where they overlap */
                    unsigned m = 0;
                    for (unsigned c = 0; c < 7; c++) if (back ? (c >> 1) <= qp : (c >> 1) >= qp) m |= 1u << c;
                    base |= m;
                } else {
                    if (relm[jj][ii] && !relm[ii][jj]) order |= (2u << qc) - 1;          /* Q in front: only channels above Q's are allowed */
                    else if (relm[ii][jj] && !relm[jj][ii]) order |= 0x7Fu & ~((1u << qc) - 1);   /* Q behind: only channels below Q's are allowed */
                }
            }
        }
        int best = -1;
        for (unsigned relax = 0; relax < 2 && best < 0; relax++) {
            unsigned forbid = relax ? base : base | order;
            int bscore = 100000;
            for (unsigned c = 0; c < 7; c++) {
                if ((forbid >> c) & 1) continue;
                if (chlongs[c] + CTLL + (R->bot - R->top) * LPL + CTLL >= SLONGS) continue;
                int score = (int)c;
                if (score < bscore) { bscore = score; best = (int)c; }
            }
            if (best >= 0 && relax) { vid_stat[6]++; BADLOG(1) }
        }
        if (best < 0) { vid_stat[1]++; BADLOG(0) continue; }
        ich[ii] = (int8_t)best; chlongs[best] += CTLL + (R->bot - R->top) * LPL;
        pl[npl] = (uint8_t)ii; plt[npl] = R->top; plb[npl] = R->bot; npl++;
    }
#ifdef INSTDBG
    { extern uint32_t dbg_roots[32], dbg_x[32]; extern unsigned dbg_nroots; dbg_nroots = 0;
      for (unsigned i = 0; i < nruns && i < 32; i++) {
          dbg_roots[dbg_nroots] = insts[i].combo << 28 | (unsigned)(ich[i] & 0xF) << 24 | (insts[i].top & 0xFF) << 16 | (insts[i].bot & 0xFF) << 8 | insts[i].key;
          dbg_x[dbg_nroots++] = insts[i].x0 << 8 | insts[i].n;
      } }
#endif
    TPE(tpb, 2);
    TPS(tpc);
    /* Phase B: in time order, the Copper events (palette of a pair, number of front pairs) and the streams */
    unsigned anyback = 0, anyfront = 0;
    for (unsigned i = 0; i < nruns; i++) { if (insts[i].combo >> 2) anyback = 1; else anyfront = 1; }
    int ncur = -1; unsigned ninit = 2;
    uint8_t pair_now[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, pair_init[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    ev_n = 0;
    uint8_t rorder[96]; unsigned nro = 0;
    for (unsigned i = 0; i < nruns; i++) {                                   /* by first line, then NES priority */
        if (ich[i] < 0) continue;
        unsigned j = nro++;
        while (j > 0 && (insts[rorder[j - 1]].top > insts[i].top || (insts[rorder[j - 1]].top == insts[i].top && insts[rorder[j - 1]].key > insts[i].key))) { rorder[j] = rorder[j - 1]; j--; }
        rorder[j] = (uint8_t)i;
    }
    for (unsigned oi_ = 0; oi_ < nro; oi_++) {
        const Inst *R = &insts[rorder[oi_]];
        unsigned pal = R->combo & 3, back = R->combo >> 2, ch = (unsigned)ich[rorder[oi_]], P = ch >> 1;
        unsigned lo = back ? 0 : P + 1, hi = back ? P : 4;                         /* number of front pairs that suits everything on screen at this line */
        if (anyback && anyfront) for (unsigned j = 0; j < nruns; j++) {
            const Inst *Q = &insts[j];
            if (ich[j] < 0 || Q == R || !(Q->top <= R->top && Q->bot > R->top)) continue;
            unsigned QP = (unsigned)ich[j] >> 1;
            if (Q->combo >> 2) { if (QP < hi) hi = QP; } else if (QP + 1 > lo) lo = QP + 1;
        }
        if (lo > hi) lo = hi;
        unsigned nn = !anyback ? 4 : !anyfront ? 0 : ncur < 0 ? (2 < lo ? lo : 2 > hi ? hi : 2) : ((unsigned)ncur < lo ? lo : (unsigned)ncur > hi ? hi : (unsigned)ncur);
        if (pair_now[P] == 0xFF) pair_init[P] = (uint8_t)pal;
        else if (pair_now[P] != pal) {
            if (ev_n >= EVMAX) { vid_stat[1]++; continue; }
            ev_line[ev_n] = R->top; ev_pair[ev_n] = (uint8_t)P; ev_pal[ev_n] = (uint8_t)pal; ev_n++;
        }
        if (ncur < 0) { ncur = (int)nn; ninit = nn; }
        else if ((int)nn != ncur) {
            if (ev_n >= EVMAX) { vid_stat[1]++; continue; }
            ev_line[ev_n] = R->top; ev_pair[ev_n] = 0xFE; ev_pal[ev_n] = (uint8_t)nn; ev_n++; ncur = (int)nn;
        }
        pair_now[P] = (uint8_t)pal;
        emit_inst(ch, R); placed += R->n;
    }
    pair_used = 0;
    for (unsigned q = 0; q < 4; q++) {
        pair_pal[q] = pair_init[q] == 0xFF ? (pair_cur[q] == 0xFF ? 0 : pair_cur[q]) : pair_init[q];      /* an unused pair keeps what it has */
        pair_final[q] = pair_now[q] == 0xFF ? pair_pal[q] : pair_now[q];
        if (pair_init[q] != 0xFF) pair_used |= (uint8_t)(1u << q);
    }
    pair_n_front = (uint8_t)ninit; bplcon2_val = ninit | ninit << 3;
    TPE(tpc, 6);
    for (unsigned c = 0; c < 8; c++) for (unsigned q = 0; q < CTLL; q++) *sp_wp[c]++ = 0;
    vid_stat[2] = placed;
    {   /* remember what this buffer shows */
        uint32_t *c = (uint32_t *)oam_snap[set]; const uint32_t *o = (const uint32_t *)ppu_oam;
        for (unsigned i = 0; i < 64; i++) c[i] = o[i];
        for (unsigned q = 0; q < 4; q++) pair_snap[set][q] = pair_pal[q];
        ev_snap_n[set] = (uint8_t)ev_n; for (unsigned e = 0; e < ev_n; e++) { ev_snap_line[set][e] = ev_line[e]; ev_snap_pair[set][e] = ev_pair[e]; ev_snap_pal[set][e] = ev_pal[e]; }
        pu_snap[set] = pair_used; for (unsigned q = 0; q < 4; q++) pf_snap[set][q] = pair_final[q];
        nf_snap[set] = pair_n_front; bplcon2_snap[set] = bplcon2_val; placed_snap[set] = placed; key_snap[set] = key; snap_ok[set] = 1;
    }
}

/* ------------------------------------------------------------------ copper */
#define CL_WORDS 2048
static uint16_t copperlist[2][CL_WORDS] __attribute__((section(".bss.chip"), aligned(4)));
static unsigned cl_n;
static uint16_t *clp;
static void CMV(unsigned reg, unsigned val) { *clp++ = (uint16_t)reg; *clp++ = (uint16_t)val; }
static unsigned cl_wrap;
static void CWAIT(unsigned line)
{
    if (line >= 256) { if (!cl_wrap) { *clp++ = 0xFFDF; *clp++ = 0xFFFE; cl_wrap = 1; } line -= 256; }
    *clp++ = (uint16_t)((line << 8) | 0x01); *clp++ = 0xFFFE;
}
static void set_planes(const uint8_t *base[4], unsigned off, unsigned scroll, unsigned mod)
{
    for (unsigned k = 0; k < 4; k++) { uint32_t a = (uint32_t)base[k] + off; CMV(0x0E0 + k * 4, a >> 16); CMV(0x0E2 + k * 4, a & 0xFFFF); }
#ifdef BPL16
    CMV(0x102, scroll | (scroll << 4));
#else
    CMV(0x102, (scroll & 15) | ((scroll & 15) << 4) | ((scroll & 16) ? 0x4400 : 0));          /* AGA: delay bit 4 of both playfields */
#endif
    CMV(0x108, mod & 0xFFFF); CMV(0x10A, mod & 0xFFFF);
}

#ifdef HUD
uint8_t hudbm[4][PADB + 40 * 20 + PADB] __attribute__((section(".bss.chip"), aligned(4)));
#else
void vid_debug_hex(unsigned x, unsigned y, uint32_t val, unsigned nd) { (void)x; (void)y; (void)val; (void)nd; }
#endif

static uint32_t pal_shadow[256];
static uint8_t pal_cur[32], pal_mask_cur = 0xFF, pal_valid;

__attribute__((noinline)) static void update_palette(void)
{
    unsigned grey = ppu_mask & 1;
    int chg = !pal_valid || pal_mask_cur != grey;
    for (unsigned i = 0; i < 32; i++) if (pal_cur[i] != ppu_pal[i]) { pal_cur[i] = ppu_pal[i]; chg = 1; }
    for (unsigned q = 0; q < 4; q++) { if (pair_cur[q] != pair_pal[q]) chg = 1; pair_cur[q] = pair_final[q]; }
#ifndef SPRTEST
    if (!chg) return;
#endif
    pal_valid = 1; pal_mask_cur = (uint8_t)grey;
    for (unsigned i = 0; i < 32; i++) {
        unsigned idx = (i & 3) ? i : 0;
        if (i >= 16 && (i & 3)) idx = 16 + pair_pal[(i - 16) >> 2] * 4 + (i & 3);
        unsigned c = ppu_pal[idx] & 0x3F;
        if (grey) c &= 0x30;
        pal_shadow[i] = ((uint32_t)nes_rgb[c][0] << 16) | ((uint32_t)nes_rgb[c][1] << 8) | nes_rgb[c][2];
    }
    for (unsigned i = 1; i < 32; i++) if (!(i & 3)) pal_shadow[i] = pal_shadow[0];
#ifdef SPRTEST
    for (unsigned i = 16; i < 32; i++) pal_shadow[i] = (i & 1 ? 0xFF0000 : 0x00FF00) | (i & 2 ? 0x0000FF : 0) | ((i & 4) ? 0x808000 : 0);
#endif
#ifdef NOBG
    for (unsigned i = 0; i < 32; i += 4) pal_shadow[i] = 0x5C94FC;
#endif
    plat_set_palette32(pal_shadow);
}

/* Copper colour events: the three colours of a sprite pair (registers 16 + 4 pair + 1..3) are rewritten with both nibble passes (LOCT) of bank 0 */
#define BPLCON3_BASE 0x0C20                         /* as in platform.c: bank 0, LOCT 0, lores sprites, border blanked */
#define BPLCON3_LOCT 0x0200
static void cop_pair_colours(unsigned pair, unsigned pal)
{
    uint32_t c[3];
    unsigned grey = ppu_mask & 1;
    for (unsigned k = 1; k < 4; k++) {
        unsigned v = ppu_pal[16 + pal * 4 + k] & 0x3F;
        if (grey) v &= 0x30;
        c[k - 1] = ((uint32_t)nes_rgb[v][0] << 16) | ((uint32_t)nes_rgb[v][1] << 8) | nes_rgb[v][2];
    }
    CMV(0x106, BPLCON3_BASE);
    for (unsigned k = 0; k < 3; k++) CMV(0x180 + (16 + pair * 4 + 1 + k) * 2, ((c[k] >> 12) & 0xF00) | ((c[k] >> 8) & 0xF0) | ((c[k] >> 4) & 0xF));
    CMV(0x106, BPLCON3_BASE | BPLCON3_LOCT);
    for (unsigned k = 0; k < 3; k++) CMV(0x180 + (16 + pair * 4 + 1 + k) * 2, ((c[k] >> 8) & 0xF00) | ((c[k] >> 4) & 0xF0) | (c[k] & 0xF));
    CMV(0x106, BPLCON3_BASE);
}
static unsigned ev_next;
#ifndef EVLEAD
#define EVLEAD 0
#endif
static void flush_events(unsigned upto)           /* all events on lines before `upto` (NES lines) */
{
    while (ev_next < ev_n && ev_line[ev_next] < upto) {
        CWAIT(plat_vstart + ev_line[ev_next] - EVLEAD);
        if (ev_pair[ev_next] == 0xFE) CMV(0x104, ev_pal[ev_next] | ev_pal[ev_next] << 3); else cop_pair_colours(ev_pair[ev_next], ev_pal[ev_next]);
        ev_next++;
    }
}

static unsigned fset;
static uint8_t *planes_of(unsigned m, unsigned k) { return tor(m, cur_var[m], k); }

__attribute__((noinline)) static void build_copper(const Region *r, int n)
{
    uint16_t *list = copperlist[fset];
    clp = list; cl_wrap = 0;
    /* header: sprite pointers (the sprite DMA restarts every frame) */
    for (unsigned c = 0; c < 8; c++) { uint32_t a = (uint32_t)sprite_stream[fset][c]; CMV(0x120 + c * 4, a >> 16); CMV(0x122 + c * 4, a & 0xFFFF); }
    unsigned first = 1;
#ifdef BPL2
    CMV(0x104, BPL2);
#else
    CMV(0x104, bplcon2_val);
#endif
    ev_next = 0;
    /* every pair in use starts the picture with its first palette: written just before the display window (not at the top of the frame, where the vertical
     * blank interrupt may still be loading the palette with the CPU and BPLCON3 would be changed under it); a frame that is displayed twice needs this */
    if (pair_used) {
        CWAIT(plat_vstart - 3);
        for (unsigned P = 0; P < 4; P++) if (pair_used & (1u << P)) cop_pair_colours(P, pair_pal[P]);
    }
#ifdef HUD
    { uint32_t c = pal_shadow[15]; CMV(0x19E, ((c >> 12) & 0xF00) | ((c >> 8) & 0xF0) | ((c >> 4) & 0xF)); }
#endif
    for (int i = 0; i < n; i++) {
        const uint8_t *base[4];
#ifdef NOBG
        unsigned y0 = r[i].y0, y1 = r[i].y1, blank = 1;
#else
        unsigned y0 = r[i].y0, y1 = r[i].y1, blank = !(r[i].mask & 0x08);
#endif
        if (y1 <= y0) continue;
        flush_events(y0);
        if (!first) CWAIT(plat_vstart + y0);
        if (blank) {
            for (unsigned k = 0; k < 4; k++) base[k] = &blank34[PADB];
            set_planes(base, 0, 0, (unsigned)-VID_FW);
            first = 0; continue;
        }
        unsigned v0 = r[i].v0, m = r[i].mir;
        unsigned cy = (v0 >> 5) & 31, fy = (v0 >> 12) & 7, ny = m ? (v0 >> 11) & 1 : 0, RM = m ? 480 : 240;
        if (cy >= 30) { cy = 29; fy = 7; }
        unsigned X = (m ? 0 : ((v0 >> 10) & 1) * 256) + (v0 & 31) * 8 + r[i].fx, Y = ny * 240 + cy * 8 + fy;
#ifdef BPL16
        unsigned off = ((X + 15) >> 4) * 2 - 2, d = (16 - (X & 15)) & 15;
#else
        /* A delay d < 16 shows pixel S + 32 - d of the fetched data first, d >= 16 (AGA: the fifth delay bit) one unit later: S + 64 - d (measured).
         * S = start of the fetched data; solve for the first visible pixel X.  The latter case is why 10 units are fetched. */
        unsigned Xs = X + BGADJ;                                       /* measured: the picture shows pixel X + 1 where X is asked for, so ask for one less (BGTEST) */
        unsigned d = (32 - (Xs & 31)) & 31;
        int S = (int)Xs + (int)d - (d < 16 ? 32 : 64);
        unsigned off = (unsigned)(S >> 3);
#endif

        unsigned rem = y1 - y0, yy = y0;
        for (unsigned k = 0; k < 4; k++) base[k] = planes_of(m, k);
        while (rem) {
            unsigned run = RM - Y; if (run > rem) run = rem;
            set_planes(base, Y * STRIDE + off, d, STRIDE - VID_FW);
            rem -= run; yy += run; Y = (Y + run) % RM;
            if (rem) { flush_events(yy); CWAIT(plat_vstart + yy); }
        }
        first = 0;
    }
    {   /* below the picture: black */
        const uint8_t *base[4];
        for (unsigned k = 0; k < 4; k++) base[k] = &blank34[PADB];
        flush_events(240);
        CWAIT(plat_vstart + 240);
        set_planes(base, 0, 0, (unsigned)-VID_FW);
    }
#ifdef HUD
    {   /* diagnostics band */
        const uint8_t *base[4];
        CWAIT(plat_vstart + 241);
        CMV(0x19E, 0x0FFF);                                   /* COLOR15 := white (the digits use colour index 15) */
        for (unsigned k = 0; k < 4; k++) base[k] = &hudbm[k][PADB];
        set_planes(base, 0, 0, 0);
    }
#endif
    *clp++ = 0xFFFF; *clp++ = 0xFFFE;
    cl_n = (unsigned)(clp - list);
}

/* ---------------------------------------------------------------- present */
void vid_present(const Region *r, int n)
{
    TPS(tpall);
    TPS(tp0);
    unsigned used[2] = {0, 0};
    vctrl = n ? r[0].pad : ppu_ctrl; vmask = 0;
    {   /* the control register of the region that shows most lines */
        unsigned best = 0;
        for (int i = 0; i < n; i++) if ((r[i].mask & 0x08) && (unsigned)(r[i].y1 - r[i].y0) > best) { best = (unsigned)(r[i].y1 - r[i].y0); vctrl = r[i].pad; }
    }
    for (int i = 0; i < n; i++) vmask |= r[i].mask;
    vid_cnt[3] += (uint32_t)n;
    resolve_regions(r, n, 0, 0, 0, 0, 0, 0);
    vid_chr_changed();
    for (int i = 0; i < n; i++) if (r[i].mask & 0x08) used[r[i].mir ? 1 : 0] = 1;
    for (unsigned m = 0; m < 2; m++) if (used[m]) { TPS(t0); select_variant(m); process_draw(m); TPE(t0, 6); }
    TPE(tp0, 1);
    {   /* the list being displayed is the other one: do not overwrite it before the previous COP1LC took effect */
        static uint32_t last_vbl;
        while (plat_vbl_count == last_vbl) { }
        last_vbl = plat_vbl_count;
    }
#if defined(HUD) && !defined(PROFILE)
    if (!(vid_stat[3] & 15)) {
    vid_debug_hex(2, 6, vctrl, 2); vid_debug_hex(12, 6, vmask, 2);
    for (unsigned k = 0; k < 4; k++) vid_debug_hex(24 + k * 10, 6, nes_chr_reg[k], 2);
    vid_debug_hex(66, 6, (unsigned)n, 2); vid_debug_hex(76, 6, r[0].mir, 1);
    for (int k = 0; k < n && k < 4; k++) { vid_debug_hex(84 + k * 40, 6, r[k].y0, 2); vid_debug_hex(100 + k * 40, 6, r[k].v0, 4); vid_debug_hex(84 + k * 40 + 24 + 14 + 0, 6, 0, 0); }
    }
#endif
    TPS2(tp0);
    fset ^= 1;
    build_sprites(fset);
    TPE(tp0, 3);
    update_palette();
    build_copper(r, n);
    *(volatile uint32_t *)(0xDFF080) = (uint32_t)copperlist[fset];       /* COP1LC: takes effect at the next vertical blank */
    vid_stat[3]++;
    TPE(tpall, 0);
}

/* ------------------------------------------------------- sprite 0 hit time */
uint32_t vid_spr0_hit(const Region *r, int n, int open_y0, unsigned open_v, unsigned open_fx, unsigned open_mir, unsigned open_ctrl)
{
    if (!(ppu_mask & 0x10) || !(ppu_mask & 0x08)) return NEVER;
    unsigned y = ppu_oam[0];
    if (y >= 0xEF) return NEVER;
    vid_cnt[6]++;
#ifdef NOSPR0
    return (uint32_t)(y + 1) * T_LINE + 100;      /* experiment: cost of the evaluation removed, hit time approximate */
#endif
    {   /* PPUCTRL as it applies to the lines the hit depends on (not whatever the game wrote last) */
        unsigned ln0 = y + 1;
        vctrl = open_ctrl;
        for (int i = 0; i < n; i++) if (ln0 >= r[i].y0 && ln0 < r[i].y1) { vctrl = r[i].pad; break; }
    }
    unsigned tile = ppu_oam[1], attr = ppu_oam[2], sx = ppu_oam[3], sz16 = (vctrl & 0x20) != 0, h = sz16 ? 16 : 8;
    resolve_regions(r, n, open_v, open_mir, 1, open_y0, open_ctrl, (int)(y + 1 + h) < 240 ? (int)(y + 1 + h) : 240); vid_chr_changed();
    spr_banks(sz16, (vctrl >> 3) & 1, 0);
    unsigned drawn_m = 0;
    for (unsigned rr = 0; rr < h; rr++) {
        unsigned ln = y + 1 + rr;
        if (ln >= 240) break;
        unsigned v0, fx, m, y0;
        v0 = open_v; fx = open_fx; m = open_mir; y0 = (unsigned)open_y0;
        for (int i = 0; i < n; i++) if (ln >= r[i].y0 && ln < r[i].y1) { v0 = r[i].v0; fx = r[i].fx; m = r[i].mir; y0 = r[i].y0; break; }
        if (!(drawn_m & (1u << m))) { TPS(t0); drawn_m |= 1u << m; select_variant(m); process_draw(m); TPE(t0, 6); }
        unsigned cy = (v0 >> 5) & 31, fy = (v0 >> 12) & 7, ny = m ? (v0 >> 11) & 1 : 0, RM = m ? 480 : 240;
        if (cy >= 30) { cy = 29; fy = 7; }
        unsigned X = (m ? 0 : ((v0 >> 10) & 1) * 256) + (v0 & 31) * 8 + fx, Y = (ny * 240 + cy * 8 + fy + (ln - y0)) % RM;
        unsigned fr = (attr & 0x80) ? h - 1 - rr : rr, table, t2;
        if (sz16) { table = tile & 1; t2 = (tile & 0xFE) + (fr >= 8); fr &= 7; } else { table = (vctrl & 8) ? 1 : 0; t2 = tile; }
        const uint8_t *chr = nes_chr + spr_bank_of(0, rr, table) * 4096u + t2 * 16u;
        unsigned lo = chr[fr], hi = chr[fr + 8], sp = lo | hi;
        if (attr & 0x40) { if (!spr_init_done) { for (unsigned i = 0; i < 256; i++) { unsigned q = 0; for (unsigned b = 0; b < 8; b++) if (i & (1u << b)) q |= 0x80u >> b; rev8[i] = (uint8_t)q; } spr_init_done = 1; } sp = rev8[sp]; }
        for (unsigned b = 0; b < 8; b++) {
            unsigned x = sx + b;
            if (x >= 255) break;
            if (!((sp >> (7 - b)) & 1)) continue;
            unsigned bx = (X + x) & (m ? 255 : 511), bm = 0x80u >> (bx & 7);
            if ((tor(m, cur_var[m], 0)[Y * STRIDE + (bx >> 3)] | tor(m, cur_var[m], 1)[Y * STRIDE + (bx >> 3)]) & bm) return (uint32_t)ln * T_LINE + (x + 1) * 5 + 15;
        }
    }
    return NEVER;
}

void vid_init(void)
{
    bg_text_init();
    for (unsigned i = 0; i < 8192; i++) { uint32_t h = 2166136261u; for (unsigned k = 0; k < 16; k++) h = (h ^ nes_chr[i * 16 + k]) * 16777619u; chr_h[i] = (uint16_t)(h ^ (h >> 16)); }
    vid_ctrl_changed();
    for (unsigned m = 0; m < 2; m++) for (unsigned v = 0; v < NV; v++) { var_key[m][v] = 0xFFFF; var_use[m][v] = 0; for (unsigned b = 0; b < 2; b++) { drow[m][v][b] = 0x3FFFFFFFu; for (unsigned r = 0; r < 30; r++) dcol[m][v][b][r] = 0xFFFFFFFFu; for (unsigned i = 0; i < 960; i++) drawn[m][v][b][i] = 0xFFFFFFFFu; } }
    bgtable_cur = 0;
    /* display: 4 bitplanes, lores, 17 words fetched per line (one extra for fine scrolling), sprites on */
    *(volatile uint16_t *)0xDFF096 = 0x7FFF;
    *(volatile uint16_t *)0xDFF100 = 0x4201;                  /* BPLCON0: 4 planes, COLOR, ECSENA */
#ifdef BPL16
    *(volatile uint16_t *)0xDFF1FC = (SW == 64) ? 0x000C : (SW == 32) ? 0x0004 : 0x0000;                  /* FMODE: 16 bit bitplane fetch, wide sprites */
    *(volatile uint16_t *)0xDFF092 = 0x0040;                  /* DDFSTRT (one word early) */
    *(volatile uint16_t *)0xDFF094 = 0x00C0;                  /* DDFSTOP */
#else
    *(volatile uint16_t *)0xDFF1FC = ((SW == 64) ? 0x000C : (SW == 32) ? 0x0004 : 0x0000) | 1;            /* FMODE: 32 bit bitplane fetch (half the DMA slots) */
#ifndef DDFS
#define DDFS 0x0030
#endif
    *(volatile uint16_t *)0xDFF092 = DDFS;                  /* DDFSTRT: one 32 pixel unit before the window */
    *(volatile uint16_t *)0xDFF094 = 0x00C0;                  /* DDFSTOP: 10 units */
#endif
    *(volatile uint16_t *)0xDFF102 = 0;
    *(volatile uint16_t *)0xDFF104 = 0x0000;                  /* BPLCON2: all sprite pairs in front of the playfield */
    *(volatile uint16_t *)0xDFF108 = STRIDE - VID_FW; *(volatile uint16_t *)0xDFF10A = STRIDE - VID_FW;
    {   /* an empty first frame: blank screen */
        uint16_t *p = copperlist[0]; p[0] = 0xFFFF; p[1] = 0xFFFE;
        *(volatile uint32_t *)0xDFF080 = (uint32_t)copperlist[0];
        *(volatile uint16_t *)0xDFF088 = 0;
    }
    *(volatile uint16_t *)0xDFF096 = 0x8000 | 0x0200 | 0x0100 | 0x0080 | 0x0040 | 0x0020;      /* DMAEN BPLEN COPEN BLTEN SPREN */
}
