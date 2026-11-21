/* RV32I instruction-set emulator: public interface. */
#ifndef RV32_H
#define RV32_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define RV_MEM_SIZE (1u << 20) /* 1 MiB of RAM mapped at address 0 */

typedef enum {
    RV_OK = 0,
    RV_EXIT,               /* program called exit via ECALL */
    RV_EBREAK,             /* EBREAK hit */
    RV_ILLEGAL,            /* undefined or unsupported encoding */
    RV_MISALIGNED_FETCH,   /* jump/branch target not 4-byte aligned */
    RV_MISALIGNED_ACCESS,  /* load/store not naturally aligned */
    RV_ACCESS_FAULT,       /* address outside RAM */
    RV_STEP_LIMIT          /* ran out of the step budget */
} rv_status;

/* Instruction classes, used for the coverage histogram. */
enum {
    OP_LUI, OP_AUIPC, OP_JAL, OP_JALR,
    OP_BEQ, OP_BNE, OP_BLT, OP_BGE, OP_BLTU, OP_BGEU,
    OP_LB, OP_LH, OP_LW, OP_LBU, OP_LHU, OP_SB, OP_SH, OP_SW,
    OP_ADDI, OP_SLTI, OP_SLTIU, OP_XORI, OP_ORI, OP_ANDI, OP_SLLI, OP_SRLI, OP_SRAI,
    OP_ADD, OP_SUB, OP_SLL, OP_SLT, OP_SLTU, OP_XOR, OP_SRL, OP_SRA, OP_OR, OP_AND,
    OP_FENCE, OP_ECALL, OP_EBREAK,
    OP_COUNT
};
extern const char *const rv_op_names[OP_COUNT];

typedef struct {
    uint32_t x[32];
    uint32_t pc;
    uint8_t *mem;
    uint64_t steps;
    uint64_t op_count[OP_COUNT];
    int exit_code;
    uint32_t fault_addr;       /* address that caused the last fault */
    FILE *trace;               /* if set, one line per retired instruction */
    FILE *out;                 /* where the write syscall goes (stdout by default) */
} rv_cpu;

int rv_init(rv_cpu *cpu);                 /* allocates RAM; 0 on success */
void rv_free(rv_cpu *cpu);
void rv_reset(rv_cpu *cpu, uint32_t entry);
rv_status rv_step(rv_cpu *cpu);
rv_status rv_run(rv_cpu *cpu, uint64_t max_steps);
const char *rv_status_name(rv_status s);

/* Load a flat binary at addr, or a statically linked ELF32 RISC-V executable.
   Return 0 on success, -1 with a message on stderr otherwise. */
int rv_load_bin(rv_cpu *cpu, const uint8_t *data, size_t len, uint32_t addr);
int rv_load_elf(rv_cpu *cpu, const uint8_t *data, size_t len, uint32_t *entry);

#endif
