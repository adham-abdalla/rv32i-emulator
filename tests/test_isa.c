/* Directed tests: one or more per RV32I instruction, aimed at the edges the spec
 * calls out (sign extension, x0, shift amounts, alignment, link registers). */
#include "rv32.h"

#include <stdlib.h>
#include <string.h>

static int checks, failures;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
        }                                                                   \
    } while (0)

/* ---- encoders ---------------------------------------------------------- */
static uint32_t R(uint32_t f7, int rs2, int rs1, uint32_t f3, int rd, uint32_t op) {
    return f7 << 25 | (uint32_t)rs2 << 20 | (uint32_t)rs1 << 15 | f3 << 12 | (uint32_t)rd << 7 | op;
}
static uint32_t I(int32_t imm, int rs1, uint32_t f3, int rd, uint32_t op) {
    return ((uint32_t)imm & 0xFFF) << 20 | (uint32_t)rs1 << 15 | f3 << 12 | (uint32_t)rd << 7 | op;
}
static uint32_t S(int32_t imm, int rs2, int rs1, uint32_t f3) {
    uint32_t u = (uint32_t)imm;
    return (u >> 5 & 0x7F) << 25 | (uint32_t)rs2 << 20 | (uint32_t)rs1 << 15 | f3 << 12 | (u & 0x1F) << 7 | 0x23;
}
static uint32_t B(int32_t imm, int rs2, int rs1, uint32_t f3) {
    uint32_t u = (uint32_t)imm;
    return (u >> 12 & 1) << 31 | (u >> 5 & 0x3F) << 25 | (uint32_t)rs2 << 20 | (uint32_t)rs1 << 15 | f3 << 12 |
           (u >> 1 & 0xF) << 8 | (u >> 11 & 1) << 7 | 0x63;
}
static uint32_t U(uint32_t imm, int rd, uint32_t op) { return (imm & 0xFFFFF000u) | (uint32_t)rd << 7 | op; }
static uint32_t J(int32_t imm, int rd) {
    uint32_t u = (uint32_t)imm;
    return (u >> 20 & 1) << 31 | (u >> 1 & 0x3FF) << 21 | (u >> 11 & 1) << 20 | (u >> 12 & 0xFF) << 12 |
           (uint32_t)rd << 7 | 0x6F;
}
#define ADDI(rd, rs, imm) I(imm, rs, 0, rd, 0x13)
#define ECALL 0x00000073u
#define EBREAK 0x00100073u

static rv_cpu cpu;

static void load(const uint32_t *prog, int n) {
    memset(cpu.mem, 0, RV_MEM_SIZE);
    for (int i = 0; i < n; i++)
        for (int b = 0; b < 4; b++) cpu.mem[4 * i + b] = (uint8_t)(prog[i] >> (8 * b));
    rv_reset(&cpu, 0);
}

static rv_status run(int steps) { return rv_run(&cpu, (uint64_t)steps); }

/* ---- tests ------------------------------------------------------------- */
static void test_x0_is_hardwired(void) {
    uint32_t p[] = {ADDI(0, 0, 5), R(0, 0, 0, 0, 0, 0x33), U(0x12345000, 0, 0x37)};
    load(p, 3);
    run(3);
    CHECK(cpu.x[0] == 0, "x0 = %08x", cpu.x[0]);
}

static void test_addi_sign_extends(void) {
    uint32_t p[] = {ADDI(1, 0, -1), ADDI(2, 0, 2047), ADDI(3, 0, -2048)};
    load(p, 3);
    run(3);
    CHECK(cpu.x[1] == 0xFFFFFFFF, "addi -1 -> %08x", cpu.x[1]);
    CHECK(cpu.x[2] == 2047, "addi 2047 -> %u", cpu.x[2]);
    CHECK(cpu.x[3] == 0xFFFFF800, "addi -2048 -> %08x", cpu.x[3]);
}

