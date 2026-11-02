# tests of ../display.c (A1200 display module).   make test
ROOT    := ../..
DUMPS   ?= $(ROOT)/build/ref_dd
B       := build
PREFIX  ?= m68k-linux-gnu-
CFLAGS_H := -Wall -Wextra -I.. -I$(ROOT)/rt
$(B):
	mkdir -p $(B)
$(B)/t_x86: test_a1200.c ../display.c ../display.h ../platform.h | $(B)
	gcc -O2 $(CFLAGS_H) -o $@ test_a1200.c ../display.c
$(B)/t_asan: test_a1200.c ../display.c ../display.h ../platform.h | $(B)
	gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined $(CFLAGS_H) -o $@ test_a1200.c ../display.c
$(B)/disp_m68k.o: ../display.c ../display.h ../platform.h | $(B)
	$(PREFIX)gcc -m68020 -ffreestanding -nostdlib -fno-pic -O2 -fomit-frame-pointer -fno-tree-loop-distribute-patterns -Wall -I.. -I$(ROOT)/rt -c ../display.c -o $@
$(B)/test_m68k.o: test_a1200.c | $(B)
	$(PREFIX)gcc -m68020 -O2 -static $(CFLAGS_H) -c test_a1200.c -o $@
$(B)/t_m68k: $(B)/test_m68k.o $(B)/disp_m68k.o
	$(PREFIX)gcc -m68020 -static -o $@ $^
test: $(B)/t_x86 $(B)/t_asan $(B)/t_m68k
	$(B)/t_asan kernel 1 20000
	$(B)/t_asan fuzz 1 300
	$(B)/t_asan fuzz 2 300
	$(B)/t_asan replay $(DUMPS) -t 7
	$(B)/t_asan replay $(DUMPS) -t 3 -n 2000 -x 20 -s 100
	$(B)/t_x86 replay $(DUMPS) -t 1
	qemu-m68k -cpu m68020 $(B)/t_m68k kernel 3 20000
	qemu-m68k -cpu m68020 $(B)/t_m68k fuzz 3 200
	qemu-m68k -cpu m68020 $(B)/t_m68k replay $(DUMPS) -t 11
	@echo ALL TESTS PASSED
.PHONY: test
