// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_TIME_IA64_LOCAL_TIMER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_TIME_IA64_LOCAL_TIMER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Time/ILocalTimer.hpp"

namespace Asteria::Platform::Ia64::Time
{
enum class Ia64LocalTimerState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    Faulted,
};

enum class Ia64LocalTimerInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidVector,
    HardwareFailure,
};

struct Ia64LocalTimerSnapshot
{
    Foundation::UInt64 ProgrammedDeadlineTicks;
    Ia64LocalTimerState State;
    Foundation::UInt8 Vector;
    Foundation::UInt8 Armed;
    Foundation::UInt8 Reserved[5];
};

/// Processor-local IA-64 interval timer backed by ar.itc, cr.itm, and cr.itv. One owning processor
/// serializes every operation while external interruption delivery is disabled in PSR.i and no
/// local interruption entry can run. PSR.ic may remain set for translated normal-context operation.
/// Initialize leaves cr.itv masked. ProgramDeadline is the only operation that unmasks the timer
/// vector, and no caller may invoke it until the vector has a complete entry, stack,
/// acknowledgement path, and deferred policy handler. This backend does not configure
/// processor-wide cr.tpr policy; the caller must establish a priority profile that admits the
/// selected vector before it enables PSR.i. The raw ITC tick domain is used without scaling. The
/// platform uptime policy must stop this time domain before the 64-bit ITC wraps; this bootstrap
/// backend does not extend the architectural counter.
class Ia64LocalTimer final : public Hal::Time::ILocalTimer
{
public:
    constexpr Ia64LocalTimer() noexcept = default;

    /// Preconditions:
    /// - State is Uninitialized and the caller executes at CPL0 with PSR.vm
    /// clear on the owning processor, with PSR.i clear and no local
    /// interruption entry able to run.
    [[nodiscard]] Ia64LocalTimerInitializationError Initialize(Foundation::UInt8 vector) noexcept;

    /// Reads the processor-local interval time counter.
    [[nodiscard]] Foundation::UInt64 GetTicks() const noexcept override;

    [[nodiscard]] Hal::Time::LocalTimerError ProgramDeadline(Foundation::UInt64 deadlineTicks) noexcept override;

    [[nodiscard]] Hal::Time::LocalTimerError CancelDeadline() noexcept override;

    [[nodiscard]] Hal::Time::LocalTimerError
    AcknowledgeExternallyMaskedDeadline(Foundation::UInt64 deadlineTicks) noexcept override;

    /// Preconditions:
    /// - The caller holds the same owning-processor serialization domain used
    /// by Initialize, ProgramDeadline, and CancelDeadline.
    [[nodiscard]] constexpr Ia64LocalTimerSnapshot GetSnapshot() const noexcept
    {
        return Ia64LocalTimerSnapshot{
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
    ~Ia64LocalTimer() noexcept = default;

    Ia64LocalTimer(const Ia64LocalTimer&) = delete;
    Ia64LocalTimer& operator=(const Ia64LocalTimer&) = delete;
    Ia64LocalTimer(Ia64LocalTimer&&) = delete;
    Ia64LocalTimer& operator=(Ia64LocalTimer&&) = delete;

private:
    Foundation::UInt64 m_programmedDeadlineTicks{0U};
    Foundation::UInt8 m_vector{0U};
    Ia64LocalTimerState m_state{Ia64LocalTimerState::Uninitialized};
    bool m_armed{false};
};

static_assert(sizeof(Ia64LocalTimerState) == 1U);
static_assert(sizeof(Ia64LocalTimerInitializationError) == 1U);
static_assert(sizeof(Ia64LocalTimerSnapshot) == 16U);
static_assert(alignof(Ia64LocalTimerSnapshot) == 8U);
} // Namespace Asteria::Platform::Ia64::Time

#endif // ASTERIA_PLATFORM_IA64_TIME_IA64_LOCAL_TIMER_HPP_INCLUDED
