# Zoo Keeper for the Amiga 1200 (AGA, 68EC020, 2 MB chip RAM, Kickstart 3.x).   make  ->  ../build/a1200/zookeeper_a1200.adf
#   make BUILD=name (other output dir build/name)   make SOUND=0 (no sound)   make PLAT_FMODE=0|1|3 (bitplane fetch width, default 1)   make EXTRA="-DHUD" (speed HUD)
# The game code (translated 6809 CPUs, scheduler, hardware models, glue) is the same as the A500 build; only the platform layer
# (a1200/platform.c, crt0.s, boot.s), the display module (a1200/display.c) and the compiler flags (-m68020, TEST020) differ.
PREFIX  ?= m68k-linux-gnu-
CC      := $(PREFIX)gcc
LD      := $(PREFIX)ld -z noexecstack --no-warn-rwx-segments
OBJCOPY := $(PREFIX)objcopy
HERE    := $(CURDIR)
ROOT    := $(abspath ..)
BUILD   ?= a1200
B       := $(ROOT)/build/$(BUILD)
A       := $(ROOT)/amiga
P       := $(ROOT)/platform
R       := $(ROOT)/rt
REF     := $(ROOT)/ref/src
SOUND   ?= 1
# full-fidelity sound data (sound/tools/build_sounds_full.py): all sounds, full length, 28.6 kHz, LZ4 on disk
SNDOUT  ?= $(ROOT)/sound/out_full
PLAT_FMODE ?= 1
XLFLAGS ?= --020
CFLAGS  := -MMD -MP $(EXTRA) -m68020 -ffreestanding -nostdlib -fno-pic -O2 -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks \
           -Wall -Wno-misleading-indentation -I$(HERE) -I$(R) -I$(REF) -I$(A) -DA1200 -DFLAT_DISPATCH -DHOT_IO -DBACKLOG_MAX=6u -DPLAT_FMODE=$(PLAT_FMODE)
ASFLAGS := -m68020 -Wa,--defsym,TEST020=1 -Wa,--defsym,FLAT_DISPATCH=1 -Wa,-I$(R)
ifeq ($(SOUND),1)
CFLAGS  += -DWITH_SOUND -I$(ROOT)/sound/amiga -I$(SNDOUT)
endif

OBJS := $(B)/main.o $(B)/display.o $(B)/font.o $(B)/images.o $(B)/rt_sched.o $(B)/hw_data.o $(B)/hw_video.o \
        $(B)/rt_core.o $(B)/rt_cpu.o $(B)/data_cpu.o $(B)/video_cpu.o
ifeq ($(SOUND),1)
OBJS += $(B)/snd.o $(B)/sounds_blob.o
$(B)/snd.o: snd.c $(ROOT)/sound/amiga/snd.h $(SNDOUT)/sounds.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/sounds_blob.o: $(SNDOUT)/sounds_blob.s $(SNDOUT)/sounds.bin | $(B)
	cd $(SNDOUT) && $(CC) -m68020 -x assembler -c sounds_blob.s -o $@
endif

.DEFAULT_GOAL := all
all: $(B)/zookeeper_a1200.adf

$(B):
	mkdir -p $(B)
# the translated CPUs: identical source to the A500 build (translator output is plain 68000 code, which a 68020 executes unchanged)
$(B)/data_cpu.s: $(ROOT)/tools/xl/*.py | $(B)
	cd $(ROOT) && python3 -I tools/xl/xlate.py data $@ $(XLFLAGS) > $(B)/xlate_data.log
$(B)/video_cpu.s: $(ROOT)/tools/xl/*.py | $(B)
	cd $(ROOT) && python3 -I tools/xl/xlate.py video $@ $(XLFLAGS) > $(B)/xlate_video.log
$(B)/%_cpu.o: $(B)/%_cpu.s
	(echo '	.include "vm.inc"'; echo '	.text'; cat $<) > $(B)/$*_cpu_full.s
	$(CC) $(ASFLAGS) -x assembler -c $(B)/$*_cpu_full.s -o $@
$(B)/rt_core.o: $(R)/rt_core.s | $(B)
	$(CC) $(ASFLAGS) -x assembler -c $< -o $@
$(B)/rt_cpu.o: $(R)/rt_cpu.s $(R)/vm.inc | $(B)
	$(CC) $(ASFLAGS) -x assembler -c $< -o $@
$(B)/rt_sched.o: $(R)/rt_sched.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/hw_data.o: $(R)/hw_data_unit.c $(R)/hw_data.c $(REF)/pia6821.c | $(B)
	$(CC) $(CFLAGS) -O3 -c $< -o $@        # hw_data + the PIA model as one unit (see rt/hw_data_unit.c)
$(B)/hw_video.o: $(R)/hw_video.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/main.o: $(A)/main.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/display.o: display.c display.h platform.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/font.o: $(A)/font.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/images.o: images.s $(B)/data_mem.bin | $(B)
	cd $(B) && $(CC) -m68020 -x assembler -c $(HERE)/images.s -o $@
$(B)/data_mem.bin: $(ROOT)/tools/mkimages.py | $(B)
	cd $(ROOT) && python3 tools/mkimages.py roms $(B)

PLATOBJS := $(B)/plat.o $(B)/rt.o $(B)/isr.o $(B)/rtasm.o
$(B)/plat.o: platform.c platform.h | $(B)
	$(CC) -m68020 -ffreestanding -nostdlib -fno-pic -O2 -Wall -Wextra -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -DPLAT_FMODE=$(PLAT_FMODE) -I$(HERE) -c $< -o $@
$(B)/rt.o: $(P)/rt.c | $(B)
	$(CC) -m68020 -ffreestanding -nostdlib -fno-pic -O2 -fomit-frame-pointer -fno-tree-loop-distribute-patterns -fno-delete-null-pointer-checks -I$(HERE) -c $< -o $@
$(B)/rtasm.o: $(P)/rtasm.s | $(B)
	$(CC) -m68020 -x assembler -c $< -o $@
$(B)/isr.o: isr.s | $(B)
	$(CC) -m68020 -x assembler -c $< -o $@
$(B)/crt0.o: crt0.s | $(B)
	$(CC) -m68020 -x assembler -c $< -o $@
$(B)/boot.bin: boot.s | $(B)
	$(CC) -m68020 -x assembler -c $< -o $(B)/boot.o
	$(LD) -e 0 -Ttext=0 --oformat=binary -o $@ $(B)/boot.o
	@test `stat -c %s $@` -le 1024 || (echo "bootblock > 1024 bytes"; false)
$(B)/zookeeper.elf: $(OBJS) $(PLATOBJS) $(B)/crt0.o link.ld
	$(LD) -T link.ld -Map=$(B)/zookeeper.map -o $@ $(B)/crt0.o $(OBJS) $(PLATOBJS)
$(B)/zookeeper.bin: $(B)/zookeeper.elf
	$(OBJCOPY) -O binary $< $@
$(B)/zookeeper_a1200.adf: $(B)/zookeeper.bin $(B)/boot.bin
	python3 $(P)/tools/mkdisk.py $@ $(B)/boot.bin $<
	@$(PREFIX)size $(B)/zookeeper.elf

clean:
	rm -f $(B)/*.o $(B)/*.elf $(B)/*.bin $(B)/*.adf $(B)/*_cpu.s $(B)/*_full.s $(B)/*.d
.PHONY: all clean
-include $(wildcard $(B)/*.d)
