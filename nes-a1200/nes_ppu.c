/* nes_ppu.c - NES PPU model (2C07, PAL): registers, loopy scroll, sprite-0 hit timing.
 * No pixels are produced here.  The model walks the scanlines as CPU time passes (ppu_run) and records REGIONS: stretches of lines displayed
 * under the same scroll/mask/mirroring state.  At vblank the regions go to nes_vid.c, which turns them into Copper-driven hardware scrolling. */
#include "nes.h"
#include "platform.h"
#include "nes_vid.h"

uint8_t ppu_ctrl, ppu_mask, ppu_vbl, ppu_ovf;
uint8_t ppu_nt[2][1024] __attribute__((aligned(4)));            /* internal 2 KB VRAM */
uint8_t ppu_pal[32];
uint8_t ppu_oam[256] __attribute__((aligned(4)));
uint8_t ppu_oam_dirty = 1;
static uint8_t oamaddr;
static uint16_t v, t;                  /* loopy registers */
static uint8_t fine_x, wlatch, rdbuf, bus;
static int line;                       /* next scanline to process (0..240) */

#define MAXR 64
static Region regs[MAXR];
static int nreg, region_open, v_written;
static int open_y0;
static unsigned open_v, open_fx, open_mask, open_mir, open_ctrl;

static uint32_t spr0_t;                /* frame-relative time of the sprite-0 hit; NEVER (0xFFFFFFFE) = none */
static uint8_t spr0_ok, spr0_latched;
static int spr0_last;                  /* last scanline the cached sprite-0 hit time depends on */
uint32_t ppu_oam_gen;                  /* changes whenever the OAM contents change */
#define NEVER 0xFFFFFFFEu

static int st_dirty;                   /* a register that shapes the regions was written: the next two lines are walked one by one */
static inline void spr0_inval(void) { spr0_ok = 0; }
void ppu_spr0_inval(void) { spr0_ok = 0; st_dirty = 2; }

void ppu_init(void)
{
    for (unsigned i = 0; i < 32; i++) ppu_pal[i] = 0x0F;
    line = 0; nreg = 0; region_open = 0; ppu_vbl = 0; spr0_ok = spr0_latched = 0;
    vid_init();
}

/* nametable address -> byte pointer (mirroring from the mapper) */
static inline uint8_t *nt_ptr(unsigned a, unsigned *bank)   /* a = 0x2000..0x2FFF */
{
    unsigned n = (a >> 10) & 3;
    unsigned b = nes_mirror_h ? (n >> 1) : (n & 1);
    *bank = b;
    return &ppu_nt[b][a & 0x3FF];
}

/* ---------------------------------------------------------------- scanline walk */
static inline int rendering(void) { return (ppu_mask & 0x18) != 0; }

/* end-of-line scroll update (dots 256 and 257 of the hardware) */
static inline void line_end_update(void)
{
    if (!rendering()) return;
    if ((v & 0x7000) != 0x7000) v += 0x1000;
    else {
        v &= ~0x7000u;
        unsigned cy = (v & 0x03E0) >> 5;
        if (cy == 29) { cy = 0; v ^= 0x0800; } else if (cy == 31) cy = 0; else cy++;
        v = (uint16_t)((v & ~0x03E0) | (cy << 5));
    }
    v = (uint16_t)((v & ~0x041F) | (t & 0x041F));
}

static void close_region(int y1)
{
    if (!region_open || y1 <= open_y0) return;
    if (nreg < MAXR) {
        Region *r = &regs[nreg++];
        r->y0 = (uint16_t)open_y0; r->y1 = (uint16_t)y1; r->v0 = (uint16_t)open_v; r->fx = (uint8_t)open_fx; r->mask = (uint8_t)open_mask; r->mir = (uint8_t)open_mir; r->pad = (uint8_t)open_ctrl;
    } else regs[nreg - 1].y1 = (uint16_t)y1;
}

/* make the open region match the state that applies to line `ln` (closing the previous one if anything changed) */
static void region_sync(int ln)
{
    if (region_open) {
        if (!v_written && !((v ^ open_v) & 0x041F) && fine_x == open_fx && !((ppu_mask ^ open_mask) & 0x08) && nes_mirror_h == open_mir && !((ppu_ctrl ^ open_ctrl) & 0x38)) return;
        close_region(ln);
    }
    region_open = 1; v_written = 0;
    open_y0 = ln; open_v = v; open_fx = fine_x; open_mask = ppu_mask; open_mir = nes_mirror_h; open_ctrl = ppu_ctrl;
    if (spr0_ok && ln <= spr0_last) spr0_inval();
}

