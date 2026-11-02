/* nes_vid.h - native Amiga video back end: persistent planar nametable bitmaps scrolled by the display hardware, hardware sprites. */
#ifndef NES_VID_H
#define NES_VID_H
#include <stdint.h>
/* bitplane fetch: 32 bit (10 units of 32 pixels = 40 bytes per line and plane, 32 pixels of scroll slack) unless BPL16 (17 words = 34 bytes, 16 pixels slack) */
#ifdef BPL16
#define VID_FW 34
#define VID_FSLACK 16
#else
#define VID_FW 40
#define VID_FSLACK 32
#endif

/* one stretch of scanlines displayed under constant PPU state (the PPU model closes a region whenever a register write changes the state) */
typedef struct { uint16_t y0, y1, v0; uint8_t fx, mask, mir, pad; } Region;

extern uint8_t  ppu_ctrl, ppu_mask, ppu_vbl, ppu_ovf;
extern uint8_t  ppu_nt[2][1024];
extern uint8_t  ppu_pal[32];
extern uint8_t  ppu_oam[256];
extern uint8_t  ppu_oam_dirty;

void     vid_init(void);
void     vid_nt_changed(unsigned bank, unsigned off, unsigned oldv, unsigned newv);          /* a nametable or attribute byte changed */
void     vid_chr_changed(void);                                /* a CHR bank register changed */
void     vid_ctrl_changed(void);                               /* PPUCTRL bit 4 / 5 changed */
void     vid_present(const Region *r, int n);                  /* end of frame: update bitmaps, build the Copper list and the sprite streams */
uint32_t vid_spr0_hit(const Region *r, int n, int open_y0, unsigned open_v, unsigned open_fx, unsigned open_mir, unsigned open_ctrl);   /* hit time in 1/16 cycle, or 0xFFFFFFFE = never */
void     vid_debug_hex(unsigned x, unsigned y, uint32_t val, unsigned nd);
extern uint32_t vid_stat[8], vid_cnt[8];
#endif
