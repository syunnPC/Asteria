// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_SYNCHRONIZATION_ATOMIC_UINT64_HPP_INCLUDED
#define ASTERIA_PLATFORM_SYNCHRONIZATION_ATOMIC_UINT64_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Synchronization
{
static_assert(__atomic_always_lock_free(sizeof(Foundation::UInt64), nullptr));

/// One always-lock-free publication word.
class AtomicUInt64 final
{
public:
    constexpr AtomicUInt64() noexcept = default;

    AtomicUInt64(const AtomicUInt64&) = delete;
    AtomicUInt64& operator=(const AtomicUInt64&) = delete;
    AtomicUInt64(AtomicUInt64&&) = delete;
    AtomicUInt64& operator=(AtomicUInt64&&) = delete;

    /// Publishes all preceding writes before value becomes observable.
    /// Blocking: wait-free.
    void StoreRelease(const Foundation::UInt64 value) noexcept
    {
        __atomic_store_n(&m_value, value, __ATOMIC_RELEASE);
    }

    /// Observes writes published by the matching release store.
    /// Blocking: wait-free.
    [[nodiscard]] Foundation::UInt64 LoadAcquire() const noexcept
    {
        return __atomic_load_n(&m_value, __ATOMIC_ACQUIRE);
    }

    /// Diagnostic counter; does not publish transaction fields. Wait-free.
    void IncrementRelaxed() noexcept
    {
        static_cast<void>(__atomic_fetch_add(&m_value, 1U, __ATOMIC_RELAXED));
    }

    /// Replaces the value and acquires writes published by the replaced value.
    /// Blocking: wait-free.
    [[nodiscard]] Foundation::UInt64 ExchangeAcquireRelease(const Foundation::UInt64 value) noexcept
    {
        return __atomic_exchange_n(&m_value, value, __ATOMIC_ACQ_REL);
    }

    [[nodiscard]] bool CompareExchangeStrongAcquireRelease(Foundation::UInt64& expected,
                                                           const Foundation::UInt64 desired) noexcept
    {
        return __atomic_compare_exchange_n(&m_value, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

private:
    Foundation::UInt64 m_value{0U};
};

static_assert(sizeof(AtomicUInt64) == sizeof(Foundation::UInt64));
static_assert(alignof(AtomicUInt64) == alignof(Foundation::UInt64));

/// Applies always-lock-free atomic operations to one externally owned,
/// naturally aligned UInt64 word without changing that word's storage layout.
class AtomicUInt64Reference final
{
public:
    explicit constexpr AtomicUInt64Reference(volatile Foundation::UInt64& value) noexcept : m_value{&value}
    {
    }

    AtomicUInt64Reference(const AtomicUInt64Reference&) = delete;
    AtomicUInt64Reference& operator=(const AtomicUInt64Reference&) = delete;
    AtomicUInt64Reference(AtomicUInt64Reference&&) = delete;
    AtomicUInt64Reference& operator=(AtomicUInt64Reference&&) = delete;

    /// Acquires an externally owned publication word through a read-only borrow.
    /// The word must have the same alignment and atomic-access contract as this reference.
    [[nodiscard]] static Foundation::UInt64 LoadAcquire(const volatile Foundation::UInt64& value) noexcept
    {
        return __atomic_load_n(&value, __ATOMIC_ACQUIRE);
    }

    [[nodiscard]] Foundation::UInt64 LoadAcquire() const noexcept
    {
        return LoadAcquire(*m_value);
    }

    void StoreRelaxed(const Foundation::UInt64 value) noexcept
    {
        __atomic_store_n(m_value, value, __ATOMIC_RELAXED);
    }

    void StoreRelease(const Foundation::UInt64 value) noexcept
    {
        __atomic_store_n(m_value, value, __ATOMIC_RELEASE);
    }

    [[nodiscard]] bool CompareExchangeStrongAcquireRelease(Foundation::UInt64& expected,
                                                           const Foundation::UInt64 desired) noexcept
    {
        return __atomic_compare_exchange_n(m_value, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    void FetchOrRelease(const Foundation::UInt64 value) noexcept
    {
        static_cast<void>(__atomic_fetch_or(m_value, value, __ATOMIC_RELEASE));
    }

    void IncrementRelease() noexcept
    {
        static_cast<void>(__atomic_fetch_add(m_value, Foundation::UInt64{1U}, __ATOMIC_RELEASE));
    }

private:
    volatile Foundation::UInt64* m_value;
};
} // namespace Asteria::Platform::Synchronization

#endif // ASTERIA_PLATFORM_SYNCHRONIZATION_ATOMIC_UINT64_HPP_INCLUDED
