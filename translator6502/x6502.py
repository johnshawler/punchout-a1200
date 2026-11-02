#!/usr/bin/env python3
"""6502 -> 68020 (GNU as, m68k) translator: front end (decode, discovery, flag liveness) and emitter.  PROTOTYPE.

68020 register map
  d0=A d1=X d2=Y d3=S      bytes, upper 24 bits always zero
  d4=V  d5=C               byte flags, 0 = clear, non-zero = set (C: 0/FF).  N and Z live in the 68k CCR between producer and consumer only.
  d6=CCR save / scratch    d7=operand / address scratch
  a0=helper address arg    a1=window dispatch table end   a2=fixed dispatch table end
  a3=window base-$8000     a4=fixed ROM base-$A000        a5=6502 RAM ($0000-$07FF)    a6=cycle counter (negative; event when > 0)
Dispatch: a 16-bit 6502 address in d7.w indexes a table of native entry points, scaled x4, sign-extended index (fixed: $A000-$FFFF
= -$6000..-1, window: $8000-$9FFF = -$8000..-$6001), so a2/a1 point at the END of their tables.
Flags: only flags that are live (see liveness()) are materialised.  D and I are ignored (2A03 has no decimal mode).
Assumption checked by the test harness / traces: N and Z are not live across JSR/RTS/JMP.
"""
import sys, collections, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ref6502 import OPS

ALL = set('NZCV')
BR = {'BPL': 'N', 'BMI': 'N', 'BVC': 'V', 'BVS': 'V', 'BCC': 'C', 'BCS': 'C', 'BNE': 'Z', 'BEQ': 'Z'}
def uses(mn):
    if mn in BR: return {BR[mn]}
    if mn in ('ADC', 'SBC', 'ROL', 'ROR'): return {'C'}
    if mn == 'PHP': return set(ALL)
    return set()
def defs(mn):
    if mn in ('ADC', 'SBC'): return set('NZCV')
    if mn in ('ASL', 'LSR', 'ROL', 'ROR', 'CMP', 'CPX', 'CPY'): return set('NZC')
    if mn in ('LDA', 'LDX', 'LDY', 'AND', 'ORA', 'EOR', 'INC', 'DEC', 'INX', 'INY', 'DEX', 'DEY', 'TAX', 'TAY', 'TXA', 'TYA', 'TSX', 'PLA'): return set('NZ')
    if mn == 'BIT': return set('NZV')
    if mn in ('CLC', 'SEC'): return {'C'}
    if mn == 'CLV': return {'V'}
    if mn in ('PLP', 'RTI'): return set(ALL)
    return set()

class Ins:
    def __init__(self, bank, addr, op):
        mn, md, L, cyc, pen = OPS[op]
        self.bank = bank; self.addr = addr; self.op = op; self.mn = mn; self.md = md; self.len = L; self.cyc = cyc
        self.arg = 0; self.succ = []; self.live_in = set(); self.live_out = set(); self.native_c = False; self.label = False
    def __repr__(self): return '%02X:%04X %s %s %X' % (self.bank & 0xFF, self.addr, self.mn, self.md, self.arg)

def mem_class(a):
    if a < 0x2000: return 'ram'
    if a < 0x4020: return 'io'
    if a < 0x8000: return 'open'
    if a < 0xA000: return 'win'
    return 'rom'

