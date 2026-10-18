// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_LIBC_ABI_H_INCLUDED
#define ASTERIA_LIBC_ABI_H_INCLUDED

/* Architecture selection, never a test macro: hosted symbols cannot interpose libc. */
#if defined(ASTERIA_TARGET_AMD64) || defined(ASTERIA_TARGET_IA64)
#define ASTERIA_LIBC_NAME(name) name
#define ASTERIA_LIBC_TYPE(name) name
#else
#define ASTERIA_LIBC_NAME(name) asteria_libc_##name
#define ASTERIA_LIBC_TYPE(name) asteria_libc_##name
#endif
#ifdef __cplusplus
#define ASTERIA_LIBC_BEGIN                                                                                             \
    extern "C"                                                                                                         \
    {
#define ASTERIA_LIBC_END }
#define ASTERIA_LIBC_NOEXCEPT noexcept
#define ASTERIA_LIBC_NODISCARD [[nodiscard]]
#define ASTERIA_LIBC_NORETURN [[noreturn]]
#else
#define ASTERIA_LIBC_BEGIN
#define ASTERIA_LIBC_END
#define ASTERIA_LIBC_NOEXCEPT
#define ASTERIA_LIBC_NODISCARD
#define ASTERIA_LIBC_NORETURN _Noreturn
#endif
#endif
