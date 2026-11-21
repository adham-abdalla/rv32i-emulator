/* Tiny freestanding runtime for the sample programs. */
#ifndef RT_H
#define RT_H
#include <stdint.h>
#include <stddef.h>

void put_str(const char *s);
void put_uint(uint32_t v);
void put_int(int32_t v);
void put_hex(uint32_t v);
#endif
