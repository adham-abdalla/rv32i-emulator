/* Runtime support: output through the emulator's write syscall, plus the helper
 * routines the compiler calls on RV32I, which has no multiply or divide instructions. */
#include "rt.h"

static long sys_write(const void *buf, size_t len) {
    register long a0 __asm__("a0") = 1;
    register long a1 __asm__("a1") = (long)buf;
    register long a2 __asm__("a2") = (long)len;
    register long a7 __asm__("a7") = 64;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

void put_str(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    sys_write(s, n);
}

void put_uint(uint32_t v) {
    char buf[11];
    int i = 10;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put_str(buf + i);
}

void put_int(int32_t v) {
    if (v < 0) { put_str("-"); put_uint(0u - (uint32_t)v); }
    else put_uint((uint32_t)v);
}

void put_hex(uint32_t v) {
    char buf[11] = "0x";
    for (int i = 0; i < 8; i++) buf[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    buf[10] = 0;
    put_str(buf);
}

/* ---- compiler-rt replacements (shift-and-add / restoring division) ---- */
uint32_t __mulsi3(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    while (b) { if (b & 1) r += a; a <<= 1; b >>= 1; }
    return r;
}

uint32_t __udivmodsi4(uint32_t n, uint32_t d, uint32_t *rem) {
    if (d == 0) { if (rem) *rem = n; return 0xFFFFFFFFu; } /* matches the M extension's result */
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    if (rem) *rem = r;
    return q;
}
uint32_t __udivsi3(uint32_t n, uint32_t d) { return __udivmodsi4(n, d, 0); }
uint32_t __umodsi3(uint32_t n, uint32_t d) { uint32_t r; __udivmodsi4(n, d, &r); return r; }
int32_t __divsi3(int32_t a, int32_t b) {
    uint32_t q = __udivsi3(a < 0 ? 0u - (uint32_t)a : (uint32_t)a, b < 0 ? 0u - (uint32_t)b : (uint32_t)b);
    return (a < 0) != (b < 0) ? (int32_t)(0u - q) : (int32_t)q;
}
int32_t __modsi3(int32_t a, int32_t b) {
    uint32_t r = __umodsi3(a < 0 ? 0u - (uint32_t)a : (uint32_t)a, b < 0 ? 0u - (uint32_t)b : (uint32_t)b);
    return a < 0 ? (int32_t)(0u - r) : (int32_t)r;
}

void *memset(void *p, int c, size_t n) { uint8_t *d = p; while (n--) *d++ = (uint8_t)c; return p; }
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
