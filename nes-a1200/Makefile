# Mike Tyson's Punch-Out!! (NES, PAL) on the Amiga 1200: translated 6502 code + C hardware model.  make -> $(B)/punchout_a1200.adf
# Inputs (never committed): PRG=prg.bin (128 KiB) CHR=chr.bin (128 KiB) TRACE=trace.bin (executed-address seeds, see ../translator6502/tools)
PREFIX ?= m68k-linux-gnu-
CC := $(PREFIX)gcc
LD := $(PREFIX)ld -z noexecstack --no-warn-rwx-segments
HERE := $(CURDIR)
PL := $(abspath ../platform-a1200)
B ?= $(HERE)/build
PRG ?= prg.bin
CHR ?= chr.bin
TRACE ?= trace.bin
CFLAGS := $(if $(HUD),-DHUD,) -m68020 -ffreestanding -nostdlib -fno-pic -O3 -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -Wall -I$(HERE) -I$(PL) $(EXTRA)
ifdef FAST
CRT0 := $(B)/crt0_fast.o
LDS := link_fast.ld
BOOTDEF := -Wa,--defsym,FAST=1
else
CRT0 := $(B)/crt0.o
LDS := link.ld
BOOTDEF :=
endif
OBJS := $(CRT0) $(B)/isr.o $(B)/plat.o $(B)/rt.o $(B)/rtasm.o $(B)/nes_rt.o $(B)/nes_core.o $(B)/nes_ppu.o $(B)/nes_vid.o $(B)/nes_interp.o $(B)/nes_main.o $(B)/nes_apu.o $(B)/code.o $(B)/data.o
all: $(B)/punchout_a1200.adf
$(B):
	mkdir -p $(B)
$(B)/code.s: gen.py $(wildcard ../translator6502/*.py) $(PRG) $(TRACE) | $(B)
	python3 gen.py $(PRG) $(TRACE) $(B)
$(B)/code.o: $(B)/code.s
	$(CC) -m68020 -x assembler -c $< -o $@
$(B)/data.o: data.s $(PRG) $(CHR) | $(B)
	cp $(PRG) $(B)/prg.bin; cp $(CHR) $(B)/chr.bin; cd $(B) && $(CC) -m68020 -x assembler -c $(HERE)/data.s -o $@
$(B)/nes_rt.o: nes_rt.s | $(B)
	$(CC) -m68020 -x assembler -c $< -o $@
$(B)/%.o: %.c nes.h nes_vid.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/plat.o: platform.c $(PL)/platform.h | $(B)
	$(CC) -m68020 -ffreestanding -nostdlib -fno-pic -O2 -Wall -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -DPLAT_FMODE=1 -I$(PL) -c $< -o $@
$(B)/rt.o: $(PL)/rt.c | $(B)
	$(CC) -m68020 -ffreestanding -nostdlib -fno-pic -O2 -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -I$(PL) -c $< -o $@
$(B)/rtasm.o: $(PL)/rtasm.s | $(B)
	$(CC) -m68020 -x assembler -c $< -o $@
$(B)/isr.o: $(PL)/isr.s | $(B)
	$(CC) -m68020 -x assembler -c $< -o $@
$(B)/crt0.o: $(PL)/crt0.s | $(B)
	$(CC) -m68020 -x assembler -c $< -o $@
$(B)/crt0_fast.o: crt0_fast.s | $(B)
	$(CC) -m68020 -x assembler -c $< -o $@
$(B)/boot.bin: $(PL)/boot.s | $(B)
	$(CC) -m68020 $(BOOTDEF) -x assembler -c $< -o $(B)/boot.o
	$(LD) -e 0 -Ttext=0 --oformat=binary -o $@ $(B)/boot.o
$(B)/punchout.elf: $(OBJS) $(LDS)
	$(LD) -T $(LDS) -Map=$(B)/punchout.map -o $@ $(OBJS)
$(B)/punchout.bin: $(B)/punchout.elf
	$(PREFIX)objcopy -O binary $< $@
$(B)/punchout_a1200.adf: $(B)/punchout.bin $(B)/boot.bin
	python3 mkdisk.py $@ $(B)/boot.bin $<
	@$(PREFIX)size $(B)/punchout.elf
clean:
	rm -rf $(B)
.PHONY: all clean
