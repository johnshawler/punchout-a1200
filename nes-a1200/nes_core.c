/* nes_core.c - memory map, MMC2 (PxROM) mapper, scheduler / events, I/O dispatch.
 * Memory map and mapper behaviour follow MAME nes.cpp / bus/nes/mmc2.cpp (see FACTS).  Time unit: 1/16 CPU cycle (PAL: 1 PPU dot = 5 units). */
#include "nes.h"
#include "platform.h"

extern char rt_undef[];
extern const uint32_t fix_count, win_count;
extern const uint16_t fix_addr[], win_addr[];
extern const uint8_t win_bank[];
extern const uint32_t fix_ptr[], win_ptr[];

uint8_t  nes_ram[2048] __attribute__((aligned(4)));
static uint32_t fixtab[24576];
static uint32_t wintab_pool[9][8192];     /* tables for the banks that contain translated code (8 at most); the rest share the all-undefined table */
static uint32_t *wintab[16];
uint32_t nes_win_base, nes_win_tab, nes_a4, nes_fix_tab_end, nes_a6_init;
int32_t  nes_stall;
uint8_t  nes_prg_bank;
uint8_t  nes_chr_reg[4];
uint8_t  nes_mirror_h = 1;

/* scheduler state */
static uint32_t ev_t16;          /* frame-relative time at the last sync, 1/16 cycle */
static int32_t  ev_limit;        /* cycles until the next event as set at the last sync (a6 = -ev_limit then) */
static uint8_t  vbl_done, pre_done;
static uint8_t  nmi_req;
uint32_t nes_frames;
uint32_t nes_stat[8];
uint32_t nes_tprof[8];
uint32_t nes_dbg[8];

static void set_bank(unsigned b)
{
    nes_prg_bank = (uint8_t)(b & 15);
    nes_win_base = (uint32_t)nes_prg + nes_prg_bank * 0x2000u - 0x8000u;
    nes_win_tab  = (uint32_t)wintab[nes_prg_bank] + 131072u;
}

int nes_has_entry(uint32_t pc)
{
    if (pc >= 0xA000) return (char *)fixtab[pc - 0xA000] != rt_undef;
    if (pc >= 0x8000) return (char *)wintab[nes_prg_bank][pc - 0x8000] != rt_undef;
    return 0;
}

void nes_init(void)
{
    for (unsigned i = 0; i < 24576; i++) fixtab[i] = (uint32_t)rt_undef;
    for (unsigned s = 0; s < 9; s++) for (unsigned i = 0; i < 8192; i++) wintab_pool[s][i] = (uint32_t)rt_undef;
    { unsigned used = 0, has[16] = {0};
      for (uint32_t i = 0; i < win_count; i++) has[win_bank[i] & 15] = 1;
      for (unsigned b = 0; b < 16; b++) wintab[b] = wintab_pool[8];
      for (unsigned b = 0; b < 16; b++) if (has[b] && used < 8) wintab[b] = wintab_pool[used++]; }
    for (uint32_t i = 0; i < fix_count; i++) fixtab[fix_addr[i] - 0xA000] = fix_ptr[i];
    for (uint32_t i = 0; i < win_count; i++) wintab[win_bank[i] & 15][win_addr[i] - 0x8000] = win_ptr[i];
    nes_a4 = (uint32_t)nes_prg + 0x20000u - 0x6000u - 0xA000u;
    nes_fix_tab_end = (uint32_t)&fixtab[24576];
    set_bank(0);
    ppu_init();
    ev_t16 = 0; vbl_done = pre_done = 0;
    ev_limit = (int32_t)(T_VBL / 16 + 1);
    nes_a6_init = (uint32_t)(-ev_limit);
}

uint32_t nes_now16(int32_t a6) { return ev_t16 + (uint32_t)(a6 + ev_limit) * 16u; }

/* ------------------------------------------------------------------ memory */
uint32_t nes_rd8(uint32_t a, int32_t a6)
{
    a &= 0xFFFF;
    if (a < 0x2000) return nes_ram[a & 0x7FF];
    if (a < 0x4020) return nes_io_rd(a, a6);
    if (a < 0x8000) return 0;
    if (a < 0xA000) return nes_prg[nes_prg_bank * 0x2000u + a - 0x8000u];
    return nes_prg[0x20000u - 0x6000u + a - 0xA000u];
}

void nes_wr8(uint32_t a, uint32_t v, int32_t a6)
{
    a &= 0xFFFF; v &= 0xFF;
    if (a < 0x2000) nes_ram[a & 0x7FF] = (uint8_t)v;
    else if (a < 0x4020) nes_io_wr(a, v, a6);
    else if (a >= 0x8000) nes_map_wr(a, v, a6);
}

/* MMC2: $A000 PRG bank (8 KB at $8000), $B000/$C000/$D000/$E000 CHR registers, $F000 mirroring (MAME mmc2.cpp pxrom_write) */
uint32_t nes_wlog[8], nes_wlog_n;
void nes_map_wr(uint32_t a, uint32_t v, int32_t a6)
{
    ppu_run(nes_now16(a6));
    if ((a & 0xF000) >= 0xB000) { nes_wlog[nes_wlog_n & 7] = (nes_now16(a6) / T_LINE) << 16 | ((a >> 12) & 15) << 8 | (v & 0xFF); nes_wlog_n++; }
    switch (a & 0xF000) {
    case 0xA000: set_bank(v); break;
    case 0xB000: if (nes_chr_reg[0] != (v & 31)) { nes_chr_reg[0] = (uint8_t)(v & 31); ppu_spr0_inval(); } break;
    case 0xC000: if (nes_chr_reg[1] != (v & 31)) { nes_chr_reg[1] = (uint8_t)(v & 31); ppu_spr0_inval(); } break;
    case 0xD000: if (nes_chr_reg[2] != (v & 31)) { nes_chr_reg[2] = (uint8_t)(v & 31); ppu_spr0_inval(); } break;
    case 0xE000: if (nes_chr_reg[3] != (v & 31)) { nes_chr_reg[3] = (uint8_t)(v & 31); ppu_spr0_inval(); } break;
    case 0xF000: if (nes_mirror_h != (v & 1)) { nes_mirror_h = (uint8_t)(v & 1); ppu_spr0_inval(); } break;
    }
}

