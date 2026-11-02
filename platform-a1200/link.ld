/* Linker script for the Zoo Keeper game (A1200: 2 MB chip RAM; copy of amiga/link.ld with the VRAM block at the top of chip RAM).
 *
 * The whole program is ONE flat image linked at PAYLOAD_BASE (0x800) and
 * loaded there by the bootblock.  Only .text/.rodata/.data are stored on disk;
 * .bss, .stack and .chipbuf are NOLOAD (zeroed / cleared by crt0 / plat_init).
 *
 *   0x000000-0x0007FF  vectors (0-0xFF) + bootblock relocator (0x200) + spare
 *   0x000800-...       .text .rodata .data   <- payload on disk
 *                      .bss                  <- zeroed by crt0
 *                      .stack                <- supervisor stack (also used by ISRs)
 *                      .chipbuf              <- 61440 byte display buffer (8 interleaved planes)
 *   _end ... 0x1F0000  FREE for the game (symbol _end, aligned to 4);  0x1F0000-0x1FFFFF chunky VRAM
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
        _pr0 = .;  *main.o(.text .text.*)
        _pr1 = .;  *display.o(.text .text.*)
        _pr2 = .;  *rt_sched.o(.text .text.*)
        _pr3 = .;  *hw_data.o(.text .text.*)
        _pr4 = .;  *hw_video.o(.text .text.*)
        _pr5 = .;  *pia6821.o(.text .text.*)
        _pr6 = .;  *rt_core.o(.text .text.*) *rt_cpu.o(.text .text.*)
        _pr7 = .;  *data_cpu.o(.text .text.*)
        _pr8 = .;  *video_cpu.o(.text .text.*)
        _pr9 = .;  *(.text .text.*)
        _pr10 = .;
        *(.rodata .rodata.*)
    }
    /* data CPU address space: RAM $0000-$07FF at data_mem, ROM $8000-$FFFF at data_mem+$8000, and the program's .bss in the unused 30 KB between */
    .dmem_lo : ALIGN(4) {
        data_mem = .;
        *(.dmem_lo)
    }
    .bss data_mem + 0x800 (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    }
    ASSERT(_bss_end <= data_mem + 0x8000, ".bss does not fit into the unused part of the data CPU address space")
    .dmem_hi data_mem + 0x8000 : {
        *(.dmem_hi)
    }
    .data : ALIGN(4) {
        *(.data .data.*)
        . = ALIGN(4);
    }
    _load_end = .;
    ASSERT(_load_end <= 0x1F0000, "image does not fit below the VRAM")
    .vhw (NOLOAD) : ALIGN(4) {                        /* dirty tables, palette RAM, video latches */
        *(.vhw)
    }
    .dtab (NOLOAD) : ALIGN(4) {                       /* flat dispatch tables of the two translated CPUs (2 x 256 KB), filled at start-up */
        *(.dtab)
    }
    .sndpool (NOLOAD) : ALIGN(4) {                    /* expanded PCM of the sounds: resident pool + scratch buffer (see snd.c) */
        *(.sndpool)
    }
    .stack (NOLOAD) : ALIGN(4) {
        . += _stack_size;
        _stack_top = .;
    }
    .chipbuf (NOLOAD) : ALIGN(4) {
        *(.chipbuf)
        . = ALIGN(4);
    }
    _end = .;
    ASSERT(_end <= 0x1F0000, "image + buffers exceed the 64 KB aligned VRAM block at the top of chip RAM")
    .vram 0x1F0000 (NOLOAD) : {                         /* chunky VRAM: 64 KB aligned (low word of a host pointer = VRAM offset), the top 64 KB of the 2 MB chip RAM */
        *(.vram)
    }
    _vram_end = .;
    ASSERT(_vram_end <= 0x200000, "VRAM does not fit in chip RAM")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
