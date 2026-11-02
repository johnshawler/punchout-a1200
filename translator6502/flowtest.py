#!/usr/bin/env python3
"""Lockstep control-flow test: the REAL PRG code is run from the RESET vector on (a) the reference 6502 core and (b) the translated
68020 code under qemu-m68k, both against the same mock machine (RAM, deterministic pseudo-random I/O reads, MMC2-style PRG bank
register at $A000-$AFFF).  Both sides log a checkpoint at every event-check site (loop headers that are back-edge targets, JSR, JMP (ind));
every 11th checkpoint an NMI is injected on both sides.  Compared per checkpoint: PC, A, X, Y, S, C and V when live, RAM checksum, and the
cycle-counter delta.  This covers branches, JSR/RTS/RTI, indirect jumps, the dispatch tables, bank re-dispatch, cycle counting and the
event stubs.   usage: flowtest.py PRG.bin trace.bin [max_checkpoints] [seed]"""
import sys, os, random, subprocess, struct, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import program as PG
from program import load_seeds, translate, emit_program, mark_back_edges
from ref6502 import CPU, OPS

PG.THR = '-32768'            # fire at every check site
NMI_EVERY = 11

class Mock(CPU):
    def __init__(self, rom, ram):
        super().__init__(None); self.rom = rom; self.ram = ram; self.bank = 0; self.lfsr = 0x12345678
    def getp(self, brk=0): return self.N << 7 | self.V << 6 | 0x20 | brk << 4 | self.Z << 1 | self.C      # I and D are not modelled by the port
    def setp(self, p): self.N = p >> 7 & 1; self.V = p >> 6 & 1; self.Z = p >> 1 & 1; self.C = p & 1
    def io(self):
        x = self.lfsr; x ^= (x << 13) & 0xFFFFFFFF; x ^= x >> 17; x ^= (x << 5) & 0xFFFFFFFF; self.lfsr = x; return (x >> 8) & 0xFF
    def rd(self, a):
        a &= 0xFFFF
        if a < 0x2000: return self.ram[a & 0x7FF]
        if a < 0x4020: return self.io()
        if a < 0x8000: return 0
        if a < 0xA000: return self.rom[self.bank * 0x2000 + a - 0x8000]
        return self.rom[0x20000 - 0x6000 + a - 0xA000]
    def fetch(self, a): return self.rd(a) if a >= 0x8000 else None
    def wr(self, a, v):
        a &= 0xFFFF
        if a < 0x2000: self.ram[a & 0x7FF] = v & 0xFF
        elif 0xA000 <= a < 0xB000: self.bank = v & 15

def ramsum(ram):
    d6 = 0
    for k in range(0, 0x800, 4):
        w = struct.unpack('>I', bytes(ram[k:k + 4]))[0]; d6 = ((d6 << 1 | d6 >> 31) & 0xFFFFFFFF) ^ w
    return (d6 ^ (d6 >> 16)) & 0xFFFF

