/* Quicksort 256 pseudo-random signed values, then verify order and checksum.
 * Exercises loads/stores, signed compares, multiply/divide helpers and recursion. */
#include "rt.h"

#define N 256
static int32_t v[N];

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }

static void quicksort(int32_t *a, int lo, int hi) {
    while (lo < hi) {
        int32_t pivot = a[lo + (hi - lo) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) i++;
            while (a[j] > pivot) j--;
            if (i <= j) { int32_t t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { quicksort(a, lo, j); lo = i; }
        else { quicksort(a, i, hi); hi = j; }
    }
}

int main(void) {
    uint32_t seed = 2026, sum = 0;
    for (int i = 0; i < N; i++) { v[i] = (int32_t)lcg(&seed) % 100000; sum += (uint32_t)v[i]; }
    quicksort(v, 0, N - 1);
    uint32_t after = 0;
    int sorted = 1;
    for (int i = 0; i < N; i++) { after += (uint32_t)v[i]; if (i && v[i - 1] > v[i]) sorted = 0; }
    put_str("min "); put_int(v[0]); put_str(" max "); put_int(v[N - 1]);
    put_str(" sorted "); put_str(sorted ? "yes" : "NO");
    put_str(" checksum "); put_hex(after); put_str("\n");
    return sorted && after == sum ? 0 : 1;
}
