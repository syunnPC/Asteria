// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_DISPATCH_COORDINATOR_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_DISPATCH_COORDINATOR_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/Scheduler/Thread.hpp"
#include "Kernel/Time/IMonotonicClock.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Scheduler
{
enum class DispatchCoordinatorState : Foundation::UInt8
{
    Idle,
    Active,
    Faulted,
};

enum class DispatchCoordinatorError : Foundation::UInt8
{
    None,
    AlreadyActive,
    NotActive,
    Faulted,
    InvalidThread,
    ThreadRegistryMismatch,
    ThreadInstanceMismatch,
    InvalidCpu,
    InvalidDisposition,
    ClockExhausted,
    ThreadPinRejected,
    ThreadRejected,
    SchedulingContextRejected,
    InvariantViolation,
};

struct DispatchCoordinatorFailure
{
    Foundation::UInt64 ObservedTicks;
    DispatchCoordinatorError Error;
    Object::ObjectPinError Pin;
    SchedulingContextError Context;
    ThreadError Thread;
    Foundation::UInt8 Reserved[4];
};

struct DispatchAdmission
{
    SchedulingContextDispatch Context;
    ThreadSnapshot Thread;
    Foundation::UInt64 StartTicks;
};

struct DispatchCompletion
{
    SchedulingContextRunReceipt Context;
    ThreadSnapshot Thread;
};

struct DispatchCoordinatorSnapshot
{
    ThreadId Thread;
    DispatchCoordinatorState State;
    CpuId Processor;
    Foundation::UInt8 Reserved[6];
};

/// Coordinates one architecture-neutral Thread and SchedulingContext run. This object is a
/// kernel-internal lifetime and accounting primitive. It does not grant authority, select a ready
/// thread, arm a timer, or switch registers. A successful Start retains the exact Thread generation
/// until Finish commits. State operations serialize on the coordinator lock and may spin; callers
/// prevent same-CPU reentry and do not call them from interrupt context.
class DispatchCoordinator final
{
public:
    /// Preconditions:
    /// - objectRegistry and clock outlive this coordinator.
    /// - Direct Thread and SchedulingContext transitions are excluded while a
    /// coordinator operation or active dispatch uses those objects.
    /// - After Start succeeds, the exact SchedulingContext storage remains
    /// alive and immovable until Finish succeeds or fault handling ends.
    DispatchCoordinator(Object::ObjectRegistry& objectRegistry, Time::IMonotonicClock& clock) noexcept
        : m_objectRegistry{objectRegistry}, m_clock{clock}
    {
    }

    /// Preconditions:
    /// - GetSnapshot.State is Idle.
    /// - No concurrent operation accesses this object during destruction.
    ~DispatchCoordinator() noexcept = default;

    DispatchCoordinator(const DispatchCoordinator&) = delete;
    DispatchCoordinator& operator=(const DispatchCoordinator&) = delete;
    DispatchCoordinator(DispatchCoordinator&&) = delete;
    DispatchCoordinator& operator=(DispatchCoordinator&&) = delete;

    /// Starts the exact context-current Thread generation. Failure restores a
    /// transient Thread admission and leaves the coordinator idle unless an
    /// internal invariant fails, in which case the coordinator becomes Faulted.
    /// A successful call retains context by reference under the constructor's
    /// active-dispatch lifetime precondition and pins thread through the registry.
    [[nodiscard]] Foundation::Result<DispatchAdmission, DispatchCoordinatorFailure>
    Start(SchedulingContext& context, Thread& thread, ThreadId threadId, CpuId processor) noexcept;

    /// Accounts the active run and then commits its Thread disposition.
    /// A recoverable clock/context failure leaves the dispatch active for retry.
    [[nodiscard]] Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>
    Finish(ThreadRunDisposition disposition, Foundation::UInt64 exitCode = 0U) noexcept;

    /// Accounts one externally captured stop as Ready at the exact supplied
    /// local-clock sample. This is a non-ISR scheduler-continuation operation;
    /// it does not read the clock and cannot express Blocked or Exited.
    [[nodiscard]] Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>
    FinishExternallyStoppedAsReady(Foundation::UInt64 observedTicks) noexcept;

    [[nodiscard]] DispatchCoordinatorSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>
    FinishAtLocked(ThreadRunDisposition disposition, Foundation::UInt64 exitCode,
                   Foundation::UInt64 observedTicks) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Time::IMonotonicClock& m_clock;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Object::ObjectPin m_threadPin;
    SchedulingContext* m_context{nullptr};
    Thread* m_thread{nullptr};
    ThreadId m_threadId{};
    CpuId m_processor{};
    DispatchCoordinatorState m_state{DispatchCoordinatorState::Idle};
};

static_assert(sizeof(DispatchCoordinatorState) == 1U);
static_assert(sizeof(DispatchCoordinatorError) == 1U);
static_assert(sizeof(DispatchCoordinatorFailure) == 16U);
static_assert(alignof(DispatchCoordinatorFailure) == 8U);
static_assert(sizeof(DispatchAdmission) == 80U);
static_assert(alignof(DispatchAdmission) == 8U);
static_assert(sizeof(DispatchCompletion) == 96U);
static_assert(alignof(DispatchCompletion) == 8U);
static_assert(sizeof(DispatchCoordinatorSnapshot) == 24U);
static_assert(alignof(DispatchCoordinatorSnapshot) == 8U);
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_KERNEL_SCHEDULER_DISPATCH_COORDINATOR_HPP_INCLUDED