RUNTIME = r'''
	.text
	.globl _start
_start:
	lea ramimg,%a5
	move.l #3,%d0
	moveq #0,%d1
	move.l %a5,%d2
	move.l #2064,%d3
	trap #0
	lea rom,%a4
	add.l #0x10000,%a4
	lea fixtab_end,%a2
	lea bank_tab,%a0
	moveq #0,%d6
	move.b 2052(%a5),%d6
	and.w #15,%d6
	lsl.w #3,%d6
	move.l (%a0,%d6.w),%a3
	move.l 4(%a0,%d6.w),%a1
	moveq #0,%d0
	moveq #0,%d1
	moveq #0,%d2
	moveq #0,%d3
	moveq #0,%d4
	moveq #0,%d5
	move.b 2048(%a5),%d0
	move.b 2049(%a5),%d1
	move.b 2050(%a5),%d2
	move.b 2051(%a5),%d3
	tst.b 2053(%a5)
	beq.s 1f
	st %d5
1:	tst.b 2054(%a5)
	beq.s 2f
	st %d4
2:	movea.w #-30000,%a6
	move.l #logbuf,(logptr)
	moveq #0,%d7
	move.w 2056(%a5),%d7
	cmp.w #-24576,%d7
	blt.s 7f
	move.l (%a2,%d7.w*4),%a0
	bra.s 8f
7:	move.l (%a1,%d7.w*4),%a0
8:	move #0,%ccr
	jmp (%a0)

rt_dispatch_d7:
	cmp.w #-24576,%d7
	blt.s 7f
	move.l (%a2,%d7.w*4),%a0
	jmp (%a0)
7:	move.l (%a1,%d7.w*4),%a0
	jmp (%a0)

| ---- checkpoint logging -------------------------------------------------------------
rt_event:
	moveq #0,%d6
rt_event_f:
	movem.l %d0-%d7/%a0-%a1,-(%sp)
	move.w %d7,(ev_pc)
	move.w %d6,(ev_ccr)
	move.l (logptr),%a0
	move.w %d7,(%a0)+
	move.b %d0,(%a0)+
	move.b %d1,(%a0)+
	move.b %d2,(%a0)+
	move.b %d3,(%a0)+
	tst.b %d5
	sne %d7
	and.b #1,%d7
	move.b %d7,(%a0)+
	tst.b %d4
	sne %d7
	and.b #1,%d7
	move.b %d7,(%a0)+
	move.l %a0,(logptr)
	bsr ramsum
	move.l (logptr),%a0
	move.w %d6,(%a0)+
	move.w %a6,%d7
	add.w #30000,%d7
	move.w %d7,(%a0)+
	move.l %a0,(logptr)
	movea.w #-30000,%a6
	addq.l #1,(ckcount)
	cmp.l #MAXCK,(ckcount)
	beq rt_finish
	subq.l #1,(nmi_cd)
	bne.s 1f
	move.l #11,(nmi_cd)
	movem.l (%sp)+,%d0-%d7/%a0-%a1
	addq.l #4,%sp
	bra nmi_deliver
1:	movem.l (%sp)+,%d0-%d7/%a0-%a1
	rts

nmi_deliver:
	move.w (ev_ccr),%d6
	moveq #0x20,%d7
	btst #3,%d6
	beq.s 1f
	or.b #0x80,%d7
1:	btst #2,%d6
	beq.s 2f
	or.b #2,%d7
2:	tst.b %d4
	beq.s 3f
	or.b #0x40,%d7
3:	tst.b %d5
	beq.s 4f
	or.b #1,%d7
4:	move.w (ev_pc),%d6
	ror.w #8,%d6
	move.b %d6,(0x100,%a5,%d3.w)
	subq.b #1,%d3
	ror.w #8,%d6
	move.b %d6,(0x100,%a5,%d3.w)
	subq.b #1,%d3
	move.b %d7,(0x100,%a5,%d3.w)
	subq.b #1,%d3
	move.l #NMIPC,%d7
	bra rt_dispatch_d7

| ---- RTI / PHP / PLP ---------------------------------------------------------------------
rt_rti:
	addq.b #1,%d3
	move.b (0x100,%a5,%d3.w),%d7
	move.b %d7,(rti_p)
	moveq #0,%d4
	btst #6,%d7
	beq.s 1f
	st %d4
1:	moveq #0,%d5
	btst #0,%d7
	beq.s 2f
	st %d5
2:	addq.b #1,%d3
	moveq #0,%d6
	move.b (0x100,%a5,%d3.w),%d6
	addq.b #1,%d3
	moveq #0,%d7
	move.b (0x100,%a5,%d3.w),%d7
	lsl.w #8,%d7
	or.w %d6,%d7
	cmp.w #-24576,%d7
	blt.s 3f
	move.l (%a2,%d7.w*4),%a0
	bra.s 4f
3:	move.l (%a1,%d7.w*4),%a0
4:	move.b (rti_p),%d7
	moveq #0,%d6
	btst #7,%d7
	beq.s 5f
	or.b #8,%d6
5:	btst #1,%d7
	beq.s 6f
	or.b #4,%d6
6:	move %d6,%ccr
	jmp (%a0)

rt_php:
	move %ccr,%d6
	moveq #0x30,%d7
	btst #3,%d6
	beq.s 1f
	or.b #0x80,%d7
1:	btst #2,%d6
	beq.s 2f
	or.b #2,%d7
2:	tst.b %d4
	beq.s 3f
	or.b #0x40,%d7
3:	tst.b %d5
	beq.s 4f
	or.b #1,%d7
4:	move.b %d7,(0x100,%a5,%d3.w)
	subq.b #1,%d3
	move %d6,%ccr
	rts

rt_plp:
	addq.b #1,%d3
	move.b (0x100,%a5,%d3.w),%d7
	moveq #0,%d4
	btst #6,%d7
	beq.s 1f
	st %d4
1:	moveq #0,%d5
	btst #0,%d7
	beq.s 2f
	st %d5
2:	moveq #0,%d6
	btst #7,%d7
	beq.s 3f
	or.b #8,%d6
3:	btst #1,%d7
	beq.s 4f
	or.b #4,%d6
4:	move %d6,%ccr
	rts

rt_brk:
rt_unsupported:
rt_undef:
	move.w %d7,(ev_pc)
	move.l (logptr),%a0
	move.w %d7,(%a0)+
	move.b %d0,(%a0)+
	move.b %d1,(%a0)+
	move.b %d2,(%a0)+
	move.b %d3,(%a0)+
	move.b #0,(%a0)+
	move.b #0,(%a0)+
	move.l %a0,(logptr)
	bsr ramsum
	move.l (logptr),%a0
	move.w %d6,(%a0)+
	move.w #0xFFFF,(%a0)+
	move.l %a0,(logptr)
rt_finish:
	move.l #4,%d0
	move.l #1,%d1
	move.l #logbuf,%d2
	move.l (logptr),%d3
	sub.l %d2,%d3
	trap #0
	move.l #1,%d0
	moveq #0,%d1
	trap #0

| ---- RAM checksum (same algorithm as the python side) -> d6.w ; preserves everything else ---
ramsum:
	movem.l %d0-%d1/%a0,-(%sp)
	moveq #0,%d6
	lea (%a5),%a0
	move.w #511,%d0
1:	move.l (%a0)+,%d1
	rol.l #1,%d6
	eor.l %d1,%d6
	dbra %d0,1b
	move.l %d6,%d1
	swap %d1
	eor.w %d1,%d6
	movem.l (%sp)+,%d0-%d1/%a0
	rts

| ---- memory helpers (preserve everything except d7 / a0) -----------------------------------
rt_io_rd:
	move.l %d6,-(%sp)
	move.l (lfsr),%d6
	move.l %d6,%d7
	lsl.l #8,%d7
	lsl.l #5,%d7
	eor.l %d7,%d6
	move.l %d6,%d7
	lsr.l #8,%d7
	lsr.l #8,%d7
	lsr.l #1,%d7
	eor.l %d7,%d6
	move.l %d6,%d7
	lsl.l #5,%d7
	eor.l %d7,%d6
	move.l %d6,(lfsr)
	lsr.l #8,%d6
	move.b %d6,%d7
	move.l (%sp)+,%d6
	tst.b %d7
	rts
rt_io_wr:
	rts
| a0 = 6502 address, d7.b = value
rt_map_wr:
	movem.l %d6/%a0,-(%sp)
	move.w %a0,%d6
	cmp.w #0xA000,%d6
	bcs.s 1f
	cmp.w #0xB000,%d6
	bcc.s 1f
	and.w #15,%d7
	lsl.w #3,%d7
	lea bank_tab,%a0
	move.l (%a0,%d7.w),%a3
	move.l 4(%a0,%d7.w),%a1
1:	movem.l (%sp)+,%d6/%a0
	rts
rt_rd8:
	move.l %d6,-(%sp)
	move.w %d7,%d6
	and.l #0xFFFF,%d6
	cmp.w #0x2000,%d6
	bcc.s 1f
	and.w #0x7FF,%d6
	move.b (%a5,%d6.l),%d7
	bra.s 9f
1:	cmp.w #0x4020,%d6
	bcc.s 2f
	move.l (%sp)+,%d6
	bra rt_io_rd
2:	cmp.w #0x8000,%d6
	bcc.s 3f
	moveq #0,%d7
	bra.s 9f
3:	cmp.w #0xA000,%d6
	bcc.s 4f
	move.b (%a3,%d6.l),%d7
	bra.s 9f
4:	move.b (%a4,%d6.l),%d7
9:	move.l (%sp)+,%d6
	tst.b %d7
	rts
rt_wr8_d0:
	move.b %d0,-(%sp)
	bra.s rt_wr8
rt_wr8_d1:
	move.b %d1,-(%sp)
	bra.s rt_wr8
rt_wr8_d2:
	move.b %d2,-(%sp)
rt_wr8:
	movem.l %d6/%a0,-(%sp)
	and.l #0xFFFF,%d7
	cmp.w #0x2000,%d7
	bcc.s 1f
	and.w #0x7FF,%d7
	move.b 8(%sp),(%a5,%d7.l)
	bra.s 9f
1:	cmp.w #0xA000,%d7
	bcs.s 9f
	cmp.w #0xB000,%d7
	bcc.s 9f
	moveq #0,%d6
	move.b 8(%sp),%d6
	and.w #15,%d6
	lsl.w #3,%d6
	lea bank_tab,%a0
	move.l (%a0,%d6.w),%a3
	move.l 4(%a0,%d6.w),%a1
9:	movem.l (%sp)+,%d6/%a0
	addq.l #2,%sp
	rts
'''

