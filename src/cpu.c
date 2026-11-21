/* RV32I base integer ISA: fetch, decode, execute.
 * Follows the RISC-V Unprivileged ISA spec (20191213), chapter 2.
 * No CSRs or privileged mode: ECALL is handled as a tiny Linux-style syscall layer. */
#include "rv32.h"

#include <stdlib.h>
#include <string.h>

const char *const rv_op_names[OP_COUNT] = {
    "lui", "auipc", "jal", "jalr",
    "beq", "bne", "blt", "bge", "bltu", "bgeu",
    "lb", "lh", "lw", "lbu", "lhu", "sb", "sh", "sw",
    "addi", "slti", "sltiu", "xori", "ori", "andi", "slli", "srli", "srai",
    "add", "sub", "sll", "slt", "sltu", "xor", "srl", "sra", "or", "and",
    "fence", "ecall", "ebreak",
};

const char *rv_status_name(rv_status s) {
    static const char *const n[] = {"ok", "exit", "ebreak", "illegal instruction", "misaligned fetch",
                                    "misaligned access", "access fault", "step limit"};
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "?";
}

int rv_init(rv_cpu *cpu) {
    memset(cpu, 0, sizeof *cpu);
    cpu->mem = calloc(1, RV_MEM_SIZE);
    cpu->out = stdout;
    return cpu->mem ? 0 : -1;
}

void rv_free(rv_cpu *cpu) {
    free(cpu->mem);
    cpu->mem = NULL;
}

void rv_reset(rv_cpu *cpu, uint32_t entry) {
    memset(cpu->x, 0, sizeof cpu->x);
    memset(cpu->op_count, 0, sizeof cpu->op_count);
    cpu->pc = entry;
    cpu->steps = 0;
    cpu->exit_code = 0;
    cpu->x[2] = RV_MEM_SIZE - 16; /* sp: top of RAM, 16-byte aligned */
}

/* ---- field extraction ------------------------------------------------- */
static inline uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }
static inline int32_t sext(uint32_t v, int width) { return (int32_t)(v << (32 - width)) >> (32 - width); }

static inline int32_t imm_i(uint32_t in) { return (int32_t)in >> 20; }
static inline int32_t imm_s(uint32_t in) { return sext((bits(in, 31, 25) << 5) | bits(in, 11, 7), 12); }
static inline int32_t imm_b(uint32_t in) {
    return sext((bits(in, 31, 31) << 12) | (bits(in, 7, 7) << 11) | (bits(in, 30, 25) << 5) | (bits(in, 11, 8) << 1), 13);
}
static inline uint32_t imm_u(uint32_t in) { return in & 0xFFFFF000u; }
static inline int32_t imm_j(uint32_t in) {
    return sext((bits(in, 31, 31) << 20) | (bits(in, 19, 12) << 12) | (bits(in, 20, 20) << 11) | (bits(in, 30, 21) << 1), 21);
}

/* ---- memory ------------------------------------------------------------ */
static rv_status check(rv_cpu *c, uint32_t addr, uint32_t size) {
    if (addr & (size - 1)) { c->fault_addr = addr; return RV_MISALIGNED_ACCESS; }
    if (addr > RV_MEM_SIZE - size) { c->fault_addr = addr; return RV_ACCESS_FAULT; }
    return RV_OK;
}