/* advance the scroll registers over k scanlines at once (rendering on): per line the fine Y / coarse Y step, then the horizontal bits come from t */
static void advance_v(unsigned k)
{
    while (k) {
        unsigned fy = (v >> 12) & 7, step = 8 - fy;
        if (step > k) step = k;
        if (fy + step < 8) v = (uint16_t)(v + (step << 12));
        else {
            unsigned cy = (v & 0x03E0) >> 5;
            v &= ~0x7000u;
            if (cy == 29) { cy = 0; v ^= 0x0800; } else if (cy == 31) cy = 0; else cy++;
            v = (uint16_t)((v & ~0x03E0) | (cy << 5));
        }
        k -= step;
    }
    v = (uint16_t)((v & ~0x041F) | (t & 0x041F));
}

void ppu_run(uint32_t now)
{
    int upto;
    if (now < 5) return;
    upto = (int)((now - 5) / T_LINE) + 1;
    if (upto > 240) upto = 240;
    if (upto <= line) return;
    while (st_dirty > 0 && line < upto) { region_sync(line); line_end_update(); line++; st_dirty--; }
    if (line < upto) {
        region_sync(line);
        if (rendering()) advance_v((unsigned)(upto - line));
        line = upto;
    }
}

/* ---------------------------------------------------------------- registers */
static void spr0_eval(void)
{
    if (spr0_ok) return;
    TPS(t0);
    region_sync(line);
    spr0_t = vid_spr0_hit(regs, nreg, open_y0, open_v, open_fx, open_mir, open_ctrl);
    spr0_ok = 1; spr0_last = (int)ppu_oam[0] + 1 + ((ppu_ctrl & 0x20) ? 16 : 8);
    TPE(t0, 2);
}

uint32_t ppu_spr0_time(void)
{
    if (spr0_latched) return 0xFFFFFFFFu;
    spr0_eval();
    return spr0_t;
}

uint32_t ppu_reg_rd(uint32_t r, uint32_t now)
{
    uint8_t d;
    ppu_run(now);
    switch (r) {
    case 2: {
        unsigned hit = spr0_latched;
        if (!hit && (ppu_mask & 0x18) == 0x18 && now < 241u * T_LINE) {
            spr0_eval();
            if (spr0_t != NEVER && now >= spr0_t) { spr0_latched = 1; hit = 1; }
        }
        d = (uint8_t)((ppu_vbl ? 0x80 : 0) | (hit ? 0x40 : 0) | (ppu_ovf ? 0x20 : 0) | (bus & 0x1F));
        ppu_vbl = 0; wlatch = 0; bus = d;
        return d;
    }
    case 4: bus = ppu_oam[oamaddr]; return bus;
    case 7: {
        unsigned a = v & 0x3FFF, b;
        if (a >= 0x3F00) { unsigned i = a & 0x1F; if ((i & 0x13) == 0x10) i &= ~0x10u; d = (uint8_t)(ppu_pal[i] & 0x3F); rdbuf = *nt_ptr(0x2000 | (a & 0xFFF), &b); }
        else { d = rdbuf;
            if (a < 0x2000) rdbuf = nes_chr[(unsigned)nes_chr_reg[(a < 0x1000) ? 1 : 3] * 4096u + (a & 0xFFF)];
            else rdbuf = *nt_ptr(a, &b); }
        v = (uint16_t)(v + ((ppu_ctrl & 4) ? 32 : 1));
        bus = d; return d;
    }
    default: return bus;
    }
}

/* ---- text patches: the game draws its text with ordinary nametable tiles (A = $0B ... Z = $24, space = $FF; $FD/$FE are the MMC2 latch tiles, blank too), so
 * phrases can be replaced once per frame just before the frame is presented.  The game's own writes go to tm_raw (and show at once); a row that was written is
 * rebuilt from tm_raw with the replacements applied, so a half written line is simply redone when the rest arrives. */
static uint8_t tm_raw[2][960];
static uint32_t tm_dirty[2];

typedef struct { uint8_t b, r, c, pal, tile, on; } TmFix;
static TmFix tm_fixes[96];
static unsigned tm_nfix;

