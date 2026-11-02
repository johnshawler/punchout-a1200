| nes_rt.s - run-time glue between the translated 6502 code (68020) and the C hardware model.
| Register map of the translated code: d0=A d1=X d2=Y d3=S d4=V d5=C (bytes) d6=ccr save d7=operand/pc a1=window dispatch table end a2=fixed table end
| a3=window base-$8000 a4=fixed ROM base-$A000 a5=6502 RAM a6=cycle counter (negative; the event check fires when it is > 0).
| C ABI (m68k gcc): d0,d1,a0,a1 are caller-saved, everything else is preserved by the C functions.
    .text
    .globl nes_run, rt_dispatch_d7, rt_event, rt_event_f, rt_undef, rt_unsupported, rt_brk, rt_rti, rt_php, rt_plp
    .globl rt_warp_ram, rt_warp_poll, rt_io_rd, rt_io_wr, rt_map_wr, rt_rd8, rt_wr8_d0, rt_wr8_d1, rt_wr8_d2

| void nes_run(uint32_t pc)  - never returns.  Variables are set up by nes_init().
nes_run:
    move.l  4(%sp),%d7
    lea     nes_ram,%a5
    move.l  nes_a4,%a4
    move.l  nes_fix_tab_end,%a2
    move.l  nes_win_tab,%a1
    move.l  nes_win_base,%a3
    moveq   #0,%d0
    moveq   #0,%d1
    moveq   #0,%d2
    moveq   #0,%d4
    moveq   #0,%d5
    move.l  #0xFD,%d3
    move.l  nes_a6_init,%a6
    cmp.w   #-24576,%d7
    blt.s   7f
    move.l  (%a2,%d7.w*4),%a0
    bra.s   8f
7:  move.l  (%a1,%d7.w*4),%a0
8:  move    #0,%ccr
    jmp     (%a0)

rt_dispatch_d7:
    cmp.w   #-24576,%d7
    blt.s   7f
    move.l  (%a2,%d7.w*4),%a0
    jmp     (%a0)
7:  move.l  (%a1,%d7.w*4),%a0
    jmp     (%a0)

| ---- context save / restore around the C scheduler and interpreter --------------------------------
    .macro SAVE_CTX
    lea     -16(%sp),%sp
    move.b  %d0,0(%sp)
    move.b  %d1,1(%sp)
    move.b  %d2,2(%sp)
    move.b  %d3,3(%sp)
    move.b  %d5,4(%sp)
    move.b  %d4,5(%sp)
    move.w  %d6,8(%sp)
    move.w  %d7,10(%sp)
    move.l  %a6,12(%sp)
    .endm
    .macro LOAD_CTX                 | action in a0 on exit; sp restored
    movea.l %d0,%a0
    moveq   #0,%d0
    move.b  0(%sp),%d0
    moveq   #0,%d1
    move.b  1(%sp),%d1
    moveq   #0,%d2
    move.b  2(%sp),%d2
    moveq   #0,%d3
    move.b  3(%sp),%d3
    move.b  4(%sp),%d5
    move.b  5(%sp),%d4
    moveq   #0,%d6
    move.w  8(%sp),%d6
    moveq   #0,%d7
    move.w  10(%sp),%d7
    move.l  12(%sp),%a6
    lea     16(%sp),%sp
    move.l  nes_win_tab,%a1
    move.l  nes_win_base,%a3
    .endm

rt_event:
    moveq   #0,%d6
rt_event_f:                         | d7 = 6502 pc of the interrupted instruction, d6 = ccr (N,Z)
    SAVE_CTX
    move.l  %sp,-(%sp)
    jsr     nes_event
    addq.l  #4,%sp
    LOAD_CTX
    cmpa.w  #0,%a0
    bne.s   resume_at_pc
    rts                             | back to the stub, which jumps to the code after the check
resume_at_pc:                       | continue at the 6502 address in d7 (NMI vector, interpreter exit ...) with N,Z from d6
    addq.l  #4,%sp                  | drop the stub's return address
    cmp.w   #-24576,%d7
    blt.s   7f
    move.l  (%a2,%d7.w*4),%a0
    move    %d6,%ccr
    jmp     (%a0)
7:  move.l  (%a1,%d7.w*4),%a0
    move    %d6,%ccr
    jmp     (%a0)