/* ---------------------------------------------------------------------- I/O */
uint32_t nes_io_rd(uint32_t a, int32_t a6)
{
    nes_stat[2]++;
    uint32_t now = nes_now16(a6);
    if (a < 0x4000) return ppu_reg_rd(a & 7, now);
    if (a == 0x4016) return pad_read();
    if (a == 0x4017) return 0x40;
    return apu_rd(a & 0x1F);
}

void nes_io_wr(uint32_t a, uint32_t v, int32_t a6)
{
    uint32_t now = nes_now16(a6);
    if (a < 0x4000) {
        uint8_t old = ppu_ctrl;
        ppu_reg_wr(a & 7, v, now, a6);
        if ((a & 7) == 0 && !(old & 0x80) && (v & 0x80) && ppu_vbl) { nmi_req = 1; goto force; }
        return;
    }
    if (a == 0x4014) {
        uint32_t base = (v & 0xFF) << 8;
        ppu_run(now);
        if (base < 0x2000) ppu_oam_dma(&nes_ram[base & 0x7FF]);
        else { uint8_t page[256] __attribute__((aligned(4))); for (unsigned i = 0; i < 256; i++) page[i] = (uint8_t)nes_rd8(base + i, a6); ppu_oam_dma(page); }
        nes_stall += 513;
        return;
    }
    if (a == 0x4016) { pad_write(v); return; }
    apu_wr(a & 0x1F, v, now);
    return;
force:  /* make the next event check fire right away: a6' = 1, same elapsed time */
    { int32_t st = 1 - a6; nes_stall += st; ev_limit -= st; }
}

/* ---------------------------------------------------------------- scheduler */
static uint32_t next_event_time(uint32_t now)
{
    uint32_t t = T_FRAME;
    if (!vbl_done && T_VBL < t) t = T_VBL;
    if (!pre_done && T_PRE < t) t = T_PRE;
    (void)now;
    return t;
}

/* A pure wait loop is about to spin again.  Nothing the loop can observe changes before `target`, so virtual time jumps there:
 * a6 := 1 (the next event check fires) and the elapsed time since the last sync becomes target - ev_t16. */
extern uint32_t ppu_spr0_time(void);
void nes_warp(uint32_t kind, int32_t a6)
{
    nes_stat[6] += 1; if (kind) nes_stat[4] += 0;
    uint32_t now = nes_now16(a6), target = next_event_time(now);
    if (kind) {
        uint32_t s0 = ppu_spr0_time();
        if (s0 != 0xFFFFFFFFu && s0 > now && s0 < target) target = s0;
        else if (s0 == 0xFFFFFFFFu && now < 240u * T_LINE) {              /* hit time not known yet: step one scanline */
            uint32_t nl = (now / T_LINE + 1) * T_LINE + 8;
            if (nl < target) target = nl;
        }
    }
    if (target <= now) target = now + 16;
    int32_t el = (int32_t)((target - ev_t16 + 15) / 16);
    nes_stall += 1 - a6;
    ev_limit = el - 1;
}

/* Called from the translated code when its cycle counter passes zero (and from the interpreter).  Returns 1 if c->pc was changed
 * (NMI), the asm then continues there.  Sets c->a6 for the next event. */
static int ev_seen;
int nes_event(Cpu *c)
{
    nes_stat[0]++;
    uint32_t now = ev_t16 + (uint32_t)(c->a6 + ev_limit) * 16u;
    int jump = 0;
    ppu_run(now);
    if (!vbl_done && now >= T_VBL) { vbl_done = 1; ppu_vblank(); nes_frames++; if (ppu_ctrl & 0x80) nmi_req = 1; }
    if (!pre_done && now >= T_PRE) { pre_done = 1; ppu_prerender(); }
    if (now >= T_FRAME) { now -= T_FRAME; vbl_done = pre_done = 0; ppu_frame_start(); apu_frame(); }
    if (nmi_req) {
        nmi_req = 0;
        uint8_t p = (uint8_t)(0x20 | ((c->ccr & 8) ? 0x80 : 0) | ((c->ccr & 4) ? 2 : 0) | (c->v ? 0x40 : 0) | (c->c ? 1 : 0));
        nes_ram[0x100 + c->s] = (uint8_t)(c->pc >> 8); c->s--;
        nes_ram[0x100 + c->s] = (uint8_t)c->pc;        c->s--;
        nes_ram[0x100 + c->s] = p;                     c->s--;
        c->pc = (uint16_t)(nes_prg[0x1FFFA] | nes_prg[0x1FFFB] << 8);
        c->ccr = 0;
        jump = 1;
    }
    ev_t16 = now;
    uint32_t nxt = next_event_time(now);
    if (nxt <= now) nxt = now + 16;
    ev_limit = (int32_t)((nxt - now + 15) / 16);
    c->a6 = -ev_limit;
    nes_dbg[0] = now; nes_dbg[1] = (uint32_t)ev_limit; nes_dbg[2] = vbl_done | pre_done << 1 | nmi_req << 2 | ppu_vbl << 3; nes_dbg[3] = nes_frames; nes_dbg[4] = c->pc; nes_dbg[5] = nes_stat[0];
    return jump;
}
