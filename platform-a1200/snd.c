/*
 * snd.c (A1200 / full-fidelity data) - Zoo Keeper sound module (see sound/amiga/snd.h).  Freestanding 68020.
 * Derived from sound/amiga/snd.c; the PCM sounds are now LZ4-compressed full-length 28.6 kHz recordings (sound/out_full, tools/build_sounds_full.py):
 * the frequently played ones are expanded into a pool at start-up, the others into one scratch buffer when they are played (a new command always
 * replaces the sound that is playing, so one scratch buffer is enough).  A sound longer than Paula's 65535-word DMA length (E4, 5.6 s) is played
 * as two chunks.
 *
 * Playback models
 *   PCM : channels 0 (left) and 1 (right) play the same 8-bit sample, each at its own nibble volume.  Paula has no
 *         one-shot mode, so after the DMA has latched LC/LEN (first audio "interrupt request" flag) the reload registers
 *         are pointed at a silence word (or at the loop part for sounds with a loop); the flag is polled, not enabled.
 *   SEQ : the arcade's tone engine mixes two looped sine tables sample by sample.  Here oscillator 1 = channels 0 (L) and
 *         1 (R), oscillator 2 = channels 3 (L) and 2 (R) (Paula sums 0+3 on the left, 1+2 on the right).  A note is
 *         "play table A and table B, looped, at the fixed tone-engine sample rate until the next note"; notes are switched
 *         from snd_tick() by DMA-off / reprogram / DMA-on so the oscillator phase restarts exactly like the ROM's does.
 */
#include <stdint.h>
#include "snd.h"
#include "sounds.h"
#include "lz4.h"

extern const uint8_t snd_blob[];            /* out_full/sounds.bin (out_full/sounds_blob.s) */

/* expanded PCM: resident pool + scratch buffer, chip RAM (linker section .sndpool, not stored on disk) */
static uint8_t snd_pool[SND_POOL_BYTES + 4] __attribute__((section(".sndpool"), aligned(4)));
static uint8_t snd_scratch[SND_SCRATCH_BYTES + 4] __attribute__((section(".sndpool"), aligned(4)));

/* expand sound d into dst (delta-coded streams are summed up) */
static void pcm_expand(const snd_def_t *d, uint8_t *dst)
{
    lz4_expand(snd_blob + d->off, d->clen, dst);
    if (d->delta) { uint32_t i; int8_t a = 0; for (i = 0; i < d->rlen; i++) { a = (int8_t)(a + (int8_t)dst[i]); dst[i] = (uint8_t)a; } }
}

#define R16(o)  (*(volatile uint16_t *)(0xDFF000u + (o)))
#define R32(o)  (*(volatile uint32_t *)(0xDFF000u + (o)))
#define DMACON    0x096
#define INTENA    0x09A
#define INTREQ    0x09C
#define INTREQR   0x01E
#define ADKCON    0x09E
#define AUDLC(c)  (0x0A0 + ((c) << 4))      /* AUDxLCH/LCL as one long write */
#define AUDLEN(c) (0x0A4 + ((c) << 4))
#define AUDPER(c) (0x0A6 + ((c) << 4))
#define AUDVOL(c) (0x0A8 + ((c) << 4))
#define SETBIT    0x8000
#define DMAEN     0x0200
#define AUDMASK   0x000F
#define IRQ_AUD(c) (0x0080 << (c))
#define IRQ_AUDALL 0x0780

#define CIA_TBLO (*(volatile uint8_t *)0xBFE601)
#define CIA_TBHI (*(volatile uint8_t *)0xBFE701)
#define CIA_CRB  (*(volatile uint8_t *)0xBFEF01)

/* 16x16 -> 32 unsigned multiply (a single MULU.W; avoids the libgcc 32-bit multiply on the 68000) */
static inline uint32_t mulu16(uint16_t a, uint16_t b)
{
    uint32_t r = a;
    __asm__("mulu.w %1,%0" : "+d"(r) : "dmi"(b));
    return r;
}

static const uint16_t silence[2] __attribute__((aligned(4))) = { 0, 0 };

static uint8_t  g_ntsc;
static uint8_t  g_vol;                      /* last arcade volume byte */
static const snd_def_t *g_cur;              /* sound being played (0 = none) */
static uint8_t  g_seq;                      /* a SEQ script is running */
static uint8_t  g_chunk;                    /* a PCM sound's second DMA chunk is queued; silence is queued when it has started */
static uint16_t g_k, g_n;                   /* current note index, number of notes */
static const snd_note_t *g_notes;
static uint16_t g_prev;                     /* last CIA reading */
static uint32_t g_acc, g_next;              /* time since command / end time of the current note, E-clock cycles */

