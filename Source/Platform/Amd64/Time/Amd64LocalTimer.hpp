// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_TIME_AMD64_LOCAL_TIMER_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_TIME_AMD64_LOCAL_TIMER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Time/ILocalTimer.hpp"
#include "Platform/Amd64/Interrupt/Amd64LocalApic.hpp"

namespace Asteria::Platform::Amd64::Time
{
enum class Amd64LocalTimerState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    Faulted,
};

enum class Amd64LocalTimerInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidVector,
    RequiredFeatureUnavailable,
    LocalApicUnavailable,
    HardwareFailure,
};

struct Amd64LocalTimerSnapshot
{
    Foundation::UInt64 ProgrammedDeadlineTicks;
    Amd64LocalTimerState State;
    Foundation::UInt8 Vector;
    Foundation::UInt8 Armed;
    Foundation::UInt8 Reserved[5];
};

/// Processor-local AMD64 one-shot timer backed by invariant TSC and the local APIC TSC-deadline
/// mode. One owning processor serializes every operation with local interrupts disabled. Initialize
/// verifies CPUID support, preserves the current APIC mode, and leaves the timer LVT masked.
/// ProgramDeadline is forbidden until the selected vector has a complete interrupt-entry,
/// acknowledgement, and deferred-dispatch path. Cross-CPU TSC synchronization is a separate SMP
/// admission requirement; this primitive exposes only its owning CPU domain.
class Amd64LocalTimer final : public Hal::Time::ILocalTimer
{
public:
    constexpr Amd64LocalTimer() noexcept = default;

    /// Preconditions:
    /// - The xAPIC base, when selected, is identity mapped UC.
    /// - State is Uninitialized and the caller executes at CPL0 on the owning
    /// processor with local interrupts disabled.
    [[nodiscard]] Amd64LocalTimerInitializationError Initialize(Foundation::UInt8 vector) noexcept;

    [[nodiscard]] Foundation::UInt64 GetTicks() const noexcept override;

    [[nodiscard]] Hal::Time::LocalTimerError ProgramDeadline(Foundation::UInt64 deadlineTicks) noexcept override;

    [[nodiscard]] Hal::Time::LocalTimerError CancelDeadline() noexcept override;

    [[nodiscard]] Hal::Time::LocalTimerError
    AcknowledgeExternallyMaskedDeadline(Foundation::UInt64 deadlineTicks) noexcept override;

    /// Preconditions:
    /// - The caller holds the owning-processor serialization domain.
    [[nodiscard]] constexpr Amd64LocalTimerSnapshot GetSnapshot() const noexcept
    {
        return Amd64LocalTimerSnapshot{
            .ProgrammedDeadlineTicks = m_programmedDeadlineTicks,
            .State = m_state,
            .Vector = m_vector,
            .Armed = static_cast<Foundation::UInt8>(m_armed ? 1U : 0U),
            .Reserved = {0U, 0U, 0U, 0U, 0U},
        };
    }

    /// Preconditions:
    /// - State is Uninitialized or Ready and the timer is disarmed, with no
    /// concurrent operation; alternatively, the owning processor is already
    /// in a non-returning fail-stop path.
    ~Amd64LocalTimer() noexcept = default;

    Amd64LocalTimer(const Amd64LocalTimer&) = delete;
    Amd64LocalTimer& operator=(const Amd64LocalTimer&) = delete;
    Amd64LocalTimer(Amd64LocalTimer&&) = delete;
    Amd64LocalTimer& operator=(Amd64LocalTimer&&) = delete;

private:
    Interrupt::Amd64LocalApic m_apic;
    Foundation::UInt64 m_programmedDeadlineTicks{0U};
    Foundation::UInt8 m_vector{0U};
    Amd64LocalTimerState m_state{Amd64LocalTimerState::Uninitialized};
    bool m_armed{false};
};

static_assert(sizeof(Amd64LocalTimerState) == 1U);
static_assert(sizeof(Amd64LocalTimerInitializationError) == 1U);
static_assert(sizeof(Amd64LocalTimerSnapshot) == 16U);
static_assert(alignof(Amd64LocalTimerSnapshot) == 8U);
} // Namespace Asteria::Platform::Amd64::Time

#endif // ASTERIA_PLATFORM_AMD64_TIME_AMD64_LOCAL_TIMER_HPP_INCLUDED