static unsigned tm_pal(unsigned b, unsigned r, unsigned c)             /* palette (0..3) of the 2x2 tile block holding tile (c, r) */
{
    return (ppu_nt[b][0x3C0 + (r >> 2) * 8 + (c >> 2)] >> (((r & 2) ? 4 : 0) | ((c & 2) ? 2 : 0))) & 3;
}
static void tm_setpal(unsigned b, unsigned r, unsigned c, unsigned pal)
{
    unsigned off = 0x3C0 + (r >> 2) * 8 + (c >> 2), sh = ((r & 2) ? 4 : 0) | ((c & 2) ? 2 : 0), old = ppu_nt[b][off];
    unsigned nv = (old & ~(3u << sh)) | (pal << sh);
    if (nv != old) { ppu_nt[b][off] = (uint8_t)nv; vid_nt_changed(b, off, old, nv); }
}
static void tm_fix(unsigned b, unsigned r, unsigned c, unsigned pal, unsigned tile)      /* text that moved into a neighbouring colour block keeps its original colour */
{
    tm_setpal(b, r, c, pal);
    for (unsigned i = 0; i < 96; i++) { TmFix *f = &tm_fixes[i]; if (f->on && f->b == b && f->r == r && f->c == c) { f->pal = (uint8_t)pal; f->tile = (uint8_t)tile; return; } }
    TmFix *f = &tm_fixes[tm_nfix++ % 96];
    f->b = (uint8_t)b; f->r = (uint8_t)r; f->c = (uint8_t)c; f->pal = (uint8_t)pal; f->tile = (uint8_t)tile; f->on = 1;
}
static void tm_refix(void)                               /* the game may rewrite the attribute table after the text: keep the colours while the text is there */
{
    for (unsigned i = 0; i < 96; i++) {
        TmFix *f = &tm_fixes[i];
        if (!f->on) continue;
        if (ppu_nt[f->b][f->r * 32 + f->c] != f->tile) f->on = 0;
        else if (tm_pal(f->b, f->r, f->c) != f->pal) tm_setpal(f->b, f->r, f->c, f->pal);
    }
}
static void tm_put(unsigned b, unsigned off, unsigned v)
{
    unsigned old = ppu_nt[b][off];
    if (old != v) { ppu_nt[b][off] = (uint8_t)v; vid_nt_changed(b, off, old, v); }
}

typedef struct { const char *o, *n; uint8_t oe[24], ne[24], on, nn; } NameRule;
static NameRule nrules[] = {
    { "PUSH START", "PRESS ENTER" },
    { "GLASS JOE", "GURU JOE" }, { "VON KAISER", "VBLANK KAISER" }, { "PISTON HONDA", "PISTON HAYNIE" }, { "DON FLAMENCO", "DON FLAMIGOS" },
    { "KING HIPPO", "FAT AGNUS" }, { "GREAT TIGER", "GAYLE TIGER" }, { "BALD BULL", "BALD BILL HERD" }, { "SODA POPINSKI", "SODA PAULASKI" },
    { "MR.SANDMAN", "MR. BLITTERMAN" }, { "MR. SANDMAN", "MR. BLITTERMAN" }, { "SUPER MACHO MAN", "SUPER ST MAN" }, { "LITTLE MAC", "LITTLE MITCHY" },
    { "DOC LOUIS", "DOCTOR JAY" }, { "PARIS,", "WEST CHESTER," }, { "FRANCE", "PENNSYLVANIA" },
    { "HONDA", "HAYNIE" }, { "FLAMENCO", "FLAMIGOS" }, { "HIPPO", "AGNUS" }, { "BULL", "BILL HERD" }, { "POPINSKI", "PAULASKI" }, { "SANDMAN", "BLITTERMAN" },
    { "MACHO MAN", "ST MAN" }, { "MACHO", "ST" }, { "GLASS", "GURU" }, { "LOUIS", "JAY" }, { "MAC", "MITCHY" },
};
#define NNR ((unsigned)(sizeof nrules / sizeof nrules[0]))
static unsigned tm_enc(const char *s, uint8_t *o)
{
    unsigned n = 0;
    for (; *s; s++) o[n++] = (uint8_t)(*s == ' ' ? 0xFF : *s == '.' ? 0x25 : *s == ',' ? 0x2B : 0x0B + (*s - 'A'));
    return n;
}
static inline int tm_isletter(unsigned t) { return t >= 0x0B && t <= 0x24; }
static inline int tm_isblank(unsigned t) { return t == 0xFF || t == 0xFD; }
static unsigned tm_run(const uint8_t *row, int from, int step)       /* blank tiles in a row from `from` going in direction `step` */
{
    unsigned n = 0;
    for (int x = from; x >= 0 && x < 32 && tm_isblank(row[x]); x += step) n++;
    return n;
}
static int tm_find(const uint8_t *row, const uint8_t *w, unsigned n)
{
    for (unsigned x = 0; x + n <= 32; x++) { unsigned k = 0; while (k < n && row[x + k] == w[k]) k++; if (k == n) return (int)x; }
    return -1;
}

