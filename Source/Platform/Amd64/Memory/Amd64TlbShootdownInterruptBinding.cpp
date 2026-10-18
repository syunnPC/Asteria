// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/Amd64TlbShootdownInterruptBinding.hpp"

#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Amd64/Interrupt/Amd64LocalApic.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ReadX2ApicIdentifier() noexcept;

extern "C" void AsteriaAmd64WriteX2ApicEndOfInterrupt() noexcept;

extern "C" void AsteriaAmd64HandleTlbShootdownInterrupt() noexcept;

#if defined(ASTERIA_TARGET_AMD64)
namespace
{
// The binding admits only x2APIC CPUs. This immutable backend addresses the
// calling CPU's MSRs, so no CPU lookup or discovery is needed in the ISR.
constinit Asteria::Platform::Amd64::Interrupt::Amd64LocalApic g_interruptApic;
}
extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ReadX2ApicIdentifier() noexcept
{
    return g_interruptApic.IsReady() ? g_interruptApic.GetId() : 0xFFFF'FFFF'FFFF'FFFFULL;
}
extern "C" void AsteriaAmd64WriteX2ApicEndOfInterrupt() noexcept
{
    if (g_interruptApic.IsReady())
    {
        g_interruptApic.Write(0xB0U, 0U);
    }
}
#endif
// The interrupt entry is in Amd64TlbShootdownInterruptBinding.S.
#if !defined(ASTERIA_TARGET_AMD64)
extern "C" void AsteriaAmd64TlbShootdownInterruptEntry() noexcept
{
    AsteriaAmd64HandleTlbShootdownInterrupt();
}
#endif

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt32{0xFFFF'FFFFULL};
constexpr Foundation::ProcessorMask ValidProcessorMask{static_cast<Foundation::ProcessorMask>(
    (Foundation::ProcessorMask{1U} << Kernel::Scheduler::MaximumProcessorCount) - 1U)};

constinit Platform::Synchronization::AtomicUInt64 g_bindingAddress;

[[nodiscard]] constexpr Foundation::ProcessorMask ProcessorBit(const Foundation::UInt8 processor) noexcept
{
    return static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processor);
}

[[nodiscard]] constexpr Kernel::Memory::TlbShootdownDrainSummary EmptyDrainSummary() noexcept
{
    return Kernel::Memory::TlbShootdownDrainSummary{
        .ReceivedCount = 0U,
        .CompletedCount = 0U,
        .RejectedCount = 0U,
        .ReceiveError = Kernel::Memory::TlbShootdownReceiveError::None,
        .FirstRemoteError = Kernel::Memory::TlbShootdownError::None,
        .FirstLocalError = Hal::Memory::LocalTlbInvalidationError::None,
        .FirstAddressSpaceError = Kernel::Memory::AddressSpaceError::None,
        .Processor = Kernel::Scheduler::CpuId{},
        .Reserved = {},
    };
}
} // namespace

Amd64TlbShootdownInterruptBindingInitializationError Amd64TlbShootdownInterruptBinding::Initialize(
    const Amd64X2ApicTlbShootdownNotifier& notifier,
    Kernel::Memory::ITlbShootdownDrain* const (&drains)[Kernel::Scheduler::MaximumProcessorCount]) noexcept
{
    if (m_state != Amd64TlbShootdownInterruptBindingState::Uninitialized)
    {
        return Amd64TlbShootdownInterruptBindingInitializationError::AlreadyInitialized;
    }
    if (g_bindingAddress.LoadAcquire() != 0U)
    {
        return Amd64TlbShootdownInterruptBindingInitializationError::BindingAlreadyInstalled;
    }

    const Amd64X2ApicTlbShootdownNotifierSnapshot notifierSnapshot = notifier.GetSnapshot();
    if (notifierSnapshot.State != Amd64X2ApicTlbShootdownNotifierState::Ready)
    {
        return Amd64TlbShootdownInterruptBindingInitializationError::NotifierNotReady;
    }
    const Foundation::ProcessorMask onlineProcessorMask = notifierSnapshot.OnlineProcessorMask;
    if ((onlineProcessorMask == 0U) ||
        ((onlineProcessorMask & static_cast<Foundation::ProcessorMask>(~ValidProcessorMask)) != 0U))
    {
        return Amd64TlbShootdownInterruptBindingInitializationError::NotifierNotReady;
    }

    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        const bool online = (onlineProcessorMask & ProcessorBit(processor)) != 0U;
        if (online ? ((drains[processor] == nullptr) || (drains[processor]->GetProcessor().GetValue() != processor))
                   : (drains[processor] != nullptr))
        {
            return Amd64TlbShootdownInterruptBindingInitializationError::InvalidDrain;
        }
    }

    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        m_drains[processor] = drains[processor];
        m_x2ApicIds[processor] = notifierSnapshot.X2ApicIds[processor];
    }
#if defined(ASTERIA_TARGET_AMD64)
    g_interruptApic = notifier.GetApic();
#endif
    m_onlineProcessorMask = onlineProcessorMask;
    m_vector = notifierSnapshot.Vector;
    m_state = Amd64TlbShootdownInterruptBindingState::Ready;
    g_bindingAddress.StoreRelease(static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(this)));
    return Amd64TlbShootdownInterruptBindingInitializationError::None;
}

Amd64TlbShootdownInterruptResult Amd64TlbShootdownInterruptBinding::HandleInterrupt() noexcept
{
    Amd64TlbShootdownInterruptResult result{
        .Drain = EmptyDrainSummary(),
        .ObservedX2ApicId = AsteriaAmd64ReadX2ApicIdentifier(),
        .Error = Amd64TlbShootdownInterruptError::NotReady,
        .Reserved = {},
    };

    if (m_state == Amd64TlbShootdownInterruptBindingState::Ready)
    {
        if (result.ObservedX2ApicId <= MaximumUInt32)
        {
            for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
            {
                if (((m_onlineProcessorMask & ProcessorBit(processor)) != 0U) &&
                    (m_x2ApicIds[processor] == static_cast<Foundation::UInt32>(result.ObservedX2ApicId)))
                {
                    result.Drain = m_drains[processor]->Drain();
                    result.Error = Amd64TlbShootdownInterruptError::None;
                    break;
                }
            }
        }
        if (result.Error != Amd64TlbShootdownInterruptError::None)
        {
            result.Error = Amd64TlbShootdownInterruptError::InvalidProcessor;
        }
    }

    AsteriaAmd64WriteX2ApicEndOfInterrupt();
    return result;
}
} // namespace Asteria::Platform::Amd64::Memory

extern "C" void AsteriaAmd64HandleTlbShootdownInterrupt() noexcept
{
    const Asteria::Foundation::UInt64 bindingAddress = Asteria::Platform::Amd64::Memory::g_bindingAddress.LoadAcquire();
    if (bindingAddress == 0U)
    {
        AsteriaAmd64WriteX2ApicEndOfInterrupt();
        return;
    }
    auto* const binding = reinterpret_cast<Asteria::Platform::Amd64::Memory::Amd64TlbShootdownInterruptBinding*>(
        static_cast<Asteria::Foundation::UIntPointer>(bindingAddress));
    static_cast<void>(binding->HandleInterrupt());
}