static void test_add_sub_wrap(void) {
    uint32_t p[] = {R(0, 2, 1, 0, 3, 0x33), R(0x20, 2, 0, 0, 4, 0x33)};
    load(p, 2);
    cpu.x[1] = 0x7FFFFFFF;
    cpu.x[2] = 1;
    run(2);
    CHECK(cpu.x[3] == 0x80000000, "add overflow wraps: %08x", cpu.x[3]);
    CHECK(cpu.x[4] == 0xFFFFFFFF, "0 - 1 = %08x", cpu.x[4]);
}

static void test_slt_signed_vs_unsigned(void) {
    uint32_t p[] = {R(0, 2, 1, 2, 3, 0x33), R(0, 2, 1, 3, 4, 0x33), I(-1, 2, 3, 5, 0x13), I(1, 6, 3, 7, 0x13),
                    I(-5, 1, 2, 8, 0x13)};
    load(p, 5);
    cpu.x[1] = 0xFFFFFFFF; /* -1 */
    cpu.x[2] = 1;
    run(5);
    CHECK(cpu.x[3] == 1, "slt -1 < 1 signed");
    CHECK(cpu.x[4] == 0, "sltu 0xffffffff < 1 is false");
    CHECK(cpu.x[5] == 1, "sltiu 1 < 0xffffffff (imm -1 sign-extended then compared unsigned)");
    CHECK(cpu.x[7] == 1, "sltiu x6=0 < 1 (seqz)");
    CHECK(cpu.x[8] == 0, "slti -1 < -5 is false");
}

static void test_logic_immediates(void) {
    uint32_t p[] = {I(-1, 1, 4, 2, 0x13), I(0x0F0, 1, 6, 3, 0x13), I(0x0FF, 1, 7, 4, 0x13)};
    load(p, 3);
    cpu.x[1] = 0x12345678;
    run(3);
    CHECK(cpu.x[2] == ~0x12345678u, "xori -1 = not");
    CHECK(cpu.x[3] == (0x12345678u | 0xF0), "ori");
    CHECK(cpu.x[4] == 0x78, "andi");
}

static void test_shifts(void) {
    uint32_t p[] = {
        I(31, 1, 1, 2, 0x13),                     /* slli x2, x1, 31 */
        I(31, 3, 5, 4, 0x13),                     /* srli x4, x3, 31 */
        I(0x400 | 31, 3, 5, 5, 0x13),             /* srai x5, x3, 31 */
        R(0, 6, 3, 5, 7, 0x33),                   /* srl x7, x3, x6 (x6 = 33 -> shift by 1) */
        R(0x20, 6, 3, 5, 8, 0x33),                /* sra x8, x3, x6 */
        R(0, 6, 1, 1, 9, 0x33),                   /* sll x9, x1, x6 */
    };
    load(p, 6);
    cpu.x[1] = 1;
    cpu.x[3] = 0x80000000;
    cpu.x[6] = 33;
    run(6);
    CHECK(cpu.x[2] == 0x80000000, "slli 31");
    CHECK(cpu.x[4] == 1, "srli is logical");
    CHECK(cpu.x[5] == 0xFFFFFFFF, "srai is arithmetic");
    CHECK(cpu.x[7] == 0x40000000, "srl uses low 5 bits of rs2: %08x", cpu.x[7]);
    CHECK(cpu.x[8] == 0xC0000000, "sra uses low 5 bits of rs2: %08x", cpu.x[8]);
    CHECK(cpu.x[9] == 2, "sll uses low 5 bits of rs2");
}

static void test_slli_with_bit25_is_illegal(void) {
    uint32_t p[] = {I(32, 1, 1, 2, 0x13)};
    load(p, 1);
    CHECK(run(1) == RV_ILLEGAL, "RV32I has 5-bit shamt; shamt[5]=1 must trap");
}

static void test_lui_auipc(void) {
    uint32_t p[] = {ADDI(0, 0, 0), U(0xDEADB000, 1, 0x37), U(0x00001000, 2, 0x17)};
    load(p, 3);
    run(3);
    CHECK(cpu.x[1] == 0xDEADB000, "lui");
    CHECK(cpu.x[2] == 8 + 0x1000, "auipc uses its own pc: %08x", cpu.x[2]);
}