class Prog:
    """Decoded code.  Keys are (bank, addr); bank = -1 for the fixed area $A000-$FFFF."""
    def __init__(self, rom): self.rom = rom; self.ins = {}
    def byte(self, bank, a): return self.rom[0x20000 - 0x6000 + a - 0xA000] if a >= 0xA000 else self.rom[bank * 0x2000 + a - 0x8000]
    def key(self, bank, a): return (-1, a) if a >= 0xA000 else (bank, a)
    def decode(self, bank, a):
        k = self.key(bank, a)
        if k in self.ins: return self.ins[k]
        op = self.byte(bank, a)
        if op not in OPS: return None
        i = Ins(k[0], a, op)
        if i.len >= 2: i.arg = self.byte(bank, a + 1)
        if i.len == 3: i.arg |= self.byte(bank, a + 2) << 8
        self.ins[k] = i; return i
    def discover(self, seeds, follow=True):
        """recursive descent from (bank, addr) seeds; returns list of indirect JMP sites"""
        st = list(seeds); ind = []; done = set()
        if not follow:
            for bank, a in seeds:
                i = self.decode(bank, a)
                if i is None: continue
                nxt = a + i.len; i.succ = []
                if i.md == 'rel':
                    off = i.arg - 256 if i.arg > 127 else i.arg; i.succ = [nxt, (nxt + off) & 0xFFFF]
                elif i.mn == 'JMP' and i.md == 'abs': i.succ = [i.arg]
                elif i.mn in ('JMP', 'RTS', 'RTI', 'BRK'):
                    if i.mn == 'JMP': ind.append(i)
                elif i.mn == 'JSR': i.succ = [nxt]
                else: i.succ = [nxt]
            return ind
        while st:
            bank, a = st.pop()
            while True:
                if bank == -1 and a < 0xA000: break
                k = self.key(bank, a)
                if k in done: break
                i = self.decode(bank, a)
                if i is None: break
                done.add(k)
                i.label = i.label or False
                nxt = a + i.len; i.succ = []
                if i.md == 'rel':
                    off = i.arg - 256 if i.arg > 127 else i.arg; t = (nxt + off) & 0xFFFF
                    i.succ = [nxt, t]; st.append((bank, t))
                elif i.mn == 'JMP' and i.md == 'abs': i.succ = [i.arg]; st.append((bank, i.arg)); break
                elif i.mn == 'JMP': ind.append(i); break
                elif i.mn == 'JSR': i.succ = [nxt]; st.append((bank, i.arg)); st.append((bank, nxt)); break
                elif i.mn in ('RTS', 'RTI', 'BRK'): break
                else: i.succ = [nxt]
                a = nxt
        return ind

    def succ_keys(self, i):
        for s in i.succ:
            yield (-1, s) if s >= 0xA000 else ((i.bank, s) if i.bank >= 0 else (None, s))

    def liveness(self):
        """flag liveness over the CFG.  Flags cross calls: a callee's RTS keeps whatever its callers' return points need
        (interprocedural: RTS out = union of live_in at the return points of every JSR to a function that can reach that RTS)."""
        ins = self.ins
        for i in ins.values(): i.live_in = set(); i.live_out = set()
        # functions: reachable set from each JSR target, not entering callees (a JSR continues at its return point), following tail JMPs
        retpts = collections.defaultdict(set)         # target key -> return-point keys of its JSRs
        for i in ins.values():
            if i.mn == 'JSR': retpts[self.kk(i, i.arg)].add(self.kk(i, i.addr + i.len))
        rts_ret = collections.defaultdict(set)        # RTS key -> return-point keys
        for t, rps in retpts.items():
            seen = set(); st = [t]
            while st:
                k = st.pop()
                if k in seen or k not in ins: continue
                seen.add(k); i = ins[k]
                if i.mn == 'RTS': rts_ret[k] |= rps
                elif i.mn in ('RTI', 'BRK') or (i.mn == 'JMP' and i.md == 'ind'): pass
                elif i.mn == 'JSR': st.append(self.kk(i, i.addr + i.len))
                else:
                    for a in i.succ: st.append(self.kk(i, a))
        self.rts_ret = rts_ret
        changed = True
        while changed:
            changed = False
            for k, i in ins.items():
                if i.mn == 'JSR':
                    rp = ins.get(self.kk(i, i.addr + i.len)); out = set(rp.live_in) if rp is not None else set(ALL)
                    t = ins.get(self.kk(i, i.arg)); lin = (set(t.live_in) if t is not None else set(ALL)) | set('CV')
                elif i.mn == 'RTS':
                    out = set('CV')
                    for rk in rts_ret.get(k, ()):
                        rp = ins.get(rk); out |= rp.live_in if rp is not None else ALL
                    lin = set(out)
                elif i.mn in ('RTI', 'BRK') or (i.mn == 'JMP' and i.md == 'ind'):
                    out = set('CV'); lin = set(out)
                else:
                    out = set()
                    for kk_ in self.succ_keys(i):
                        j = ins.get(kk_) if kk_[0] is not None else None
                        out |= j.live_in if j is not None else ALL
                    lin = (out - defs(i.mn)) | uses(i.mn)
                if lin != i.live_in or out != i.live_out: i.live_in = lin; i.live_out = out; changed = True
        # native-C optimisation: CMP/CPX/CPY directly followed by a branch on C after which C is dead
        for i in self.ins.values():
            if i.mn in ('CMP', 'CPX', 'CPY') and i.succ:
                j = self.ins.get(self.kk(i, i.succ[0]))
                if j is not None and j.mn in ('BCC', 'BCS') and 'C' not in j.live_out: i.native_c = True

    def kk(self, i, a): return (-1, a) if a >= 0xA000 else (i.bank, a)

