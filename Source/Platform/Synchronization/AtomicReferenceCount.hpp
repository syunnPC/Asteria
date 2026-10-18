// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_SYNCHRONIZATION_ATOMIC_REFERENCE_COUNT_HPP_INCLUDED
#define ASTERIA_PLATFORM_SYNCHRONIZATION_ATOMIC_REFERENCE_COUNT_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Synchronization
{
class AtomicReferenceCount final
{
public:
    static constexpr Foundation::UInt32 MaximumValue{~Foundation::UInt32{0U}};

    constexpr AtomicReferenceCount() noexcept = default;

    AtomicReferenceCount(const AtomicReferenceCount&) = delete;
    AtomicReferenceCount& operator=(const AtomicReferenceCount&) = delete;
    AtomicReferenceCount(AtomicReferenceCount&&) = delete;
    AtomicReferenceCount& operator=(AtomicReferenceCount&&) = delete;

    /// Adds one reference unless the counter reached maximumValue. The default
    /// preserves full UInt32 saturation behavior.
    /// Preconditions:
    /// - Calls that add or reset references are externally serialized.
    /// - Concurrent Release calls are permitted.
    /// Blocking:
    /// - Wait-free and implemented by one lock-free atomic read and, on success, one add.
    [[nodiscard]] bool TryAcquire(Foundation::UInt32 maximumValue = MaximumValue) noexcept;

    /// Drops one reference with release ordering.
    /// Preconditions:
    /// - The counter is nonzero and the caller owns exactly one reference.
    /// Blocking:
    /// - Wait-free and implemented by one lock-free atomic operation.
    void Release() noexcept;

    /// Reads the count with acquire ordering.
    [[nodiscard]] Foundation::UInt32 Load() const noexcept;

    /// Sets the count to zero before first use or after exclusive reclamation.
    /// Preconditions:
    /// - No concurrent operation accesses this counter.
    void Reset() noexcept;

private:
    Foundation::UInt32 m_value{0U};
};
} // Namespace Asteria::Platform::Synchronization

#endif // ASTERIA_PLATFORM_SYNCHRONIZATION_ATOMIC_REFERENCE_COUNT_HPP_INCLUDED
