# nes-a1200 - Mike Tyson's Punch-Out!! (NES, PAL, MMC2) on the Amiga 1200

STATUS: **playable. The fast-RAM build (`make FAST=1`, needs an A1200 with 2 MB chip RAM and at least 1.5 MB fast RAM at $200000) runs fights at about 99 % of real time in FS-UAE; the stock build (no fast RAM) is no longer maintained (about 73 % in fights, almost no chip RAM left).**  Sound is approximate.  Built and tested only in FS-UAE (A1200, 68EC020, KS 3.1); nothing on real hardware yet.  PAL only.

This edition also carries Amiga-themed text patches (`nes_ppu.c`: fighter names, hometown, `PRESS ENTER`; `nes_vid.c`: the repeating background text of the intro screens).  They rewrite nametable text at run time and never touch the ROM data.

## What it is
* 6502 code -> 68020 by static translation (`../translator6502`, generated into `build/code.s` by `gen.py`); code the static map misses runs in a 6502 interpreter (`nes_interp.c`).
* Hardware model in C: `nes_core.c` (memory map, MMC2 mapper, scheduler/NMI), `nes_ppu.c` (PPU registers, loopy scroll, sprite-0 timing; it draws nothing, it records
  *regions* of scanlines shown under one scroll/mask/mirroring state), `nes_vid.c` (the video back end, below), `nes_apu.c` (sound), `nes_main.c` (pads, HUD), `nes_rt.s` (glue, fast paths, time-warp of wait loops).
* Platform layer reused from `../platform-a1200` (bootblock, CIA tick = NES PAL frame rate 50.007 Hz, keyboard/joystick); `platform.c` here is a copy with the NES tick parameters.

### Native video (nes_vid.c)
The first version emulated the PPU scanline by scanline into a chunky buffer and converted it to bitplanes: ~13-18 % speed in a bout.  Now the picture is never rebuilt:
* **Background**: persistent planar bitmaps of the nametables (4 bitplanes = palette*4+pixel; one for each mirroring mode, 3 "variants" per mode for different CHR bank settings, because the game animates by switching banks).
  A tile is redrawn only when the game changed the nametable/attribute byte or the CHR bank it comes from.  The display hardware scrolls: every region becomes a few Copper moves (bitplane pointers, BPLCON1, modulo),
  32 bit bitplane fetch (half the DMA slots; AGA scroll quirk measured: a delay >= 16 shows the data one 32 pixel unit later, so 10 units are fetched).
* **MMC2 CHR latches** decide which 4 KB bank each background tile comes from (tiles $FD/$FE flip a latch while the PPU fetches them).  Resolved per tile row from the flip tiles of the row, the
  scroll window of the region that shows the row, and the sprite size of that region (8x16 sprites fetch a dummy tile $FF that sets latch 1).  The model was checked in Python (`test/mmc2_oracle.py`, `test/mmc2_evcheck.py`) against the real latch behaviour of the reference emulator
  (instrumented fceumm, `../translator6502/tools/fceumm_ppu_dump.patch` + `frontend_dump.c`): 0 mismatching tile-lines in 11 sampled frames of a bout/menu script run (2 tiles at a mid-frame scroll change in one frame),
  and the event-based algorithm used in C agrees with the per-tile model on 856 sampled frames.  The C code itself was not run against these dumps.
* **Sprites**: AGA hardware sprites, 64 pixels wide; sprite pair p shows NES sprite palette p; NES sprites of one (palette, priority) combination are merged into
  instances (control long + 4 longs per line); the combinations in use are packed into the 4 pairs, front ones first (BPLCON2 decides which pairs are behind the background).
  MMC2 also flips latch 0 from sprite tiles: the bank of each sprite row is derived from the flip tiles in fetch order.
* **Sprite-0 hit**: timed lazily from the bitmaps (pixel overlap of sprite 0 with the background), cached until something it depends on changes.