/* ---------------------------------------------------------------- helpers */
static unsigned mkvol(unsigned base, unsigned nib)
{
    unsigned v = (unsigned)((mulu16((uint16_t)base, snd_vol_tbl[nib & 15]) + 32) >> 6);
    return v > 64 ? 64 : v;
}

static uint16_t cia_now(void)
{
    uint8_t h = CIA_TBHI, l = CIA_TBLO;
    if (CIA_TBHI != h) { h = CIA_TBHI; l = CIA_TBLO; }      /* low byte rolled over between the reads */
    return (uint16_t)((h << 8) | l);
}

/* note duration (100 us units) -> CIA E-clock cycles:  PAL 70.9375/unit, NTSC 71.59375/unit */
static uint32_t dur_cycles(unsigned dur)
{
    return mulu16((uint16_t)dur, g_ntsc ? 71 : 70) + (mulu16((uint16_t)dur, g_ntsc ? 38 : 60) >> 6);
}

static uint16_t g_t_off;                    /* CIA reading when the audio DMA was last switched off */
static uint16_t g_per_on;                   /* AUDxPER the channels were last started with */
static uint8_t  g_dma_on;                   /* audio DMA currently enabled by us */

static void aud_off(void)
{
    R16(DMACON) = AUDMASK;                  /* clear the four audio DMA enables */
    R16(INTENA) = IRQ_AUDALL;               /* no audio interrupts from us */
    R16(INTREQ) = IRQ_AUDALL;
    if (g_dma_on) { g_t_off = cia_now(); g_dma_on = 0; }
}

/* A channel whose DMA was just switched off only goes idle at the end of its current sample period; re-enabling DMA before
 * that would NOT restart it from the new LC/LEN.  Wait until one (previous) period + margin has passed since aud_off().
 * AUDxPER counts 3.55 MHz colour clocks, the CIA timer counts E clocks = 1/5 of that. */
static void dma_settle(unsigned per)
{
    unsigned need;
    if (g_per_on > per) per = g_per_on;
    need = (unsigned)(mulu16((uint16_t)per, 13108) >> 16) + 24;      /* per/5 + margin */
    while ((uint16_t)(g_t_off - cia_now()) < need) ;
}
static void dma_on(unsigned mask, unsigned per)
{
    R16(DMACON) = SETBIT | DMAEN | mask; g_dma_on = 1; g_per_on = (uint16_t)per;
}

/* ---------------------------------------------------------------- public API */
void snd_init(void)
{
    unsigned c;
    R16(DMACON) = SETBIT | DMAEN;
    aud_off();
    R16(ADKCON) = 0x00FF;                   /* no audio modulation / attach */
    for (c = 0; c < 4; c++) { R16(AUDVOL(c)) = 0; R16(AUDLEN(c)) = 1; R16(AUDPER(c)) = 124; R32(AUDLC(c)) = (uint32_t)silence; }
    CIA_CRB = 0x00;                         /* timer B: stop, load 0xFFFF, continuous run from the E clock */
    CIA_TBLO = 0xFF; CIA_TBHI = 0xFF;
    CIA_CRB = 0x01;
    for (c = 0; c < SND_NDEFS; c++)
        if (snd_defs[c].kind == SND_KIND_PCM && snd_defs[c].res != 0xFFFFFFFFu) pcm_expand(&snd_defs[c], snd_pool + snd_defs[c].res);
    g_prev = cia_now(); g_t_off = g_prev - 2000; g_chunk = 0; g_cur = 0; g_seq = 0; g_ntsc = 0; g_vol = 0; g_dma_on = 0; g_per_on = 0;
}

void snd_set_ntsc(int ntsc) { g_ntsc = ntsc ? 1 : 0; }

void snd_stop(void)
{
    unsigned c;
    aud_off();
    for (c = 0; c < 4; c++) R16(AUDVOL(c)) = 0;
    g_seq = 0; g_cur = 0; g_chunk = 0;
}

static void apply_volume(void)
{
    unsigned l, r;
    if (!g_cur) return;
    l = mkvol(g_cur->base_vol, g_vol >> 4); r = mkvol(g_cur->base_vol, g_vol & 15);
    R16(AUDVOL(0)) = l; R16(AUDVOL(1)) = r;
    if (g_seq) { R16(AUDVOL(3)) = l; R16(AUDVOL(2)) = r; }
}

void snd_volume(unsigned vol) { g_vol = (uint8_t)vol; apply_volume(); }

