// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Time/Amd64LocalTimer.hpp"
#include "Platform/Amd64/Interrupt/Amd64LocalApic.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ReadTimeStampCounter() noexcept;

extern "C" Asteria::Foundation::UInt64
AsteriaAmd64InitializeLocalTimer(Asteria::Foundation::UInt64 maskedTimerVector,
                                 Asteria::Platform::Amd64::Interrupt::Amd64LocalApic* apicCache) noexcept;

extern "C" Asteria::Foundation::UInt64
AsteriaAmd64ProgramLocalTimer(Asteria::Foundation::UInt64 deadlineTicks, Asteria::Foundation::UInt64 timerVector,
                              Asteria::Foundation::UInt64 rollbackDeadlineTicks,
                              Asteria::Foundation::UInt64 rollbackTimerVector,
                              Asteria::Foundation::UInt64 maskedTimerVector,
                              Asteria::Platform::Amd64::Interrupt::Amd64LocalApic* apicCache) noexcept;

extern "C" Asteria::Foundation::UInt64
AsteriaAmd64CancelLocalTimer(Asteria::Foundation::UInt64 maskedTimerVector,
                             Asteria::Platform::Amd64::Interrupt::Amd64LocalApic* apicCache) noexcept;

#if defined(ASTERIA_TARGET_AMD64)
namespace Asteria::Platform::Amd64::Time
{
namespace
{
[[nodiscard]] Foundation::UInt64 ReadDeadline() noexcept
{
    Foundation::UInt32 low{0U};
    Foundation::UInt32 high{0U};
    __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(0x6E0U) : "memory");
    return (static_cast<Foundation::UInt64>(high) << 32U) | low;
}
void WriteDeadline(const Foundation::UInt64 ticks) noexcept
{
    __asm__ __volatile__("wrmsr"
                         :
                         : "c"(0x6E0U), "a"(static_cast<Foundation::UInt32>(ticks)),
                           "d"(static_cast<Foundation::UInt32>(ticks >> 32U))
                         : "memory");
}
[[nodiscard]] bool LvtMatches(const Interrupt::Amd64LocalApic& apic, const Foundation::UInt64 lvt) noexcept
{
    return (apic.Read(0x320U) & 0x700FFU) == (lvt & 0x700FFU);
}
} // Namespace
} // Namespace Asteria::Platform::Amd64::Time

