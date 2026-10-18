// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_LIBC_STDLIB_H_INCLUDED
#define ASTERIA_LIBC_STDLIB_H_INCLUDED

#include "asteria_libc.h"
#include <stddef.h>

ASTERIA_LIBC_BEGIN
#define RAND_MAX 2147483647
/* The div structures follow the target C ABI. */
typedef struct
{
    int quot;
    int rem;
} ASTERIA_LIBC_TYPE(div_t);
typedef struct
{
    long quot;
    long rem;
} ASTERIA_LIBC_TYPE(ldiv_t);
typedef struct
{
    long long quot;
    long long rem;
} ASTERIA_LIBC_TYPE(lldiv_t);
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(abs)(int value) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD long ASTERIA_LIBC_NAME(labs)(long value) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD long long ASTERIA_LIBC_NAME(llabs)(long long value) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD ASTERIA_LIBC_TYPE(div_t)
    ASTERIA_LIBC_NAME(div)(int numerator, int denominator) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD ASTERIA_LIBC_TYPE(ldiv_t)
    ASTERIA_LIBC_NAME(ldiv)(long numerator, long denominator) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD ASTERIA_LIBC_TYPE(lldiv_t)
    ASTERIA_LIBC_NAME(lldiv)(long long numerator, long long denominator) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(atoi)(const char* text) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD long ASTERIA_LIBC_NAME(atol)(const char* text) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD long long ASTERIA_LIBC_NAME(atoll)(const char* text) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD long ASTERIA_LIBC_NAME(strtol)(const char* text, char** end, int base) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD unsigned long ASTERIA_LIBC_NAME(strtoul)(const char* text, char** end,
                                                                int base) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD long long ASTERIA_LIBC_NAME(strtoll)(const char* text, char** end,
                                                            int base) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD unsigned long long ASTERIA_LIBC_NAME(strtoull)(const char* text, char** end,
                                                                      int base) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD double ASTERIA_LIBC_NAME(strtod)(const char* text, char** end) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD float ASTERIA_LIBC_NAME(strtof)(const char* text, char** end) ASTERIA_LIBC_NOEXCEPT;
void ASTERIA_LIBC_NAME(qsort)(void* base, size_t count, size_t size,
                              int (*compare)(const void*, const void*)) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD void* ASTERIA_LIBC_NAME(bsearch)(const void* key, const void* base, size_t count, size_t size,
                                                        int (*compare)(const void*, const void*)) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(rand)(void) ASTERIA_LIBC_NOEXCEPT;
void ASTERIA_LIBC_NAME(srand)(unsigned int seed) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD void* ASTERIA_LIBC_NAME(malloc)(size_t size) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD void* ASTERIA_LIBC_NAME(calloc)(size_t count, size_t size) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD void* ASTERIA_LIBC_NAME(realloc)(void* pointer, size_t size) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD void* ASTERIA_LIBC_NAME(aligned_alloc)(size_t alignment, size_t size) ASTERIA_LIBC_NOEXCEPT;
void ASTERIA_LIBC_NAME(free)(void* pointer) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(posix_memalign)(void** result, size_t alignment,
                                                             size_t size) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_END

#endif
