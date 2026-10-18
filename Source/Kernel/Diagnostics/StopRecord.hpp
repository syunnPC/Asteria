// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_STOP_RECORD_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_STOP_RECORD_HPP_INCLUDED

#include "Kernel/Diagnostics/StopCode.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Kernel::Diagnostics
{
enum class StopState : Foundation::UInt64
{
    Running = 0U,
    Stopping = 1U,
    Frozen = 2U,
    Unconfirmed = 3U,
};

/// ISA-neutral payload and publication word. Only the owning CPU writes the
/// payload; release State publishes it. Snapshot storage belongs to the ISA.
struct StopRecord
{
    Foundation::UInt32 Code;
    Foundation::UInt32 Reserved;
    Foundation::UInt64 Parameters[4U];
    Foundation::UInt64 State;
};
static_assert(__builtin_offsetof(StopRecord, Code) == 0U);
static_assert(__builtin_offsetof(StopRecord, Reserved) == 4U);
static_assert(__builtin_offsetof(StopRecord, Parameters) == 8U);
static_assert(__builtin_offsetof(StopRecord, State) == 40U);
static_assert(sizeof(StopRecord) == 48U);

/// Wait-free latch: zero means no stop, otherwise winner's CPU index + 1.
/// A CPU publishes its own record before calling TryLatch.
class StopLatch final
{
public:
    [[nodiscard]] bool TryLatch(const Foundation::UInt64 processor) noexcept
    {
        Foundation::UInt64 expected{0U};
        return m_owner.CompareExchangeStrongAcquireRelease(expected, processor + 1U);
    }
    [[nodiscard]] Foundation::UInt64 Owner() const noexcept
    {
        return m_owner.LoadAcquire();
    }

private:
    Platform::Synchronization::AtomicUInt64 m_owner;
};

/// The CPU may publish Stopping once; freezing never overwrites its payload.
[[nodiscard]] inline bool BeginStop(StopRecord& record, const StopCode code, const Foundation::UInt64 p1,
                                    const Foundation::UInt64 p2, const Foundation::UInt64 p3,
                                    const Foundation::UInt64 p4) noexcept
{
    if (Platform::Synchronization::AtomicUInt64Reference{record.State}.LoadAcquire() !=
        static_cast<Foundation::UInt64>(StopState::Running))
    {
        return false;
    }
    record.Code = static_cast<Foundation::UInt32>(code);
    record.Parameters[0U] = p1;
    record.Parameters[1U] = p2;
    record.Parameters[2U] = p3;
    record.Parameters[3U] = p4;
    Platform::Synchronization::AtomicUInt64Reference{record.State}.StoreRelease(
        static_cast<Foundation::UInt64>(StopState::Stopping));
    return true;
}
/// Winner only, after the confirmation bound. CAS cannot erase a late freeze.
inline void MarkStopUnconfirmed(StopRecord& record) noexcept
{
    Platform::Synchronization::AtomicUInt64Reference state{record.State};
    Foundation::UInt64 observed = state.LoadAcquire();
    for (Foundation::Size attempt{0U}; attempt < 2U; ++attempt)
    {
        if (observed != static_cast<Foundation::UInt64>(StopState::Running) &&
            observed != static_cast<Foundation::UInt64>(StopState::Stopping))
        {
            return;
        }
        if (state.CompareExchangeStrongAcquireRelease(observed,
                                                      static_cast<Foundation::UInt64>(StopState::Unconfirmed)))
        {
            return;
        }
    }
}
inline void FreezeStop(StopRecord& record) noexcept
{
    Platform::Synchronization::AtomicUInt64Reference{record.State}.StoreRelease(
        static_cast<Foundation::UInt64>(StopState::Frozen));
}
} // namespace Asteria::Kernel::Diagnostics

#endif // ASTERIA_KERNEL_DIAGNOSTICS_STOP_RECORD_HPP_INCLUDED