static void test_jal_jalr(void) {
    uint32_t p[] = {
        J(12, 1),               /* 0: jal ra, 12 */
        ADDI(10, 0, 99),        /* 4: skipped */
        ADDI(10, 0, 99),        /* 8: skipped */
        I(1, 5, 0, 5, 0x67),    /* 12: jalr x5, 1(x5) -> target (x5+1)&~1, link written after reading x5 */
    };
    load(p, 4);
    cpu.x[5] = 0x100;
    run(2);
    CHECK(cpu.x[1] == 4, "jal links pc+4");
    CHECK(cpu.pc == 0x100, "jalr clears bit 0 of target: pc=%08x", cpu.pc);
    CHECK(cpu.x[5] == 16, "jalr rd==rs1 uses old rs1 then writes link");
    CHECK(cpu.x[10] == 0, "skipped instructions did not run");
}

static void test_branches(void) {
    /* each pair: branch +8 over an "addi x10, x10, 1" marker. Taken -> marker skipped. */
    struct { uint32_t f3; uint32_t a, b; int taken; const char *name; } cases[] = {
        {0, 5, 5, 1, "beq equal"},       {0, 5, 6, 0, "beq not equal"},
        {1, 5, 6, 1, "bne"},             {4, 0xFFFFFFFF, 0, 1, "blt -1 < 0"},
        {5, 0, 0xFFFFFFFF, 1, "bge 0 >= -1"}, {6, 0, 0xFFFFFFFF, 1, "bltu 0 < max"},
        {7, 0xFFFFFFFF, 0, 1, "bgeu max >= 0"}, {6, 0xFFFFFFFF, 0, 0, "bltu max < 0 false"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t p[] = {B(8, 2, 1, cases[i].f3), ADDI(10, 10, 1), ADDI(0, 0, 0)};
        load(p, 3);
        cpu.x[1] = cases[i].a;
        cpu.x[2] = cases[i].b;
        run(2);
        CHECK((cpu.x[10] == 0) == cases[i].taken, "%s", cases[i].name);
    }
    uint32_t back[] = {ADDI(1, 1, 1), B(-4, 2, 1, 1)}; /* loop until x1 == x2 */
    load(back, 2);
    cpu.x[2] = 5;
    run(100);
    CHECK(cpu.x[1] == 5, "backward branch loop counted to %u", cpu.x[1]);
}

static void test_misaligned_jump_traps_without_link(void) {
    uint32_t p[] = {J(6, 1)};
    load(p, 1);
    CHECK(run(1) == RV_MISALIGNED_FETCH, "jal to pc+6 traps");
    CHECK(cpu.x[1] == 0, "rd not written on trap");
    uint32_t q[] = {B(6, 0, 0, 0)};
    load(q, 1);
    CHECK(run(1) == RV_MISALIGNED_FETCH, "taken branch to misaligned target traps");
    uint32_t r[] = {B(6, 1, 0, 0), ADDI(3, 0, 1)};
    load(r, 2);
    cpu.x[1] = 1;
    CHECK(run(2) == RV_STEP_LIMIT && cpu.x[3] == 1, "not-taken misaligned branch does not trap");
}

static void test_loads_sign_and_zero_extend(void) {
    uint32_t p[] = {I(0, 1, 0, 2, 0x03), I(0, 1, 4, 3, 0x03), I(0, 1, 1, 4, 0x03), I(0, 1, 5, 5, 0x03),
                    I(0, 1, 2, 6, 0x03), I(-4, 7, 2, 8, 0x03)};
    load(p, 6);
    uint8_t data[] = {0x80, 0xFF, 0x34, 0x12};
    memcpy(cpu.mem + 0x1000, data, 4);
    cpu.x[1] = 0x1000;
    cpu.x[7] = 0x1004;
    run(6);
    CHECK(cpu.x[2] == 0xFFFFFF80, "lb sign-extends");
    CHECK(cpu.x[3] == 0x80, "lbu zero-extends");
    CHECK(cpu.x[4] == 0xFFFFFF80, "lh sign-extends 0xff80");
    CHECK(cpu.x[5] == 0xFF80, "lhu zero-extends");
    CHECK(cpu.x[6] == 0x1234FF80, "lw little-endian: %08x", cpu.x[6]);
    CHECK(cpu.x[8] == 0x1234FF80, "negative load offset");
}

static void test_stores_write_only_their_width(void) {
    uint32_t p[] = {S(0, 2, 1, 0), S(4, 2, 1, 1), S(8, 2, 1, 2)};
    load(p, 3);
    memset(cpu.mem + 0x2000, 0xAA, 12);
    cpu.x[1] = 0x2000;
    cpu.x[2] = 0x11223344;
    run(3);
    CHECK(cpu.mem[0x2000] == 0x44 && cpu.mem[0x2001] == 0xAA, "sb writes one byte");
    CHECK(cpu.mem[0x2004] == 0x44 && cpu.mem[0x2005] == 0x33 && cpu.mem[0x2006] == 0xAA, "sh writes two bytes");
    CHECK(!memcmp(cpu.mem + 0x2008, "\x44\x33\x22\x11", 4), "sw writes four bytes little-endian");
}

static void test_memory_faults(void) {
    uint32_t p[] = {I(2, 1, 2, 2, 0x03)};
    load(p, 1);
    cpu.x[1] = 0x1000;
    CHECK(run(1) == RV_MISALIGNED_ACCESS && cpu.fault_addr == 0x1002, "misaligned lw");
    uint32_t q[] = {S(0, 2, 1, 2)};
    load(q, 1);
    cpu.x[1] = RV_MEM_SIZE;
    CHECK(run(1) == RV_ACCESS_FAULT, "store past end of RAM");
    load(q, 1);
    cpu.x[1] = RV_MEM_SIZE - 4;
    CHECK(run(1) == RV_STEP_LIMIT, "store to the last word is fine");
    load(p, 1);
    cpu.pc = RV_MEM_SIZE;
    CHECK(run(1) == RV_ACCESS_FAULT, "fetch past end of RAM");
}

static void test_ecall_exit_and_write(void) {
    const char msg[] = "hi\n";
    uint32_t p[] = {ADDI(10, 0, 1), ADDI(11, 0, 0x100), ADDI(12, 0, 3), ADDI(17, 0, 64), ECALL,
                    ADDI(10, 0, 42), ADDI(17, 0, 93), ECALL};
    load(p, 8);
    memcpy(cpu.mem + 0x100, msg, 3);
    char buf[16] = {0};
    FILE *out = fmemopen(buf, sizeof buf, "w");
    cpu.out = out;
    rv_status s = run(100);
    fclose(out);
    cpu.out = stdout;
    CHECK(s == RV_EXIT && cpu.exit_code == 42, "exit(42): status %s code %d", rv_status_name(s), cpu.exit_code);
    CHECK(!strcmp(buf, "hi\n"), "write syscall output '%s'", buf);
    CHECK(cpu.steps == 8, "ecall retires as an instruction");
}

static void test_unknown_syscall_returns_enosys(void) {
    uint32_t p[] = {ADDI(17, 0, 999), ECALL};
    load(p, 2);
    run(2);
    CHECK(cpu.x[10] == (uint32_t)-38, "ENOSYS");
}

static void test_ebreak_and_illegal(void) {
    uint32_t p[] = {EBREAK};
    load(p, 1);
    CHECK(run(1) == RV_EBREAK, "ebreak stops");
    uint32_t z[] = {0x00000000};
    load(z, 1);
    CHECK(run(1) == RV_ILLEGAL, "all-zero word is defined illegal");
    uint32_t csr[] = {0x30002573}; /* csrr a0, mstatus */
    load(csr, 1);
    CHECK(run(1) == RV_ILLEGAL, "CSR access is not RV32I");
    uint32_t f7[] = {R(0x01, 2, 1, 0, 3, 0x33)}; /* mul: M extension */
    load(f7, 1);
    CHECK(run(1) == RV_ILLEGAL, "M-extension mul rejected");
}

static void test_fence_is_nop(void) {
    uint32_t p[] = {0x0FF0000F, 0x0000100F, ADDI(1, 0, 7)};
    load(p, 3);
    CHECK(run(3) == RV_STEP_LIMIT && cpu.x[1] == 7, "fence / fence.i");
}

/* ---- loader ------------------------------------------------------------ */
static size_t make_elf(uint8_t *e, uint32_t entry, uint32_t paddr, uint32_t filesz, uint32_t memsz) {
    memset(e, 0, 128);
    memcpy(e, "\x7f" "ELF\x01\x01\x01", 7);
    e[16] = 2; e[18] = 243;                         /* ET_EXEC, EM_RISCV */
    e[24] = entry & 0xFF; e[25] = entry >> 8 & 0xFF; e[26] = entry >> 16 & 0xFF;
    e[28] = 52;                                    /* phoff */
    e[42] = 32; e[44] = 1;                          /* phentsize, phnum */
    uint8_t *ph = e + 52;
    ph[0] = 1;                                     /* PT_LOAD */
    ph[4] = 84;                                    /* offset */
    for (int i = 0; i < 4; i++) {
        ph[12 + i] = paddr >> (8 * i) & 0xFF;
        ph[16 + i] = filesz >> (8 * i) & 0xFF;
        ph[20 + i] = memsz >> (8 * i) & 0xFF;
    }
    uint32_t prog[] = {ADDI(10, 0, 7), ADDI(17, 0, 93), ECALL};
    memcpy(e + 84, prog, sizeof prog);
    return 84 + sizeof prog;
}

static void test_elf_loader(void) {
    uint8_t e[128];
    uint32_t entry = 0;
    size_t n = make_elf(e, 0x1000, 0x1000, 12, 64);
    memset(cpu.mem, 0xEE, RV_MEM_SIZE);
    CHECK(rv_load_elf(&cpu, e, n, &entry) == 0 && entry == 0x1000, "valid ELF loads");
    CHECK(cpu.mem[0x1000 + 12] == 0 && cpu.mem[0x1000 + 63] == 0, ".bss is zeroed");
    rv_reset(&cpu, entry);
    CHECK(run(10) == RV_EXIT && cpu.exit_code == 7, "loaded program runs");

    fprintf(stderr, "(expected load errors follow)\n");
    CHECK(rv_load_elf(&cpu, e, 40, &entry) != 0, "truncated header rejected");
    n = make_elf(e, 0x1000, 0x1000, 12, 64); e[0] = 0;
    CHECK(rv_load_elf(&cpu, e, n, &entry) != 0, "bad magic rejected");
    n = make_elf(e, 0x1000, 0x1000, 12, 64); e[18] = 62;
    CHECK(rv_load_elf(&cpu, e, n, &entry) != 0, "x86-64 machine rejected");
    n = make_elf(e, 0x1000, RV_MEM_SIZE - 8, 12, 64);
    CHECK(rv_load_elf(&cpu, e, n, &entry) != 0, "segment past RAM rejected");
    n = make_elf(e, 0x1000, 0x1000, 4000, 4000);
    CHECK(rv_load_elf(&cpu, e, n, &entry) != 0, "segment data past end of file rejected");
    n = make_elf(e, 0x1000, 0x1000, 64, 12);
    CHECK(rv_load_elf(&cpu, e, n, &entry) != 0, "filesz > memsz rejected");
    n = make_elf(e, 0x1002, 0x1000, 12, 64);
    CHECK(rv_load_elf(&cpu, e, n, &entry) != 0, "misaligned entry rejected");
}

int main(void) {
    if (rv_init(&cpu)) return 1;
    test_x0_is_hardwired();
    test_addi_sign_extends();
    test_add_sub_wrap();
    test_slt_signed_vs_unsigned();
    test_logic_immediates();
    test_shifts();
    test_slli_with_bit25_is_illegal();
    test_lui_auipc();
    test_jal_jalr();
    test_branches();
    test_misaligned_jump_traps_without_link();
    test_loads_sign_and_zero_extend();
    test_stores_write_only_their_width();
    test_memory_faults();
    test_ecall_exit_and_write();
    test_unknown_syscall_returns_enosys();
    test_ebreak_and_illegal();
    test_fence_is_nop();
    test_elf_loader();
    rv_free(&cpu);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
