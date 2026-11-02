    .section .rodata
    .globl nes_prg, nes_chr
    .align 4
nes_prg: .incbin "prg.bin"
nes_chr: .incbin "chr.bin"
