/* Linker script: one flat image at 0x800 (loaded by the bootblock), .bss and big tables NOLOAD after it. */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)
PROVIDE(_stack_size = 0x2000);
SECTIONS
{
    . = 0x800;
    .text : {
        KEEP(*(.text.start))
        _g0 = .; *plat.o(.text .text.*) *isr.o(.text .text.*) *crt0.o(.text .text.*) *rt.o(.text .text.*) *rtasm.o(.text .text.*)
        _g1 = .; *nes_rt.o(.text .text.*)
        _g2 = .; *nes_core.o(.text .text.*)
        _g3 = .; *nes_ppu.o(.text .text.*) *nes_vid.o(.text .text.*)
        _g4 = .; *nes_interp.o(.text .text.*)
        _g5 = .; *nes_main.o(.text .text.*) *nes_apu.o(.text .text.*)
        _g6 = .; *code.o(.text .text.*)
        _g7 = .; *(.text .text.*)
        _g8 = .;
        *(.rodata .rodata.*)
    }
    .data : ALIGN(4) { *(.data .data.*) . = ALIGN(4); }
    _load_end = .;
    ASSERT(_load_end <= 0x1F0000, "image too big")
    .bss (NOLOAD) : ALIGN(16) { _bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); _bss_end = .; }
    .stack (NOLOAD) : ALIGN(4) { . += _stack_size; _stack_top = .; }
    .chipbuf (NOLOAD) : ALIGN(4) { *(.chipbuf) . = ALIGN(4); }
    _end = .;
    ASSERT(_end <= 0x200000, "does not fit in 2 MB chip RAM")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
