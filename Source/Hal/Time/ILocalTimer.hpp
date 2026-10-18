// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_TIME_I_LOCAL_TIMER_HPP_INCLUDED
#define ASTERIA_HAL_TIME_I_LOCAL_TIMER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Time/IMonotonicClock.hpp"

namespace Asteria::Hal::Time
{
enum class LocalTimerError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidDeadline,
    DeadlineReached,
    HardwareFailure,
};

/// One processor-local, one-shot deadline source in the same raw tick domain
/// exposed by GetTicks.
///
/// Programming operations are serialized by the owning processor policy and
/// run with local interrupts disabled. They are bounded, allocation-free,
/// callback-free, and never acquire a kernel lock. NotReady and InvalidDeadline
/// leave the previously programmed deadline unchanged. DeadlineReached keeps
/// that deadline authoritative: it is either still armed or has already
/// produced a latched event after the hardware automatically disarmed it. The
/// caller services the boundary synchronously and later acknowledges any stale
/// event. HardwareFailure means the register state is no longer trustworthy and
/// the owning processor must fail-stop.
class ILocalTimer : public IMonotonicClock
{
public:
    /// Programs one finite absolute deadline strictly after the timer's own
    /// current clock sample.
    [[nodiscard]] virtual LocalTimerError ProgramDeadline(Foundation::UInt64 deadlineTicks) noexcept = 0;

    /// Removes the programmed deadline. Success guarantees that this source
    /// cannot newly assert the removed deadline. An event already latched by
    /// the interrupt controller may still be delivered; interrupt handling
    /// acknowledges it and re-evaluates current policy as a stale event.
    [[nodiscard]] virtual LocalTimerError CancelDeadline() noexcept = 0;

    /// Consumes the exact software-owned deadline after target interruption
    /// entry has already masked the hardware source and verified that mask by
    /// readback. This operation validates deadlineTicks against the currently
    /// armed deadline and clears only the backend's software state; it performs
    /// no hardware write. NotReady, InvalidDeadline, and HardwareFailure leave
    /// the software deadline unchanged.
    [[nodiscard]] virtual LocalTimerError
    AcknowledgeExternallyMaskedDeadline(Foundation::UInt64 deadlineTicks) noexcept = 0;

protected:
    /// Timer storage is processor-owned and is never deleted through the HAL
    /// interface.
    constexpr ILocalTimer() noexcept = default;
    ~ILocalTimer() noexcept = default;
};

static_assert(sizeof(LocalTimerError) == 1U);
} // namespace Asteria::Hal::Time

#endif // ASTERIA_HAL_TIME_I_LOCAL_TIMER_HPP_INCLUDED
