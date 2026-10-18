// SPDX-License-Identifier: Apache-2.0

#include "DispatchCoordinator.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Scheduler
{
namespace
{
[[nodiscard]] constexpr DispatchCoordinatorFailure
MakeFailure(const DispatchCoordinatorError error, const Object::ObjectPinError pin = Object::ObjectPinError::Ok,
            const SchedulingContextError context = SchedulingContextError::None,
            const ThreadError thread = ThreadError::None, const Foundation::UInt64 observedTicks = 0U) noexcept
{
    return DispatchCoordinatorFailure{
        .ObservedTicks = observedTicks,
        .Error = error,
        .Pin = pin,
        .Context = context,
        .Thread = thread,
        .Reserved = {0U, 0U, 0U, 0U},
    };
}

[[nodiscard]] constexpr bool IsKnownDisposition(const ThreadRunDisposition disposition) noexcept
{
    switch (disposition)
    {
    case ThreadRunDisposition::Ready:
    case ThreadRunDisposition::Blocked:
    case ThreadRunDisposition::Exited:
        return true;
    }
    return false;
}
} // namespace

Foundation::Result<DispatchAdmission, DispatchCoordinatorFailure>
DispatchCoordinator::Start(SchedulingContext& context, Thread& thread, const ThreadId threadId,
                           const CpuId processor) noexcept
{
    using ResultType = Foundation::Result<DispatchAdmission, DispatchCoordinatorFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DispatchCoordinatorState::Faulted)
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::Faulted));
    }
    if (m_state == DispatchCoordinatorState::Active)
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::AlreadyActive));
    }
    if (!threadId.IsValid())
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::InvalidThread));
    }
    if (!threadId.IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::ThreadRegistryMismatch));
    }
    if (!processor.IsValid())
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::InvalidCpu));
    }
    if (m_threadPin.IsValid())
    {
        m_state = DispatchCoordinatorState::Faulted;
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::InvariantViolation));
    }

    const Object::ObjectPinError pinError =
        m_objectRegistry.PinObject(threadId.GetObjectId(), Object::ObjectType::Thread, m_threadPin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::ThreadPinRejected, pinError));
    }
    if (&m_threadPin.GetObject() != static_cast<Object::IKernelObject*>(&thread))
    {
        m_threadPin.Reset();
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::ThreadInstanceMismatch));
    }

    const Foundation::UInt64 nowTicks = m_clock.GetTicks();
    if (nowTicks == Object::MonotonicTime::Infinite().GetTicks())
    {
        m_threadPin.Reset();
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::ClockExhausted, Object::ObjectPinError::Ok,
                                               SchedulingContextError::None, ThreadError::None, nowTicks));
    }
    const Object::MonotonicTime now{nowTicks};

    const ThreadError threadError = thread.BeginRun(processor);
    if (threadError != ThreadError::None)
    {
        m_threadPin.Reset();
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::ThreadRejected, Object::ObjectPinError::Ok,
                                               SchedulingContextError::None, threadError, nowTicks));
    }

    const auto contextResult = context.StartRun(threadId, processor, now);
    if (!contextResult.HasValue())
    {
        const auto rollback = thread.FinishRun(processor, ThreadRunDisposition::Ready);
        if (!rollback.HasValue())
        {
            m_context = &context;
            m_thread = &thread;
            m_threadId = threadId;
            m_processor = processor;
            m_state = DispatchCoordinatorState::Faulted;
            return ResultType::Failure(MakeFailure(DispatchCoordinatorError::InvariantViolation,
                                                   Object::ObjectPinError::Ok, contextResult.GetError(),
                                                   rollback.GetError(), nowTicks));
        }

        m_threadPin.Reset();
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::SchedulingContextRejected,
                                               Object::ObjectPinError::Ok, contextResult.GetError(), ThreadError::None,
                                               nowTicks));
    }

    m_context = &context;
    m_thread = &thread;
    m_threadId = threadId;
    m_processor = processor;
    m_state = DispatchCoordinatorState::Active;
    return ResultType::Success(DispatchAdmission{
        .Context = contextResult.GetValue(),
        .Thread = thread.GetSnapshot(),
        .StartTicks = nowTicks,
    });
}

Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>
DispatchCoordinator::Finish(const ThreadRunDisposition disposition, const Foundation::UInt64 exitCode) noexcept
{
    using ResultType = Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DispatchCoordinatorState::Faulted)
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::Faulted));
    }
    if (m_state != DispatchCoordinatorState::Active)
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::NotActive));
    }
    if (!IsKnownDisposition(disposition) || ((disposition != ThreadRunDisposition::Exited) && (exitCode != 0U)))
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::InvalidDisposition, Object::ObjectPinError::Ok,
                                               SchedulingContextError::None, ThreadError::InvalidDisposition));
    }

    const Foundation::UInt64 nowTicks = m_clock.GetTicks();
    if (nowTicks == Object::MonotonicTime::Infinite().GetTicks())
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::ClockExhausted, Object::ObjectPinError::Ok,
                                               SchedulingContextError::None, ThreadError::None, nowTicks));
    }

    return FinishAtLocked(disposition, exitCode, nowTicks);
}

Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>
DispatchCoordinator::FinishExternallyStoppedAsReady(const Foundation::UInt64 observedTicks) noexcept
{
    using ResultType = Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == DispatchCoordinatorState::Faulted)
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::Faulted));
    }
    if (m_state != DispatchCoordinatorState::Active)
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::NotActive));
    }
    if (observedTicks == Object::MonotonicTime::Infinite().GetTicks())
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::ClockExhausted, Object::ObjectPinError::Ok,
                                               SchedulingContextError::None, ThreadError::None, observedTicks));
    }
    return FinishAtLocked(ThreadRunDisposition::Ready, 0U, observedTicks);
}

Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>
DispatchCoordinator::FinishAtLocked(const ThreadRunDisposition disposition, const Foundation::UInt64 exitCode,
                                    const Foundation::UInt64 observedTicks) noexcept
{
    using ResultType = Foundation::Result<DispatchCompletion, DispatchCoordinatorFailure>;

    const auto contextResult = m_context->EndRun(m_processor, Object::MonotonicTime{observedTicks});
    if (!contextResult.HasValue())
    {
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::SchedulingContextRejected,
                                               Object::ObjectPinError::Ok, contextResult.GetError(), ThreadError::None,
                                               observedTicks));
    }
    if (!contextResult.GetValue().Thread.IsEqualTo(m_threadId))
    {
        m_state = DispatchCoordinatorState::Faulted;
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::InvariantViolation, Object::ObjectPinError::Ok,
                                               SchedulingContextError::None, ThreadError::None, observedTicks));
    }

    const auto threadResult = m_thread->FinishRun(m_processor, disposition, exitCode);
    if (!threadResult.HasValue())
    {
        m_state = DispatchCoordinatorState::Faulted;
        return ResultType::Failure(MakeFailure(DispatchCoordinatorError::InvariantViolation, Object::ObjectPinError::Ok,
                                               SchedulingContextError::None, threadResult.GetError(), observedTicks));
    }

    const DispatchCompletion completion{
        .Context = contextResult.GetValue(),
        .Thread = threadResult.GetValue(),
    };
    m_context = nullptr;
    m_thread = nullptr;
    m_threadId = ThreadId{};
    m_processor = CpuId{};
    m_state = DispatchCoordinatorState::Idle;
    m_threadPin.Reset();
    return ResultType::Success(completion);
}

DispatchCoordinatorSnapshot DispatchCoordinator::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return DispatchCoordinatorSnapshot{
        .Thread = m_threadId,
        .State = m_state,
        .Processor = m_processor,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}
} // namespace Asteria::Kernel::Scheduler
