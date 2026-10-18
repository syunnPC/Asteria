// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64DeviceInterruptPoll.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
namespace
{
constexpr Foundation::UInt64 SpuriousVector{15U};
constexpr Foundation::UInt64 TimerVector{0xE8U};
// Every device vector and a timer tick ahead of them: a bound on one poll.
constexpr Foundation::Size MaximumAccepts{Ia64InterruptLines::VectorCount + 1U};
} // namespace

Ia64DeviceInterruptPoll DeliverPendingDeviceInterrupts(Ia64InterruptLines& lines) noexcept
{
    Ia64DeviceInterruptPoll result{Ia64DeviceInterruptPoll::None};
    for (Foundation::Size accepted{0U}; accepted < MaximumAccepts; ++accepted)
    {
        if (AsteriaIa64ReadPendingDeviceVectors() == 0U)
        {
            return result;
        }
        const Foundation::UInt64 vector = AsteriaIa64AcceptPendingInterrupt();
        if (vector == SpuriousVector)
        {
            return result;
        }
        if (Ia64InterruptLines::IsDeviceVector(vector))
        {
            // Relinquished first, as the PL3 claim does.
            AsteriaIa64EndAcceptedInterrupt();
            lines.Dispatch(vector);
            result = Ia64DeviceInterruptPoll::Delivered;
            continue;
        }
        // A timer tick that pended in idle is ahead of every device vector;
        // the run it ended is gone and the next dispatch programs its own
        // deadline, so it is acknowledged as the PL3 claim acknowledges a
        // tick that is not due.
        AsteriaIa64EndAcceptedInterrupt();
        if (vector != TimerVector)
        {
            return Ia64DeviceInterruptPoll::Unexpected;
        }
    }
    return result;
}
} // namespace Asteria::Platform::Ia64::Interrupt

#if !defined(__ia64__)
extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadPendingDeviceVectors() noexcept
{
    return 0U;
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64AcceptPendingInterrupt() noexcept
{
    return 15U;
}

extern "C" void AsteriaIa64EndAcceptedInterrupt() noexcept
{
}
#endif
