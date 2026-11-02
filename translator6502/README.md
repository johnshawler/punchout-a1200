# translator6502 -- 6502 -> 68020 translator prototype (Mike Tyson's Punch-Out!! Europe, NES, MMC2)

Status: PROTOTYPE.  Nothing here has run on an Amiga or in FS-UAE.  See "What is verified" below.

| file | purpose |
|---|---|
| `ref6502.py` | clean reference 6502 (official opcodes, decimal mode optional).  Validated against Klaus Dormann's `6502_functional_test` (30,646,177 instructions, success trap $3469) |
| `x6502.py` | decode, discovery, flag liveness (N,Z,C,V), per-instruction emitter (68020, GNU as syntax).  Register map and flag scheme in the module docstring |
| `program.py` | whole-program emission: branches, JSR/RTS/JMP through a dispatch table, cycle counting (`lea (N,%a6),%a6` per block), event checks at loop headers, bank-switch re-dispatch |
| `test6502.py` | per-instruction test: random cases, reference core vs translated code run under `qemu-m68k` (registers, live flags, RAM checksum, I/O write log) |
| `cost.py` | 68020 cost model applied to the REAL per-frame instruction counts from the reference emulator trace |
| `tools/fceumm_trace.patch` | patch for libretro-fceumm (commit ac0d00a) that counts executed instructions per (PRG window bank, PC) |
| `tools/frontend_trace.c` | headless libretro frontend: runs the core with an input script, dumps RAM, screenshots and the per-frame instruction counts |
| `tools/script1.txt` | input script (START/A taps) that reaches a Glass Joe fight |

## Reproduce
```
python3 ref6502.py 6502_functional_test.bin                     # reference core self-test
python3 test6502.py prg.bin 3000 1                              # per-instruction test (needs m68k-linux-gnu-as/ld, qemu-m68k)
# trace: build fceumm with tools/fceumm_trace.patch, gcc tools/frontend_trace.c -ldl, run
#   fe core.so game.nes 6000 ramdump 100000 tools/script1.txt trace.bin
python3 program.py prg.bin trace.bin out.s                      # translate the executed instruction set
python3 cost.py prg.bin trace.bin pal                           # per-frame 68020 cost on real counts
```
`prg.bin` is the 128 KiB PRG ROM of the cartridge (not included, ROMs are never committed).

## What is verified
* Reference core: Dormann functional test passes.
* Per-instruction translation: 6 seeds x 3000 random cases (18,000), 135 (mnemonic, mode) forms, 0 failures.  Random live-flag sets exercise the liveness paths.
  Covers every official opcode except branches, JMP, JSR, RTS, RTI, BRK, PHP, PLP.
* The executed instruction set (6,968 instructions from a 6000-frame scripted run) translates and assembles (~84-90 KB of 68020 code).

## What is NOT verified
* Branches, JSR/RTS/JMP, the dispatch tables, bank-switch re-dispatch, cycle counting, event checks: emitted but not tested.
* Assumption "N and Z are not live across JSR/RTS/JMP": not checked.
* The code map is the executed set of one 120 s scripted run, not a complete static discovery; indirect-jump targets are not closed.
* The cost model uses 68020 timings from memory of the manual plus assumed chip-RAM penalties.  It is an estimate, not a measurement.
* No PPU, APU, MMC2 CHR latch, input or Amiga platform code exists yet.  Cost of the hardware model is NOT in the numbers.
