/* Sieve of Eratosthenes up to 10000 on bytes: exercises sb/lbu and tight loops. */
#include "rt.h"

#define LIMIT 10000
static uint8_t composite[LIMIT + 1];

int main(void) {
    uint32_t count = 0, last = 0;
    for (uint32_t i = 2; i <= LIMIT; i++) {
        if (composite[i]) continue;
        count++;
        last = i;
        for (uint32_t j = i * i; j <= LIMIT; j += i) composite[j] = 1;
    }
    put_str("primes <= 10000: "); put_uint(count); put_str(", largest "); put_uint(last); put_str("\n");
    return count == 1229 ? 0 : 1;
}
