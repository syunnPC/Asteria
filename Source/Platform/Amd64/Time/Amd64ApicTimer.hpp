// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_TIME_AMD64_APIC_TIMER_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_TIME_AMD64_APIC_TIMER_HPP_INCLUDED

#include "Hal/Time/ILocalTimer.hpp"
#include "Platform/Amd64/Interrupt/Amd64LocalApic.hpp"

namespace Asteria::Platform::Amd64::Time
{
/// The IDT vector of the local-timer interrupt.
inline constexpr Foundation::UInt32 ApicTimerVector{0xE8U};
inline constexpr Foundation::UInt32 ApicSpuriousVector{0xFFU};
inline constexpr Foundation::UInt32 ApicErrorVector{0xFEU};
/// XAPIC register offsets (Intel SDM vol. 3, 11.4.1): the LVT timer entry, the timer's initial
/// count, and the in-service word holding the timer vector's bit.
inline constexpr Foundation::UInt32 ApicLvtTimerRegister{0x320U};
inline constexpr Foundation::UInt32 ApicInitialCountRegister{0x380U};
inline constexpr Foundation::UInt32 ApicTimerInServiceRegister{0x100U + ((ApicTimerVector / 32U) * 0x10U)};
inline constexpr Foundation::UInt32 ApicTimerInServiceBit{1U << (ApicTimerVector % 32U)};
/// The LVT timer entry while no deadline is programmed: masked (bit 16),
/// one-shot, at the timer vector. Initialize, CancelDeadline and
/// AcknowledgeInterrupt leave it with a zero initial count.
inline constexpr Foundation::UInt32 ApicLvtMaskBit{0x1'0000U};
inline constexpr Foundation::UInt32 ApicTimerMaskedLvt{ApicLvtMaskBit | ApicTimerVector};

static_assert(ApicTimerInServiceRegister == 0x170U);
static_assert(ApicTimerMaskedLvt == 0x1'00E8U);

/// Local APIC countdown backend for machines without a TSC-deadline timer. The
/// owning CPU calibrates its local counter against TSC with interrupts off.
/// Tick values are CPU-local; no thread migration or cross-CPU deadline is
/// admitted by this bootstrap profile.
class Amd64ApicTimer final : public Hal::Time::ILocalTimer
{
public:
    constexpr Amd64ApicTimer() noexcept = default;
    /// Owning CPU, IF clear, an enabled local APIC. Masks unused implemented
    /// LVTs with a legal ignored vector; LINT1 remains masked until stop online.
    static void MaskUnusedInterrupts(const Interrupt::Amd64LocalApic& apic) noexcept
    {
        const Foundation::UInt32 maximumLvt = (apic.Read(0x30U) >> 16U) & 0xFFU;
        constexpr Foundation::UInt32 masked{ApicLvtMaskBit | ApicErrorVector};
        if (maximumLvt >= 5U)
        {
            apic.Write(0x330U, masked);
        }
        if (maximumLvt >= 4U)
        {
            apic.Write(0x340U, masked);
        }
        if (maximumLvt >= 6U)
        {
            apic.Write(0x2F0U, masked);
        }
        apic.Write(0x350U, masked);
        apic.Write(0x360U, masked);
        apic.Write(0x370U, masked);
    }
    [[nodiscard]] const Interrupt::Amd64LocalApic& GetApic() const noexcept
    {
        return m_apic;
    }
    [[nodiscard]] bool Initialize(Foundation::UInt64 mappedApicAddress) noexcept;
    [[nodiscard]] Foundation::UInt64 GetTicks() const noexcept override;
    [[nodiscard]] Hal::Time::LocalTimerError ProgramDeadline(Foundation::UInt64 deadlineTicks) noexcept override;
    [[nodiscard]] Hal::Time::LocalTimerError CancelDeadline() noexcept override;
    [[nodiscard]] Hal::Time::LocalTimerError
    AcknowledgeExternallyMaskedDeadline(Foundation::UInt64 deadlineTicks) noexcept override;
    /// Masks and acknowledges the vector. Early/chunked countdowns are rearmed;
    /// true publishes one due deadline for normal-context queue processing.
    [[nodiscard]] bool AcknowledgeInterrupt() noexcept;
    /// Signals the end of a device interrupt this processor took.
    void EndOfInterrupt() noexcept;
    void EnableWatchdog(Foundation::UInt64 interval) noexcept;
    [[nodiscard]] static constexpr Foundation::UInt32 CountdownFor(const Foundation::UInt64 now,
                                                                   const Foundation::UInt64 deadline,
                                                                   const Foundation::UInt64 ticksPerCount) noexcept
    {
        if (ticksPerCount == 0U || deadline <= now)
        {
            return 0U;
        }
        const Foundation::UInt64 delta = deadline - now;
        const Foundation::UInt64 count = delta / ticksPerCount + (delta % ticksPerCount != 0U ? 1U : 0U);
        return count > 0xFFFF'FFFFU ? 0xFFFF'FFFFU : static_cast<Foundation::UInt32>(count);
    }
    [[nodiscard]] Foundation::UInt32 Read(Foundation::UInt32 offset) const noexcept;
    void Write(Foundation::UInt32 offset, Foundation::UInt32 value) noexcept;
    [[nodiscard]] constexpr Foundation::UInt64 GetDeadline() const noexcept
    {
        return m_deadline;
    }

private:
    Interrupt::Amd64LocalApic m_apic;
    Foundation::UInt64 m_ticksPerCount{0U};
    void Rearm(Foundation::UInt64 now) noexcept;
    Foundation::UInt64 m_deadline{0U};
    Foundation::UInt64 m_watchdogInterval{0U};
    Foundation::UInt64 m_watchdogWake{0U};
};
} // Namespace Asteria::Platform::Amd64::Time

#endif // ASTERIA_PLATFORM_AMD64_TIME_AMD64_APIC_TIMER_HPP_INCLUDED