def lab(i_or_bank, addr=None):
    if addr is None: bank, addr = i_or_bank.bank, i_or_bank.addr
    else: bank = i_or_bank
    return ('F_%04X' % addr) if addr >= 0xA000 else ('W%X_%04X' % (bank, addr))

class Emit:
    def __init__(self, prog):
        self.p = prog; self.o = []; self.st = collections.Counter(); self.n = 0
    def I(self, s): self.o.append('\t' + s)
    def raw(self, s): self.o.append(s)

    # ---- addressing ---------------------------------------------------------
    def ea(self, i, write=False):
        """-> (pre, ea, kind); kind: imm|mem|io|open|map|dyn.  'mem' EAs are usable directly by m68k byte instructions."""
        md, a = i.md, i.arg
        if md == 'imm': return [], '#%d' % a, 'imm'
        if md == 'zp': return [], '(%d,%%a5)' % a, 'mem'
        if md in ('zpx', 'zpy'):
            r = 'd1' if md == 'zpx' else 'd2'
            return ['move.l %%%s,%%d7' % r, 'add.b #%d,%%d7' % a], '(%a5,%d7.w)', 'mem'
        if md == 'abs':
            c = mem_class(a)
            if c == 'ram': return [], '(%d,%%a5)' % (a & 0x7FF), 'mem'
            if c == 'win': return [], '(%d,%%a3)' % a, 'map' if write else 'mem'
            if c == 'rom': return [], '(%d,%%a4)' % a, 'map' if write else 'mem'
            return [], a, 'io' if c == 'io' else 'open'
        if md in ('abx', 'aby'):
            r = 'd1' if md == 'abx' else 'd2'; c = mem_class(a)
            if c == 'ram': return [], '(%d,%%a5,%%%s.w)' % (a & 0x7FF, r), 'mem'
            if write and c in ('win', 'rom'): return ['move.w #%d,%%d7' % a, 'add.w %%%s,%%d7' % r], None, 'dyn'
            if c == 'win': return [], '(%d,%%a3,%%%s.w)' % (a, r), 'mem'
            if c == 'rom': return [], '(%d,%%a4,%%%s.w)' % (a, r), 'mem'
            return ['move.w #%d,%%d7' % a, 'add.w %%%s,%%d7' % r], None, 'dyn'
        if md == 'izy':
            if a == 0xFF: return ['move.b (0,%a5),%d7', 'lsl.w #8,%d7', 'move.b (255,%a5),%d7', 'add.w %d2,%d7'], None, 'dyn'
            return ['move.w (%d,%%a5),%%d7' % a, 'ror.w #8,%d7', 'add.w %d2,%d7'], None, 'dyn'
        if md == 'izx':
            return ['move.l %d1,%d7', 'add.b #%d,%%d7' % a, 'lea (%a5,%d7.w),%a0', 'addq.b #1,%d7', 'move.b (%a5,%d7.w),%d7', 'lsl.w #8,%d7', 'move.b (%a0),%d7'], None, 'dyn'
        raise ValueError(md)

    def read_into(self, i, dst):
        pre, ea, k = self.ea(i)
        for x in pre: self.I(x)
        if k in ('imm', 'mem'): self.I('move.b %s,%s' % (ea, dst))
        elif k == 'io':
            self.I('lea (%d).w,%%a0' % ea); self.I('jsr rt_io_rd'); self.st['io_rd'] += 1
            if dst != '%d7': self.I('move.b %%d7,%s' % dst)
        elif k == 'open': self.I('moveq #0,%s' % dst)
        else:
            self.I('jsr rt_rd8'); self.st['dyn_rd'] += 1
            if dst != '%d7': self.I('move.b %%d7,%s' % dst)

    def direct_src(self, i):
        """operand as a direct m68k source EA if possible (emits its pre-code), else None"""
        if i.md in ('imm', 'zp', 'abs', 'abx', 'aby'):
            pre, ea, k = self.ea(i)
            if k in ('imm', 'mem'):
                for x in pre: self.I(x)
                return ea
        return None

    def write_from(self, i, reg):
        pre, ea, k = self.ea(i, True)
        for x in pre: self.I(x)
        if k == 'mem': self.I('move.b %%%s,%s' % (reg, ea))
        elif k == 'io':
            self.I('move.b %%%s,%%d7' % reg); self.I('lea (%d).w,%%a0' % ea); self.I('jsr rt_io_wr'); self.st['io_wr'] += 1
        elif k == 'map':
            self.I('move.b %%%s,%%d7' % reg); self.I('move.l #%d,%%a0' % i.arg); self.I('jsr rt_map_wr'); self.st['map_wr'] += 1
        elif k == 'open': pass
        else: self.I('jsr rt_wr8_%s' % reg); self.st['dyn_wr'] += 1

    # ---- single instruction ---------------------------------------------------
    def ins(self, i):
        mn, md, lo = i.mn, i.md, i.live_out
        nz = bool(lo & {'N', 'Z'})
        I = self.I
        if mn in ('LDA', 'LDX', 'LDY'): self.read_into(i, {'LDA': '%d0', 'LDX': '%d1', 'LDY': '%d2'}[mn]); return
        if mn in ('STA', 'STX', 'STY'):
            reg = {'STA': 'd0', 'STX': 'd1', 'STY': 'd2'}[mn]
            if i.live_in & {'N', 'Z'}: I('move %ccr,%d6'); self.write_from(i, reg); I('move %d6,%ccr'); self.st['ccr_wrap'] += 1
            else: self.write_from(i, reg)
            return
        if mn in ('AND', 'ORA', 'EOR'):
            m = {'AND': 'and', 'ORA': 'or', 'EOR': 'eor'}[mn]
            s = self.direct_src(i) if mn != 'EOR' else None
            if s is not None: I('%s.b %s,%%d0' % (m, s))
            else:
                self.read_into(i, '%d7'); I('%s.b %%d7,%%d0' % m)
            return
        if mn in ('ADC', 'SBC'):
            self.read_into(i, '%d7')
            I('move.b %d5,%d6')
            if mn == 'ADC': I('neg.b %d6'); I('addx.b %d7,%d0')
            else: I('not.b %d6'); I('neg.b %d6'); I('subx.b %d7,%d0')
            if 'C' in lo: I('scs %d5' if mn == 'ADC' else 'scc %d5'); self.st['mat_C'] += 1
            if 'V' in lo: I('svs %d4'); self.st['mat_V'] += 1
            if nz: I('tst.b %d0')
            return
        if mn in ('CMP', 'CPX', 'CPY'):
            r = {'CMP': '%d0', 'CPX': '%d1', 'CPY': '%d2'}[mn]
            s = self.direct_src(i)
            if s is None: self.read_into(i, '%d7'); s = '%d7'
            I('cmp.b %s,%s' % (s, r))
            if 'C' in lo and not i.native_c: I('scc %d5'); self.st['mat_C'] += 1
            return
        if mn == 'BIT':
            self.read_into(i, '%d7')
            if 'V' in lo: I('move.b %d7,%d4'); I('and.b #0x40,%d4'); self.st['mat_V'] += 1
            if nz:
                I('move.b %d7,%d6'); I('and.b %d0,%d6'); I('move %ccr,%d6'); I('and.w #4,%d6')
                I('tst.b %d7'); I('move %ccr,%d7'); I('and.w #8,%d7'); I('or.w %d7,%d6'); I('move %d6,%ccr')
            return
        if mn in ('ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC'):
            r = '%d0' if md == 'acc' else '%d7'
            if md != 'acc':
                pre, ea, k = self.ea(i, True)
                if k != 'mem': raise NotImplementedError('RMW to %s' % k)
                for x in pre: I(x)
                if pre: I('lea %s,%%a0' % ea); ea = '(%a0)'
                I('move.b %s,%%d7' % ea)
            if mn == 'INC': I('addq.b #1,%s' % r)
            elif mn == 'DEC': I('subq.b #1,%s' % r)
            else:
                if mn in ('ROL', 'ROR'): I('move.b %d5,%d6'); I('neg.b %d6')
                I({'ASL': 'asl.b #1,%s', 'LSR': 'lsr.b #1,%s', 'ROL': 'roxl.b #1,%s', 'ROR': 'roxr.b #1,%s'}[mn] % r)
                if 'C' in lo: I('scs %d5'); self.st['mat_C'] += 1
            if md != 'acc': I('move.b %%d7,%s' % ea)
            return
        t = {'TAX': ('%d0', '%d1'), 'TAY': ('%d0', '%d2'), 'TXA': ('%d1', '%d0'), 'TYA': ('%d2', '%d0'), 'TSX': ('%d3', '%d1'), 'TXS': ('%d1', '%d3')}
        if mn == 'TXS':
            if i.live_in & {'N', 'Z'}: I('move %ccr,%d6'); I('move.b %d1,%d3'); I('move %d6,%ccr'); self.st['ccr_wrap'] += 1
            else: I('move.b %d1,%d3')
            return
        if mn in t: I('move.b %s,%s' % t[mn]); return
        if mn in ('INX', 'INY', 'DEX', 'DEY'):
            I(('addq.b #1,%s' if mn[0] == 'I' else 'subq.b #1,%s') % ('%d1' if mn[2] == 'X' else '%d2')); return
        if mn == 'PHA':
            if i.live_in & {'N', 'Z'}: I('move %ccr,%d6'); I('move.b %d0,(0x100,%a5,%d3.w)'); I('subq.b #1,%d3'); I('move %d6,%ccr'); self.st['ccr_wrap'] += 1
            else: I('move.b %d0,(0x100,%a5,%d3.w)'); I('subq.b #1,%d3')
            return
        if mn == 'PLA': I('addq.b #1,%d3'); I('move.b (0x100,%a5,%d3.w),%d0'); return
        if mn == 'CLC': I('sf %d5'); return
        if mn == 'SEC': I('st %d5'); return
        if mn == 'CLV': I('sf %d4'); return
        if mn in ('SEI', 'CLI', 'SED', 'CLD', 'NOP'): return
        if mn == 'PHP': I('jsr rt_php'); self.st['php'] += 1; return
        if mn == 'PLP': I('jsr rt_plp'); self.st['plp'] += 1; return
        raise NotImplementedError(mn)

    # ---- branch condition -----------------------------------------------------
    def branch_cc(self, i, prev_native_c):
        """returns (setup_lines, 68k condition mnemonic suffix) for a taken test of this 6502 branch"""
        mn = i.mn
        if mn == 'BEQ': return [], 'eq'
        if mn == 'BNE': return [], 'ne'
        if mn == 'BMI': return [], 'mi'
        if mn == 'BPL': return [], 'pl'
        if mn in ('BCS', 'BCC'):
            if prev_native_c: return [], 'cc' if mn == 'BCS' else 'cs'       # CMP left C inverted in the 68k C flag
            return ['tst.b %d5'], 'ne' if mn == 'BCS' else 'eq'
        if mn in ('BVS', 'BVC'): return ['tst.b %d4'], 'ne' if mn == 'BVS' else 'eq'
        raise ValueError(mn)

    def dispatch(self, restore=None):
        """jump to the translated code for the 6502 address in d7.w.  restore: None, 'd6' or 'stack' = reload the CCR (N,Z) saved there before jumping"""
        rs = {None: [], 'd6': ['move %d6,%ccr'], 'stack': ['move (%sp)+,%ccr']}[restore]
        self.I('cmp.w #-24576,%d7'); self.I('blt.s 7f')
        self.I('move.l (%a2,%d7.w*4),%a0')
        for x in rs: self.I(x)
        self.I('jmp (%a0)')
        self.raw('7:'); self.I('move.l (%a1,%d7.w*4),%a0')
        for x in rs: self.I(x)
        self.I('jmp (%a0)')

def cost_text_len(lines): return len(lines)
