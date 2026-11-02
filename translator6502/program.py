#!/usr/bin/env python3
"""Whole-program emission for the 6502 -> 68020 translator: control flow, cycle counting, dispatch.
usage: program.py PRG.bin trace.bin out.s     (trace.bin: per-frame (bank,pc,count) records, see sound/ref notes)"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x6502 import Prog, Emit, BR, lab, defs, uses
from ref6502 import OPS

def load_seeds(trace):
    d = open(trace, 'rb').read(); o = 0; seen = set(); frames = []
    while o < len(d):
        n, = struct.unpack_from('<I', d, o); o += 4; recs = []
        for k in range(n):
            b, pc, c = struct.unpack_from('<BHH', d, o + 5 * k); recs.append((b, pc, c)); seen.add((-1 if pc >= 0xA000 else b, pc))
        o += 5 * n; frames.append(recs)
    return seen, frames

def translate(rom, seedset, vectors=True, follow=False):
    p = Prog(rom)
    seeds = [(k[0], k[1]) for k in seedset]
    if vectors:
        for v in (0xFFFA, 0xFFFC, 0xFFFE): seeds.append((-1, p.byte(0, v) | p.byte(0, v + 1) << 8))
    ind = p.discover(seeds, follow=follow)
    # discover() starts window-code seeds at the bank given; keep the instruction map keyed by the real bank
    p.liveness()
    for i in p.ins.values():
        if i.mn in ('CMP', 'CPX', 'CPY') and i.native_c:
            j = p.ins.get(p.kk(i, i.succ[0]))
            if j is not None: j.after_native = True
    return p

def leaders(p, seedset):
    L = set(seedset)
    for k, i in p.ins.items():
        nk = p.kk(i, i.addr + i.len)
        if i.md == 'rel': L.add(p.kk(i, i.succ[1])); L.add(nk)
        elif i.mn in ('JMP', 'JSR'):
            if i.md == 'abs': L.add(p.kk(i, i.arg))
            L.add(nk)
        elif i.mn in ('RTS', 'RTI', 'BRK'): L.add(nk)
    return L

THR = '0'          # event threshold: the check is  cmpa.w #THR,%a6 ; jbgt  (the lockstep test uses -32768 = fire at every check site)

def emit_program(p, seedset):
    """-> seg (key -> asm lines), stubs [(label, addr, wrapped, return_label)], keys, helper-call stats, leader set, block cycle sums, check sites"""
    em = Emit(p)
    L = leaders(p, seedset)
    keys = sorted(p.ins, key=lambda k: (k[0], k[1]))
    seg = {}; stubs = []; checks = {}
    blk_cyc = {}
    for k in keys:
        if k in L:
            c = 0; cur = k
            while True:
                i = p.ins.get(cur)
                if i is None: break
                c += i.cyc
                if i.md == 'rel' or i.mn in ('JMP', 'JSR', 'RTS', 'RTI', 'BRK'): break
                nk = p.kk(i, i.addr + i.len)
                if nk in L or nk not in p.ins: break
                cur = nk
            blk_cyc[k] = c
    nhelpers = collections.Counter()

    def check(i, tag, wrap, restore=True):
        """event check: counter > threshold -> out-of-line stub; the stub returns to ek_<tag>; wrap keeps N,Z (in d6) across the check"""
        if wrap: em.I('move %ccr,%d6')
        em.I('cmpa.w #%s,%%a6' % THR); em.I('jbgt ev_%s' % tag)
        em.raw('ek_%s:' % tag)
        if wrap and restore: em.I('move %d6,%ccr')
        stubs.append((tag, i.addr, wrap, 'ek_%s' % tag))
        checks.setdefault((i.bank, i.addr), []).append((tag, wrap))

    unsupported = {}
    for idx, k in enumerate(keys):
        i = p.ins[k]; em.o = []; nstub = len(stubs)
        try:
            emit_one(p, em, i, k, idx, keys, L, blk_cyc, check, nhelpers)
        except NotImplementedError as e:
            em.o = [lab(i) + ':']; del stubs[nstub:]; checks.pop(k, None)
            em.I('move.w #%d,%%d7' % i.addr); em.I('jbra rt_unsupported'); unsupported[k] = str(e)
        seg[k] = em.o; nhelpers.update(em.st); em.st = collections.Counter()
    return seg, stubs, keys, nhelpers, L, blk_cyc, checks, unsupported

def emit_one(p, em, i, k, idx, keys, L, blk_cyc, check, nhelpers):
    if True:
        em.raw(lab(i) + ':')                                  # dispatch / branch target: the label is BEFORE the block prologue
        if k in L:
            em.I('lea (%d,%%a6),%%a6' % blk_cyc.get(k, 0))
            if k in BACK: check(i, lab(i), bool(i.live_in & {'N', 'Z'}))
        mn = i.mn
        if i.md == 'rel':
            setup, cc = em.branch_cc(i, getattr(i, 'after_native', False))
            tk = p.kk(i, i.succ[1])
            inv = {'eq': 'ne', 'ne': 'eq', 'mi': 'pl', 'pl': 'mi', 'cc': 'cs', 'cs': 'cc'}[cc]
            if setup and (i.live_out & {'N', 'Z'}):
                # the C/V test clobbers N,Z which are still needed after the branch: save and restore them on both paths
                if (tk[0] != -1 and i.bank == -1) or (tk[0] >= 0 and i.bank >= 0 and tk[0] != i.bank): raise NotImplementedError('cross-region branch with live N/Z')
                em.I('move %ccr,%d6'); em.I(setup[-1]); em.I('jb%s 1f' % inv); em.I('move %d6,%ccr'); em.I('jbra %s' % lab(tk[0], i.succ[1])); em.raw('1:'); em.I('move %d6,%ccr'); em.st['ccr_wrap'] += 1
            elif WARP and (i.bank, i.addr) in SPINS and not setup:
                em.I('jb%s 1f' % inv); em.I('jsr rt_warp_%s' % SPINS[(i.bank, i.addr)]); em.I('jbra %s' % lab(tk[0], i.succ[1])); em.raw('1:'); em.st['warp'] += 1
            else:
                for x in setup: em.I(x)
                if i.bank == -1 and tk[0] != -1:
                    em.I('jb%s 1f' % inv); em.I('move.w #%d,%%d7' % i.succ[1]); em.dispatch(); em.raw('1:')
                elif tk[0] >= 0 and i.bank >= 0 and tk[0] != i.bank: raise NotImplementedError('branch between banks')
                else: em.I('jb%s %s' % (cc, lab(tk[0], i.succ[1])))
        elif mn == 'JMP' and i.md == 'abs':
            if i.arg >= 0xA000 or (i.bank != -1 and 0x8000 <= i.arg < 0xA000): em.I('jbra %s' % lab(i.bank if i.arg < 0xA000 else -1, i.arg))
            else:
                nzl = bool(i.live_in & {'N', 'Z'})
                if nzl: em.I('move %ccr,%d6')
                em.I('move.w #%d,%%d7' % i.arg); em.dispatch('d6' if nzl else None)
        elif mn == 'JMP':
            if i.live_in & {'N', 'Z'}: raise NotImplementedError('N/Z live across JMP (ind)')
            check(i, 'j' + lab(i), False)
            em.I('move.w (%d,%%a5),%%d7' % (i.arg & 0x7FF)); em.I('ror.w #8,%d7'); em.dispatch()
        elif mn == 'JSR':
            r = (i.addr + 2) & 0xFFFF
            nzl = bool(i.live_in & {'N', 'Z'})            # the callee reads the caller's N/Z: keep them in d6 across the check, the pushes and the dispatch
            check(i, 'j' + lab(i), nzl, restore=False)
            em.I('move.b #%d,(0x100,%%a5,%%d3.w)' % (r >> 8)); em.I('subq.b #1,%d3')
            em.I('move.b #%d,(0x100,%%a5,%%d3.w)' % (r & 255)); em.I('subq.b #1,%d3')
            if nzl: em.I('move %d6,%ccr')
            if i.arg >= 0xA000 or (i.bank != -1 and 0x8000 <= i.arg < 0xA000): em.I('jbra %s' % lab(i.bank if i.arg < 0xA000 else -1, i.arg))
            else: em.I('move.w #%d,%%d7' % i.arg); em.dispatch('d6' if nzl else None)
        elif mn == 'RTS':
            # byte-wise pulls: the 6502 stack pointer wraps inside page 1, a 16-bit read would not.  N,Z that a caller still needs go through the host stack.
            nzl = bool(i.live_out & {'N', 'Z'})
            if nzl: em.I('move %ccr,-(%sp)')
            em.I('addq.b #1,%d3'); em.I('moveq #0,%d7'); em.I('move.b (0x100,%a5,%d3.w),%d7'); em.I('addq.b #1,%d3')
            em.I('move.b (0x100,%a5,%d3.w),%d6'); em.I('lsl.w #8,%d6'); em.I('or.w %d6,%d7'); em.I('addq.w #1,%d7'); em.dispatch('stack' if nzl else None)
        elif mn == 'RTI': em.I('jbra rt_rti')
        elif mn == 'BRK': em.I('move.w #%d,%%d7' % i.addr); em.I('jbra rt_undef')
        else:
            em.ins(i)
            # a write to the mapper from window code changes what executes next: continue through the dispatch table
            if mn in ('STA', 'STX', 'STY') and i.md == 'abs' and i.arg >= 0x8000 and k[0] != -1:
                em.I('move.w #%d,%%d7' % (i.addr + i.len)); em.dispatch(); em.st['bank_redispatch'] += 1
            nk = p.kk(i, i.addr + i.len)
            nxt_k = keys[idx + 1] if idx + 1 < len(keys) else None
            if nk != nxt_k:
                if nk in p.ins: em.I('jbra %s' % lab(nk[0], i.addr + i.len))
                else: em.I('move.w #%d,%%d7' % ((i.addr + i.len) & 0xFFFF)); em.I('jbra rt_undef')

BACK = set()
WARP = False                 # emit time-warp calls in pure wait loops (nes-a1200 runtime only)
SPINS = {}                   # branch key -> 'ram' | 'poll' : backward branch that closes a pure wait loop

def find_spins(p):
    """pure wait loops: <= 5 consecutive instructions from a backward branch target to the branch, no stores/calls/other branches,
    reads only RAM (type 'ram': nothing but an interrupt can change the outcome) or exactly the PPU status port $2002 (type 'poll')."""
    SPINS.clear()
    OK = {'LDA', 'LDX', 'LDY', 'AND', 'ORA', 'EOR', 'CMP', 'CPX', 'CPY', 'BIT', 'NOP'}
    for k, b in p.ins.items():
        if b.md != 'rel': continue
        t = p.kk(b, b.succ[1])
        if t[1] > b.addr or t not in p.ins: continue
        cur = t; body = []
        while cur != k and len(body) < 6:
            i = p.ins.get(cur)
            if i is None: break
            body.append(i); cur = p.kk(i, i.addr + i.len)
        else:
            pass
        if cur != k or not body or len(body) > 5: continue
        kind = 'ram'; good = True
        for i in body:
            if i.mn not in OK: good = False; break
            if i.md in ('imm', 'acc', 'imp'): continue
            if i.md in ('zp', 'zpx', 'zpy', 'abx', 'aby', 'izx', 'izy'):
                if i.md != 'zp': good = False; break
                continue
            if i.md == 'abs':
                if i.arg < 0x2000: continue
                if 0x2000 <= i.arg < 0x4000 and (i.arg & 7) == 2: kind = 'poll'; continue
            good = False; break
        if good: SPINS[k] = kind

def mark_back_edges(p):
    """loop headers get an event check: targets of backward conditional branches AND of backward absolute JMPs"""
    for k, i in p.ins.items():
        if i.md == 'rel':
            t = p.kk(i, i.succ[1])
            if t[1] <= i.addr and t in p.ins: BACK.add(t)
        elif i.mn == 'JMP' and i.md == 'abs':
            t = p.kk(i, i.arg)
            if t[1] <= i.addr and t in p.ins: BACK.add(t)

if __name__ == '__main__':
    rom = open(sys.argv[1], 'rb').read()[:0x20000]
    seedset, frames = load_seeds(sys.argv[2])
    p = translate(rom, seedset)
    mark_back_edges(p)
    seg, stubs, keys, st, _, _, _, _ = emit_program(p, seedset)
    with open(sys.argv[3], 'w') as f:
        f.write('\t.text\n')
        for k in keys: f.write('\n'.join(seg[k]) + '\n')
        for tag, a, wrap, ret in stubs: f.write('ev_%s:\tmove.w #%d,%%d7\n\tjsr %s\n\tjbra %s\n' % (tag, a, 'rt_event_f' if wrap else 'rt_event', ret))
    n = sum(len(v) for v in seg.values())
    print('instructions translated: %d (fixed %d, window %d); asm lines %d; helpers/flags: %s' % (len(keys), sum(1 for k in keys if k[0] == -1), sum(1 for k in keys if k[0] >= 0), n, dict(st)))
