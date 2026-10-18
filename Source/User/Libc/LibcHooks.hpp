// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_LIBC_LIBC_HOOKS_HPP_INCLUDED
#define ASTERIA_USER_LIBC_LIBC_HOOKS_HPP_INCLUDED

#include "Include/asteria_libc.h"

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

namespace Asteria::User::Libc
{
// Pages are 4096 bytes, aligned to 4096, contiguous and initially writable.
// Callbacks must not reenter libc, throw, or call the kernel except in the personality.
struct AsteriaPageProvider
{
    void* Context;
    void* (*Reserve)(void* context, size_t pages) noexcept;
    bool (*Release)(void* context, void* base, size_t pages) noexcept;
};

struct LockHook
{
    void* Context;
    void (*Lock)(void* context) noexcept;
    void (*Unlock)(void* context) noexcept;
};

struct Hooks
{
    AsteriaPageProvider Pages;
    LockHook HeapLock;
    LockHook ConversionLock;
    int* (*ErrnoLocation)() noexcept;
    void (*AssertFail)(const char* expression, const char* file, unsigned int line, const char* function) noexcept;
};

// Install exactly once, before concurrent use. Null hooks select single-thread defaults.
// The two locks must be distinct; sink callbacks run under ConversionLock.
[[nodiscard]] bool InstallHooks(const Hooks& hooks) noexcept;
[[nodiscard]] const Hooks& GetHooks() noexcept;
void SetError(int error) noexcept;
void Lock(const LockHook& hook) noexcept;
void Unlock(const LockHook& hook) noexcept;

// Write returns false on failure. Repetition is compressed so snprintf with huge
// precision is bounded by the destination size rather than the would-be output.
struct FormatSink
{
    void* Context;
    bool (*Write)(void* context, const char* bytes, size_t count, size_t repetitions) noexcept;
};
[[nodiscard]] int Format(const FormatSink& sink, const char* format, va_list arguments) noexcept;
}

#endif
