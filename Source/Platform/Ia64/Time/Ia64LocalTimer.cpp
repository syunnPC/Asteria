// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Time/Ia64LocalTimer.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadIntervalTimeCounter() noexcept;

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InitializeLocalTimer(Asteria::Foundation::UInt64 maskedInterruptVector) noexcept;

extern "C" Asteria::Foundation::UInt64
AsteriaIa64ProgramLocalTimer(Asteria::Foundation::UInt64 deadlineTicks, Asteria::Foundation::UInt64 interruptVector,
                             Asteria::Foundation::UInt64 rollbackDeadlineTicks,
                             Asteria::Foundation::UInt64 rollbackInterruptVector,
                             Asteria::Foundation::UInt64 maskedInterruptVector) noexcept;

extern "C" Asteria::Foundation::UInt64
AsteriaIa64CancelLocalTimer(Asteria::Foundation::UInt64 maskedInterruptVector) noexcept;

// The target entry points are in Ia64LocalTimer.S.

namespace Asteria::Platform::Ia64::Time
{
namespace
{
constexpr Foundation::UInt8 MinimumExternalInterruptVector{16U};
constexpr Foundation::UInt64 InterruptVectorMaskBit{Foundation::UInt64{1U} << 16U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 AdapterSuccess{0U};
constexpr Foundation::UInt64 AdapterDeadlineReached{2U};
}

Ia64LocalTimerInitializationError Ia64LocalTimer::Initialize(const Foundation::UInt8 vector) noexcept
{
    if (m_state != Ia64LocalTimerState::Uninitialized)
    {
        return Ia64LocalTimerInitializationError::AlreadyInitialized;
    }
    if (vector < MinimumExternalInterruptVector)
    {
        return Ia64LocalTimerInitializationError::InvalidVector;
    }

    const Foundation::UInt64 maskedInterruptVector = static_cast<Foundation::UInt64>(vector) | InterruptVectorMaskBit;
    if (AsteriaIa64InitializeLocalTimer(maskedInterruptVector) != 0U)
    {
        m_state = Ia64LocalTimerState::Faulted;
        return Ia64LocalTimerInitializationError::HardwareFailure;
    }

    m_programmedDeadlineTicks = 0U;
    m_vector = vector;
    m_armed = false;
    m_state = Ia64LocalTimerState::Ready;
    return Ia64LocalTimerInitializationError::None;
}

Foundation::UInt64 Ia64LocalTimer::GetTicks() const noexcept
{
    return AsteriaIa64ReadIntervalTimeCounter();
}

Hal::Time::LocalTimerError Ia64LocalTimer::ProgramDeadline(const Foundation::UInt64 deadlineTicks) noexcept
{
    if (m_state == Ia64LocalTimerState::Uninitialized)
    {
        return Hal::Time::LocalTimerError::NotReady;
    }
    if (m_state == Ia64LocalTimerState::Faulted)
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
        m_state = Ia64LocalTimerState::Faulted;
        return Hal::Time::LocalTimerError::HardwareFailure;
    }
    if (deadlineTicks <= observedTicks)
    {
        return Hal::Time::LocalTimerError::DeadlineReached;
    }

    const Foundation::UInt64 interruptVector = static_cast<Foundation::UInt64>(m_vector);
    const Foundation::UInt64 maskedInterruptVector = interruptVector | InterruptVectorMaskBit;
    const Foundation::UInt64 rollbackInterruptVector = m_armed ? interruptVector : maskedInterruptVector;
    const Foundation::UInt64 adapterStatus = AsteriaIa64ProgramLocalTimer(
        deadlineTicks, interruptVector, m_programmedDeadlineTicks, rollbackInterruptVector, maskedInterruptVector);
    if (adapterStatus == AdapterDeadlineReached)
    {
        return Hal::Time::LocalTimerError::DeadlineReached;
    }
    if (adapterStatus != AdapterSuccess)
    {
        m_state = Ia64LocalTimerState::Faulted;
        return Hal::Time::LocalTimerError::HardwareFailure;
    }

    m_programmedDeadlineTicks = deadlineTicks;
    m_armed = true;
    return Hal::Time::LocalTimerError::None;
}

Hal::Time::LocalTimerError Ia64LocalTimer::CancelDeadline() noexcept
{
    if (m_state == Ia64LocalTimerState::Uninitialized)
    {
        return Hal::Time::LocalTimerError::NotReady;
    }
    if (m_state == Ia64LocalTimerState::Faulted)
    {
        if (m_vector >= MinimumExternalInterruptVector)
        {
            const Foundation::UInt64 maskedInterruptVector =
                static_cast<Foundation::UInt64>(m_vector) | InterruptVectorMaskBit;
            // A prior native readback failure is terminal, but a later
            // serialized mask attempt can still reduce interrupt exposure.
            static_cast<void>(AsteriaIa64CancelLocalTimer(maskedInterruptVector));
        }
        return Hal::Time::LocalTimerError::HardwareFailure;
    }
    if (!m_armed)
    {
        return Hal::Time::LocalTimerError::None;
    }

    const Foundation::UInt64 maskedInterruptVector = static_cast<Foundation::UInt64>(m_vector) | InterruptVectorMaskBit;
    if (AsteriaIa64CancelLocalTimer(maskedInterruptVector) != 0U)
    {
        m_state = Ia64LocalTimerState::Faulted;
        return Hal::Time::LocalTimerError::HardwareFailure;
    }

    m_programmedDeadlineTicks = 0U;
    m_armed = false;
    return Hal::Time::LocalTimerError::None;
}

Hal::Time::LocalTimerError
Ia64LocalTimer::AcknowledgeExternallyMaskedDeadline(const Foundation::UInt64 deadlineTicks) noexcept
{
    if (m_state == Ia64LocalTimerState::Uninitialized)
    {
        return Hal::Time::LocalTimerError::NotReady;
    }
    if (m_state == Ia64LocalTimerState::Faulted)
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
} // namespace Asteria::Platform::Ia64::Time
