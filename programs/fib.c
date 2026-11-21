/* Iterative and recursive Fibonacci: exercises calls, the stack and 32-bit wraparound. */
#include "rt.h"

static uint32_t fib_rec(uint32_t n) { return n < 2 ? n : fib_rec(n - 1) + fib_rec(n - 2); }

int main(void) {
    uint32_t a = 0, b = 1;
    for (int i = 0; i < 50; i++) { uint32_t t = a + b; a = b; b = t; }
    put_str("fib(50) mod 2^32 = "); put_uint(a); put_str("\n");
    put_str("fib_rec(20) = "); put_uint(fib_rec(20)); put_str("\n");
    return fib_rec(20) == 6765 ? 0 : 1;
}
