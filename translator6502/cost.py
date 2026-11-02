#!/usr/bin/env python3
"""68020 cost model for the translated code, weighted by the REAL per-frame instruction counts from the reference-emulator trace.
usage: cost.py PRG.bin trace.bin [pal|ntsc]

Timing model (documented approximation, NOT a measurement):
  exec clocks  = best-case 68020 timing (instruction cache hit, no wait states), table in BASE/EA_EXTRA below, from memory of the MC68020 manual
  + D per data access      : extra clocks for a chip-RAM data access on a stock A1200 (no fast RAM)
  + F per instruction longword fetched : extra clocks when the code is NOT in the 256-byte instruction cache (code lives in chip RAM)
Scenarios:  A = best case (D=0,F=0)   B = chip data, code cached (D=4,F=0)   C = chip data, code uncached (D=4,F=5)
Helper calls (runtime routines the translator emits calls to) are charged the constants in HELPER (68020 clocks, without the hardware model behind them).
"""
import sys, os, re, collections, statistics, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x6502 import Emit
from program import load_seeds, translate, emit_program, mark_back_edges
from ref6502 import OPS

HELPER = {'rt_io_rd': 60, 'rt_io_wr': 60, 'rt_rd8': 30, 'rt_wr8_d0': 30, 'rt_wr8_d1': 30, 'rt_wr8_d2': 30, 'rt_map_wr': 80, 'rt_php': 40, 'rt_plp': 40, 'rt_event': 0}
FUSE = {0xBFC6: ({0xBFC6, 0xBFC9, 0xBFCA}, 1500, 1.0), 0xAF41: ({0xAF41, 0xAF44, 0xAF45}, 400, 1.0)}   # fill loops: STA $2007,DEX,BNE / STA $0200,X,INX,BNE -> one fused call (cost: const + clocks per iteration)
WAIT = {0xAF0B, 0xAF0D, 0xA0B5, 0xA0B8, 0xA0BA, 0xA0BC, 0xA0BF, 0xA0C1}

def split_ops(s):
    out = []; d = 0; cur = ''
    for ch in s:
        if ch == ',' and d == 0: out.append(cur); cur = ''
        else:
            d += ch == '('; d -= ch == ')'; cur += ch
    if cur: out.append(cur)
    return [o.strip() for o in out]

def ea_info(op, size):
    """-> (extension_words, mem_access, extra_clocks, kind)"""
    if re.fullmatch(r'%[da]\d|%sp|%ccr', op): return 0, 0, 0, 'reg'
    if op.startswith('#'): return (2 if size == 'l' else 1), 0, 0, 'imm'
    if re.fullmatch(r'\(%a\d\)\+?|-\(%a\d\)', op): return 0, 1, 3, 'ind'
    if re.fullmatch(r'-?\w+\(%a\d\)|\(-?\w+,%a\d\)', op): return 1, 1, 4, 'd16'
    if '%d' in op or '.w*' in op or '.l*' in op or re.search(r'\.[wl]\)', op): return 1, 1, 6, 'idx'
    if op.endswith(').w'): return 1, 1, 4, 'abs'
    return 2, 1, 6, 'abs'

BASE = {'moveq': 2, 'move': 2, 'movea': 2, 'add': 2, 'sub': 2, 'and': 2, 'or': 2, 'eor': 2, 'cmp': 2, 'cmpa': 4, 'addx': 2, 'subx': 2, 'neg': 2, 'not': 2,
        'tst': 2, 'clr': 2, 'addq': 2, 'subq': 2, 'scc': 4, 'scs': 4, 'svs': 4, 'sf': 4, 'st': 4, 'ror': 4, 'rol': 4, 'lsl': 4, 'lsr': 4, 'asl': 4, 'roxl': 4,
        'roxr': 4, 'lea': 2, 'swap': 2, 'ext': 2, 'btst': 4, 'jbra': 6, 'jmp': 6, 'jsr': 9, 'jbgt': 4, 'jbeq': 5, 'jbne': 5, 'jbmi': 5, 'jbpl': 5, 'jbcc': 5, 'jbcs': 5,
        'blt': 5, 'rts': 10, 'lea_idx': 5}

def line_cost(line):
    """-> (exec_clocks, bytes, data_accesses, helper_clocks)"""
    line = line.split('|')[0].strip()
    if not line or line.endswith(':'): return 0, 0, 0, 0
    m = re.match(r'(\w+)(?:\.(\w))?\s*(.*)', line)
    mn, sz, rest = m.group(1), m.group(2) or 'w', m.group(3)
    ops = split_ops(rest)
    byt = 2; clk = BASE.get(mn, 4); dacc = 0; helper = 0
    if mn in ('jbra', 'jbgt', 'jbeq', 'jbne', 'jbmi', 'jbpl', 'jbcc', 'jbcs', 'blt'): return clk, 4, 0, 0
    if mn == 'jsr':
        h = ops[0]; return clk, 6, 1, HELPER.get(h, 30)
    if mn == 'jmp': return clk, 2, 0, 0
    for k, o in enumerate(ops):
        ext, mem, extra, kind = ea_info(o, sz)
        byt += 2 * ext
        if mem:
            dacc += 1; clk += extra
            if kind == 'idx' and '*' in o: clk += 2
    if mn == 'lea' and len(ops) == 2:
        clk = 5 if ea_info(ops[0], 'l')[3] == 'idx' else 3; dacc = 0
    if mn == 'moveq': byt = 2
    if mn in ('addq', 'subq') and ops and ea_info(ops[1], sz)[1]: clk += 2
    return clk, byt, dacc, helper

