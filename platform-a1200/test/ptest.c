/* Platform test for the A1200 layer: draws a 1-pixel frame (palette index 255 = red) around the 256x240 display, a 16-pixel marker at the
 * top left corner in index 2 (green) and index 1 (blue) fill; palette entry 0 is bright green so that a non-blanked border would show.
 * Prints nothing: inspected through FS-UAE full-frame screenshots (1 lores pixel = 2 screenshot pixels). */
#include <stdint.h>
#include "platform.h"
static uint8_t *scr;
static void put(unsigned x, unsigned y, unsigned idx)
{
    uint8_t *p = scr + y * PLAT_STRIDE + (x >> 3);
    for (unsigned b = 0; b < 8; b++) {
        uint8_t m = (uint8_t)(0x80 >> (x & 7));
        if ((idx >> b) & 1) p[b * PLAT_PLANE_STRIDE] |= m; else p[b * PLAT_PLANE_STRIDE] &= (uint8_t)~m;
    }
}
int main(void)
{
    static uint32_t pal[256];
    pal[0] = 0x00FF00; pal[1] = 0x000080; pal[2] = 0x00FFFF; pal[255] = 0xFF0000; pal[3] = 0xFFFFFF;
    for (unsigned i = 4; i < 255; i++) pal[i] = (i << 16) | ((255 - i) << 8) | 0x40;
    plat_set_palette256(pal);
    scr = plat_front();
    for (unsigned y = 0; y < PLAT_H; y++) for (unsigned x = 0; x < 256; x++) put(x, y, 1);
    for (unsigned x = 0; x < 256; x++) { put(x, 0, 255); put(x, PLAT_H - 1, 255); }
    for (unsigned y = 0; y < PLAT_H; y++) { put(0, y, 255); put(255, y, 255); }
    for (unsigned y = 2; y < 18; y++) for (unsigned x = 2; x < 18; x++) put(x, y, 2);
    for (unsigned x = 20; x < 236; x++) put(x, 120, 3);
    for (;;) ;
}
