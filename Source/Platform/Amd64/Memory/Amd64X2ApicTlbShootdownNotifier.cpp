// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/Amd64X2ApicTlbShootdownNotifier.hpp"
#include "Platform/Amd64/Interrupt/Amd64LocalApic.hpp"

extern "C" void AsteriaAmd64WriteX2ApicInterruptCommand(Asteria::Foundation::UInt64 command) noexcept;

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
constexpr Foundation::UInt8 MinimumExternalInterruptVector{32U};
constexpr Foundation::UInt8 MaximumUsableInterruptVector{254U};
constexpr Foundation::UInt32 BroadcastX2ApicId{0xFFFF'FFFFU};
constexpr Foundation::ProcessorMask ValidProcessorMask{static_cast<Foundation::ProcessorMask>(
    (Foundation::ProcessorMask{1U} << Kernel::Scheduler::MaximumProcessorCount) - 1U)};

[[nodiscard]] constexpr Foundation::ProcessorMask ProcessorBit(const Foundation::UInt8 processor) noexcept
{
    return static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processor);
}
} // namespace

Amd64X2ApicTlbShootdownNotifierInitializationError Amd64X2ApicTlbShootdownNotifier::Initialize(
    const Foundation::UInt8 vector, const Foundation::ProcessorMask onlineProcessorMask,
    const Foundation::UInt32 (&x2ApicIds)[Kernel::Scheduler::MaximumProcessorCount]) noexcept
{
    if (m_state != Amd64X2ApicTlbShootdownNotifierState::Uninitialized)
    {
        return Amd64X2ApicTlbShootdownNotifierInitializationError::AlreadyInitialized;
    }
    if ((vector < MinimumExternalInterruptVector) || (vector > MaximumUsableInterruptVector))
    {
        return Amd64X2ApicTlbShootdownNotifierInitializationError::InvalidVector;
    }
    if ((onlineProcessorMask == 0U) ||
        ((onlineProcessorMask & static_cast<Foundation::ProcessorMask>(~ValidProcessorMask)) != 0U))
    {
        return Amd64X2ApicTlbShootdownNotifierInitializationError::InvalidProcessorMask;
    }

    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        const Foundation::ProcessorMask processorBit = ProcessorBit(processor);
        if ((onlineProcessorMask & processorBit) == 0U)
        {
            continue;
        }
        if (x2ApicIds[processor] == BroadcastX2ApicId)
        {
            return Amd64X2ApicTlbShootdownNotifierInitializationError::InvalidX2ApicId;
        }
        for (Foundation::UInt8 previous{0U}; previous < processor; ++previous)
        {
            if (((onlineProcessorMask & ProcessorBit(previous)) != 0U) && (x2ApicIds[previous] == x2ApicIds[processor]))
            {
                return Amd64X2ApicTlbShootdownNotifierInitializationError::InvalidX2ApicId;
            }
        }
    }

#if defined(ASTERIA_TARGET_AMD64)
    if (!m_apic.InitializeIdentity() || !m_apic.IsX2Apic())
    {
        return Amd64X2ApicTlbShootdownNotifierInitializationError::LocalApicUnavailable;
    }
#endif
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        m_x2ApicIds[processor] = ((onlineProcessorMask & ProcessorBit(processor)) != 0U) ? x2ApicIds[processor] : 0U;
    }
    m_onlineProcessorMask = onlineProcessorMask;
    m_vector = vector;
    m_state = Amd64X2ApicTlbShootdownNotifierState::Ready;
    return Amd64X2ApicTlbShootdownNotifierInitializationError::None;
}

void Amd64X2ApicTlbShootdownNotifier::Notify(const Foundation::ProcessorMask targetProcessorMask) noexcept
{
    if ((m_state != Amd64X2ApicTlbShootdownNotifierState::Ready) || (targetProcessorMask == 0U) ||
        ((targetProcessorMask & static_cast<Foundation::ProcessorMask>(~m_onlineProcessorMask)) != 0U))
    {
        return;
    }

    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        if ((targetProcessorMask & ProcessorBit(processor)) == 0U)
        {
            continue;
        }
#if defined(ASTERIA_TARGET_AMD64)
        static_cast<void>(m_apic.Send(m_x2ApicIds[processor], m_vector));
#else
        const Foundation::UInt64 command = (static_cast<Foundation::UInt64>(m_x2ApicIds[processor]) << 32U) |
                                           static_cast<Foundation::UInt64>(m_vector);
        AsteriaAmd64WriteX2ApicInterruptCommand(command);
#endif
    }
}
} // namespace Asteria::Platform::Amd64::Memory
