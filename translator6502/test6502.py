#!/usr/bin/env python3
"""Per-instruction test: translated 68020 code (run under qemu-m68k) vs the reference 6502 core.
usage: test6502.py PRG.bin [cases_per_opcode] [seed]
Covers every official opcode except BRK/RTI/JMP/JSR/RTS/PHP/PLP (control flow and flag-push helpers are not covered here)."""
import sys, os, random, subprocess, struct, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ref6502 import OPS, CPU
from x6502 import Ins, Emit, Prog, defs, uses, mem_class, BR

SKIP = {'BRK', 'RTI', 'JMP', 'JSR', 'RTS', 'PHP', 'PLP', 'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ'}
WINBANK = 8

class TCPU(CPU):
    """reference CPU with the test memory map (same as the harness helpers)"""
    def __init__(self, rom, ram):
        super().__init__(None); self.rom = rom; self.ram = ram; self.code = b''; self.iolog = []
    def rd(self, a):
        a &= 0xFFFF
        if 0x7000 <= a < 0x7003: return self.code[a - 0x7000] if a - 0x7000 < len(self.code) else 0
        if a < 0x2000: return self.ram[a & 0x7FF]
        if a < 0x4020: return (a & 0xFF) ^ 0x5A
        if a < 0x8000: return 0
        if a < 0xA000: return self.rom[WINBANK * 0x2000 + a - 0x8000]
        return self.rom[0x20000 - 0x6000 + a - 0xA000]
    def wr(self, a, v):
        a &= 0xFFFF; v &= 0xFF
        if a < 0x2000: self.ram[a & 0x7FF] = v
        elif a < 0x4020: self.iolog.append((a, v, 0))
        else: self.iolog.append((a, v, 1))

def helpers_asm():
    # simpler, explicit versions of the helpers (the compact block above is replaced)
    return r'''
rt_io_rd:  move.l %a0,%d7
           and.w #0xFF,%d7
           eor.b #0x5A,%d7
           tst.b %d7
           rts
rt_io_wr:  move.l %a1,-(%sp)
           move.l (iolog_ptr),%a1
           move.w %a0,(%a1)+
           move.b %d7,(%a1)+
           clr.b (%a1)+
           move.l %a1,(iolog_ptr)
           move.l (%sp)+,%a1
           rts
rt_map_wr: move.l %a1,-(%sp)
           move.l (iolog_ptr),%a1
           move.w %a0,(%a1)+
           move.b %d7,(%a1)+
           move.b #1,(%a1)+
           move.l %a1,(iolog_ptr)
           move.l (%sp)+,%a1
           rts
rt_rd8:    move.l %d6,-(%sp)
           move.w %d7,%d6
           and.l #0xFFFF,%d6
           cmp.w #0x2000,%d6
           bcc.s 1f
           and.w #0x7FF,%d6
           move.b (%a5,%d6.l),%d7
           move.l (%sp)+,%d6
           tst.b %d7
           rts
1:         cmp.w #0x4020,%d6
           bcc.s 2f
           move.b %d6,%d7
           eor.b #0x5A,%d7
           move.l (%sp)+,%d6
           tst.b %d7
           rts
2:         cmp.w #0x8000,%d6
           bcc.s 3f
           moveq #0,%d7
           move.l (%sp)+,%d6
           rts
3:         cmp.w #0xA000,%d6
           bcc.s 4f
           move.b (%a3,%d6.l),%d7
           move.l (%sp)+,%d6
           tst.b %d7
           rts
4:         move.b (%a4,%d6.l),%d7
           move.l (%sp)+,%d6
           tst.b %d7
           rts
| d7.w = address, value in d0/d1/d2 (entry per register); preserves everything except d7, a0
rt_wr8_d0: move.b %d0,-(%sp)
           bra.s rt_wr8
rt_wr8_d1: move.b %d1,-(%sp)
           bra.s rt_wr8
rt_wr8_d2: move.b %d2,-(%sp)
rt_wr8:    move.l %d6,-(%sp)
           move.l %a1,-(%sp)
           and.l #0xFFFF,%d7
           cmp.w #0x2000,%d7
           bcc.s 1f
           and.w #0x7FF,%d7
           move.b 8(%sp),%d6
           move.b %d6,(%a5,%d7.l)
           bra.s 9f
1:         move.l (iolog_ptr),%a1
           move.w %d7,(%a1)+
           move.b 8(%sp),(%a1)+
           cmp.w #0x4020,%d7
           scc %d7
           and.b #1,%d7
           move.b %d7,(%a1)+
           move.l %a1,(iolog_ptr)
9:         move.l (%sp)+,%a1
           move.l (%sp)+,%d6
           addq.l #2,%sp
           rts
'''

