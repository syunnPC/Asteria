// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_SYNCHRONIZATION_SPIN_LOCK_HPP_INCLUDED
#define ASTERIA_PLATFORM_SYNCHRONIZATION_SPIN_LOCK_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Synchronization
{
class SpinLock final
{
public:
    constexpr SpinLock() noexcept = default;

    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;
    SpinLock(SpinLock&&) = delete;
    SpinLock& operator=(SpinLock&&) = delete;

    /// Acquires the lock with acquire ordering.
    /// Interrupt safety:
    /// - Must not be called from an ISR.
    /// - Callers must prevent same-CPU preemption by code that can acquire this lock.
    /// Blocking:
    /// - Spins on a read until the lock looks free, then retries the exchange.
    /// - No fairness guarantee is provided.
    void Acquire() noexcept;

    /// Releases the lock with release ordering.
    /// Preconditions:
    /// - The calling CPU owns the lock.
    void Release() noexcept;

    [[nodiscard]] Foundation::UInt64 GetOwner() const noexcept
    {
        return __atomic_load_n(&m_owner, __ATOMIC_ACQUIRE);
    }

private:
    // Zero is unlocked; all ones is unknown, otherwise processor index + 1.
    Foundation::UInt64 m_owner{0U};
};

class SpinLockGuard final
{
public:
    /// Acquires lock and retains it until this guard is destroyed.
    /// Preconditions:
    /// - lock outlives this guard.
    explicit SpinLockGuard(SpinLock& lock) noexcept;
    ~SpinLockGuard() noexcept;

    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;
    SpinLockGuard(SpinLockGuard&&) = delete;
    SpinLockGuard& operator=(SpinLockGuard&&) = delete;

private:
    SpinLock& m_lock;
};
} // namespace Asteria::Platform::Synchronization

#endif // ASTERIA_PLATFORM_SYNCHRONIZATION_SPIN_LOCK_HPP_INCLUDED
