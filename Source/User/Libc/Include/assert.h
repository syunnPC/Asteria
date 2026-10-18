// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_LIBC_ASSERT_H_INCLUDED
#define ASTERIA_LIBC_ASSERT_H_INCLUDED

#include "asteria_libc.h"

ASTERIA_LIBC_BEGIN
ASTERIA_LIBC_NORETURN void ASTERIA_LIBC_NAME(__asteria_assert_fail)(const char* expression, const char* file,
                                                                    unsigned int line,
                                                                    const char* function) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_END

#endif

#ifdef assert
#undef assert
#endif
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression)                                                                                             \
    ((expression) ? (void)0 : ASTERIA_LIBC_NAME(__asteria_assert_fail)(#expression, __FILE__, __LINE__, __func__))
#endif