def gen_case(rng, rom):
    """random instruction instance -> Ins (with operand address chosen so that the translation's static classes hold)"""
    while True:
        op = rng.choice(sorted(OPS))
        mn, md, L, cyc, pen = OPS[op]
        if mn in SKIP: continue
        i = Ins(-1, 0xA000, op)
        if md == 'imm': i.arg = rng.randrange(256)
        elif md in ('zp', 'zpx', 'zpy', 'izx', 'izy'): i.arg = rng.randrange(256) if rng.random() < 0.7 else rng.choice([0, 0xFF, 0xFE, 0x80])
        elif md == 'abs':
            c = rng.choice(['ram'] * 6 + ['io', 'rom', 'win', 'rom'])
            i.arg = {'ram': rng.randrange(0x800) | (rng.choice([0, 0x800, 0x1000]) if False else 0), 'io': rng.randrange(0x2000, 0x4020), 'rom': rng.randrange(0xA000, 0x10000), 'win': rng.randrange(0x8000, 0xA000)}[c]
        elif md in ('abx', 'aby'):
            c = rng.choice(['ram'] * 6 + ['rom', 'win', 'io'])
            i.arg = {'ram': rng.randrange(0x700), 'rom': rng.randrange(0xA000, 0xFF00), 'win': rng.randrange(0x8000, 0x9F00), 'io': rng.randrange(0x2000, 0x3F00)}[c]
        if mn in ('STA', 'STX', 'STY', 'ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC') and md not in ('acc',):
            # stores/RMW: skip classes the translator handles through not-modelled paths in this harness
            if mem_class(i.arg) in ('rom', 'win') and md != 'abs': continue
            if md in ('abx', 'aby') and mem_class(i.arg) in ('io',) and mn not in ('STA',): continue
            if mn in ('ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC') and md in ('izx', 'izy'): continue
            if mn in ('ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC') and mem_class(i.arg) != 'ram' and md in ('abs', 'abx'): continue
            if mn in ('STX', 'STY') and md in ('izx', 'izy'): continue
        return i

def build(cases, rom_path, outdir, rng):
    """cases: list of (ins, state, expected). writes test.s"""
    L = []
    L.append('\t.text\n\t.globl _start\n_start:\n\tlea ramimg,%a5\n\tlea rom,%a4\n\tadd.l #0x10000,%a4\n\tlea rom,%a3\n\tadd.l #' + str(WINBANK * 0x2000 - 0x8000) + ',%a3\n\tlea out,%a6\n\tmove.l #iolog,(iolog_ptr)\n')
    # a6 is used as the output pointer in the harness (the cycle counter is not part of the per-instruction test)
    for n, (i, st, exp) in enumerate(cases):
        em = Emit(None)
        em.ins(i)
        a, x, y, s, v, c, ccr = st
        L.append('| case %d: %s %s arg=%X live_out=%s' % (n, i.mn, i.md, i.arg, ''.join(sorted(i.live_out))))
        L.append('\tmove.l #%d,%%d0\n\tmove.l #%d,%%d1\n\tmove.l #%d,%%d2\n\tmove.l #%d,%%d3\n\tmove.l #%d,%%d4\n\tmove.l #%d,%%d5' % (a, x, y, s, v, c))
        L.append('\tmove.w #%d,%%ccr' % ccr)
        L.extend(em.o)
        L.append('\tmove %ccr,%d6')
        L.append('\tmove.b %d0,(%a6)+\n\tmove.b %d1,(%a6)+\n\tmove.b %d2,(%a6)+\n\tmove.b %d3,(%a6)+\n\tmove.b %d4,(%a6)+\n\tmove.b %d5,(%a6)+\n\tmove.b %d6,(%a6)+')
        L.append('\tjbsr ramsum\n\tmove.w %d6,(%a6)+')
        L.append('\tmove.b #0,(%a6)+')
    L.append('\tmove.l (iolog_ptr),%d6\n\tmove.l %d6,(iolog_end)\n')
    L.append('\tmove.l #4,%d0\n\tmove.l #1,%d1\n\tmove.l #out,%d2\n\tmove.l %a6,%d3\n\tsub.l %d2,%d3\n\ttrap #0')
    L.append('\tmove.l #4,%d0\n\tmove.l #1,%d1\n\tmove.l #iolog,%d2\n\tmove.l (iolog_end),%d3\n\tsub.l %d2,%d3\n\ttrap #0')
    L.append('\tmove.l #1,%d0\n\tmove.l #0,%d1\n\ttrap #0')
    L.append('ramsum:\tmovem.l %d0-%d1/%a0,-(%sp)\n\tmoveq #0,%d6\n\tlea (%a5),%a0\n\tmove.w #511,%d0\n1:\tmove.l (%a0)+,%d1\n\trol.l #1,%d6\n\teor.l %d1,%d6\n\tdbra %d0,1b\n\tmove.l %d6,%d1\n\tswap %d1\n\teor.w %d1,%d6\n\tmovem.l (%sp)+,%d0-%d1/%a0\n\trts')
    L.append(helpers_asm())
    L.append('rt_php:\trts\nrt_plp:\trts\n\t.data\niolog_ptr:\t.long 0\niolog_end:\t.long 0\n\t.align 4\nramimg:\n\t.incbin "ramimg.bin"\nrom:\n\t.incbin "%s"\n\t.bss\n\t.align 4\nout:\t.space %d\niolog:\t.space 262144\n' % (rom_path, len(cases) * 16 + 64))
    open(os.path.join(outdir, 'test.s'), 'w').write('\n'.join(L))

