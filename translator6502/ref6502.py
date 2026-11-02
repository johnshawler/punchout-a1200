#!/usr/bin/env python3
"""Clean reference 6502 (official opcodes) used to test the 6502 -> 68020 translator.
Validated against Klaus Dormann's 6502_functional_test (run `ref6502.py dormann.bin`; success trap at $3469)."""
import sys

# opcode table: opcode -> (mnemonic, mode, bytes, base cycles, page-cross penalty)
OPS = {}
def _def(op, mn, mode, cyc, pen=0):
    L = {'imp': 1, 'acc': 1, 'imm': 2, 'zp': 2, 'zpx': 2, 'zpy': 2, 'abs': 3, 'abx': 3, 'aby': 3, 'ind': 3, 'izx': 2, 'izy': 2, 'rel': 2}[mode]
    OPS[op] = (mn, mode, L, cyc, pen)

_g1 = ['ORA', 'AND', 'EOR', 'ADC', 'STA', 'LDA', 'CMP', 'SBC']
_m1 = ['izx', 'zp', 'imm', 'abs', 'izy', 'zpx', 'aby', 'abx']
_c1 = {'izx': 6, 'zp': 3, 'imm': 2, 'abs': 4, 'izy': 5, 'zpx': 4, 'aby': 4, 'abx': 4}
for a, mn in enumerate(_g1):
    for b, md in enumerate(_m1):
        if mn == 'STA' and md == 'imm': continue
        c = _c1[md]; pen = 1 if md in ('izy', 'aby', 'abx') and mn != 'STA' else 0
        if mn == 'STA' and md in ('izy', 'aby', 'abx'): c = 6 if md == 'izy' else 5
        _def(a << 5 | b << 2 | 1, mn, md, c, pen)
_g2 = ['ASL', 'ROL', 'LSR', 'ROR', 'STX', 'LDX', 'DEC', 'INC']
for a, mn in enumerate(_g2):
    for b, md in enumerate(['imm', 'zp', 'acc', 'abs', None, 'zpx', None, 'abx']):
        if md is None or md == 'imm' and mn != 'LDX': continue
        if mn in ('STX', 'LDX') and md == 'zpx': md = 'zpy'
        if mn == 'LDX' and md == 'abx': md = 'aby'
        if mn == 'STX' and md in ('abx', 'acc'): continue
        if mn in ('LDX',) and md == 'acc': continue
        if mn in ('DEC', 'INC') and md == 'acc': continue
        if mn in ('ASL', 'ROL', 'LSR', 'ROR', 'DEC', 'INC'):
            c = {'acc': 2, 'zp': 5, 'zpx': 6, 'abs': 6, 'abx': 7}[md]; pen = 0
        else:
            c = {'imm': 2, 'zp': 3, 'zpy': 4, 'abs': 4, 'aby': 4}[md]; pen = 1 if md == 'aby' and mn == 'LDX' else 0
        _def(a << 5 | b << 2 | 2, mn, md, c, pen)
for op, mn, md, c in [(0x24, 'BIT', 'zp', 3), (0x2C, 'BIT', 'abs', 4), (0x4C, 'JMP', 'abs', 3), (0x6C, 'JMP', 'ind', 5),
                      (0x84, 'STY', 'zp', 3), (0x94, 'STY', 'zpx', 4), (0x8C, 'STY', 'abs', 4),
                      (0xA0, 'LDY', 'imm', 2), (0xA4, 'LDY', 'zp', 3), (0xB4, 'LDY', 'zpx', 4), (0xAC, 'LDY', 'abs', 4), (0xBC, 'LDY', 'abx', 4),
                      (0xC0, 'CPY', 'imm', 2), (0xC4, 'CPY', 'zp', 3), (0xCC, 'CPY', 'abs', 4),
                      (0xE0, 'CPX', 'imm', 2), (0xE4, 'CPX', 'zp', 3), (0xEC, 'CPX', 'abs', 4)]:
    _def(op, mn, md, c, 1 if op == 0xBC else 0)
for op, mn in [(0x10, 'BPL'), (0x30, 'BMI'), (0x50, 'BVC'), (0x70, 'BVS'), (0x90, 'BCC'), (0xB0, 'BCS'), (0xD0, 'BNE'), (0xF0, 'BEQ')]:
    _def(op, mn, 'rel', 2)
for op, mn, c in [(0x00, 'BRK', 7), (0x08, 'PHP', 3), (0x18, 'CLC', 2), (0x28, 'PLP', 4), (0x38, 'SEC', 2), (0x40, 'RTI', 6), (0x48, 'PHA', 3),
                  (0x58, 'CLI', 2), (0x60, 'RTS', 6), (0x68, 'PLA', 4), (0x78, 'SEI', 2), (0x88, 'DEY', 2), (0x8A, 'TXA', 2), (0x98, 'TYA', 2),
                  (0x9A, 'TXS', 2), (0xA8, 'TAY', 2), (0xAA, 'TAX', 2), (0xB8, 'CLV', 2), (0xBA, 'TSX', 2), (0xC8, 'INY', 2), (0xCA, 'DEX', 2),
                  (0xD8, 'CLD', 2), (0xE8, 'INX', 2), (0xEA, 'NOP', 2), (0xF8, 'SED', 2)]:
    _def(op, mn, 'imp', c)