def main():
    rom = open(sys.argv[1], 'rb').read()[:0x20000]
    seedset, frames = load_seeds(sys.argv[2])
    mode = sys.argv[3] if len(sys.argv) > 3 else 'pal'
    p = translate(rom, seedset); mark_back_edges(p)
    seg, stubs, keys, st, _, _, _, _ = emit_program(p, seedset)
    cost = {}
    tot_bytes = 0
    for k, lines in seg.items():
        e = b = d = h = n = 0
        for l in lines:
            c = line_cost(l)
            if c[1]: n += 1
            e += c[0]; b += c[1]; d += c[2]; h += c[3]
        cost[k] = (e, b, d, h, n); tot_bytes += b
    print('translated: %d instructions, ~%d bytes of 68020 code (model estimate)' % (len(keys), tot_bytes))
    scen = {'A best case (cache hit, no waits)': (0, 0), 'B chip data D=4, code cached': (4, 0), 'C chip data D=4, code uncached F=5': (4, 5)}
    clk_hz = 14187590 if mode == 'pal' else 14318180
    fps = 50.0070 if mode == 'pal' else 60.0988
    budget = clk_hz / fps
    print('68020 clocks per frame available (%s): %d' % (mode, budget))
    FUSED_PCS = set().union(*[v[0] for v in FUSE.values()])
    resF = {s: [] for s in scen}
    res = {s: [] for s in scen}; i68 = []; hc = []; tot_all = {s: [] for s in scen}
    for fr in frames[1000:]:
        sums = {s: 0 for s in scen}; ni = 0; nh = 0
        for b, pc, c in fr:
            key = (-1, pc) if pc >= 0xA000 else (b, pc)
            if key not in cost: continue
            if pc in WAIT: continue
            e, by, d, h, n = cost[key]
            ni += n * c; nh += h * c
            for s, (D, F) in scen.items(): sums[s] += c * (e + h + D * d + F * by / 4.0)
        for s in scen: res[s].append(sums[s] + 600)       # + two scheduler events per frame (rough)
        # same frame with the two fill loops fused
        sf = {s: 0 for s in scen}
        for b, pc, c in fr:
            key = (-1, pc) if pc >= 0xA000 else (b, pc)
            if key not in cost or pc in WAIT: continue
            if pc in FUSED_PCS:
                if pc in FUSE:
                    for s in scen: sf[s] += FUSE[pc][1] + FUSE[pc][2] * c
                continue
            e, by, d, h, n = cost[key]
            for s, (D, F) in scen.items(): sf[s] += c * (e + h + D * d + F * by / 4.0)
        for s in scen: resF[s].append(sf[s] + 600)
        i68.append(ni)
    def q(x, p): return sorted(x)[min(len(x) - 1, int(len(x) * p))]
    n6502 = []
    for fr in frames[1000:]:
        n6502.append(sum(c for b, pc, c in fr if pc not in WAIT))
    print('work per frame, frames 1000-5999 (wait loops excluded): 6502 instr median %d p99 %d max %d | translated 68020 instr median %d p99 %d' % (
        statistics.median(n6502), q(n6502, .99), max(n6502), statistics.median(i68), q(i68, .99)))
    print('68020 instructions per 6502 instruction (dynamic): %.2f' % (sum(i68) / max(1, sum(n6502))))
    for s in scen:
        r = res[s]
        print('%-40s median %7d (%4.1f%% of frame)  p99 %7d (%5.1f%%)  max %7d (%5.1f%%)  frames over budget: %d of %d' % (
            s, statistics.median(r), 100 * statistics.median(r) / budget, q(r, .99), 100 * q(r, .99) / budget, max(r), 100 * max(r) / budget,
            sum(1 for x in r if x > budget), len(r)))
    print('--- with the two fill loops fused (NOT implemented; estimate) ---')
    for s in scen:
        r = resF[s]
        print('%-40s median %7d (%4.1f%%)  p99 %7d (%5.1f%%)  max %7d (%5.1f%%)  over budget: %d of %d' % (s, statistics.median(r), 100 * statistics.median(r) / budget, q(r, .99), 100 * q(r, .99) / budget, max(r), 100 * max(r) / budget, sum(1 for x in r if x > budget), len(r)))
    # top cost contributors in the p99 frame (scenario C)
    worst = sorted(range(len(res['C chip data D=4, code uncached F=5'])), key=lambda i: res['C chip data D=4, code uncached F=5'][i])[-1]
    print('worst frame index (from 1000): %d' % worst)
    fr = frames[1000 + worst]
    agg = collections.Counter()
    for b, pc, c in fr:
        key = (-1, pc) if pc >= 0xA000 else (b, pc)
        if key in cost and pc not in WAIT:
            e, by, d, h, n = cost[key]; agg[pc >> 8 << 8] += c * (e + h + 4 * d + 5 * by / 4.0)
    print('worst frame, cost by 256-byte code page (scenario C):', [(hex(a), int(v)) for a, v in agg.most_common(6)])
    # helper-call mix per frame
    ioc = collections.Counter()
    for b, pc, c in frames[3000]:
        key = (-1, pc) if pc >= 0xA000 else (b, pc)
        if key in seg:
            for l in ([] if key[1] in WAIT else seg[key]):
                if l.strip().startswith('jsr rt_'): ioc[l.strip().split()[1]] += c
    print('helper calls in frame 3000:', dict(ioc))

if __name__ == '__main__':
    main()
