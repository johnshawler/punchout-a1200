/* Linker script for the fast-RAM build (make FAST=1): the code, tables and CPU-only data run from 1-2 MB of Zorro II / A1200 fast RAM at
 * 0x200000; only what the custom chips read (bitmaps, Copper lists, sprite streams, Paula waveforms, ...: section .bss.chip / .chipbuf) stays in
 * chip RAM.  The bootblock loads the payload to 0x800 as before; the start stub copies the image to fast RAM and continues there. */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)
PROVIDE(_stack_size = 0x2000);
SECTIONS
{
    . = 0x800;
    .start : { KEEP(*(.text.start)) }
    .fimg 0x200000 : AT(ALIGN(LOADADDR(.start) + SIZEOF(.start), 4)) {
        *crt0_fast.o(.text .text.*)
        _g0 = .; *plat.o(.text .text.*) *isr.o(.text .text.*) *rt.o(.text .text.*) *rtasm.o(.text .text.*)
        _g1 = .; *nes_rt.o(.text .text.*)
        _g2 = .; *nes_core.o(.text .text.*)
        _g3 = .; *nes_ppu.o(.text .text.*) *nes_vid.o(.text .text.*)
        _g4 = .; *nes_interp.o(.text .text.*)
        _g5 = .; *nes_main.o(.text .text.*) *nes_apu.o(.text .text.*)
        _g6 = .; *code.o(.text .text.*)
        _g7 = .; *(.text .text.*)
        _g8 = .;
        *(.rodata .rodata.*)
        . = ALIGN(4);
        *(.data .data.*)
        . = ALIGN(4);
    }
    _fimg_start = ADDR(.fimg); _fimg_lma = LOADADDR(.fimg); _fimg_longs = SIZEOF(.fimg) / 4;
    _load_end = LOADADDR(.fimg) + SIZEOF(.fimg);
    ASSERT(_load_end <= 0x1F0000, "image too big")
    . = (LOADADDR(.fimg) + SIZEOF(.fimg) + 15) & ~15;      /* chip RAM above the loaded image (dead once copied) */
    .chipbss (NOLOAD) : ALIGN(16) { _cbss_start = .; *(.bss.chip) *(.chipbuf) . = ALIGN(4); _cbss_end = .; }
    ASSERT(. <= 0x200000, "chip part does not fit in 2 MB chip RAM")
    . = ADDR(.fimg) + SIZEOF(.fimg);
    .bss (NOLOAD) : ALIGN(16) { _bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); _bss_end = .; }
    .stack (NOLOAD) : ALIGN(4) { . += _stack_size; _stack_top = .; }
    ASSERT(. <= 0x380000, "fast part does not fit in 1.5 MB")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