/* replacements in one row buffer; mc/mp/mt list the tiles that were written (column, original palette, tile) */
static void tm_names(uint8_t *row, unsigned b, unsigned r, uint8_t *mc, uint8_t *mp, uint8_t *mt, unsigned *nm)
{
    static int inited;
    if (!inited) { for (unsigned i = 0; i < NNR; i++) { nrules[i].on = (uint8_t)tm_enc(nrules[i].o, nrules[i].oe); nrules[i].nn = (uint8_t)tm_enc(nrules[i].n, nrules[i].ne); } inited = 1; }
    for (unsigned x = 0; x < 32; x++) {
        if (!tm_isletter(row[x]) || (x > 0 && tm_isletter(row[x - 1]))) continue;                /* only at the start of a word */
        for (unsigned i = 0; i < NNR; i++) {
            const NameRule *q = &nrules[i];
            unsigned n = q->on, m = q->nn, k = 1;
            if (row[x] != q->oe[0] || x + n > 32) continue;
            while (k < n && row[x + k] == q->oe[k]) k++;
            if (k < n || (x + n < 32 && tm_isletter(row[x + n]))) continue;
            unsigned pal = tm_pal(b, r, x);
            if (m <= n) {                                                                       /* same size or shorter: in place, blank the rest */
                for (unsigned j = 0; j < m; j++) { row[x + j] = q->ne[j]; mc[*nm] = (uint8_t)(x + j); mp[*nm] = (uint8_t)pal; mt[*nm] = q->ne[j]; (*nm)++; }
                for (unsigned j = m; j < n; j++) row[x + j] = 0xFF;
                x += n - 1;
                break;
            }
            unsigned ext = m - n, rr = tm_run(row, (int)(x + n), 1), rl = x ? tm_run(row, (int)x - 1, -1) : 0;
            unsigned ar = rr ? rr - 1 : 0, al = rl ? rl - 1 : 0;                                  /* leave one blank next to the text ... */
            if (x + n + rr == 32) { unsigned f = 0; while (f < rr && row[x + n + f] == 0xFF) f++; if (f == rr) ar = rr; }   /* ... except at the right edge of a row of plain blanks */
            unsigned er = ext < ar ? ext : ar, el = ext - er;
            if (el <= al) {                                                                       /* grow into the blanks next to the word */
                unsigned s = x - el;
                for (unsigned j = 0; j < m; j++) { row[s + j] = q->ne[j]; mc[*nm] = (uint8_t)(s + j); mp[*nm] = (uint8_t)pal; mt[*nm] = q->ne[j]; (*nm)++; }
                x = s + m - 1;
                break;
            }
            {   /* no room beside the word: the line is one centred text; re-centre the whole line around the longer word */
                unsigned L = 0, R = 31;
                while (L < 32 && tm_isblank(row[L])) L++;
                while (R > 0 && tm_isblank(row[R])) R--;
                int ok = L <= x && x + n - 1 <= R;
                for (unsigned c = L; ok && c + 1 <= R; c++) if (tm_isblank(row[c]) && tm_isblank(row[c + 1])) ok = 0;      /* a second column of text: leave alone */
                unsigned eL = ext / 2, eR = ext - eL;
                if (ok && L >= eL + 1 && 31 - R >= eR + 1) {
                    uint8_t seg[40]; unsigned sl = 0;
                    for (unsigned c = L; c < x; c++) seg[sl++] = row[c];
                    for (unsigned j = 0; j < m; j++) seg[sl++] = q->ne[j];
                    for (unsigned c = x + n; c <= R; c++) seg[sl++] = row[c];
                    for (unsigned j = 0; j < sl; j++) { row[L - eL + j] = seg[j]; mc[*nm] = (uint8_t)(L - eL + j); mp[*nm] = (uint8_t)pal; mt[*nm] = seg[j]; (*nm)++; }
                    x = L - eL + sl - 1;
                    break;
                }
            }
            break;                                                                              /* no room at all: leave the original */
        }
    }
}