extern "C" Asteria::Foundation::UInt64
AsteriaAmd64InitializeLocalTimer(const Asteria::Foundation::UInt64 maskedTimerVector,
                                 Asteria::Platform::Amd64::Interrupt::Amd64LocalApic* apicCache) noexcept
{
    using Asteria::Foundation::UInt32;
    UInt32 eax{0U};
    UInt32 ebx{0U};
    UInt32 ecx{0U};
    UInt32 edx{0U};
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1U), "c"(0U));
    if ((ecx & (1U << 24U)) == 0U || (edx & (1U << 5U)) == 0U)
    {
        return 1U;
    }
    // The common processor-online path gates invariant TSC before placement, including the linked
    // APIC timer path.
    auto& apic = *apicCache;
    if (!apic.InitializeIdentity())
    {
        return 2U;
    }
    apic.Write(0x320U, static_cast<UInt32>(maskedTimerVector));
    Asteria::Platform::Amd64::Time::WriteDeadline(0U);
    return Asteria::Platform::Amd64::Time::LvtMatches(apic, maskedTimerVector) &&
                   Asteria::Platform::Amd64::Time::ReadDeadline() == 0U
               ? 0U
               : 3U;
}
extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ProgramLocalTimer(
    const Asteria::Foundation::UInt64 deadlineTicks, const Asteria::Foundation::UInt64 timerVector,
    const Asteria::Foundation::UInt64 rollbackDeadlineTicks, const Asteria::Foundation::UInt64 rollbackTimerVector,
    const Asteria::Foundation::UInt64 maskedTimerVector,
    Asteria::Platform::Amd64::Interrupt::Amd64LocalApic* apicCache) noexcept
{
    const auto& apic = *apicCache;
    if (!apic.IsReady())
    {
        return 1U;
    }
    Asteria::Platform::Amd64::Time::WriteDeadline(deadlineTicks);
    apic.Write(0x320U, static_cast<Asteria::Foundation::UInt32>(timerVector));
    if (!Asteria::Platform::Amd64::Time::LvtMatches(apic, timerVector))
    {
        return 1U;
    }
    const Asteria::Foundation::UInt64 observed = Asteria::Platform::Amd64::Time::ReadDeadline();
    if (observed == deadlineTicks)
    {
        return 0U;
    }
    if (observed != 0U || AsteriaAmd64ReadTimeStampCounter() < deadlineTicks)
    {
        return 1U;
    }
    apic.Write(0x320U, static_cast<Asteria::Foundation::UInt32>(maskedTimerVector));
    Asteria::Platform::Amd64::Time::WriteDeadline(rollbackDeadlineTicks);
    apic.Write(0x320U, static_cast<Asteria::Foundation::UInt32>(rollbackTimerVector));
    if (!Asteria::Platform::Amd64::Time::LvtMatches(apic, rollbackTimerVector))
    {
        return 1U;
    }
    const Asteria::Foundation::UInt64 restored = Asteria::Platform::Amd64::Time::ReadDeadline();
    return restored == rollbackDeadlineTicks ||
                   (restored == 0U && AsteriaAmd64ReadTimeStampCounter() >= rollbackDeadlineTicks)
               ? 2U
               : 1U;
}
extern "C" Asteria::Foundation::UInt64
AsteriaAmd64CancelLocalTimer(const Asteria::Foundation::UInt64 maskedTimerVector,
                             Asteria::Platform::Amd64::Interrupt::Amd64LocalApic* apicCache) noexcept
{
    const auto& apic = *apicCache;
    if (!apic.IsReady())
    {
        return 1U;
    }
    apic.Write(0x320U, static_cast<Asteria::Foundation::UInt32>(maskedTimerVector));
    Asteria::Platform::Amd64::Time::WriteDeadline(0U);
    return Asteria::Platform::Amd64::Time::LvtMatches(apic, maskedTimerVector) &&
                   Asteria::Platform::Amd64::Time::ReadDeadline() == 0U
               ? 0U
               : 1U;
}
#endif

