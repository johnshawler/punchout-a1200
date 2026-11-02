# platform test ADF:  make -f test/ptest.mk PLAT_FMODE=3   ->  build/ptest/ptest.adf
HERE := $(CURDIR)
ROOT := $(abspath ..)
B := $(ROOT)/build/ptest$(PLAT_FMODE)$(TAG)
PREFIX ?= m68k-linux-gnu-
CC := $(PREFIX)gcc
LD := $(PREFIX)ld -z noexecstack --no-warn-rwx-segments
PLAT_FMODE ?= 1
C := -m68020 -ffreestanding -nostdlib -fno-pic -O2 -fomit-frame-pointer -fno-tree-loop-distribute-patterns -I$(HERE) -DPLAT_FMODE=$(PLAT_FMODE) $(PEXTRA)
all: $(B)/ptest.adf
$(B):
	mkdir -p $(B)
$(B)/ptest.adf: $(HERE)/test/ptest.c $(HERE)/platform.c $(HERE)/crt0.s $(HERE)/isr.s $(HERE)/boot.s $(ROOT)/platform/rt.c $(ROOT)/platform/rtasm.s $(ROOT)/platform/link.ld | $(B)
	$(CC) $(C) -c $(HERE)/test/ptest.c -o $(B)/ptest.o
	$(CC) $(C) -c $(HERE)/platform.c -o $(B)/plat.o
	$(CC) $(C) -c $(ROOT)/platform/rt.c -o $(B)/rt.o
	$(CC) -m68020 -x assembler -c $(ROOT)/platform/rtasm.s -o $(B)/rtasm.o
	$(CC) -m68020 -x assembler -c $(HERE)/isr.s -o $(B)/isr.o
	$(CC) -m68020 -x assembler -c $(HERE)/crt0.s -o $(B)/crt0.o
	$(CC) -m68020 -x assembler -c $(HERE)/boot.s -o $(B)/boot.o
	$(LD) -e 0 -Ttext=0 --oformat=binary -o $(B)/boot.bin $(B)/boot.o
	$(LD) -T $(ROOT)/platform/link.ld -o $(B)/ptest.elf $(B)/crt0.o $(B)/ptest.o $(B)/plat.o $(B)/rt.o $(B)/isr.o $(B)/rtasm.o
	$(PREFIX)objcopy -O binary $(B)/ptest.elf $(B)/ptest.bin
	python3 $(ROOT)/platform/tools/mkdisk.py $@ $(B)/boot.bin $(B)/ptest.bin
