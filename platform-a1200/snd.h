/*
 * snd.h - Zoo Keeper sound module for the Amiga 500 (OCS Paula, bare metal, no OS, no interrupts needed).
 *
 * What it does
 *   Reproduces the arcade's sound CPU protocol: the game calls snd_command(code, volbyte) with exactly the two bytes
 *   the arcade data CPU wrote to the sound PIA (command byte to port A, volume byte to port B; see sound/README.md).
 *   Like the 6802 it (a) starts the new sound, REPLACING whatever was playing (a command always restarts), (b) treats
 *   every unassigned code (0, 1-7, 0x80-0xDF ...) as "stop" -- the 6802 abandons the current sound and idles.
 *   Left/right level = high/low nibble of volbyte (arcade attenuator network, see sounds.h: snd_vol_tbl[]).
 *   Stereo on the A500: left = Paula channels 0+3, right = channels 1+2.
 *
 * Data: link out/sounds.bin (symbol snd_blob, via out/sounds_blob.s) into CHIP RAM; tables are in out/sounds.h.
 *
 * Time base: sounds of kind "SEQ" (the arcade's two-sine-oscillator tunes, commands 08-0E,28,2A-2D,2F,31-33) are note scripts
 * that the module advances from snd_tick().  snd_tick() reads CIA-A timer B (a free-running 16-bit E-clock counter that
 * snd_init() starts), so it needs no fixed call rate: call it from the game loop / vblank at 50-60 Hz or faster;
 * note boundaries are quantised to the call instants (60 Hz => notes start up to 16.7 ms late, never accumulating).
 * PCM sounds need no tick at all.  If snd_tick() is not called, SEQ sounds simply stay on their first note.
 *
 * Hardware touched: AUDx registers, DMACON (audio bits + DMAEN), INTENA/INTREQ (audio bits only, to disable/clear),
 * CIA-A timer B (CRB, TBLO/TBHI).  Everything else is left alone.  Not re-entrant: do not call from an interrupt that
 * can pre-empt the game's own snd_* call.
 */
#ifndef SND_H
#define SND_H
#include <stdint.h>

void snd_init(void);                         /* once, after reset.  Silences audio, starts the CIA timer, PAL assumed */
void snd_set_ntsc(int ntsc);                 /* 0 = PAL machine (default), 1 = NTSC: selects period table + CIA clock */
void snd_command(unsigned code, unsigned vol);/* start sound `code` (0..255) with arcade volume byte `vol`; replaces the current sound */
void snd_volume(unsigned vol);               /* arcade port-B volume write alone: re-levels the sound that is playing */
void snd_stop(void);                         /* silence now */
void snd_tick(void);                         /* advance SEQ note scripts (see above); cheap when nothing to do */
int  snd_active(void);                       /* 1 while a sound is (still) playing; a looping sound stays active until replaced */

#endif
