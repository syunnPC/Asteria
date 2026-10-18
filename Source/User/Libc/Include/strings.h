// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_LIBC_STRINGS_H_INCLUDED
#define ASTERIA_LIBC_STRINGS_H_INCLUDED

#include "asteria_libc.h"
#include <stddef.h>

ASTERIA_LIBC_BEGIN
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(strcasecmp)(const char* left, const char* right) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(strncasecmp)(const char* left, const char* right,
                                                          size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_END

#endif