static uint32_t rd_le(const uint8_t *p, uint32_t size) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static void wr_le(uint8_t *p, uint32_t v, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* ---- syscalls (a7 = number, Linux RISC-V numbering) -------------------- */
static rv_status ecall(rv_cpu *c) {
    uint32_t num = c->x[17], a0 = c->x[10], a1 = c->x[11], a2 = c->x[12];
    switch (num) {
    case 93: /* exit(code) */
        c->exit_code = (int32_t)a0;
        return RV_EXIT;
    case 64: /* write(fd, buf, len): only stdout/stderr */
        if ((a0 != 1 && a0 != 2) || a1 > RV_MEM_SIZE || a2 > RV_MEM_SIZE - a1) {
            c->x[10] = (uint32_t)-9; /* EBADF / EFAULT collapsed */
            return RV_OK;
        }
        fwrite(c->mem + a1, 1, a2, c->out);
        c->x[10] = a2;
        return RV_OK;
    default:
        c->x[10] = (uint32_t)-38; /* ENOSYS */
        return RV_OK;
    }
}

/* ---- execute one instruction ------------------------------------------- */
rv_status rv_step(rv_cpu *c) {
    if (c->pc & 3) { c->fault_addr = c->pc; return RV_MISALIGNED_FETCH; }
    if (c->pc > RV_MEM_SIZE - 4) { c->fault_addr = c->pc; return RV_ACCESS_FAULT; }

    const uint32_t in = rd_le(c->mem + c->pc, 4);
    const uint32_t opcode = bits(in, 6, 0), rd = bits(in, 11, 7), f3 = bits(in, 14, 12);
    const uint32_t rs1 = bits(in, 19, 15), rs2 = bits(in, 24, 20), f7 = bits(in, 31, 25);
    const uint32_t a = c->x[rs1], b = c->x[rs2];
    uint32_t next = c->pc + 4, val = 0;
    int write_rd = 0, op = -1;
    rv_status st = RV_OK;
    uint32_t st_addr = 0, st_val = 0, st_size = 0;

    switch (opcode) {
    case 0x37: op = OP_LUI; val = imm_u(in); write_rd = 1; break;
    case 0x17: op = OP_AUIPC; val = c->pc + imm_u(in); write_rd = 1; break;
    case 0x6F: op = OP_JAL; val = c->pc + 4; next = c->pc + (uint32_t)imm_j(in); write_rd = 1; break;
    case 0x67:
        if (f3 != 0) return RV_ILLEGAL;
        op = OP_JALR; val = c->pc + 4; next = (a + (uint32_t)imm_i(in)) & ~1u; write_rd = 1;
        break;
    case 0x63: {
        int take;
        switch (f3) {
        case 0: op = OP_BEQ; take = a == b; break;
        case 1: op = OP_BNE; take = a != b; break;
        case 4: op = OP_BLT; take = (int32_t)a < (int32_t)b; break;
        case 5: op = OP_BGE; take = (int32_t)a >= (int32_t)b; break;
        case 6: op = OP_BLTU; take = a < b; break;
        case 7: op = OP_BGEU; take = a >= b; break;
        default: return RV_ILLEGAL;
        }
        if (take) next = c->pc + (uint32_t)imm_b(in);
        break;
    }
    case 0x03: {
        static const int ops[8] = {OP_LB, OP_LH, OP_LW, -1, OP_LBU, OP_LHU, -1, -1};
        static const uint32_t size[8] = {1, 2, 4, 0, 1, 2, 0, 0};
        if (ops[f3] < 0) return RV_ILLEGAL;
        op = ops[f3];
        uint32_t addr = a + (uint32_t)imm_i(in);
        if ((st = check(c, addr, size[f3])) != RV_OK) return st;
        val = rd_le(c->mem + addr, size[f3]);
        if (f3 == 0) val = (uint32_t)sext(val, 8);
        if (f3 == 1) val = (uint32_t)sext(val, 16);
        write_rd = 1;
        break;
    }
    case 0x23: {
        static const int ops[8] = {OP_SB, OP_SH, OP_SW, -1, -1, -1, -1, -1};
        if (f3 > 2) return RV_ILLEGAL;
        op = ops[f3];
        st_size = 1u << f3;
        st_addr = a + (uint32_t)imm_s(in);
        if ((st = check(c, st_addr, st_size)) != RV_OK) return st;
        st_val = st_size == 4 ? b : b & ((1u << (8 * st_size)) - 1);
        wr_le(c->mem + st_addr, b, st_size);
        break;
    }
    case 0x13: {
        const int32_t imm = imm_i(in);
        const uint32_t shamt = rs2;
        switch (f3) {
        case 0: op = OP_ADDI; val = a + (uint32_t)imm; break;
        case 2: op = OP_SLTI; val = (int32_t)a < imm; break;
        case 3: op = OP_SLTIU; val = a < (uint32_t)imm; break;
        case 4: op = OP_XORI; val = a ^ (uint32_t)imm; break;
        case 6: op = OP_ORI; val = a | (uint32_t)imm; break;
        case 7: op = OP_ANDI; val = a & (uint32_t)imm; break;
        case 1:
            if (f7 != 0) return RV_ILLEGAL;
            op = OP_SLLI; val = a << shamt; break;
        case 5:
            if (f7 == 0x00) { op = OP_SRLI; val = a >> shamt; }
            else if (f7 == 0x20) { op = OP_SRAI; val = (uint32_t)((int32_t)a >> shamt); }
            else return RV_ILLEGAL;
            break;
        }
        write_rd = 1;
        break;
    }
    case 0x33: {
        const uint32_t sh = b & 31;
        if (f7 == 0x00) {
            switch (f3) {
            case 0: op = OP_ADD; val = a + b; break;
            case 1: op = OP_SLL; val = a << sh; break;
            case 2: op = OP_SLT; val = (int32_t)a < (int32_t)b; break;
            case 3: op = OP_SLTU; val = a < b; break;
            case 4: op = OP_XOR; val = a ^ b; break;
            case 5: op = OP_SRL; val = a >> sh; break;
            case 6: op = OP_OR; val = a | b; break;
            case 7: op = OP_AND; val = a & b; break;
            }
        } else if (f7 == 0x20 && f3 == 0) { op = OP_SUB; val = a - b; }
        else if (f7 == 0x20 && f3 == 5) { op = OP_SRA; val = (uint32_t)((int32_t)a >> sh); }
        else return RV_ILLEGAL;
        write_rd = 1;
        break;
    }
    case 0x0F:
        if (f3 > 1) return RV_ILLEGAL;
        op = OP_FENCE; /* single hart, in-order: FENCE and FENCE.I are no-ops */
        break;
    case 0x73:
        if (in == 0x00000073) { op = OP_ECALL; st = ecall(c); }
        else if (in == 0x00100073) { op = OP_EBREAK; st = RV_EBREAK; }
        else return RV_ILLEGAL; /* CSR instructions are not part of RV32I base */
        break;
    default:
        return RV_ILLEGAL;
    }

    if ((opcode == 0x6F || opcode == 0x67 || opcode == 0x63) && (next & 3)) {
        c->fault_addr = next;
        return RV_MISALIGNED_FETCH; /* spec: exception on the jump, rd not written */
    }
    if (write_rd && rd != 0) c->x[rd] = val;
    c->x[0] = 0;
    c->op_count[op]++;
    c->steps++;

    if (c->trace) {
        fprintf(c->trace, "%08x %08x", c->pc, in);
        if (write_rd && rd != 0) fprintf(c->trace, " x%u=%08x", rd, val);
        if (st_size) fprintf(c->trace, " m%u[%08x]=%08x", st_size, st_addr, st_val);
        if (op == OP_ECALL && c->x[17] != 93) fprintf(c->trace, " x10=%08x", c->x[10]);
        fputc('\n', c->trace);
    }
    c->pc = next;
    return st;
}

rv_status rv_run(rv_cpu *c, uint64_t max_steps) {
    for (uint64_t i = 0; i < max_steps; i++) {
        rv_status s = rv_step(c);
        if (s != RV_OK) return s;
    }
    return RV_STEP_LIMIT;
}
