/*
 * display.h - Zoo Keeper Amiga 1200 display module: chunky VRAM -> 8 interleaved bitplanes (AGA, 68020)
 * =====================================================================================================
 * The arcade video RAM stays chunky and authoritative (VHW.vram / VRAM: one palette index per pixel, byte = y*256+x; rows 4..243 are displayed as screen lines 0..239: rows 0..3 hold a colour-bar pattern outside the arcade's visible picture).
 * AGA has 8 bitplanes and 256 colour registers, so no index->colour mapping is needed: the plane bits ARE the arcade palette index and the
 * 256 colour registers ARE the displayed palette bank (VHW.palram[bank*256 + i], bank = VHW.palbank & 3), 8 bits per channel.  The image on
 * the Amiga screen is therefore exactly "VRAM through the palette RAM" (the A500 module needs a dynamic 32-colour map for this).
 *
 *   display_init()      after vhw_init()/plat_init().  Builds the colour decode table; marks everything dirty if VRAM is not all zero.
 *   display_frame()     once per game tick, between CPU slices: loads the palette when palette RAM or the bank changed
 *                       (plat_set_palette256, applied by the vertical-blank interrupt), then converts EVERY dirty 8-pixel group into
 *                       the planes (no budget: the work is proportional to what the game changed).  Afterwards display_backlog() == 0.
 *   display_backlog()   always 0 after display_frame().    display_invalidate()  mark the whole screen dirty.
 *
 * Dirty protocol (rt/hw_video.*): every VRAM write sets VHW.dirty[off>>3] and VHW.dirty_row[off>>8]; this module clears the group bytes it
 * converts and the row byte of every row it has converted; rows 0..3 and >= 244 are not displayed (their row bytes are cleared, group bytes kept).
 * Plane layout: interleaved, line y of plane p at plat_front() + y*256 + p*32, MSB = leftmost pixel (BPLxMOD = 224).
 */
#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdint.h>
void     display_init(void);
void     display_frame(void);
uint32_t display_backlog(void);
void     display_invalidate(void);
/* unit-test hook: convert one 8-pixel group (src = 8 chunky bytes, dst = plane 0 byte of that group in the line; plane p at dst + 32*p) */
void     display_c2p_group(const uint8_t *src, uint8_t *dst);
#endif