_def(0x20, 'JSR', 'abs', 6)
# drop STA/STX/STY none; fix the STX zp,y / LDX forms already set by group 2

class CPU:
    def __init__(self, mem=None, decimal=False):
        self.m = mem if mem is not None else bytearray(65536)
        self.a = self.x = self.y = 0; self.s = 0xFD; self.pc = 0
        self.N = self.V = self.D = self.Z = self.C = 0; self.I = 1
        self.decimal = decimal; self.cycles = 0
    # memory hooks (overridable)
    def rd(self, a): return self.m[a & 0xFFFF]
    def wr(self, a, v): self.m[a & 0xFFFF] = v & 0xFF
    def rd16(self, a): return self.rd(a) | self.rd((a + 1) & 0xFFFF) << 8
    def getp(self, brk=0):
        return self.N << 7 | self.V << 6 | 0x20 | brk << 4 | self.D << 3 | self.I << 2 | self.Z << 1 | self.C
    def setp(self, p):
        self.N = p >> 7 & 1; self.V = p >> 6 & 1; self.D = p >> 3 & 1; self.I = p >> 2 & 1; self.Z = p >> 1 & 1; self.C = p & 1
    def push(self, v): self.wr(0x100 | self.s, v); self.s = (self.s - 1) & 0xFF
    def pull(self): self.s = (self.s + 1) & 0xFF; return self.rd(0x100 | self.s)
    def nz(self, v): self.N = v >> 7 & 1; self.Z = int(v == 0); return v
    def ea(self, md):
        pc = self.pc
        if md == 'imm': self.pc += 1; return pc, 0
        if md == 'zp': self.pc += 1; return self.rd(pc), 0
        if md == 'zpx': self.pc += 1; return (self.rd(pc) + self.x) & 0xFF, 0
        if md == 'zpy': self.pc += 1; return (self.rd(pc) + self.y) & 0xFF, 0
        if md in ('abs', 'ind'): self.pc += 2; return self.rd16(pc), 0
        if md in ('abx', 'aby'):
            b = self.rd16(pc); self.pc += 2; a = (b + (self.x if md == 'abx' else self.y)) & 0xFFFF; return a, int((a ^ b) >> 8 != 0)
        if md == 'izx':
            z = (self.rd(pc) + self.x) & 0xFF; self.pc += 1; return self.rd(z) | self.rd((z + 1) & 0xFF) << 8, 0
        if md == 'izy':
            z = self.rd(pc); self.pc += 1; b = self.rd(z) | self.rd((z + 1) & 0xFF) << 8; a = (b + self.y) & 0xFFFF; return a, int((a ^ b) >> 8 != 0)
        raise ValueError(md)
    def adc(self, v):
        a = self.a; c = self.C
        if self.decimal and self.D:
            lo = (a & 15) + (v & 15) + c
            if lo > 9: lo += 6
            hi = (a >> 4) + (v >> 4) + (lo > 15)
            self.Z = int(((a + v + c) & 0xFF) == 0)
            self.N = hi >> 3 & 1
            self.V = int((~(a ^ v) & (a ^ (hi << 4)) & 0x80) != 0)
            if hi > 9: hi += 6
            self.C = int(hi > 15); self.a = (hi << 4 | lo & 15) & 0xFF; return
        t = a + v + c
        self.V = int((~(a ^ v) & (a ^ t) & 0x80) != 0); self.C = int(t > 255); self.a = self.nz(t & 0xFF)
    def sbc(self, v):
        if self.decimal and self.D:
            a = self.a; c = self.C
            t = a - v - (1 - c)
            self.V = int(((a ^ v) & (a ^ t) & 0x80) != 0); self.C = int(t >= 0); self.nz(t & 0xFF)
            lo = (a & 15) - (v & 15) - (1 - c)
            hi = (a >> 4) - (v >> 4)
            if lo < 0: lo -= 6; hi -= 1
            if hi < 0: hi -= 6
            self.a = (hi << 4 | lo & 15) & 0xFF; return
        self.adc(v ^ 0xFF)
    def cmp(self, r, v): t = r - v; self.C = int(r >= v); self.nz(t & 0xFF)
    def step(self):
        op = self.rd(self.pc); mn, md, L, cyc, pen = OPS[op]; self.pc = (self.pc + 1) & 0xFFFF
        if md == 'rel':
            off = self.rd(self.pc); self.pc += 1; cyc = 2
            t = {'BPL': not self.N, 'BMI': self.N, 'BVC': not self.V, 'BVS': self.V, 'BCC': not self.C, 'BCS': self.C, 'BNE': not self.Z, 'BEQ': self.Z}[mn]
            if t:
                np = (self.pc + (off - 256 if off > 127 else off)) & 0xFFFF; cyc += 1 + int((np ^ self.pc) >> 8 != 0); self.pc = np
            self.cycles += cyc; return cyc
        if md in ('imp', 'acc'):
            v = None; ea = None; cross = 0
        else:
            ea, cross = self.ea(md)
            if mn in ('JMP', 'JSR') and md == 'abs': pass
        cyc += pen * cross if mn not in ('STA',) else 0
        a = self.a
        if mn == 'LDA': self.a = self.nz(self.rd(ea))
        elif mn == 'LDX': self.x = self.nz(self.rd(ea))
        elif mn == 'LDY': self.y = self.nz(self.rd(ea))
        elif mn == 'STA': self.wr(ea, self.a)
        elif mn == 'STX': self.wr(ea, self.x)
        elif mn == 'STY': self.wr(ea, self.y)
        elif mn == 'ORA': self.a = self.nz(a | self.rd(ea))
        elif mn == 'AND': self.a = self.nz(a & self.rd(ea))
        elif mn == 'EOR': self.a = self.nz(a ^ self.rd(ea))
        elif mn == 'ADC': self.adc(self.rd(ea))
        elif mn == 'SBC': self.sbc(self.rd(ea))
        elif mn == 'CMP': self.cmp(a, self.rd(ea))
        elif mn == 'CPX': self.cmp(self.x, self.rd(ea))
        elif mn == 'CPY': self.cmp(self.y, self.rd(ea))
        elif mn == 'BIT': v = self.rd(ea); self.N = v >> 7; self.V = v >> 6 & 1; self.Z = int(v & a == 0)
        elif mn in ('ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC'):
            v = a if md == 'acc' else self.rd(ea)
            if mn == 'ASL': self.C = v >> 7; v = v << 1 & 255
            elif mn == 'LSR': self.C = v & 1; v >>= 1
            elif mn == 'ROL': c = self.C; self.C = v >> 7; v = (v << 1 | c) & 255
            elif mn == 'ROR': c = self.C; self.C = v & 1; v = v >> 1 | c << 7
            elif mn == 'INC': v = (v + 1) & 255
            else: v = (v - 1) & 255
            self.nz(v)
            if md == 'acc': self.a = v
            else: self.wr(ea, v)
        elif mn == 'JMP':
            if md == 'ind': self.pc = self.rd(ea) | self.rd((ea & 0xFF00) | ((ea + 1) & 0xFF)) << 8
            else: self.pc = ea
        elif mn == 'JSR':
            r = (self.pc - 1) & 0xFFFF; self.push(r >> 8); self.push(r & 255); self.pc = ea
        elif mn == 'RTS': l = self.pull(); self.pc = ((l | self.pull() << 8) + 1) & 0xFFFF
        elif mn == 'RTI': self.setp(self.pull()); l = self.pull(); self.pc = l | self.pull() << 8
        elif mn == 'BRK':
            r = (self.pc + 1) & 0xFFFF; self.push(r >> 8); self.push(r & 255); self.push(self.getp(1)); self.I = 1; self.pc = self.rd16(0xFFFE)
        elif mn == 'PHP': self.push(self.getp(1))
        elif mn == 'PLP': self.setp(self.pull())
        elif mn == 'PHA': self.push(a)
        elif mn == 'PLA': self.a = self.nz(self.pull())
        elif mn == 'CLC': self.C = 0
        elif mn == 'SEC': self.C = 1
        elif mn == 'CLI': self.I = 0
        elif mn == 'SEI': self.I = 1
        elif mn == 'CLD': self.D = 0
        elif mn == 'SED': self.D = 1
        elif mn == 'CLV': self.V = 0
        elif mn == 'TAX': self.x = self.nz(a)
        elif mn == 'TAY': self.y = self.nz(a)
        elif mn == 'TXA': self.a = self.nz(self.x)
        elif mn == 'TYA': self.a = self.nz(self.y)
        elif mn == 'TSX': self.x = self.nz(self.s)
        elif mn == 'TXS': self.s = self.x
        elif mn == 'INX': self.x = self.nz((self.x + 1) & 255)
        elif mn == 'INY': self.y = self.nz((self.y + 1) & 255)
        elif mn == 'DEX': self.x = self.nz((self.x - 1) & 255)
        elif mn == 'DEY': self.y = self.nz((self.y - 1) & 255)
        elif mn == 'NOP': pass
        else: raise ValueError(mn)
        self.pc &= 0xFFFF
        # read-modify-write absolute,X etc. already fixed; stores with indexed modes have fixed cycle counts
        self.cycles += cyc; return cyc

if __name__ == '__main__':
    mem = bytearray(open(sys.argv[1], 'rb').read()); c = CPU(mem, decimal=True); c.pc = 0x400
    n = 0
    while True:
        pc = c.pc; c.step(); n += 1
        if c.pc == pc:
            print('trap at $%04X after %d instructions' % (pc, n), 'SUCCESS' if pc == 0x3469 else 'FAIL (test case at $0200 = %02X)' % mem[0x200]); break
        if n > 200_000_000: print('timeout'); break
