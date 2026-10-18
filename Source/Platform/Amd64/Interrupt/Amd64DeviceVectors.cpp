// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Interrupt/Amd64DeviceVectors.hpp"

namespace Asteria::Platform::Amd64::Interrupt
{
namespace
{
[[nodiscard]] Foundation::UInt64 AddressOf(const Kernel::Interrupt::Interrupt& interrupt) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&interrupt));
}

// A counter only the BSP's trap path advances.
void Advance(Platform::Synchronization::AtomicUInt64& counter) noexcept
{
    counter.StoreRelease(counter.LoadAcquire() + 1U);
}
} // namespace

// The xAPIC message: address 0xFEE00000 with the destination's id in bits
// 12-19 (physical, no redirection hint), data the vector (fixed, edge).
constexpr Foundation::UInt64 MessageAddress{0xFEE0'0000U};
// Stored plus one, so zero means unset.
constexpr Foundation::UInt64 BroadcastId{0xFFU};

bool Amd64DeviceVectors::SetDestination(const Foundation::UInt64 apicId) noexcept
{
    if (apicId >= BroadcastId)
    {
        return false;
    }
    m_destination.StoreRelease(apicId + 1U);
    return true;
}

Foundation::UInt64 Amd64DeviceVectors::Reserve() noexcept
{
    const Foundation::UInt64 first = m_next.LoadAcquire();
    for (Foundation::Size step{0U}; (m_destination.LoadAcquire() != 0U) && (step < VectorCount); ++step)
    {
        const Foundation::Size index = (first + step) % VectorCount;
        Foundation::UInt64 expected{0U};
        if (m_bound[index].CompareExchangeStrongAcquireRelease(expected, 1U))
        {
            m_next.StoreRelease(index + 1U);
            return FirstVector + index;
        }
    }
    return 0U;
}

bool Amd64DeviceVectors::Publish(const Foundation::UInt64 vector, Kernel::Interrupt::Interrupt& interrupt) noexcept
{
    if (!IsDeviceVector(vector))
    {
        return false;
    }
    Foundation::UInt64 expected{1U};
    return m_bound[vector - FirstVector].CompareExchangeStrongAcquireRelease(expected, AddressOf(interrupt));
}

void Amd64DeviceVectors::Release(const Foundation::UInt64 vector) noexcept
{
    if (IsDeviceVector(vector))
    {
        m_bound[vector - FirstVector].StoreRelease(0U);
    }
}

Hal::Interrupt::MessageTarget Amd64DeviceVectors::Bind(Kernel::Interrupt::Interrupt& interrupt) noexcept
{
    const Foundation::UInt64 destination = m_destination.LoadAcquire();
    const Foundation::UInt64 first = m_next.LoadAcquire();
    for (Foundation::Size step{0U}; (destination != 0U) && (step < VectorCount); ++step)
    {
        const Foundation::Size index = (first + step) % VectorCount;
        Foundation::UInt64 expected{0U};
        if (m_bound[index].CompareExchangeStrongAcquireRelease(expected, AddressOf(interrupt)))
        {
            m_next.StoreRelease(index + 1U);
            return Hal::Interrupt::MessageTarget{.Vector = FirstVector + index,
                                                 .Address = MessageAddress | ((destination - 1U) << 12U),
                                                 .Data = static_cast<Foundation::UInt16>(FirstVector + index),
                                                 .Reserved = {}};
        }
    }
    return Hal::Interrupt::MessageTarget{};
}

bool Amd64DeviceVectors::Unbind(const Foundation::UInt64 vector, const Kernel::Interrupt::Interrupt& interrupt) noexcept
{
    if (!IsDeviceVector(vector))
    {
        return false;
    }
    Foundation::UInt64 expected = AddressOf(interrupt);
    return m_bound[vector - FirstVector].CompareExchangeStrongAcquireRelease(expected, 0U);
}

void Amd64DeviceVectors::Dispatch(const Foundation::UInt64 vector) noexcept
{
    const Foundation::UInt64 bound = IsDeviceVector(vector) ? m_bound[vector - FirstVector].LoadAcquire() : 0U;
    if (bound <= 1U)
    {
        Advance(m_spurious);
        return;
    }
    // The Interrupt's own state machine refuses a stale or masked delivery.
    static_cast<void>(reinterpret_cast<Kernel::Interrupt::Interrupt*>(static_cast<Foundation::UIntPointer>(bound))
                          ->HandleInterrupt());
    Advance(m_delivered);
}

Foundation::UInt64 Amd64DeviceVectors::GetDelivered() const noexcept
{
    return m_delivered.LoadAcquire();
}

Foundation::UInt64 Amd64DeviceVectors::GetSpurious() const noexcept
{
    return m_spurious.LoadAcquire();
}
} // namespace Asteria::Platform::Amd64::Interrupt
