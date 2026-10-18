// SPDX-License-Identifier: Apache-2.0

#include "LibcHooks.hpp"

#include "Include/errno.h"
#include "Include/assert.h"

namespace Asteria::User::Libc
{
namespace
{
Hooks g_hooks{};
int g_errno{0};
bool g_installed{false};

[[nodiscard]] bool ValidLock(const LockHook& hook) noexcept
{
    return (hook.Lock == nullptr) == (hook.Unlock == nullptr);
}
}

bool InstallHooks(const Hooks& hooks) noexcept
{
    if (g_installed || !ValidLock(hooks.HeapLock) || !ValidLock(hooks.ConversionLock) ||
        ((hooks.Pages.Reserve == nullptr) != (hooks.Pages.Release == nullptr)))
    {
        return false;
    }
    g_hooks = hooks;
    g_installed = true;
    return true;
}

const Hooks& GetHooks() noexcept
{
    return g_hooks;
}

void SetError(const int error) noexcept
{
    *ASTERIA_LIBC_NAME(__asteria_errno_location)() = error;
}

void Lock(const LockHook& hook) noexcept
{
    if (hook.Lock != nullptr)
    {
        hook.Lock(hook.Context);
    }
}

void Unlock(const LockHook& hook) noexcept
{
    if (hook.Unlock != nullptr)
    {
        hook.Unlock(hook.Context);
    }
}
}

extern "C" int* ASTERIA_LIBC_NAME(__asteria_errno_location)() noexcept
{
    const auto callback = Asteria::User::Libc::GetHooks().ErrnoLocation;
    if (callback != nullptr)
    {
        return callback();
    }
    return &Asteria::User::Libc::g_errno;
}

extern "C" [[noreturn]] void ASTERIA_LIBC_NAME(__asteria_assert_fail)(const char* const expression,
                                                                      const char* const file, const unsigned int line,
                                                                      const char* const function) noexcept
{
    const auto callback = Asteria::User::Libc::GetHooks().AssertFail;
    if (callback != nullptr)
    {
        callback(expression, file, line, function);
    }
    // Compiler adapter: a returning failure hook cannot continue execution.
    __builtin_trap();
}
