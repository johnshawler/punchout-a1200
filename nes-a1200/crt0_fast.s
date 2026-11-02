| crt0_fast.s - payload entry for the fast-RAM build.  `_start` (section .text.start, at 0x800 in chip RAM) copies the image to fast RAM
| (0x200000) and continues in `_start_fast` there, which is the same as platform-a1200/crt0.s except that .bss is split in a fast and a chip part.
    .section .text.start,"ax"
    .globl  _start
_start:
    move.w  #0x2700,%sr
    moveq   #0,%d0
    movec   %d0,%vbr
    move.l  #0x0808,%d0             | caches off + cleared while the code is moved
    movec   %d0,%cacr
    lea     _fimg_lma,%a0
    lea     _fimg_start,%a1
    move.l  #_fimg_longs,%d0
1:  move.l  (%a0)+,(%a1)+
    subq.l  #1,%d0
    bne.s   1b
    jmp     _start_fast

    .text
_start_fast:
    move.l  #0x0809,%d0             | CACR: clear I-cache, enable it (data cache off: the custom chips must never be cached)
    movec   %d0,%cacr
    lea     _stack_top,%sp
    jsr     plat_hw_reset           | quiesce all chips (no BSS use before here)
    lea     _bss_start,%a0
    lea     _bss_end,%a1
2:  cmp.l   %a0,%a1
    bls.s   3f
    clr.l   (%a0)+
    bra.s   2b
3:  lea     _cbss_start,%a0
    lea     _cbss_end,%a1
4:  cmp.l   %a0,%a1
    bls.s   5f
    clr.l   (%a0)+
    bra.s   4b
5:  lea     plat_vectors,%a0        | the 64 exception vectors live at 0 (chip RAM)
    sub.l   %a1,%a1
    moveq   #63,%d0
6:  move.l  (%a0)+,(%a1)+
    dbra    %d0,6b
    jsr     plat_init
    jsr     main
7:  bra.s   7b
