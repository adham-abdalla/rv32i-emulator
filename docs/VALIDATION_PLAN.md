# Validation plan

**Device under test:** `src/cpu.c` and `src/loader.c` (RV32I base integer ISA, RISC-V Unprivileged spec 20191213, chapter 2).
**Goal:** every base instruction behaves as the spec says, including its edge cases, and faults are raised instead of silently doing the wrong thing.

## Three layers

| Layer | What | Finds | Where |
|---|---|---|---|
| Directed tests | 19 test functions, 72 checks, written from the spec text | known edge cases: sign extension, x0, shift amounts, alignment, link registers, loader input checks | `tests/test_isa.c` |
| Real programs | C programs cross-compiled with clang for `rv32i`, output compared to a native build | integration bugs: calling convention, stack, compiler-generated sequences, runtime helpers | `programs/`, `tools/run_programs.py` |
| Random co-simulation | 1000 random programs (500 instructions each in CI), trace compared step by step with an independent Python model | bugs nobody thought to write a test for | `tools/cosim.py` |

The directed tests and the programs run under AddressSanitizer and UBSan in CI, so an out-of-bounds read in the emulator fails the build even when the result happens to be right.

## Coverage matrix

| Instruction group | Directed | Programs | Co-sim |
|---|---|---|---|
| LUI, AUIPC | ✓ (auipc uses its own pc) | ✓ | ✓ |
| JAL, JALR | ✓ (link value, bit 0 cleared, rd == rs1, misaligned target) | ✓ (calls/returns) | ✓ (odd jalr offsets) |
| BEQ BNE BLT BGE BLTU BGEU | ✓ (signed vs unsigned, backward loop, misaligned target taken and not taken) | ✓ | ✓ |
| LB LH LW LBU LHU | ✓ (sign/zero extension, little endian, negative offset) | ✓ | ✓ |
| SB SH SW | ✓ (only width bytes change) | ✓ | ✓ |
| ADDI SLTI SLTIU XORI ORI ANDI | ✓ (12-bit immediate limits, sltiu with -1) | ✓ | ✓ |
| SLLI SRLI SRAI | ✓ (shift 31, shamt[5] illegal) | ✓ | ✓ |
| ADD SUB SLL SLT SLTU XOR SRL SRA OR AND | ✓ (overflow wraps, shift uses rs2[4:0]) | ✓ | ✓ |
| FENCE, FENCE.I | ✓ (no-op) | – | ✓ |
| ECALL | ✓ (exit, write, unknown → ENOSYS) | ✓ | ✓ (exit) |
| EBREAK | ✓ | – | – (stops the program, so not generated) |
| Illegal encodings | ✓ (all-zero word, CSR access, M-extension MUL) | – | – |
| Memory faults | ✓ (misaligned load, store past RAM, fetch past RAM, last valid word) | – | – |
| ELF loader | ✓ (valid file, .bss zeroed, 7 malformed files rejected) | ✓ | – |

Co-simulation prints its own functional coverage on every run (39/40, EBREAK excluded by design).

## Is the checker itself trustworthy?

A checker that can never fail proves nothing. `tools/cosim.py --self-test` plants a bug in the reference model (SRA computed as a logical shift) and confirms co-simulation reports a mismatch. CI runs this on every push.

The reference model shares no code with the emulator: different language, table-driven decode, Python's unbounded integers masked to 32 bits instead of C's fixed-width types.

## Defect log

| ID | Found by | Symptom | Root cause | Fix |
|---|---|---|---|---|
| DEF-01 | Programs (`primes.elf`) | Misaligned access at pc 0x101c during start-up | Linker script placed `.bss` right after byte data; crt0 clears `.bss` one word at a time | `ALIGN(4)` on `.bss` start and end in `programs/link.ld` |
| DEF-02 | Co-sim (seed 2, step limit) | Program looped forever in both emulator and model | Test generator bug: a forward jump could land between `auipc` and `jalr`, so `jalr` used a stale register | Generator now builds programs from units and jumps skip whole units |

DEF-02 was a bug in the test, not the emulator. Both sides agreed (same infinite loop), which is exactly what lock-step comparison should show.

## Out of scope

- Extensions (M, A, C, F, D, Zicsr) and privileged mode: decoded as illegal.
- Self-modifying code needing FENCE.I semantics beyond in-order execution.
- Timing: this is an instruction-accurate model, not cycle-accurate.
