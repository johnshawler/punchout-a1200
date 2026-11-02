/* sndtest.c - A1200 sound test: plays every PCM-class sound of sound/out_full once, full length, each preceded by a 14 kHz marker burst, then
 * a few commands that must replace / stop a sound.  The audio output is recorded by FS-UAE (OpenAL wave backend) and compared with the
 * data by sound/tools/verify_full_wav.py.  Nothing is drawn; the platform layer (crt0/plat_init) only provides the timing. */
#include <stdint.h>
#include "platform.h"
#include "snd.h"
#include "sounds.h"
#define R16(o)  (*(volatile uint16_t *)(0xDFF000u + (o)))
#define R32(o)  (*(volatile uint32_t *)(0xDFF000u + (o)))
static const int8_t marker_wave[32] __attribute__((aligned(4))) = {
    127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127,127,-127 };
static void wait_ticks(unsigned n) { uint32_t end = plat_tick_count + n; while ((int32_t)(plat_tick_count - end) < 0) snd_tick(); }
static void marker(void)
{
    unsigned c;
    snd_stop();
    for (c = 0; c < 2; c++) { R32(0xA0 + (c << 4)) = (uint32_t)marker_wave; R16(0xA4 + (c << 4)) = 16; R16(0xA6 + (c << 4)) = 124; R16(0xA8 + (c << 4)) = 64; }
    R16(0x096) = 0x8000 | 0x0200 | 0x0003;
    wait_ticks(2);
    R16(0x096) = 0x0003; R16(0xA8) = 0; R16(0xB8) = 0;
    wait_ticks(10);
}
int main(void)
{
    unsigned i, code;
    snd_init(); snd_set_ntsc(!plat_is_pal);
    wait_ticks(60);
    /* phase 1: every PCM sound once (first command code that maps to each definition), volume byte 0 */
    for (i = 1; i < SND_NDEFS; i++) {
        const snd_def_t *d = &snd_defs[i];
        if (d->kind != SND_KIND_PCM) continue;
        for (code = 0; code < 256; code++) if (snd_cmd_def[code] == i) break;
        marker();
        snd_command(code, 0x00);
        wait_ticks((d->rlen * 60u) / 28604u + 20u);
        snd_stop();
        wait_ticks(20);
    }
    /* phase 2: replace / stop: a long PCM sound (E2) cut by F1 after 1 s, F1 cut by "stop" (0x00) after 10 ticks */
    marker(); snd_command(0xE2, 0x00); wait_ticks(60); snd_command(0xF1, 0x00); wait_ticks(10); snd_command(0x00, 0); wait_ticks(60);
    marker();
    for (;;) wait_ticks(60);
}