namespace Asteria::Platform::Amd64::Time
{
namespace
{
constexpr Foundation::UInt8 MinimumExternalInterruptVector{32U};
constexpr Foundation::UInt64 LocalVectorTableMaskBit{Foundation::UInt64{1U} << 16U};
constexpr Foundation::UInt64 TscDeadlineModeBit{Foundation::UInt64{1U} << 18U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 AdapterSuccess{0U};
constexpr Foundation::UInt64 AdapterDeadlineReached{2U};
} // Namespace

Amd64LocalTimerInitializationError Amd64LocalTimer::Initialize(const Foundation::UInt8 vector) noexcept
{
    if (m_state != Amd64LocalTimerState::Uninitialized)
    {
        return Amd64LocalTimerInitializationError::AlreadyInitialized;
    }
    if (vector < MinimumExternalInterruptVector)
    {
        return Amd64LocalTimerInitializationError::InvalidVector;
    }

    const Foundation::UInt64 maskedTimerVector =
        static_cast<Foundation::UInt64>(vector) | TscDeadlineModeBit | LocalVectorTableMaskBit;
    const Foundation::UInt64 adapterStatus = AsteriaAmd64InitializeLocalTimer(maskedTimerVector, &m_apic);
    if (adapterStatus == 1U)
    {
        m_state = Amd64LocalTimerState::Faulted;
        return Amd64LocalTimerInitializationError::RequiredFeatureUnavailable;
    }
    if (adapterStatus == 2U)
    {
        m_state = Amd64LocalTimerState::Faulted;
        return Amd64LocalTimerInitializationError::LocalApicUnavailable;
    }
    if (adapterStatus != AdapterSuccess)
    {
        m_state = Amd64LocalTimerState::Faulted;
        return Amd64LocalTimerInitializationError::HardwareFailure;
    }

    m_programmedDeadlineTicks = 0U;
    m_vector = vector;
    m_armed = false;
    m_state = Amd64LocalTimerState::Ready;
    return Amd64LocalTimerInitializationError::None;
}

Foundation::UInt64 Amd64LocalTimer::GetTicks() const noexcept
{
    return AsteriaAmd64ReadTimeStampCounter();
}

Hal::Time::LocalTimerError Amd64LocalTimer::ProgramDeadline(const Foundation::UInt64 deadlineTicks) noexcept
{
    if (m_state == Amd64LocalTimerState::Uninitialized)
    {
        return Hal::Time::LocalTimerError::NotReady;
    }
    if (m_state == Amd64LocalTimerState::Faulted)
    {
        return Hal::Time::LocalTimerError::HardwareFailure;
    }
    if ((deadlineTicks == 0U) || (deadlineTicks == MaximumUInt64))
    {
        return Hal::Time::LocalTimerError::InvalidDeadline;
    }

    const Foundation::UInt64 observedTicks = GetTicks();
    if (observedTicks == MaximumUInt64)
    {
        m_state = Amd64LocalTimerState::Faulted;
        return Hal::Time::LocalTimerError::HardwareFailure;
    }
    if (deadlineTicks <= observedTicks)
    {
        return Hal::Time::LocalTimerError::DeadlineReached;
    }

    const Foundation::UInt64 timerVector = static_cast<Foundation::UInt64>(m_vector) | TscDeadlineModeBit;
    const Foundation::UInt64 maskedTimerVector = timerVector | LocalVectorTableMaskBit;
    const Foundation::UInt64 rollbackTimerVector = m_armed ? timerVector : maskedTimerVector;
    const Foundation::UInt64 adapterStatus = AsteriaAmd64ProgramLocalTimer(
        deadlineTicks, timerVector, m_programmedDeadlineTicks, rollbackTimerVector, maskedTimerVector, &m_apic);
    if (adapterStatus == AdapterDeadlineReached)
    {
        return Hal::Time::LocalTimerError::DeadlineReached;
    }
    if (adapterStatus != AdapterSuccess)
    {
        m_state = Amd64LocalTimerState::Faulted;
        return Hal::Time::LocalTimerError::HardwareFailure;
    }

    m_programmedDeadlineTicks = deadlineTicks;
    m_armed = true;
    return Hal::Time::LocalTimerError::None;
}

Hal::Time::LocalTimerError Amd64LocalTimer::CancelDeadline() noexcept
{
    if (m_state == Amd64LocalTimerState::Uninitialized)
    {
        return Hal::Time::LocalTimerError::NotReady;
    }
    if (m_state == Amd64LocalTimerState::Faulted)
    {
        return Hal::Time::LocalTimerError::HardwareFailure;
    }
    if (!m_armed)
    {
        return Hal::Time::LocalTimerError::None;
    }

    const Foundation::UInt64 maskedTimerVector =
        static_cast<Foundation::UInt64>(m_vector) | TscDeadlineModeBit | LocalVectorTableMaskBit;
    if (AsteriaAmd64CancelLocalTimer(maskedTimerVector, &m_apic) != AdapterSuccess)
    {
        m_state = Amd64LocalTimerState::Faulted;
        return Hal::Time::LocalTimerError::HardwareFailure;
    }

    m_programmedDeadlineTicks = 0U;
    m_armed = false;
    return Hal::Time::LocalTimerError::None;
}

Hal::Time::LocalTimerError
Amd64LocalTimer::AcknowledgeExternallyMaskedDeadline(const Foundation::UInt64 deadlineTicks) noexcept
{
    if (m_state == Amd64LocalTimerState::Uninitialized)
    {
        return Hal::Time::LocalTimerError::NotReady;
    }
    if (m_state == Amd64LocalTimerState::Faulted)
    {
        return Hal::Time::LocalTimerError::HardwareFailure;
    }
    if ((deadlineTicks == 0U) || (deadlineTicks == MaximumUInt64) || !m_armed ||
        (deadlineTicks != m_programmedDeadlineTicks))
    {
        return Hal::Time::LocalTimerError::InvalidDeadline;
    }

    m_programmedDeadlineTicks = 0U;
    m_armed = false;
    return Hal::Time::LocalTimerError::None;
}
} // Namespace Asteria::Platform::Amd64::Time
