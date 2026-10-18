// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_DEVICE_VECTORS_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_DEVICE_VECTORS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Interrupt/IMessageVectors.hpp"
#include "Kernel/Interrupt/Interrupt.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
/// The vectors device interrupts arrive on, 0x40-0x4F, each bound to at most one kernel Interrupt.
/// Bind and Unbind run in normal context; Dispatch is the trap path's, ISR-safe and lock-free. A
/// vector bound to nothing is spurious: it is counted and acknowledged, never a fail-stop, since a
/// device can write any vector while interrupt remapping is off. Binding rotates through the
/// vectors, so a stale message rarely meets a new owner; the message that raises a vector is the
/// xAPIC's physical, fixed, edge message to the destination processor.
class Amd64DeviceVectors final : public Hal::Interrupt::IMessageVectors
{
public:
    static constexpr Foundation::UInt64 FirstVector{0x40U};
    static constexpr Foundation::Size VectorCount{16U};

    constexpr Amd64DeviceVectors() noexcept = default;

    Amd64DeviceVectors(const Amd64DeviceVectors&) = delete;
    Amd64DeviceVectors& operator=(const Amd64DeviceVectors&) = delete;

    [[nodiscard]] static constexpr bool IsDeviceVector(const Foundation::UInt64 vector) noexcept
    {
        return (vector >= FirstVector) && (vector < (FirstVector + VectorCount));
    }

    /// The xAPIC id of the processor the vectors arrive on, below 0xFF;
    /// before it is set every Bind refuses.
    [[nodiscard]] bool SetDestination(Foundation::UInt64 apicId) noexcept;

    [[nodiscard]] Foundation::UInt64 Reserve() noexcept;
    [[nodiscard]] bool Publish(Foundation::UInt64 vector, Kernel::Interrupt::Interrupt& interrupt) noexcept;
    void Release(Foundation::UInt64 vector) noexcept;
    [[nodiscard]] Foundation::UInt64 GetDestination() const noexcept
    {
        return m_destination.LoadAcquire() - 1U;
    }

    /// The next free vector after the last one bound, now bound to
    /// interrupt, and its message; a zero Vector when every vector is bound.
    [[nodiscard]] Hal::Interrupt::MessageTarget Bind(Kernel::Interrupt::Interrupt& interrupt) noexcept override;

    /// Frees vector, once its source can no longer raise it; false for a
    /// vector bound to something else.
    [[nodiscard]] bool Unbind(Foundation::UInt64 vector,
                              const Kernel::Interrupt::Interrupt& interrupt) noexcept override;

    /// Enters the Interrupt bound to vector, or counts a spurious vector.
    void Dispatch(Foundation::UInt64 vector) noexcept;

    [[nodiscard]] Foundation::UInt64 GetDelivered() const noexcept;
    [[nodiscard]] Foundation::UInt64 GetSpurious() const noexcept;

private:
    Platform::Synchronization::AtomicUInt64 m_bound[VectorCount]{};
    Platform::Synchronization::AtomicUInt64 m_next;
    Platform::Synchronization::AtomicUInt64 m_destination;
    Platform::Synchronization::AtomicUInt64 m_delivered;
    Platform::Synchronization::AtomicUInt64 m_spurious;
};
} // Namespace Asteria::Platform::Amd64::Interrupt

#endif // ASTERIA_PLATFORM_AMD64_INTERRUPT_AMD64_DEVICE_VECTORS_HPP_INCLUDED
