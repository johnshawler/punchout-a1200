/* Linker script for the Zoo Keeper Amiga platform layer.
 *
 * The whole program is ONE flat image linked at PAYLOAD_BASE (0x800) and
 * loaded there by the bootblock.  Only .text/.rodata/.data are stored on disk;
 * .bss, .stack and .chipbuf are NOLOAD (zeroed / cleared by crt0 / plat_init).
 *
 *   0x000000-0x0007FF  vectors (0-0xFF) + bootblock relocator (0x200) + spare
 *   0x000800-...       .text .rodata .data   <- payload on disk
 *                      .bss                  <- zeroed by crt0
 *                      .stack                <- supervisor stack (also used by ISRs)
 *                      .chipbuf              <- 2 x 38400 byte display buffers
 *   _end ... 0x80000   FREE for the game (symbol _end, aligned to 4)
 *
 * _chip_limit: the link fails if the image (incl. display buffers) crosses it.
 */
OUTPUT_FORMAT("elf32-m68k")
OUTPUT_ARCH(m68k)
ENTRY(_start)

PROVIDE(_chip_limit = 0x1F0000);
PROVIDE(_stack_size = 0x800);

SECTIONS
{
    . = 0x800;
    _payload_base = .;
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    .data : ALIGN(4) {
        *(.data .data.*)
        . = ALIGN(4);
    }
    _load_end = .;

    .bss (NOLOAD) : ALIGN(4) {
        _bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    }
    .sndpool (NOLOAD) : ALIGN(4) { *(.sndpool) }
    .stack (NOLOAD) : ALIGN(4) {
        . += _stack_size;
        _stack_top = .;
    }
    .chipbuf (NOLOAD) : ALIGN(4) {
        *(.chipbuf)
        . = ALIGN(4);
    }
    _end = .;
    ASSERT(_end <= _chip_limit, "image + buffers exceed _chip_limit (chip RAM budget)")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
