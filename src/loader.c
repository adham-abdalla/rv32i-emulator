/* Program loaders: flat binary and ELF32 (statically linked, RISC-V, little endian).
 * Every offset and size from the file is bounds-checked before use, because the
 * input is untrusted: a truncated or hostile ELF must fail cleanly, not crash. */
#include "rv32.h"

#include <string.h>

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int fail(const char *why) {
    fprintf(stderr, "load error: %s\n", why);
    return -1;
}

int rv_load_bin(rv_cpu *cpu, const uint8_t *data, size_t len, uint32_t addr) {
    if (addr > RV_MEM_SIZE || len > RV_MEM_SIZE - addr) return fail("binary does not fit in RAM");
    memcpy(cpu->mem + addr, data, len);
    return 0;
}

int rv_load_elf(rv_cpu *cpu, const uint8_t *d, size_t len, uint32_t *entry) {
    if (len < 52) return fail("file too small for an ELF header");
    if (memcmp(d, "\x7f" "ELF", 4) != 0) return fail("not an ELF file");
    if (d[4] != 1) return fail("not ELF32");
    if (d[5] != 1) return fail("not little endian");
    if (u16(d + 16) != 2) return fail("not an executable (ET_EXEC)");
    if (u16(d + 18) != 243) return fail("not a RISC-V binary (e_machine != 243)");

    const uint32_t phoff = u32(d + 28);
    const uint16_t phentsize = u16(d + 42), phnum = u16(d + 44);
    if (phentsize < 32) return fail("bad program header size");
    if (phoff > len || (uint64_t)phnum * phentsize > len - phoff) return fail("program headers outside file");

    int loaded = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = d + phoff + (size_t)i * phentsize;
        if (u32(ph) != 1) continue; /* PT_LOAD only */
        const uint32_t off = u32(ph + 4), paddr = u32(ph + 12), filesz = u32(ph + 16), memsz = u32(ph + 20);
        if (filesz > memsz) return fail("segment filesz > memsz");
        if (off > len || filesz > len - off) return fail("segment data outside file");
        if (paddr > RV_MEM_SIZE || memsz > RV_MEM_SIZE - paddr) return fail("segment does not fit in RAM");
        memcpy(cpu->mem + paddr, d + off, filesz);
        memset(cpu->mem + paddr + filesz, 0, memsz - filesz); /* .bss */
        loaded++;
    }
    if (!loaded) return fail("no loadable segments");
    *entry = u32(d + 24);
    if (*entry & 3 || *entry > RV_MEM_SIZE - 4) return fail("entry point invalid");
    return 0;
}