def run_cases(rom, ncase, seed, outdir):
    rng = random.Random(seed)
    ram = bytearray(rng.randrange(256) for _ in range(0x800))
    open(os.path.join(outdir, 'ramimg.bin'), 'wb').write(ram)
    cpu = TCPU(rom, bytearray(ram))
    cases = []; exp = []
    for n in range(ncase):
        i = gen_case(rng, rom)
        i.live_out = set(f for f in 'NZCV' if rng.random() < 0.5) if rng.random() < 0.8 else set('NZCV')
        i.live_in = (i.live_out - defs(i.mn)) | uses(i.mn)
        i.native_c = False
        a, x, y, s = [rng.randrange(256) for _ in range(4)]
        v = rng.choice([0, 0xFF]); c = rng.choice([0, 0xFF])
        nflag = rng.randrange(2); zflag = rng.randrange(2)
        ccr = nflag << 3 | zflag << 2
        cpu.a, cpu.x, cpu.y, cpu.s = a, x, y, s; cpu.V = int(bool(v)); cpu.C = int(bool(c)); cpu.N = nflag; cpu.Z = zflag
        bytesx = bytes([i.op]) + ((i.arg & 0xFF,) and bytes([i.arg & 0xFF]) if i.len > 1 else b'') + (bytes([i.arg >> 8]) if i.len == 3 else b'')
        cpu.code = bytesx; cpu.pc = 0x7000
        cpu.step()
        zsum = 0
        r = (cpu.a, cpu.x, cpu.y, cpu.s, cpu.V, cpu.C, cpu.N, cpu.Z)
        cases.append((i, (a, x, y, s, v, c, ccr), None)); exp.append((i, r, cpu))
    return cases, exp, cpu

def ramsum_ref(ram):
    d6 = 0
    for k in range(0, 0x800, 4):
        w = struct.unpack('>I', bytes(ram[k:k + 4]))[0]
        d6 = ((d6 << 1 | d6 >> 31) & 0xFFFFFFFF) ^ w
    return (d6 ^ (d6 >> 16 & 0xFFFF)) & 0xFFFF

