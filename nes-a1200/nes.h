/* nes.h - NES hardware model on the Amiga 1200: shared declarations */
#ifndef NES_H
#define NES_H
#include <stdint.h>

/* CPU context handed to C by the asm glue (nes_rt.s): 16 bytes */
typedef struct { uint8_t a, x, y, s, c, v, pad0, pad1; uint16_t ccr; uint16_t pc; int32_t a6; } Cpu;

extern uint8_t nes_ram[2048];
extern const uint8_t nes_prg[131072], nes_chr[131072];
extern uint32_t nes_win_base, nes_win_tab, nes_a4, nes_fix_tab_end, nes_a6_init;
extern int32_t nes_stall;
extern uint8_t nes_prg_bank;
extern uint8_t nes_chr_reg[4];
extern uint32_t nes_stat[8];
extern uint32_t nes_tprof[8];
#ifdef TPROF               /* per-section timers in scanlines (-DTPROF): they cost real time themselves (a CIA TOD read), so speed figures are taken without */
uint32_t plat_scanlines(void);
#define TPS(v) uint32_t v = plat_scanlines()
#define TPS2(v) v = plat_scanlines()
#define TPE(v, i) (nes_tprof[i] += plat_scanlines() - (v))
#else
#define TPS(v) (void)0
#define TPS2(v) (void)0
#define TPE(v, i) (void)0
#endif           /* scanlines (64 us) spent: 0 ppu_run, 1 render_line, 2 present, 3 render_bg */            /* debug counters: 0 events 1 interp instr 2 io reads 3 bg renders 4 composites 5 rows converted 6 interp entries */
extern uint8_t nes_mirror_h;                      /* 1 = horizontal mirroring (NT0=NT1, NT2=NT3) */

/* timing: one unit = 1/16 CPU cycle = 1/5 PPU dot.  PAL: 341 dots = 1705 units per scanline, 312 lines per frame */
#define T_LINE   1705
#define T_FRAME  (312 * T_LINE)
#define T_VBL    (241 * T_LINE + 5)
#define T_PRE    (311 * T_LINE + 5)

uint32_t nes_rd8(uint32_t addr, int32_t a6);
void     nes_wr8(uint32_t addr, uint32_t val, int32_t a6);
uint32_t nes_io_rd(uint32_t addr, int32_t a6);
void     nes_io_wr(uint32_t addr, uint32_t val, int32_t a6);
void     nes_map_wr(uint32_t addr, uint32_t val, int32_t a6);
int      nes_event(Cpu *c);
int      nes_interp(Cpu *c);
void     nes_init(void);
void     nes_run(uint32_t pc) __attribute__((noreturn));
int      nes_has_entry(uint32_t pc);
uint32_t nes_now16(int32_t a6);

/* ppu */
void     ppu_init(void);
void     ppu_run(uint32_t now);              /* render all scanlines that started before `now` (frame-relative time) */
void     ppu_frame_start(void);
uint32_t ppu_reg_rd(uint32_t reg, uint32_t now);
void     ppu_reg_wr(uint32_t reg, uint32_t val, uint32_t now, int32_t a6);
void     ppu_oam_dma(const uint8_t *src256);
void     ppu_vblank(void);                   /* sets the vblank flag, presents the frame */
void     ppu_prerender(void);
extern uint8_t ppu_ctrl;
extern uint8_t ppu_vbl;
void     ppu_spr0_inval(void);
void     ppu_data_wr(uint32_t d, int32_t a6);

/* apu / input */
void     apu_wr(uint32_t reg, uint32_t val, uint32_t now);
uint32_t apu_rd(uint32_t reg);
void     apu_frame(void);
uint8_t  pad_read(void);
void     pad_write(uint32_t v);
#endif
