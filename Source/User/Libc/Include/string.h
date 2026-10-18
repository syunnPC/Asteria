// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_LIBC_STRING_H_INCLUDED
#define ASTERIA_LIBC_STRING_H_INCLUDED

#include "asteria_libc.h"
#include <stddef.h>

ASTERIA_LIBC_BEGIN
ASTERIA_LIBC_NODISCARD void* ASTERIA_LIBC_NAME(memcpy)(void* destination, const void* source,
                                                       size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD void* ASTERIA_LIBC_NAME(memmove)(void* destination, const void* source,
                                                        size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD void* ASTERIA_LIBC_NAME(memset)(void* destination, int value,
                                                       size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD void* ASTERIA_LIBC_NAME(memchr)(const void* data, int value, size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(memcmp)(const void* left, const void* right,
                                                     size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strcpy)(char* destination, const char* source) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strncpy)(char* destination, const char* source,
                                                        size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strcat)(char* destination, const char* source) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strncat)(char* destination, const char* source,
                                                        size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strchr)(const char* text, int value) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strrchr)(const char* text, int value) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strstr)(const char* text, const char* needle) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strpbrk)(const char* text, const char* accept) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strtok)(char* text, const char* delimiters) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strtok_r)(char* text, const char* delimiters,
                                                         char** state) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strdup)(const char* text) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strndup)(const char* text, size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD char* ASTERIA_LIBC_NAME(strerror)(int error) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD size_t ASTERIA_LIBC_NAME(strlen)(const char* text) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD size_t ASTERIA_LIBC_NAME(strnlen)(const char* text, size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD size_t ASTERIA_LIBC_NAME(strspn)(const char* text, const char* accept) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD size_t ASTERIA_LIBC_NAME(strcspn)(const char* text, const char* reject) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD size_t ASTERIA_LIBC_NAME(strxfrm)(char* destination, const char* source,
                                                         size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(strcmp)(const char* left, const char* right) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(strncmp)(const char* left, const char* right,
                                                      size_t count) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_NODISCARD int ASTERIA_LIBC_NAME(strcoll)(const char* left, const char* right) ASTERIA_LIBC_NOEXCEPT;
ASTERIA_LIBC_END

#endif