rt_undef:                           | d7 = 6502 address that has no translated code: interpret it
rt_unsupported:
rt_brk:
    move    %ccr,%d6
    SAVE_CTX
    move.l  %sp,-(%sp)
    jsr     nes_interp
    addq.l  #4,%sp
    LOAD_CTX
    cmp.w   #-24576,%d7
    blt.s   7f
    move.l  (%a2,%d7.w*4),%a0
    move    %d6,%ccr
    jmp     (%a0)
7:  move.l  (%a1,%d7.w*4),%a0
    move    %d6,%ccr
    jmp     (%a0)

| ---- RTI / PHP / PLP ----------------------------------------------------------------------------
rt_rti:
    addq.b  #1,%d3
    move.b  (0x100,%a5,%d3.w),%d7
    move.b  %d7,rti_p
    moveq   #0,%d4
    btst    #6,%d7
    beq.s   1f
    st      %d4
1:  moveq   #0,%d5
    btst    #0,%d7
    beq.s   2f
    st      %d5
2:  addq.b  #1,%d3
    moveq   #0,%d6
    move.b  (0x100,%a5,%d3.w),%d6
    addq.b  #1,%d3
    moveq   #0,%d7
    move.b  (0x100,%a5,%d3.w),%d7
    lsl.w   #8,%d7
    or.w    %d6,%d7
    cmp.w   #-24576,%d7
    blt.s   3f
    move.l  (%a2,%d7.w*4),%a0
    bra.s   4f
3:  move.l  (%a1,%d7.w*4),%a0
4:  move.b  rti_p,%d7
    moveq   #0,%d6
    btst    #7,%d7
    beq.s   5f
    or.b    #8,%d6
5:  btst    #1,%d7
    beq.s   6f
    or.b    #4,%d6
6:  move    %d6,%ccr
    jmp     (%a0)

rt_php:
    move    %ccr,%d6
    moveq   #0x30,%d7
    btst    #3,%d6
    beq.s   1f
    or.b    #0x80,%d7
1:  btst    #2,%d6
    beq.s   2f
    or.b    #2,%d7
2:  tst.b   %d4
    beq.s   3f
    or.b    #0x40,%d7
3:  tst.b   %d5
    beq.s   4f
    or.b    #1,%d7
4:  move.b  %d7,(0x100,%a5,%d3.w)
    subq.b  #1,%d3
    move    %d6,%ccr
    rts

rt_plp:
    addq.b  #1,%d3
    move.b  (0x100,%a5,%d3.w),%d7
    moveq   #0,%d4
    btst    #6,%d7
    beq.s   1f
    st      %d4
1:  moveq   #0,%d5
    btst    #0,%d7
    beq.s   2f
    st      %d5
2:  moveq   #0,%d6
    btst    #7,%d7
    beq.s   3f
    or.b    #8,%d6
3:  btst    #1,%d7
    beq.s   4f
    or.b    #4,%d6
4:  move    %d6,%ccr
    rts

| ---- memory and I/O helpers (preserve everything except d7, a0 and the condition codes) ------------
rt_io_rd:                           | a0 = address ($2000-$401F) -> d7.b, N,Z set
    movem.l %d0-%d1/%a1,-(%sp)
    move.l  %a6,-(%sp)
    move.l  %a0,-(%sp)
    jsr     nes_io_rd
    addq.l  #8,%sp
    move.l  %d0,%d7
    adda.l  nes_stall,%a6
    clr.l   nes_stall
    movem.l (%sp)+,%d0-%d1/%a1
    tst.b   %d7
    rts
rt_io_wr:                           | a0 = address, d7.b = value
    cmpa.w  #0x2007,%a0
    bne.s   io_wr_full
    movem.l %d0-%d1/%a0-%a1,-(%sp)       | PPU data port: lean C entry
    move.l  %a6,-(%sp)
    and.l   #0xFF,%d7
    move.l  %d7,-(%sp)
    jsr     ppu_data_wr
    addq.l  #8,%sp
    movem.l (%sp)+,%d0-%d1/%a0-%a1
    rts
io_wr_full:
    movem.l %d0-%d1/%a1,-(%sp)
    move.l  %a6,-(%sp)
    and.l   #0xFF,%d7
    move.l  %d7,-(%sp)
    move.l  %a0,-(%sp)
    jsr     nes_io_wr
    lea     12(%sp),%sp
    adda.l  nes_stall,%a6
    clr.l   nes_stall
    movem.l (%sp)+,%d0-%d1/%a1
    rts