/* program the four channels for note k (DMA must be off) */
static void note_regs(unsigned k, unsigned per)
{
    const snd_note_t *n = &g_notes[k];
    const snd_wave_t *a = &snd_waves[n->w1], *b = &snd_waves[n->w2];
    uint32_t la = (uint32_t)snd_blob + ((uint32_t)a->off_w << 1), lb = (uint32_t)snd_blob + ((uint32_t)b->off_w << 1);
    R32(AUDLC(0)) = la; R16(AUDLEN(0)) = a->len_w; R16(AUDPER(0)) = per;
    R32(AUDLC(1)) = la; R16(AUDLEN(1)) = a->len_w; R16(AUDPER(1)) = per;
    R32(AUDLC(3)) = lb; R16(AUDLEN(3)) = b->len_w; R16(AUDPER(3)) = per;
    R32(AUDLC(2)) = lb; R16(AUDLEN(2)) = b->len_w; R16(AUDPER(2)) = per;
    apply_volume();
}

/* note-to-note switch from snd_tick(): oscillator phases restart, as in the ROM */
static void next_note(unsigned k)
{
    unsigned per = g_ntsc ? g_cur->per_ntsc : g_cur->per_pal;
    R16(DMACON) = AUDMASK; g_t_off = cia_now(); g_dma_on = 0;
    note_regs(k, per);
    dma_settle(per);
    dma_on(AUDMASK, per);
}

static void wait_latch(unsigned mask)
{
    unsigned i;
    for (i = 0; i < 6000; i++) if ((R16(INTREQR) & mask) == mask) break;    /* DMA has taken LC/LEN (<= ~1 scanline) */
}

void snd_command(unsigned code, unsigned vol)
{
    const snd_def_t *d = &snd_defs[snd_cmd_def[code & 255]];
    aud_off();
    g_seq = 0; g_cur = 0; g_chunk = 0;
    g_vol = (uint8_t)vol;
    if (d->kind == SND_KIND_SILENT) { unsigned c; for (c = 0; c < 4; c++) R16(AUDVOL(c)) = 0; return; }
    g_cur = d;
    if (d->kind == SND_KIND_PCM) {
        const uint8_t *buf;
        uint32_t words = d->rlen >> 1, first, rest;
        unsigned per = g_ntsc ? d->per_ntsc : d->per_pal, c;
        if (d->res != 0xFFFFFFFFu) buf = snd_pool + d->res;
        else { pcm_expand(d, snd_scratch); buf = snd_scratch; }
        first = words > 65535 ? 65535 : words; rest = words - first;
        for (c = 0; c < 2; c++) { R32(AUDLC(c)) = (uint32_t)buf; R16(AUDLEN(c)) = (uint16_t)first; R16(AUDPER(c)) = per; }
        R16(AUDVOL(2)) = 0; R16(AUDVOL(3)) = 0;
        apply_volume();
        dma_settle(per);
        dma_on(0x0003, per);
        wait_latch(IRQ_AUD(0) | IRQ_AUD(1));
        if (rest) { for (c = 0; c < 2; c++) { R32(AUDLC(c)) = (uint32_t)buf + (first << 1); R16(AUDLEN(c)) = (uint16_t)rest; } g_chunk = 1; }
        else      { for (c = 0; c < 2; c++) { R32(AUDLC(c)) = (uint32_t)silence; R16(AUDLEN(c)) = 1; } }
        R16(INTREQ) = IRQ_AUD(0) | IRQ_AUD(1);     /* from now on INTREQR bit = "buffer has run out / next one started" */
    } else {
        g_notes = &snd_notes[d->off]; g_n = d->len_w; g_k = 0; g_seq = 1;
        {
            unsigned per = g_ntsc ? d->per_ntsc : d->per_pal;
            note_regs(0, per);
            dma_settle(per);
            g_prev = cia_now(); g_acc = 0; g_next = dur_cycles(g_notes[0].dur);
            dma_on(AUDMASK, per);
        }
    }
}

void snd_tick(void)
{
    uint16_t now;
    unsigned k;
    if (g_chunk) {                                       /* second chunk of a long PCM sound: when it has started, queue the silence word */
        if (R16(INTREQR) & IRQ_AUD(0)) {
            R32(AUDLC(0)) = (uint32_t)silence; R16(AUDLEN(0)) = 1; R32(AUDLC(1)) = (uint32_t)silence; R16(AUDLEN(1)) = 1;
            R16(INTREQ) = IRQ_AUD(0) | IRQ_AUD(1); g_chunk = 0;
        }
        return;
    }
    if (!g_seq) return;
    now = cia_now();
    g_acc += (uint16_t)(g_prev - now); g_prev = now;     /* timer counts down */
    if ((int32_t)(g_acc - g_next) < 0) return;
    k = g_k;
    do {
        if (++k >= g_n) {
            if (!g_cur->loop) { snd_stop(); return; }   /* script finished */
            k = 0;
        }
        g_next += dur_cycles(g_notes[k].dur);
    } while ((int32_t)(g_acc - g_next) >= 0);
    g_k = k;
    next_note(k);
}

int snd_active(void)
{
    if (!g_cur) return 0;
    if (g_seq || g_chunk) return 1;
    return (R16(INTREQR) & IRQ_AUD(0)) ? 0 : 1;
}