def ref_run(p, L, blk_cyc, checks, unsup, rom, ram, pc, bank, regs, maxck):
    """reference run; returns (expected records, executed keys, nmi count)"""
    vec = lambda a: rom[0x20000 - 0x6000 + a - 0xA000] | rom[0x20000 - 0x6000 + a + 1 - 0xA000] << 8
    NMI = vec(0xFFFA)
    cpu = Mock(rom, bytearray(ram)); cpu.pc = pc; cpu.bank = bank
    cpu.a, cpu.x, cpu.y, cpu.s, cpu.C, cpu.V = regs
    expect = []; acc = 0; cd = NMI_EVERY; nck = 0; seen = set(); nnmi = 0
    while True:
        a = cpu.pc
        key = (-1, a) if a >= 0xA000 else (cpu.bank, a)
        if key not in p.ins or key in unsup:
            expect.append((a, cpu.a, cpu.x, cpu.y, cpu.s, 0, 0, ramsum(cpu.ram), 0xFFFF, 0)); break
        i = p.ins[key]; seen.add(key)
        if key in L: acc += blk_cyc.get(key, 0)
        injected = stop = False
        for tag, wrap in checks.get(key, []):
            live = i.live_in
            expect.append((a, cpu.a, cpu.x, cpu.y, cpu.s, cpu.C if 'C' in live else None, cpu.V if 'V' in live else None, ramsum(cpu.ram), acc, 1))
            acc = 0; nck += 1
            if nck == maxck: stop = True; break
            cd -= 1
            if cd == 0:
                cd = NMI_EVERY
                p_ = cpu.getp(0) if wrap else (cpu.getp(0) & ~0x82)
                cpu.push(a >> 8); cpu.push(a & 255); cpu.push(p_); cpu.pc = NMI; injected = True; nnmi += 1; break
        if stop: break
        if injected: continue
        cpu.step()
    return expect, seen, nnmi

