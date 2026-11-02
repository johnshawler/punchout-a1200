/* nes_apu.c - NES APU on Paula.  Not a sample-accurate model: the register writes set up four looped waveforms (pulse 1, pulse 2, triangle, noise), and once per
 * emulated frame the envelope / length / sweep / linear-counter steps of the frame sequencer (4-step mode) are run and the resulting period and volume are
 * written to Paula.  The DMC channel (crowd noise, voice samples) is decoded to 8 bit when started and played on Paula channel 3 in place of the noise channel while it runs.  Cheap enough for a stock A1200.
 *
 * Paula period = 3546895 Hz / sample rate (PAL).  A waveform of L samples per cycle at NES frequency f needs sample rate L*f; the NES pulse frequency is
 * f = 1662607 / (16 (T+1)), so period = 34.13 (T+1) / L.  Paula cannot go below period ~124, so high notes use shorter loops (L = 32, 16, 8, 4). */
#include "nes.h"
#include "platform.h"

#define AUDREG(ch, o) (*(volatile uint16_t *)(0xDFF0A0 + (ch) * 16 + (o)))
#define AUDLC(ch)     (*(volatile uint32_t *)(0xDFF0A0 + (ch) * 16))

static int8_t wave_pulse[4][4][32] __attribute__((section(".bss.chip"), aligned(4)));    /* [duty][log2 of 32/L][sample] */
static int8_t wave_tri[4][32] __attribute__((section(".bss.chip"), aligned(4)));         /* triangle: L = 32, 16, 8, 4 */
static int8_t wave_noise[2][2048] __attribute__((section(".bss.chip"), aligned(4)));     /* [0] long mode (LFSR output), [1] short mode (93 step loop, repeated) */
static uint8_t wave_ready;

static const uint8_t len_table[32] = { 10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14, 12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30 };
static const uint16_t noise_period[16] = { 4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778 };   /* PAL */

