# sound test ADF:  make -f test/sndtest.mk   (from a1200/)   ->  build/sndtest/sndtest.adf
HERE := $(CURDIR)
ROOT := $(abspath ..)
B := $(ROOT)/build/sndtest
SNDOUT ?= $(ROOT)/sound/out_full
PREFIX ?= m68k-linux-gnu-
CC := $(PREFIX)gcc
LD := $(PREFIX)ld -z noexecstack --no-warn-rwx-segments
C := -m68020 -ffreestanding -nostdlib -fno-pic -O2 -fomit-frame-pointer -fno-tree-loop-distribute-patterns -I$(HERE) -I$(ROOT)/sound/amiga -I$(SNDOUT)
all: $(B)/sndtest.adf
$(B):
	mkdir -p $(B)
$(B)/sndtest.adf: $(HERE)/test/sndtest.c $(HERE)/snd.c $(HERE)/platform.c $(HERE)/crt0.s $(HERE)/isr.s $(HERE)/boot.s $(ROOT)/platform/rt.c $(ROOT)/platform/rtasm.s $(HERE)/test/sndtest.ld $(SNDOUT)/sounds.bin | $(B)
	$(CC) $(C) -c $(HERE)/test/sndtest.c -o $(B)/sndtest.o
	$(CC) $(C) -c $(HERE)/snd.c -o $(B)/snd.o
	$(CC) $(C) -c $(HERE)/platform.c -o $(B)/plat.o
	$(CC) $(C) -c $(ROOT)/platform/rt.c -o $(B)/rt.o
	$(CC) -m68020 -x assembler -c $(ROOT)/platform/rtasm.s -o $(B)/rtasm.o
	$(CC) -m68020 -x assembler -c $(HERE)/isr.s -o $(B)/isr.o
	$(CC) -m68020 -x assembler -c $(HERE)/crt0.s -o $(B)/crt0.o
	$(CC) -m68020 -x assembler -c $(HERE)/boot.s -o $(B)/boot.o
	cd $(SNDOUT) && $(CC) -m68020 -x assembler -c sounds_blob.s -o $(B)/blob.o
	$(LD) -e 0 -Ttext=0 --oformat=binary -o $(B)/boot.bin $(B)/boot.o
	$(LD) -T $(HERE)/test/sndtest.ld -Map=$(B)/sndtest.map -o $(B)/sndtest.elf $(B)/crt0.o $(B)/sndtest.o $(B)/snd.o $(B)/blob.o $(B)/plat.o $(B)/rt.o $(B)/isr.o $(B)/rtasm.o
	$(PREFIX)objcopy -O binary $(B)/sndtest.elf $(B)/sndtest.bin
	python3 $(ROOT)/platform/tools/mkdisk.py $@ $(B)/boot.bin $(B)/sndtest.bin