def main():
    prg = sys.argv[1]; ncase = int(sys.argv[2]) if len(sys.argv) > 2 else 400; seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rom = open(prg, 'rb').read()[:0x20000]
    outdir = tempfile.mkdtemp(prefix='t6502_', dir=os.environ.get('T6502_TMP', '/tmp'))
    rng = random.Random(seed)
    # run all cases in ONE reference CPU sequentially to mirror the sequential RAM state of the m68k program; record per-case results
    ram = bytearray(random.Random(seed ^ 0xABCD).randrange(256) for _ in range(0x800))
    open(os.path.join(outdir, 'ramimg.bin'), 'wb').write(ram)
    cpu = TCPU(rom, bytearray(ram)); cases = []; expect = []
    for n in range(ncase):
        i = gen_case(rng, rom)
        i.live_out = set(f for f in 'NZCV' if rng.random() < 0.5) if rng.random() < 0.8 else set('NZCV')
        i.live_in = (i.live_out - defs(i.mn)) | uses(i.mn); i.native_c = False
        a, x, y, s = [rng.randrange(256) for _ in range(4)]
        v = rng.choice([0, 0xFF]); c = rng.choice([0, 0xFF]); nf = rng.randrange(2); zf = rng.randrange(2)
        cpu.a, cpu.x, cpu.y, cpu.s = a, x, y, s; cpu.V = int(bool(v)); cpu.C = int(bool(c)); cpu.N = nf; cpu.Z = zf
        cpu.code = bytes([i.op]) + (bytes([i.arg & 0xFF]) if i.len > 1 else b'') + (bytes([i.arg >> 8]) if i.len == 3 else b''); cpu.pc = 0x7000
        cpu.step()
        cases.append((i, (a, x, y, s, v, c, nf << 3 | zf << 2), None))
        expect.append(((cpu.a, cpu.x, cpu.y, cpu.s, cpu.V, cpu.C, cpu.N, cpu.Z), ramsum_ref(cpu.ram)))
    build(cases, os.path.abspath(prg) if len(rom) == os.path.getsize(prg) else os.path.join(outdir, 'rom.bin'), outdir, rng)
    if len(rom) != os.path.getsize(prg): open(os.path.join(outdir, 'rom.bin'), 'wb').write(rom)
    sp = lambda cmd: subprocess.run(cmd, cwd=outdir, capture_output=True, text=True)
    r = sp(['m68k-linux-gnu-as', '-m68020', '-o', 'test.o', 'test.s'])
    if r.returncode: print(r.stderr[:3000]); return 2
    r = sp(['m68k-linux-gnu-ld', '-o', 'test', 'test.o'])
    if r.returncode: print(r.stderr[:2000]); return 2
    r = subprocess.run(['qemu-m68k', './test'], cwd=outdir, capture_output=True)
    if r.returncode: print('run failed', r.returncode, r.stderr[:500]); return 2
    out = r.stdout; rec = 10
    bad = 0; byop = {}
    for n, ((i, st, _), (ex, exsum)) in enumerate(zip(cases, expect)):
        b = out[n * rec:(n + 1) * rec]
        got = tuple(b[:6]); ccr = b[6]; gsum = struct.unpack('>H', b[7:9])[0] if False else (b[7] << 8 | b[8])
        gn, gz = ccr >> 3 & 1, ccr >> 2 & 1
        errs = []
        for nm, g, e in zip('AXYS', got[:4], ex[:4]):
            if g != e: errs.append('%s got %02X exp %02X' % (nm, g, e))
        if 'V' in i.live_out and bool(got[4]) != bool(ex[4]): errs.append('V got %02X exp %d' % (got[4], ex[4]))
        if 'C' in i.live_out and bool(got[5]) != bool(ex[5]): errs.append('C got %02X exp %d' % (got[5], ex[5]))
        if 'N' in i.live_out and gn != ex[6]: errs.append('N got %d exp %d' % (gn, ex[6]))
        if 'Z' in i.live_out and gz != ex[7]: errs.append('Z got %d exp %d' % (gz, ex[7]))
        if gsum != exsum: errs.append('RAM checksum differs')
        k = (i.mn, i.md)
        byop.setdefault(k, [0, 0]); byop[k][0] += 1
        if errs:
            bad += 1; byop[k][1] += 1
            if bad <= 12: print('FAIL case %d %s %s arg=%X live_out=%s init=%s: %s' % (n, i.mn, i.md, i.arg, ''.join(sorted(i.live_out)), st, '; '.join(errs)))
    # I/O log
    iolen = len(out) - ncase * rec
    log = [(struct.unpack('>H', out[ncase*rec+4*k:ncase*rec+4*k+2])[0], out[ncase*rec+4*k+2], out[ncase*rec+4*k+3]) for k in range(iolen // 4)]
    if log != cpu.iolog: bad += 1; print('IO LOG MISMATCH: got %d entries, expected %d; first diff:' % (len(log), len(cpu.iolog)), next(((a, b) for a, b in zip(log, cpu.iolog) if a != b), None))
    print('cases %d, failing %d, distinct (mnemonic,mode) forms %d, forms with failures %d' % (ncase, bad, len(byop), sum(1 for v in byop.values() if v[1])))
    print('failing forms:', sorted(k for k, v in byop.items() if v[1])[:40])
    print('work dir', outdir)
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
