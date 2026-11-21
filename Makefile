# rv32emu build. `make check` runs everything CI runs.
CC       ?= gcc
CFLAGS   ?= -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror
SANITIZE ?=
RVCC     ?= clang
RVFLAGS  := --target=riscv32-unknown-elf -march=rv32i -mabi=ilp32 -O2 -ffreestanding -fno-builtin \
            -nostdlib -static -fuse-ld=lld -Wl,-T,programs/link.ld -Wall -Werror
SRC      := src/cpu.c src/loader.c
PROGRAMS := build/fib.elf build/sort.elf build/primes.elf

.PHONY: all test programs run-programs cosim check clean

all: build/rv32emu

build:
	mkdir -p build

build/rv32emu: $(SRC) src/main.c include/rv32.h | build
	$(CC) $(CFLAGS) $(SANITIZE) -Iinclude $(SRC) src/main.c -o $@

build/test_isa: $(SRC) tests/test_isa.c include/rv32.h | build
	$(CC) $(CFLAGS) $(SANITIZE) -Iinclude $(SRC) tests/test_isa.c -o $@

test: build/test_isa
	./build/test_isa

build/%.elf: programs/%.c programs/rt.c programs/rt.h programs/crt0.S programs/link.ld | build
	$(RVCC) $(RVFLAGS) programs/crt0.S programs/rt.c $< -o $@

programs: $(PROGRAMS)

run-programs: build/rv32emu programs
	python3 tools/run_programs.py build programs/expected.txt

cosim: build/rv32emu
	python3 tools/cosim.py --emu build/rv32emu --seeds 200 --length 400

check: test run-programs cosim

clean:
	rm -rf build