def compare(out, expect, tag):
    n_ck = len(expect); bad = 0; first = None
    for n in range(max(n_ck, len(out) // 12)):
        if n >= n_ck or (n + 1) * 12 > len(out):
            print('%s: record count mismatch at %d (ref %d, got %d)' % (tag, n, n_ck, len(out) // 12)); return 1, n
        pc, A, X, Y, S, C, V, rs, dl = struct.unpack('>HBBBBBBHH', out[n * 12:(n + 1) * 12])
        e = expect[n]; errs = []
        if pc != e[0]: errs.append('pc %04X exp %04X' % (pc, e[0]))
        for nm, g, x in zip('AXYS', (A, X, Y, S), e[1:5]):
            if g != x: errs.append('%s %02X exp %02X' % (nm, g, x))
        if e[5] is not None and C != e[5]: errs.append('C %d exp %d' % (C, e[5]))
        if e[6] is not None and V != e[6]: errs.append('V %d exp %d' % (V, e[6]))
        if rs != e[7]: errs.append('RAM checksum')
        if e[9] and dl != e[8]: errs.append('cycle delta %d exp %d' % (dl, e[8]))
        if errs:
            print('%s: MISMATCH at checkpoint %d: %s' % (tag, n, '; '.join(errs))); return 1, n
    return 0, None

def main():
    prg = sys.argv[1]; maxck = int(sys.argv[3]) if len(sys.argv) > 3 else 3000; nruns = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    fuzz = len(sys.argv) > 5 and sys.argv[5] == 'fuzz'
    rom = open(prg, 'rb').read()[:0x20000]
    seedset, _ = load_seeds(sys.argv[2])
    outdir = tempfile.mkdtemp(prefix='flow_', dir=os.environ.get('T6502_TMP', '/tmp'))
    p = translate(rom, seedset, follow=True)
    mark_back_edges(p)
    seg, stubs, keys, st, L, blk_cyc, checks, unsup = emit_program(p, seedset)
    print('translated %d instructions (%d unsupported -> stub), %d check sites' % (len(keys), len(unsup), len(checks)))
    open(os.path.join(outdir, 'rom.bin'), 'wb').write(rom)
    vec = lambda a: rom[0x20000 - 0x6000 + a - 0xA000] | rom[0x20000 - 0x6000 + a + 1 - 0xA000] << 8
    RESET, NMI = vec(0xFFFC), vec(0xFFFA)
    tabs = ['\t.data\n\t.align 4\nfixtab:']
    for a in range(0xA000, 0x10000): tabs.append('\t.long %s' % ('F_%04X' % a if (-1, a) in p.ins else 'rt_undef'))
    tabs.append('fixtab_end:')
    for b in range(16):
        tabs.append('wintab%d:' % b)
        for a in range(0x8000, 0xA000): tabs.append('\t.long %s' % ('W%X_%04X' % (b, a) if (b, a) in p.ins else 'rt_undef'))
    tabs.append('bank_tab:')
    for b in range(16): tabs.append('\t.long rom+%d, wintab%d+131072' % (b * 0x2000 - 0x8000, b))
    rt = RUNTIME.replace('NMIPC', str(NMI)).replace('MAXCK', str(maxck))
    import re as _re
    with open(os.path.join(outdir, 'flow.s'), 'w') as f:
        f.write(rt)
        for k in keys: f.write('\n'.join(seg[k]) + '\n')
        text = '\n'.join('\n'.join(seg[k]) for k in keys)
        defined = set(_re.findall(r'^(\w+):', text, _re.M)); used = set(_re.findall(r'\b(?:jb\w+|jbra)\s+((?:W[0-9A-F]_|F_)[0-9A-F]{4})\b', text))
        for lbl in sorted(used - defined): f.write('%s:\tmove.w #%d,%%d7\n\tjbra rt_undef\n' % (lbl, int(lbl[-4:], 16)))
        for tag, a, wrap, ret in stubs: f.write('ev_%s:\tmove.w #%d,%%d7\n\tjsr %s\n\tjbra %s\n' % (tag, a, 'rt_event_f' if wrap else 'rt_event', ret))
        f.write('\n'.join(tabs) + '\n')
        f.write('\t.align 4\nev_pc:\t.word 0\nev_ccr:\t.word 0\nrti_p:\t.byte 0\n\t.align 4\nlogptr:\t.long 0\nckcount:\t.long 0\nnmi_cd:\t.long %d\nlfsr:\t.long 0x12345678\nramimg:\n\t.space 2064\nrom:\n\t.incbin "rom.bin"\n\t.bss\n\t.align 4\nlogbuf:\t.space %d\n' % (NMI_EVERY, 16 * (maxck + 8)))
    sp = lambda cmd: subprocess.run(cmd, cwd=outdir, capture_output=True, text=True)
    r = sp(['m68k-linux-gnu-as', '-m68020', '-o', 'flow.o', 'flow.s'])
    if r.returncode: print(r.stderr[:3000]); return 2
    r = sp(['m68k-linux-gnu-ld', '-o', 'flow', 'flow.o'])
    if r.returncode: print(r.stderr[:2000]); return 2
    usable = [k for k in keys if k not in unsup]
    allseen = set(); tot_ck = 0; bad = 0; nmi_tot = 0
    for run in range(nruns):
        rng = random.Random(run + 1)
        ram = bytearray(rng.randrange(256) for _ in range(0x800))
        if fuzz and run > 0:
            k = rng.choice(usable); pc = k[1]; bank = k[0] if k[0] >= 0 else rng.randrange(16)
            regs = (rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.randrange(2), rng.randrange(2))
        else:
            pc, bank, regs = RESET, 0, (0, 0, 0, 0xFD, 0, 0)
        expect, seen, nn = ref_run(p, L, blk_cyc, checks, unsup, rom, ram, pc, bank, regs, maxck)
        allseen |= seen; nmi_tot += nn; tot_ck += len(expect)
        blob = bytes(ram) + bytes([regs[0], regs[1], regs[2], regs[3], bank, 0xFF if regs[4] else 0, 0xFF if regs[5] else 0, 0]) + struct.pack('>H', pc) + b'\0' * 6
        r = subprocess.run(['qemu-m68k', './flow'], cwd=outdir, input=blob, capture_output=True, timeout=600)
        rc, at = compare(r.stdout, expect, 'run %d (start %04X bank %d)' % (run, pc, bank))
        bad += rc
    mn = collections_count(p, allseen)
    print('runs %d, checkpoints compared %d, failing runs %d | coverage: %d distinct instructions executed of %d translated, %d/%d mnemonics, banks %s, NMIs injected %d' % (
        nruns, tot_ck, bad, len(allseen), len(keys), len(mn), len(set(i.mn for i in p.ins.values())), sorted(set(k[0] for k in allseen if k[0] >= 0)), nmi_tot))
    print('executed mnemonics:', ' '.join(sorted(mn)))
    print('work dir', outdir)
    return 1 if bad else 0

def collections_count(p, seen): return set(p.ins[k].mn for k in seen)

if __name__ == '__main__':
    sys.exit(main())
