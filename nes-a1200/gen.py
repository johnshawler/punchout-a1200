#!/usr/bin/env python3
"""gen.py PRG.bin trace.bin out_dir : translate the 6502 code of the cartridge to 68020 (GNU as) for the NES-on-A1200 build.
Writes out_dir/code.s (translated code, event stubs) and out_dir/entries.s (dispatch entry lists: the tables themselves are built at start-up).
Code that is not covered (data decoded as code, paths never traced) is handled at run time by the 6502 interpreter (nes_interp.c)."""
import sys, os, re
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'translator6502'))
import program as PG
from program import load_seeds, translate, emit_program, mark_back_edges
PG.THR = '0'; PG.WARP = True
prg = open(sys.argv[1], 'rb').read()[:0x20000]
seeds, _ = load_seeds(sys.argv[2])
out = sys.argv[3]; os.makedirs(out, exist_ok=True)
p = translate(prg, seeds, follow=True); mark_back_edges(p); PG.find_spins(p)
print('wait loops found: %d (ram %d, poll %d)' % (len(PG.SPINS), sum(1 for v in PG.SPINS.values() if v == 'ram'), sum(1 for v in PG.SPINS.values() if v == 'poll')), sorted(hex(k[1]) for k in PG.SPINS)[:20])
seg, stubs, keys, st, L, blk, checks, unsup = emit_program(p, seeds)
text = '\n'.join('\n'.join(seg[k]) for k in keys)
defined = set(re.findall(r'^(\w+):', text, re.M))
used = set(re.findall(r'\b(?:jb\w+|jbra)\s+((?:W[0-9A-F]_|F_)[0-9A-F]{4})\b', text))
with open(os.path.join(out, 'code.s'), 'w') as f:
    f.write('\t.text\n\t.align 2\n')
    for k in keys: f.write('\n'.join(seg[k]) + '\n')
    for lbl in sorted(used - defined): f.write('%s:\tmove.w #%d,%%d7\n\tjbra rt_undef\n' % (lbl, int(lbl[-4:], 16)))
    for tag, a, wrap, ret in stubs: f.write('ev_%s:\tmove.w #%d,%%d7\n\tjsr %s\n\tjbra %s\n' % (tag, a, 'rt_event_f' if wrap else 'rt_event', ret))
fix = [(k[1], 'F_%04X' % k[1]) for k in keys if k[0] == -1 and k not in unsup]
win = [(k[0], k[1], 'W%X_%04X' % (k[0], k[1])) for k in keys if k[0] >= 0 and k not in unsup]
with open(os.path.join(out, 'code.s'), 'a') as f:   # same object file: the entry labels are local symbols
    f.write('\t.section .rodata\n\t.align 2\n\t.globl fix_count, fix_addr, fix_ptr, win_count, win_bank, win_addr, win_ptr\n')
    f.write('fix_count:\t.long %d\nfix_addr:\n' % len(fix))
    for a, l in fix: f.write('\t.word %d\n' % a)
    f.write('\t.align 2\nfix_ptr:\n')
    for a, l in fix: f.write('\t.long %s\n' % l)
    f.write('win_count:\t.long %d\nwin_bank:\n' % len(win))
    for b, a, l in win: f.write('\t.byte %d\n' % b)
    f.write('\t.align 2\nwin_addr:\n')
    for b, a, l in win: f.write('\t.word %d\n' % a)
    f.write('\t.align 2\nwin_ptr:\n')
    for b, a, l in win: f.write('\t.long %s\n' % l)
print('translated %d instructions (fixed %d, window %d); unsupported %d; stubs %d' % (len(keys), len(fix), len(win), len(unsup), len(stubs)))