static void tm_pair(unsigned b, unsigned rp, unsigned rs)        /* "PUSH" on row rp and "START" on row rs (two lines of the same sign) */
{
    static const uint8_t push[4] = { 0x1A, 0x1F, 0x1D, 0x12 }, press[5] = { 0x1A, 0x1C, 0x0F, 0x1D, 0x1D };
    static const uint8_t start[5] = { 0x1D, 0x1E, 0x0B, 0x1C, 0x1E }, enter[5] = { 0x0F, 0x18, 0x1E, 0x0F, 0x1C };
    uint8_t *pr = &tm_raw[b][rp * 32], *sr = &tm_raw[b][rs * 32];
    int xp = tm_find(pr, push, 4), xs = tm_find(sr, start, 5);
    if (xp < 0 || xs < 0) return;
    unsigned s;
    if (xp > 0 && pr[xp - 1] == 0xFF) s = (unsigned)xp - 1;
    else if (xp + 4 < 32 && pr[xp + 4] == 0xFF) s = (unsigned)xp;
    else return;
    { unsigned pal = tm_pal(b, rp, (unsigned)xp); for (unsigned i = 0; i < 5; i++) { tm_put(b, rp * 32 + s + i, press[i]); tm_fix(b, rp, s + i, pal, press[i]); } }
    { unsigned pal = tm_pal(b, rs, (unsigned)xs); for (unsigned i = 0; i < 5; i++) { tm_put(b, rs * 32 + (unsigned)xs + i, enter[i]); tm_fix(b, rs, (unsigned)xs + i, pal, enter[i]); } }
}
static void tm_row(unsigned b, unsigned r)
{
    static const uint8_t push[4] = { 0x1A, 0x1F, 0x1D, 0x12 }, start[5] = { 0x1D, 0x1E, 0x0B, 0x1C, 0x1E };
    uint8_t tmp[32], mc[64], mp[64], mt[64];
    unsigned nm = 0;
    for (unsigned c = 0; c < 32; c++) tmp[c] = tm_raw[b][r * 32 + c];
    tm_names(tmp, b, r, mc, mp, mt, &nm);
    for (unsigned c = 0; c < 32; c++) tm_put(b, r * 32 + c, tmp[c]);
    for (unsigned i = 0; i < nm; i++) tm_fix(b, r, mc[i], mp[i], mt[i]);
    const uint8_t *row = &tm_raw[b][r * 32];
    if (tm_find(row, push, 4) >= 0) { for (unsigned rr = r + 1; rr <= r + 2 && rr < 30; rr++) tm_pair(b, r, rr); }
    if (tm_find(row, start, 5) >= 0) { for (unsigned rr = (r >= 2 ? r - 2 : 0); rr < r; rr++) tm_pair(b, rr, r); }
}
static void tm_scan(void)
{
    tm_refix();
    for (unsigned b = 0; b < 2; b++) {
        uint32_t m = tm_dirty[b];
        tm_dirty[b] = 0;
        while (m) { unsigned r = (unsigned)__builtin_ctz(m); m &= m - 1; if (r < 30) tm_row(b, r); }
    }
}

static inline void data_wr(uint32_t d)
{
    unsigned a = v & 0x3FFF;
    bus = (uint8_t)d;
    if (a >= 0x3F00) { unsigned i = a & 0x1F; if ((i & 0x13) == 0x10) i &= ~0x10u; ppu_pal[i] = (uint8_t)(d & 0x3F); }
    else if (a >= 0x2000) { unsigned b; uint8_t *p = nt_ptr(a, &b); unsigned off = a & 0x3FF;
        if (off < 0x3C0) { if (tm_raw[b][off] == (uint8_t)d) goto same; tm_raw[b][off] = (uint8_t)d; tm_dirty[b] |= 1u << (off >> 5); }       /* the row is rebuilt from tm_raw at the next vblank */
        if (*p != (uint8_t)d) { unsigned old = *p; *p = (uint8_t)d; vid_nt_changed(b, off, old, d); if (spr0_ok && line <= spr0_last) spr0_inval(); }
        same:; }
    v = (uint16_t)(v + ((ppu_ctrl & 4) ? 32 : 1));
}

