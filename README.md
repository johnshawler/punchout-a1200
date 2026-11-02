# Punch-Out!! on the Amiga 1200

A native Amiga 1200 (AGA) port of *Mike Tyson's Punch-Out!!* (NES, PAL version), made by translating the game's 6502 code to 68020 code and drawing it with the Amiga's own hardware: persistent planar background bitmaps scrolled by the Copper, AGA hardware sprites, and Paula sound. It boots from a floppy disk image.

This edition has Amiga-themed text changes: the fighters are renamed with Amiga puns (Guru Joe, VBlank Kaiser, Piston Haynie, Don Flamigos, Fat Agnus, Gayle Tiger, Bald Bill Herd, Soda Paulaski, Mr. Blitterman, Super ST Man; Little Mitchy and Doctor Jay on the player's side), the intro background repeats "ONLY AMIGA MAKES IT POSSIBLE", and "PUSH START" reads "PRESS ENTER". These are run-time rewrites of on-screen text; the ROM data is never modified.

## Requirements
* Amiga 1200 (AGA, stock 68EC020), **PAL**, with 2 MB chip RAM and **at least 1.5 MB fast RAM at $200000** (without it the boot block shows a yellow screen).
* Boots from a floppy image (ADF) in DF0; no Workbench or hard drive needed.
* Controls: keyboard through joystick port 1 emulation (X = A, Z = B, Space = Select, Return = Start, arrows), or a joystick (fire = A, second button = B).
* Speed: about 99 % of real time in fights in FS-UAE (A1200, 2 MB chip + 2 MB fast RAM). Not yet tested on real hardware. Reports from MiSTer Minimig-AGA say it can run slower there; `make HUD=1 EXTRA=-DSPEEDONLY` builds a version that shows the speed in per cent under the picture.
* The older build without fast RAM is no longer maintained.

## ROM and legal
**No game data is included.** You need your own legally obtained ROM of *Mike Tyson's Punch-Out!! (Europe)*. The disk image the build produces contains that game data, so do not share it. This project is not affiliated with or endorsed by Nintendo, Commodore or anyone else; names and trademarks belong to their owners. No licence has been chosen for the code yet.

## Building
See `nes-a1200/README.md` for the details (how the PRG/CHR halves of the ROM are split out, how the executed-address list `trace.bin` is made with the reference emulator patch in `translator6502/tools`, and the FS-UAE test scripts). In short:

```
cd nes-a1200
make FAST=1 PRG=prg.bin CHR=chr.bin TRACE=trace.bin B=$PWD/build   # -> build/punchout_a1200.adf
```
A cross toolchain (`m68k-linux-gnu-gcc`) and Python 3 are needed.

## Layout
* `nes-a1200/` - the game: NES hardware model in C, native Amiga video/sound back end, build scripts.
* `translator6502/` - the 6502 to 68020 static translator and its tests.
* `platform-a1200/` - the A1200 boot block, display, input and sound layer.