typedef struct {
    uint8_t reg[4];
    uint8_t enabled, len, env_start, env_div, env_vol, sweep_div, sweep_reload, lin, lin_reload;
} Chan;
static Chan ch[4];       /* 0, 1 pulse; 2 triangle; 3 noise */
static uint8_t cur_wave[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t started;

#ifndef DMC_VOL
#define DMC_VOL 64
#endif
#ifndef DMC_BUF
#define DMC_BUF 12288
#endif
static int8_t dmc_buf[DMC_BUF] __attribute__((section(".bss.chip"), aligned(4)));
static const uint16_t dmc_rate[16] = { 398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50 };   /* PAL CPU cycles per bit */
static uint8_t dmc_ctl, dmc_level = 64, dmc_active, dmc_loop;
static uint16_t dmc_addr, dmc_len;
static int32_t dmc_frames;       /* frames left of a one-shot sample */
static uint8_t dmc_start, dmc_stop;

static void build_waves(void)
{
    static const uint8_t duty[4][8] = { {0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0}, {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1} };
    for (unsigned d = 0; d < 4; d++)
        for (unsigned k = 0; k < 4; k++) {                      /* k: samples per step = 4 >> k  (L = 32, 16, 8, 4) */
            unsigned per = 4u >> k;
            for (unsigned i = 0; i < 32; i++) wave_pulse[d][k][i] = 0;
            for (unsigned i = 0; i < 8 * per; i++) wave_pulse[d][k][i] = duty[d][i / per] ? 60 : -60;
        }
    for (unsigned k = 0; k < 4; k++) {
        unsigned L = 32u >> k;
        for (unsigned i = 0; i < L; i++) { unsigned p = i * 32 / L; int v = p < 16 ? (int)p * 8 - 60 : (31 - (int)p) * 8 - 60; wave_tri[k][i] = (int8_t)v; }
    }
    {   uint16_t lfsr = 1;
        for (unsigned i = 0; i < 2048; i++) { uint16_t fb = (uint16_t)((lfsr ^ (lfsr >> 1)) & 1); lfsr = (uint16_t)((lfsr >> 1) | (fb << 14)); wave_noise[0][i] = (lfsr & 1) ? 50 : -50; }
        lfsr = 1;
        for (unsigned i = 0; i < 93; i++) { uint16_t fb = (uint16_t)((lfsr ^ (lfsr >> 6)) & 1); lfsr = (uint16_t)((lfsr >> 1) | (fb << 14)); wave_noise[1][i] = (lfsr & 1) ? 50 : -50; }
        for (unsigned i = 93; i < 2048; i++) wave_noise[1][i] = wave_noise[1][i % 93];
    }
    wave_ready = 1;
}

void apu_wr(uint32_t reg, uint32_t val, uint32_t now)
{
    (void)now;
    if (reg < 0x10) {
        Chan *c = &ch[reg >> 2];
        c->reg[reg & 3] = (uint8_t)val;
        if ((reg & 3) == 3) {
            if (c->enabled) c->len = len_table[val >> 3];
            if (reg < 8) c->env_start = 1; else if (reg < 0xC) c->lin_reload = 1; else c->env_start = 1;
        }
        if (reg == 1 || reg == 5) ch[reg >> 2].sweep_reload = 1;
    } else if (reg == 0x10) dmc_ctl = (uint8_t)val;
    else if (reg == 0x11) dmc_level = (uint8_t)(val & 0x7F);
    else if (reg == 0x12) dmc_addr = (uint16_t)(0xC000 + val * 64);
    else if (reg == 0x13) dmc_len = (uint16_t)(val * 16 + 1);
    else if (reg == 0x15) {
        if (val & 0x10) { if (!dmc_active) dmc_start = 1; } else { dmc_stop = 1; dmc_start = 0; }
        for (unsigned i = 0; i < 4; i++) { ch[i].enabled = (uint8_t)((val >> i) & 1); if (!ch[i].enabled) ch[i].len = 0; }
    }
}

uint32_t apu_rd(uint32_t reg)
{
    if (reg == 0x15) return (uint32_t)((ch[0].len ? 1 : 0) | (ch[1].len ? 2 : 0) | (ch[2].len ? 4 : 0) | (ch[3].len ? 8 : 0) | (dmc_active ? 16 : 0));
    return 0;
}

static inline void quarter(void)             /* envelopes and the triangle's linear counter */
{
    for (unsigned i = 0; i < 4; i += (i == 1 ? 2 : 1)) {
        Chan *c = &ch[i];
        if (c->env_start) { c->env_start = 0; c->env_vol = 15; c->env_div = c->reg[0] & 15; }
        else if (c->env_div) c->env_div--;
        else { c->env_div = c->reg[0] & 15; if (c->env_vol) c->env_vol--; else if (c->reg[0] & 0x20) c->env_vol = 15; }
    }
    {   Chan *c = &ch[2];
        if (c->lin_reload) c->lin = c->reg[0] & 0x7F; else if (c->lin) c->lin--;
        if (!(c->reg[0] & 0x80)) c->lin_reload = 0;
    }
}

static inline unsigned sweep_target(unsigned i)
{
    Chan *c = &ch[i];
    unsigned t = (unsigned)c->reg[2] | (unsigned)(c->reg[3] & 7) << 8, d = t >> (c->reg[1] & 7);
    if (c->reg[1] & 8) return t - d - (i == 0 ? 1 : 0);
    return t + d;
}

static inline void half(void)                /* length counters and sweeps */
{
    for (unsigned i = 0; i < 4; i++) {
        Chan *c = &ch[i];
        unsigned halt = (i == 2) ? (c->reg[0] & 0x80) : (c->reg[0] & 0x20);
        if (c->len && !halt) c->len--;
    }
    for (unsigned i = 0; i < 2; i++) {
        Chan *c = &ch[i];
        if (c->sweep_div == 0 && (c->reg[1] & 0x80) && (c->reg[1] & 7)) {
            unsigned t = (unsigned)c->reg[2] | (unsigned)(c->reg[3] & 7) << 8, nt = sweep_target(i);
            if (t >= 8 && nt <= 0x7FF) { c->reg[2] = (uint8_t)nt; c->reg[3] = (uint8_t)((c->reg[3] & ~7) | (nt >> 8)); }
        }
        if (c->sweep_div == 0 || c->sweep_reload) { c->sweep_div = (c->reg[1] >> 4) & 7; c->sweep_reload = 0; } else c->sweep_div--;
    }
}

static void paula_set(unsigned i, const void *w, unsigned words, unsigned period, unsigned vol, unsigned id)
{
    if (!started || cur_wave[i] == 0xFF) { plat_audio_play(i, w, words, period < 124 ? 124 : period, 0, PLAT_SND_LOOP); cur_wave[i] = (uint8_t)id; }
    else if (cur_wave[i] != id) { AUDLC(i) = (uint32_t)w; AUDREG(i, 4) = (uint16_t)words; cur_wave[i] = (uint8_t)id; }
    AUDREG(i, 6) = (uint16_t)(period < 124 ? 124 : period);
#ifdef DMC_ONLY
    if (!(i == 3 && dmc_active)) vol = 0;
#endif
#ifdef ONLYCH
    if (i != ONLYCH) vol = 0;
#endif
    AUDREG(i, 8) = (uint16_t)vol;
}

static void dmc_begin(void)
{
    unsigned cyc = dmc_rate[dmc_ctl & 15], n = (unsigned)dmc_len * 8u, f = (n + DMC_BUF - 1) / DMC_BUF, f2 = (124u * 1024u + cyc * 2185u - 1) / (cyc * 2185u);
    if (f2 > f) f = f2;
    if (f < 1) f = 1;
    unsigned out = n / f;
    int lvl = dmc_level, acc = 0;
    unsigned cnt = 0, o = 0;
    for (unsigned i = 0; i < dmc_len && o < out; i++) {
        unsigned b = nes_prg[0x1C000u + ((dmc_addr - 0xC000u + i) & 0x3FFFu)];
        for (unsigned k = 0; k < 8; k++, b >>= 1) {
            if (b & 1) { if (lvl <= 125) lvl += 2; } else if (lvl >= 2) lvl -= 2;
            acc += lvl - 28;
            if (++cnt == f) { if (o < DMC_BUF) dmc_buf[o++] = (int8_t)(acc / (int)f); acc = 0; cnt = 0; }
        }
    }
    dmc_level = (uint8_t)lvl;
    unsigned words = o >> 1;
    if (!words) return;
    dmc_loop = (dmc_ctl >> 6) & 1;
    plat_audio_play(3, dmc_buf, words, (cyc * f * 2185u) >> 10, DMC_VOL, dmc_loop ? PLAT_SND_LOOP : 0);
    dmc_active = 1; cur_wave[3] = 0xFE;
    dmc_frames = dmc_loop ? 0 : (int32_t)((uint32_t)dmc_len * 8u * cyc / 33252u) + 1;     /* 33252 CPU cycles per PAL frame */
}

void apu_frame(void)
{
#ifdef NOAPU
    return;
#endif
    if (!wave_ready) build_waves();
    if (dmc_stop) { dmc_stop = 0; if (dmc_active) { dmc_active = 0; cur_wave[3] = 0xFF; } }
    if (dmc_start) { dmc_start = 0; dmc_begin(); }
    if (dmc_active && !dmc_loop && --dmc_frames <= 0) { dmc_active = 0; cur_wave[3] = 0xFF; }
    quarter(); half(); quarter(); quarter(); half(); quarter();       /* the 4-step sequence: 4 quarter, 2 half ticks per frame */
    for (unsigned i = 0; i < 2; i++) {                                 /* pulse */
        Chan *c = &ch[i];
        unsigned t = (unsigned)c->reg[2] | (unsigned)(c->reg[3] & 7) << 8, vol = (c->reg[0] & 0x10) ? (c->reg[0] & 15) : c->env_vol;
        int mute = !c->len || t < 8 || sweep_target(i) > 0x7FF;
        unsigned k = 0, per = ((t + 1) * 1092) >> 10;                   /* L = 32 */
        while (per < 124 && k < 2) { k++; per = ((t + 1) * (1092u << k)) >> 10; }
        paula_set(i, wave_pulse[c->reg[0] >> 6][k], (8u * (4u >> k)) >> 1, per, mute ? 0 : vol * 4, (c->reg[0] >> 6) | k << 2);
    }
    {   Chan *c = &ch[2];                                              /* triangle */
        unsigned t = (unsigned)c->reg[2] | (unsigned)(c->reg[3] & 7) << 8;
        int mute = !c->len || !c->lin || t < 2;
        unsigned k = 0, per = ((t + 1) * 2185) >> 10;
        while (per < 124 && k < 2) { k++; per = ((t + 1) * (2185u << k)) >> 10; }
        paula_set(2, wave_tri[k], (32u >> k) >> 1, per, mute ? 0 : 48, 0x40 | k);
    }
    if (!dmc_active) {   Chan *c = &ch[3];                             /* noise */
        unsigned vol = (c->reg[0] & 0x10) ? (c->reg[0] & 15) : c->env_vol, p = noise_period[c->reg[2] & 15], mode = (c->reg[2] >> 7) & 1;
        paula_set(3, wave_noise[mode], mode ? 1023 : 1024, (p * 2185) >> 10, (!c->len) ? 0 : vol * 4, 0x80 | mode);
    }
    started = 1;
}

#ifdef APUDBG
uint8_t apu_dbg(unsigned q, unsigned k) { return k < 4 ? ch[q].reg[k] : k == 4 ? ch[q].len : ch[q].enabled; }
#endif
