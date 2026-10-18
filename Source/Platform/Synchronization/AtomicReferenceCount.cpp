// SPDX-License-Identifier: Apache-2.0

#include "AtomicReferenceCount.hpp"

namespace Asteria::Platform::Synchronization
{
static_assert(__atomic_always_lock_free(sizeof(Foundation::UInt32), nullptr));

bool AtomicReferenceCount::TryAcquire(const Foundation::UInt32 maximumValue) noexcept
{
    // Adds are serialized by ObjectRegistry's lock. Relaxed operations are sufficient here:
    // object publication is ordered by that lock, while this counter governs lifetime only.
    const Foundation::UInt32 current = __atomic_load_n(&m_value, __ATOMIC_RELAXED);
    if (current >= maximumValue)
    {
        return false;
    }

    static_cast<void>(__atomic_fetch_add(&m_value, Foundation::UInt32{1U}, __ATOMIC_RELAXED));
    return true;
}

void AtomicReferenceCount::Release() noexcept
{
    // The matching acquire Load makes all object accesses before the final release visible
    // to the collector before it returns the backing object to its owner.
    static_cast<void>(__atomic_fetch_sub(&m_value, Foundation::UInt32{1U}, __ATOMIC_RELEASE));
}

Foundation::UInt32 AtomicReferenceCount::Load() const noexcept
{
    return __atomic_load_n(&m_value, __ATOMIC_ACQUIRE);
}

void AtomicReferenceCount::Reset() noexcept
{
    // Reset has exclusive access before first publication or after acquire-ordered collection.
    __atomic_store_n(&m_value, Foundation::UInt32{0U}, __ATOMIC_RELAXED);
}
} // Namespace Asteria::Platform::Synchronization