void ppu_reg_wr(uint32_t r, uint32_t d, uint32_t now, int32_t a6)
{
    (void)a6;
    ppu_run(now);
    bus = (uint8_t)d;
    switch (r) {
    case 0:
        st_dirty = 2;
        if ((ppu_ctrl ^ d) & 0x38) spr0_inval();
        ppu_ctrl = (uint8_t)d; t = (uint16_t)((t & ~0x0C00) | ((d & 3) << 10)); break;
    case 1: if ((ppu_mask ^ d) & 0x1E) spr0_inval(); ppu_mask = (uint8_t)d; st_dirty = 2; break;
    case 3: oamaddr = (uint8_t)d; break;
    case 4: if (ppu_oam[oamaddr] != (uint8_t)d) { ppu_oam[oamaddr] = (uint8_t)d; ppu_oam_dirty = 1; ppu_oam_gen++; if (oamaddr < 4) spr0_inval(); } oamaddr++; break;
    case 5:
        if (!wlatch) { t = (uint16_t)((t & ~0x1F) | (d >> 3)); fine_x = (uint8_t)(d & 7); }
        else t = (uint16_t)((t & ~0x73E0) | ((d & 7) << 12) | ((d >> 3) << 5));
        st_dirty = 2; wlatch ^= 1; break;
    case 6:
        if (!wlatch) t = (uint16_t)((t & 0x00FF) | ((d & 0x3F) << 8));
        else { t = (uint16_t)((t & 0xFF00) | d); v = t; v_written = 1; st_dirty = 2; }
        wlatch ^= 1; break;
    case 7: data_wr(d); break;
    }
}

/* $2007 write.  During vblank no scanline is pending, so the timed path (ppu_run) is skipped. */
void ppu_data_wr(uint32_t d, int32_t a6)
{
    if (!ppu_vbl) { uint32_t now = nes_now16(a6); ppu_reg_wr(7, d, now, a6); return; }
    data_wr(d);
}

void ppu_oam_dma(const uint8_t *src)
{
    const uint32_t *s = (const uint32_t *)src;
    if (oamaddr == 0 && !((uint32_t)src & 3)) {
        uint32_t *o = (uint32_t *)ppu_oam, diff = 0, diff0 = s[0] ^ o[0];
        for (unsigned i = 0; i < 64; i++) { uint32_t w = s[i]; diff |= w ^ o[i]; o[i] = w; }
        if (diff) { ppu_oam_dirty = 1; ppu_oam_gen++; if (diff0) spr0_inval(); }
        return;
    }
    for (unsigned i = 0; i < 256; i++) { uint8_t *o = &ppu_oam[(uint8_t)(oamaddr + i)]; if (*o != src[i]) { *o = src[i]; ppu_oam_dirty = 1; ppu_oam_gen++; } }
    spr0_inval();
}

void ppu_frame_start(void) { line = 0; nreg = 0; region_open = 0; v_written = 0; spr0_ok = 0; }

void ppu_prerender(void)
{
    ppu_vbl = 0; ppu_ovf = 0; spr0_latched = 0; spr0_ok = 0;
    if (rendering()) v = (uint16_t)((v & ~0x7BE0) | (t & 0x7BE0));
}

extern void nes_frame_end(void);
extern uint32_t vid_cnt[8], nes_frames;
/* Pacing and frame skipping.  The game clock is the 50.007 Hz CIA tick.  `due` is the tick by which the current frame should be finished; when the machine is
 * already behind schedule at vblank the picture of this frame is not built (the nametable/CHR state keeps accumulating, the next picture that is built shows
 * the current state), so game logic and sound keep their speed and only the display rate drops; at most MAX_SKIP pictures in a row. */
#ifndef MAX_SKIP
#define MAX_SKIP 2
#endif
void ppu_vblank(void)
{
    static uint32_t due; static unsigned skipped;
    ppu_run((uint32_t)240 * T_LINE + 10);
    while (line < 240) { region_sync(line); line_end_update(); line++; }
    region_sync(240);
    close_region(240);
    tm_scan();
    if (!due) due = plat_tick_count + 1;
    int32_t lag = (int32_t)(plat_tick_count - due);                           /* ticks behind (>0) or ahead (<=0) of schedule */
#ifdef FORCE_SKIP
    if (nes_frames > FORCE_SKIP) { vid_cnt[5]++; } else
#endif
    if (lag > 0 && skipped < MAX_SKIP) { skipped++; vid_cnt[5]++; }
    else { skipped = 0; vid_present(regs, nreg); }
    ppu_vbl = 1;
    nes_frame_end();
    due++;
    if ((int32_t)(plat_tick_count - due) > 4) due = plat_tick_count;          /* hopelessly late: give up the schedule */
    while ((int32_t)(plat_tick_count - due) < 0) { }                          /* ahead of schedule: wait for the tick */
}