rt_warp_ram:                        | pure RAM wait loop is spinning: jump virtual time to the next event
    movem.l %d0-%d1/%a0-%a1,-(%sp)
    move.l  %a6,-(%sp)
    clr.l   -(%sp)
    jsr     nes_warp
    addq.l  #8,%sp
    adda.l  nes_stall,%a6
    clr.l   nes_stall
    movem.l (%sp)+,%d0-%d1/%a0-%a1
    rts
rt_warp_poll:                       | $2002 poll loop is spinning: jump to the next time the status can change
    movem.l %d0-%d1/%a0-%a1,-(%sp)
    move.l  %a6,-(%sp)
    move.l  #1,-(%sp)
    jsr     nes_warp
    addq.l  #8,%sp
    adda.l  nes_stall,%a6
    clr.l   nes_stall
    movem.l (%sp)+,%d0-%d1/%a0-%a1
    rts
rt_map_wr:                          | a0 = address ($8000-$FFFF), d7.b = value
    movem.l %d0-%d1,-(%sp)
    and.l   #0xFF,%d7
    move.l  %a6,-(%sp)
    move.l  %d7,-(%sp)
    move.l  %a0,-(%sp)
    jsr     nes_map_wr
    lea     12(%sp),%sp
    movem.l (%sp)+,%d0-%d1
    move.l  nes_win_tab,%a1
    move.l  nes_win_base,%a3
    rts
rt_rd8:                             | d7.w = address -> d7.b, N,Z set; d6 preserved.  Fast paths: RAM, fixed ROM, window ROM
    and.l   #0xFFFF,%d7
    cmp.l   #0x800,%d7
    bcc.s   1f
    move.b  (%a5,%d7.l),%d7
    tst.b   %d7
    rts
1:  cmp.l   #0xA000,%d7
    bcs.s   2f
    move.b  (%a4,%d7.l),%d7
    tst.b   %d7
    rts
2:  cmp.l   #0x8000,%d7
    bcs.s   rd8_slow
    move.b  (%a3,%d7.l),%d7
    tst.b   %d7
    rts
rd8_slow:
    movem.l %d0-%d1/%a0-%a1,-(%sp)
    move.l  %a6,-(%sp)
    and.l   #0xFFFF,%d7
    move.l  %d7,-(%sp)
    jsr     nes_rd8
    addq.l  #8,%sp
    move.l  %d0,%d7
    adda.l  nes_stall,%a6
    clr.l   nes_stall
    movem.l (%sp)+,%d0-%d1/%a0-%a1
    tst.b   %d7
    rts
rt_wr8_d0:
    and.l   #0xFFFF,%d7
    cmp.l   #0x800,%d7
    bcc.s   1f
    move.b  %d0,(%a5,%d7.l)
    rts
1:  movem.l %d0-%d1/%a0-%a1,-(%sp)
    move.l  %d0,%d1
    bra.s   wr8_common
rt_wr8_d1:
    and.l   #0xFFFF,%d7
    cmp.l   #0x800,%d7
    bcc.s   1f
    move.b  %d1,(%a5,%d7.l)
    rts
1:  movem.l %d0-%d1/%a0-%a1,-(%sp)
    bra.s   wr8_common
rt_wr8_d2:
    and.l   #0xFFFF,%d7
    cmp.l   #0x800,%d7
    bcc.s   1f
    move.b  %d2,(%a5,%d7.l)
    rts
1:  movem.l %d0-%d1/%a0-%a1,-(%sp)
    move.l  %d2,%d1
wr8_common:                         | d7.w = address, d1.b = value (d0/d1/a0/a1 are saved)
    and.l   #0xFF,%d1
    and.l   #0xFFFF,%d7
    move.l  %a6,-(%sp)
    move.l  %d1,-(%sp)
    move.l  %d7,-(%sp)
    jsr     nes_wr8
    lea     12(%sp),%sp
    adda.l  nes_stall,%a6
    clr.l   nes_stall
    movem.l (%sp)+,%d0-%d1/%a0-%a1
    move.l  nes_win_tab,%a1
    move.l  nes_win_base,%a3
    rts

    .data
    .align 4
rti_p:  .byte 0
    .align 4