## Build (ROMs are never committed)
```
# prg.bin = 128 KiB PRG, chr.bin = 128 KiB CHR of the cartridge (the two halves of the .nes file after its 16-byte header)
# trace.bin = per-frame executed-address list from translator6502/tools (fceumm patch + frontend)
make PRG=prg.bin CHR=chr.bin TRACE=trace.bin B=$PWD/build            # add HUD=1 for the speed/profile overlay
test/run_fsuae.sh build/punchout_a1200.adf /path/to/kickstart31_a1200.rom out 30 shots.txt
```
Controls: keyboard **X = A, Z = B, Space = Select, Return = Start**, arrows = D-pad; joystick port 2 = D-pad, fire = A, 2nd button = B.
(The porting prompt's "start = fire" cannot hold here: the game needs A, B and Start.)

## Measured in FS-UAE (stock A1200: 68EC020 14 MHz, 2 MB chip RAM, no fast RAM, KS 3.1; HUD build; speed = emulated frames per 100 ticks of the 50.007 Hz game clock)
| scene | speed |
|---|---|
| title / intro / menus (static) | 100 % |
| bout, quiet moments | 90-100 % |
| bout, typical (scripted idle player, Glass Joe) | 60-85 % |
| bout, busy frames (many NT writes / sprite changes) | 45-60 % |
Frame skipping (`ppu_vblank`, `-DMAX_SKIP=n`, default 2 since the sprite builder rewrite) is described here as it was when it was added and **off by default**: a 3000-frame scripted benchmark did not get faster with it (the pictures that remain are the expensive full rebuilds), and it only makes the display choppier.  -Os was slower than -O2 (68 % vs 82 % in the last second of the same run; single run, noisy).
(before the native video back end: 13-18 % in a bout, ~5 % on scene changes.)  So it is **not yet a steady 50 Hz everywhere on a stock A1200**; it plays, a bout runs at roughly 2/3 to 5/6 speed.
Where the time goes in a typical bout frame (31 ms at 64 %): sprite stream building ~30 %, the translated game ~15 %, memory map/mapper/scheduler ~8 %, platform/ISRs ~7 %, PPU model ~5 %, tile/region resolution ~7 %, rest small.
The machine is slow because code and data live in chip RAM under display DMA: roughly 1-1.5 MIPS for data-heavy code.  Profile with `make EXTRA=-DPROFILE HUD=1` (`syms.sh`-style bucket list in `prof_syms.inc`).

## Sprites: how they are built now (`build_sprites`)
Hardware sprite instances are *windows*: per (palette, priority) combination the NES sprites are grouped from the leftmost one into 32 pixel wide windows, each window is cut into vertical stretches where every line is covered, and each stretch is one instance whose plane data is composed line by line with pixel-exact shifts (lowest OAM index wins).  A tall character is then one or two instances instead of dozens of runs, and channels are only reused after a gap line.  Fully transparent sprites (filler tiles, the MMC2 trigger tiles) get no instance.
Channels are assigned in NES priority order (lowest OAM index first; front instances on the lowest free channel, back instances on the highest): not within a gap line of the same channel's other instances, the other channel of a pair shows the same palette while both are on screen, overlapping sprites of different instances stack in OAM order (lower channel = in front) and front pairs lie below back pairs where they overlap.  Then, in time order, the Copper events are made: palette of a pair (3 colours, both LOCT passes) at the first line of an instance that needs another one, and the number of front pairs (BPLCON2) when the instances on screen allow another value.  Channel 7 is not used (its DMA slot is not served with this bitplane fetch).  Pair colours are loaded by the vertical-blank interrupt and written again by the Copper just before the display window (not at the top of the frame, where the interrupt is still loading the palette with BPLCON3 under its feet).
Composed instances are cached by a hash of their layout (tiles, flips, banks, offsets); pattern rows per (tile, flips, bank) as well.
What was wrong before: stacked rows of sprites needed a gap line (missing bands), channel 7, too few back-priority channels, a filler sprite (tile $FB/$FA, solid colour) landing in front of a face sprite (white square), and combinations that could not be placed at all (title glove, VS screen).  Checked against the reference emulator at frames 500, 650, 826, 1000, 1425, 1500, 2000, 3000, and a scripted run of 5900 frames reports no instance that cannot be placed; 262 instances in that run were placed with the OAM-order rule relaxed (no visible error in the frames looked at).
Known remaining difference: the colours of the NES palette table differ slightly from the reference emulator's (blue floor, lavender portrait background).  Speed work after the rewrite: counting sort by x and per-instance lists built in OAM order, cheaper channel assignment (bitmasks), a tile-row cache and a cache of composed instances (87 % hits), an early-out for sprite overlap tests; and compiling with -O3 (-67 ticks on its own).  The fast-RAM build now needs ~2830 ticks for the 2500 frame run (2847 with the old builder, 2939 before this round).

## Fast-RAM build (`make FAST=1`)
Optional second ADF for an A1200 with at least **1.5 MB of fast RAM at 0x200000** (the image itself is ~1 MB; -O3 made it larger) (Zorro II style mapping, as FS-UAE's `fast_memory` and trapdoor/MBX style boards give it).  The bootblock (`platform-a1200/boot.s`, `-DFAST`) tests 1 MB at 0x200000 and shows a **yellow screen** if it is not there; the start stub (`crt0_fast.s`) copies the image to 0x200000 and the code, tables, PRG/CHR and all CPU-only data run from there.  Only what the custom chips read (bitmaps, Copper lists, sprite streams, Paula waveforms, platform copper; section `.bss.chip`) stays in chip RAM (`link_fast.ld`).  The fast part is ~970 KB, so 1 MB would nearly do; the check asks for 1.5 MB.
Measured (same 2500-frame scripted bout as above, HUD build, identical frames and counters): **2847 ticks (~97 % of real time incl. boot, i.e. essentially 50 Hz) against 3671-3794 on the stock A1200.**  The stock build is unchanged (`make`).  Other board mappings (32 bit RAM at 0x0700 0000 / 0x1000 0000) would need a relocating loader: not done.

## Research: what it takes to reach a steady 50 Hz on a stock A1200 (measured 2500-frame scripted run, HUD builds, FS-UAE; ticks = 50.007 Hz; ~270 ticks of that are boot)
| variant | ticks | speed over the run |
|---|---|---|
| as built | 3794 | ~71 % |
| no hardware sprites built (`-DNOSPR`) | 3347 | ~81 % |
| sprite-0 evaluation stubbed (`-DNOSPR0`) | 3746 | ~72 % |
| no picture presented at all (`-DFORCE_SKIP=1`) | 2968 | ~93 % |
Counters over the 2500 frames: sprite stream rebuilds 592 (24 % of frames), sprite-0 evaluations 1545, BG tiles redrawn 13452 (5.4/frame), nametable writes 13671, regions 4642.
* A rebuild costs ~15 ms (the bout has ~50 sprites, ~400 sprite rows per frame), so ~24 % of frames take ~35 ms and the average loses 3.6 ms/frame.  This is the largest single item; frame skipping cannot fix it.
* Reference-emulator OAM dumps of 600 bout frames (`translator6502/tools/frontend_dump.c`, frames 3200-3800): 49 sprites = 21 runs on average; OAM identical in 83 % of frames; of the changed frames about half only move sprites; **99.3 % of runs have the same (tile, flip) signature as an earlier run** (81 distinct signatures) => a run cache would almost never miss.
* All other present work (BG tiles, regions, copper, palette, sprite-0) is ~3 ms/frame average; sprite-0 evaluation alone ~0.4 ms.
* Even with the whole present removed the machine only reaches ~93 %: the emulated game plus core (memory map, mapper, scheduler, PPU register model), the APU glue and the ISRs also need ~10 % less on their busiest frames.
Done so far: a sprite run cache (`emit_run`; 10 % misses, saves only ~3 % over the run: 3794 -> 3671 ticks, so row assembly was a small part of a rebuild) and a 32 colour palette path (`plat_set_palette32`, one bank in the vblank ISR).  Not done: the present fast path (3).
Cuts, in order of value (estimates, not measured):
1. **Sprite run cache**: key = (tile, flip bits, CHR bank, height) per member of a run -> pre-assembled plane data; a run is emitted by copying (or, better, by pointing the sprite channel at the cached block with a Copper SPRxPT reload) and patching only the two control longs (position).  Rebuild 15 ms -> ~1.5-2 ms.  Memory: ~100 distinct blocks x <200 bytes.
2. Palette path: `plat_set_palette256` converts 256 entries and the vertical-blank ISR writes 8 banks x 64 colour registers whenever anything changed; only 32 colours are used (1 bank, 64 writes).  Saves a few ms on every frame that changes the palette or the sprite pair packing.
3. Present fast path: when regions, scroll, CHR registers, mask/ctrl and nametable dirtiness are unchanged since the last present, only flip the existing Copper list (saves part of the ~1.8 ms of fixed per-frame present work).
4. Game side (to get the last ~7-10 %): sample the PC inside the core group (memory map / mapper / scheduler) to find the top functions; give PPU $2004/$2005/$2006/$2007 writes and the APU registers inline fast paths; measure the APU frame step (`-DNOAPU` experiment) and run it only when a register changed; keep hot loops inside the 68020's 256 byte I-cache.
5. Small: fetch 9 instead of 10 units of bitplane data per line (less chip-bus contention), HUD off.
Expected: 1+2+3 should lift a bout from ~71 % to the high 80s / low 90s; a steady 100 % needs item 4 as well and is not guaranteed.

## Not done
* A steady 50 Hz in every scene on a stock A1200.  What is left: the sprite stream builder (`spr_add`/`spr_rows`: ~9 ms in a busy bout; ideas: reuse an instance when only its position changed, hand-written row loop, build only changed sprites),
  region resolution, the translated game itself.  An accelerated A1200 (any fast RAM) would settle it.
* Sound is approximate: the four APU channels (pulse x2, triangle, noise) drive four Paula channels with looped waveforms, envelopes/length/sweep stepped once per frame (4-step sequencer assumed); The DMC sample
  (the crowd noise, $E000, looped) is decoded to 8 bit when started (decimated to fit `DMC_BUF` = 12 KB of chip RAM) and takes over Paula channel 3 from the noise channel while it runs;
  the boot-time $4011 PCM sample (CPU-timed direct writes, ~3.5 s at power-up) is not played.  Checked only by recording Paula's output (tones appear at plausible frequencies), never listened to.
* Mid-frame palette and CHR *register* changes (a frame is drawn with the registers at vblank; the MMC2 latch trick, which the game really uses, is handled), left-column clipping, sprite overflow flag, odd-frame timing,
  8-sprites-per-line limit (all sprites are drawn).
* Verification: translator tests in `../translator6502` (per-instruction, 18,000 random cases; lockstep control-flow, 80,000 checkpoints plus 2,127 fuzz checkpoints over 14 random starts, all matching).  A longer fuzz run was cut off by its time limit, not failed.  The Amiga output was compared with the reference emulator's and the earlier exact renderer's frames by eye (title, intro, fight): the intro matches except for a few one-pixel sprite columns (cause not found); no pixel-exact frame diff.
* The ROM-provided data in the ADF (`build/punchout_a1200.adf`) is your cartridge data: the ADF is git-ignored and must not be redistributed.
* `nes_pal.h` is the default palette table of libretro-fceumm (GPL-2); check the licence before redistributing the repository.

## Sprite test (no game): `-DSPRDUMP`
`test/sprdump.inc` (generated from reference-emulator state dumps, not committed) holds the sprite tables, CHR bank registers and palettes of 170 frames.  A `SPRDUMP` build presents them one after another (`SPRHOLD` ticks each) with nothing else on screen; screenshots are compared with an exact rendering of the same tables (`sprcmp.py`, scratch) and a Python model of the builder (`hwmodel*.py`) was run over ~2000 distinct states of scripted play.  This is how the white square, the title-screen glove and the VS-screen stripe were found and fixed: instances must be placed in a topological order of "must be in front" relations between instances whose sprites really overlap (not by lowest OAM index), back-priority instances are placed after the front ones, and the sprite-0 marker behind the playfield at the right edge gets no channel.
Speed in fights (9000 frames of a scripted random-punching run, `test/script2.inc`): fast-RAM build ~89 % without frame skipping, ~99 % with up to 2 skipped pictures (the default), ~101 % with 4; stock A1200 ~73 % with 2 skipped pictures.  (An earlier figure of 74 % was inflated by the test script itself: the pad was re-read through a linear search of the whole script at every controller read.)

* BG/sprite alignment: the persistent BG bitmap is drawn 1 px right of where the Copper scroll expects it, so `build_copper` uses `X + BGADJ` (`BGADJ` = -1); without it sprites that overlap BG art
  (portraits, Doc) show 1-px coloured stripes.  `BGTEST` builds show a scrolling test pattern; `TIMELOG`/`APUDBG`/`NOAPU`/`ONLYCH=n`/`DMC_ONLY` are APU/timing debug switches.
