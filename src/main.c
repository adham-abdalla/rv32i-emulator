/* rv32emu: run an RV32I program.
 *
 *   rv32emu [--trace FILE] [--stats] [--max-steps N] [--bin ADDR] program
 *
 * Exit status: the guest's exit code, or 128 + status for a fault, so scripts
 * and CI can tell "program failed" from "emulator stopped it". */
#include "rv32.h"

#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *buf = n >= 0 ? malloc(n ? (size_t)n : 1) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void usage(void) {
    fprintf(stderr, "usage: rv32emu [--trace FILE] [--stats] [--max-steps N] [--bin ADDR] program\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *trace_path = NULL, *prog = NULL;
    int stats = 0, raw = 0;
    uint64_t max_steps = 100000000ULL;
    uint32_t base = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_path = argv[++i];
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--max-steps") && i + 1 < argc) max_steps = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--bin") && i + 1 < argc) { raw = 1; base = (uint32_t)strtoul(argv[++i], NULL, 0); }
        else if (argv[i][0] == '-') usage();
        else prog = argv[i];
    }
    if (!prog) usage();

    size_t len;
    uint8_t *data = read_file(prog, &len);
    if (!data) return 2;

    rv_cpu cpu;
    if (rv_init(&cpu)) { fprintf(stderr, "out of memory\n"); return 2; }
    uint32_t entry = base;
    int rc = raw ? rv_load_bin(&cpu, data, len, base) : rv_load_elf(&cpu, data, len, &entry);
    free(data);
    if (rc) { rv_free(&cpu); return 2; }

    rv_reset(&cpu, entry);
    if (trace_path && !(cpu.trace = fopen(trace_path, "w"))) { perror(trace_path); return 2; }
    rv_status st = rv_run(&cpu, max_steps);
    fflush(stdout);
    if (cpu.trace) fclose(cpu.trace);

    if (stats) {
        fprintf(stderr, "retired %llu instructions, stopped: %s\n", (unsigned long long)cpu.steps, rv_status_name(st));
        for (int i = 0; i < OP_COUNT; i++)
            if (cpu.op_count[i]) fprintf(stderr, "  %-6s %llu\n", rv_op_names[i], (unsigned long long)cpu.op_count[i]);
    }
    int status;
    if (st == RV_EXIT) status = cpu.exit_code & 0xFF;
    else {
        fprintf(stderr, "stopped: %s at pc=%08x (addr %08x)\n", rv_status_name(st), cpu.pc, cpu.fault_addr);
        status = 128 + (int)st;
    }
    rv_free(&cpu);
    return status;
}
