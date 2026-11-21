# rv32i-emulator

[![Validation](https://github.com/adham-abdalla/rv32i-emulator/actions/workflows/ci.yml/badge.svg)](https://github.com/adham-abdalla/rv32i-emulator/actions/workflows/ci.yml)
![C11](https://img.shields.io/badge/C-C11-00599C)
![RISC-V](https://img.shields.io/badge/RISC--V-RV32I-283272)
![Sanitizers](https://img.shields.io/badge/ASan%20%2B%20UBSan-clean-2e7d4f)

An instruction-accurate **RISC-V RV32I emulator in C**, and the validation work around it: directed tests from the spec, real C programs cross-compiled with clang, and **random co-simulation** against an independent reference model.

```
$ make programs && ./build/rv32emu --stats build/sort.elf
min -99906 max 97390 sorted yes checksum 0xfffec420
retired 162218 instructions, stopped: exit
  lui    103
  auipc  2
  jal    1878
  jalr   770
  beq    533
  bne    14499
  ...
```

## What it does

- All **40 RV32I base instructions** (`src/cpu.c`, ~250 lines): integer ALU, loads/stores, branches, jumps, FENCE, ECALL, EBREAK.
- Precise faults instead of undefined behaviour: illegal instruction, misaligned fetch (jump/branch target), misaligned load/store, access outside RAM. On a fault the destination register is not written.
- **ELF32 loader** (`src/loader.c`) that bounds-checks every offset and size, so malformed files are rejected cleanly.
- Linux-style syscalls through ECALL: `exit` (93) and `write` (64); anything else returns `-ENOSYS`.
- `--trace` writes one line per retired instruction (pc, encoding, register write, store). `--stats` prints the instruction mix.
- Exit status is the guest's exit code, or `128 + fault` so scripts can tell "program failed" from "emulator stopped it".

## Validation

Full plan with the coverage matrix and defect log: [docs/VALIDATION_PLAN.md](docs/VALIDATION_PLAN.md).

| Layer | Result |
|---|---|
| Directed tests (`tests/test_isa.c`) | 72 checks across 19 tests, run under ASan + UBSan |
| Cross-compiled C programs (`programs/`) | Fibonacci (recursion, wraparound), quicksort of 256 signed values, sieve to 10 000. Output matches a native build. ~800k instructions |
| Random co-simulation (`tools/cosim.py`) | 1000 random programs per CI run, compared **step by step** with a Python reference model. Functional coverage 39/40 (EBREAK is directed-only) |
| Checker self-test | A bug is planted in the model (SRA as SRL); co-sim must catch it, or CI fails |
| Compiler matrix | gcc with sanitizers; clang with `-Wconversion -Wshadow -Wpedantic -Werror` |

Two defects were found and fixed during bring-up, both logged with root cause: a `.bss` alignment bug in the start-up code (found by a real program) and a test-generator bug (found by co-sim, with both sides agreeing).

### How co-simulation works

```
seed ──► random program generator ──► flat binary
                                   │
                ┌──────────────────┴──────────────────┐
                ▼                                     ▼
     rv32emu --trace (C, DUT)            reference model (Python)
                │                                     │
                └──────────► compare line by line ◄───┘
                             first mismatch: seed, step, both lines
```

The generator seeds registers with edge values (`0x7FFFFFFF`, `0x80000000`, `-1`, `0x800`, 31, 32, 33...), keeps loads and stores inside a data window, and only jumps forward so every program terminates. Jumps skip whole units so they can never land inside an `auipc`/`jalr` pair.

## Build and run

Needs gcc or clang. Cross-compiling the sample programs needs `clang` and `lld` (no RISC-V GCC toolchain required).

```bash
make test             # directed tests
make programs         # clang --target=riscv32 -march=rv32i ... -> build/*.elf
make run-programs     # run them and compare output
make cosim            # 200 random programs
make check            # all of the above
make test SANITIZE="-fsanitize=address,undefined"

./build/rv32emu --trace out.trace --stats build/fib.elf
./build/rv32emu --bin 0x0 program.bin     # flat binary loaded at an address
```

RV32I has no multiply or divide instructions, so `programs/rt.c` supplies the helpers the compiler calls (`__mulsi3`, `__udivsi3`, `__divsi3`, ...) along with `memset`/`memcpy` and output through the `write` syscall.

## Layout

```
include/rv32.h       CPU state, status codes, API
src/cpu.c            decode + execute, syscalls, trace
src/loader.c         flat binary and ELF32 loaders
src/main.c           command-line runner
tests/test_isa.c     directed tests with instruction encoders
programs/            crt0.S, link.ld, rt.c and sample C programs
tools/cosim.py       random program generator + reference model + comparator
tools/run_programs.py
docs/VALIDATION_PLAN.md
```

## Limits

Base ISA only (M, A, C, F, D and CSRs decode as illegal), single hart, no privileged mode, not cycle-accurate. These are listed in the validation plan as out of scope rather than left unknown.

## License

MIT
