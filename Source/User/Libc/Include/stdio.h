// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_LIBC_STDIO_H_INCLUDED
#define ASTERIA_LIBC_STDIO_H_INCLUDED

#include "asteria_libc.h"
#include <stddef.h>
#include <stdarg.h>

ASTERIA_LIBC_BEGIN
typedef struct AsteriaFile ASTERIA_LIBC_TYPE(FILE);
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(snprintf)(char* destination, size_t capacity, const char* format,
                                                       ...) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(vsnprintf)(char* destination, size_t capacity, const char* format,
                                                        va_list arguments) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(sprintf)(char* destination, const char* format, ...) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(vsprintf)(char* destination, const char* format,
                                                       va_list arguments) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_END

#endif
