// SPDX-License-Identifier: Apache-2.0

#include "Kernel/System/WaitSetSystemCallDispatcher.hpp"

#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Scheduler/ProcessorRunController.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/Scheduler/Thread.hpp"

namespace Asteria::Kernel::System
{
namespace
{
using DispatchResult = Foundation::Result<WaitSetSystemCallDispatchResult, WaitSetSystemCallDispatchFailure>;

[[nodiscard]] constexpr WaitSetSystemCallDispatchFailure
MakeFailure(const WaitSetSystemCallDispatchError error, const Ipc::WaitSetThreadDispatcherFailure& dispatcher = {},
            const Object::ObjectPinError waitSetPin = Object::ObjectPinError::Ok,
            const Object::ObjectPinError threadPin = Object::ObjectPinError::Ok,
            const Object::ObjectPinError schedulingContextPin = Object::ObjectPinError::Ok) noexcept
{
    return WaitSetSystemCallDispatchFailure{
        .Dispatcher = dispatcher,
        .WaitSetPin = waitSetPin,
        .ThreadPin = threadPin,
        .SchedulingContextPin = schedulingContextPin,
        .Error = error,
        .Reserved = {0U, 0U},
    };
}

[[nodiscard]] constexpr SystemCallError MapCapabilityError(const Object::CapabilityError error) noexcept
{
    switch (error)
    {
    case Object::CapabilityError::Ok:
        return SystemCallError::None;
    case Object::CapabilityError::InvalidArgument:
        return SystemCallError::InvalidArguments;
    case Object::CapabilityError::InvalidConfiguration:
        return SystemCallError::InvalidConfiguration;
    case Object::CapabilityError::InvalidHandle:
        return SystemCallError::InvalidHandle;
    case Object::CapabilityError::ObjectNotFound:
        return SystemCallError::ObjectNotFound;
    case Object::CapabilityError::ResourceExhausted:
        return SystemCallError::ResourceExhausted;
    case Object::CapabilityError::SpaceExhausted:
        return SystemCallError::SpaceExhausted;
    case Object::CapabilityError::RightsDenied:
        return SystemCallError::RightsDenied;
    case Object::CapabilityError::RightsAmplification:
        return SystemCallError::RightsAmplification;
    case Object::CapabilityError::NoAttenuation:
        return SystemCallError::NoAttenuation;
    case Object::CapabilityError::TypeMismatch:
        return SystemCallError::TypeMismatch;
    case Object::CapabilityError::Revoked:
        return SystemCallError::Revoked;
    case Object::CapabilityError::Expired:
        return SystemCallError::Expired;
    case Object::CapabilityError::LeaseExtension:
        return SystemCallError::LeaseExtension;
    case Object::CapabilityError::RevocationDomainExhausted:
        return SystemCallError::RevocationDomainExhausted;
    }
    return SystemCallError::InvalidConfiguration;
}

constexpr void WriteCompleted(WaitSetSystemCallDispatchResult& result, const SystemCallError error,
                              const Foundation::UInt64 value0 = 0U, const Foundation::UInt64 value1 = 0U,
                              const Ipc::WaitSetThreadDispatcherReceipt& receipt = {}) noexcept
{
    result = WaitSetSystemCallDispatchResult{
        .Response =
            SystemCallResponse{
                .Error = error,
                .Value0 = value0,
                .Value1 = value1,
            },
        .Wait = receipt,
        .Disposition = WaitSetSystemCallDisposition::Completed,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

constexpr void WriteSuspendPrepared(WaitSetSystemCallDispatchResult& result,
                                    const Ipc::WaitSetThreadDispatcherReceipt& receipt) noexcept
{
    result = WaitSetSystemCallDispatchResult{
        .Response =
            SystemCallResponse{
                .Error = SystemCallError::None,
                .Value0 = 0U,
                .Value1 = 0U,
            },
        .Wait = receipt,
        .Disposition = WaitSetSystemCallDisposition::SuspendPrepared,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

[[nodiscard]] constexpr Ipc::WaitSetThreadDispatcherReceipt
ReceiptFromSnapshot(const Ipc::WaitSetThreadDispatcherSnapshot& snapshot) noexcept
{
    return Ipc::WaitSetThreadDispatcherReceipt{
        .Dispatcher = snapshot.Dispatcher,
        .WaitSet = snapshot.WaitSet,
        .Thread = snapshot.Thread,
        .SchedulingContext = snapshot.SchedulingContext,
        .Notification = snapshot.Notification,
        .RegistrationSequence = snapshot.RegistrationSequence,
        .RunSequence = snapshot.RunSequence,
        .BlockSequence = snapshot.BlockSequence,
        .TransactionGeneration = snapshot.TransactionGeneration,
        .Tag = snapshot.Tag,
        .QueueSequence = snapshot.QueueSequence,
        .State = snapshot.State,
        .TimedOut = snapshot.TimedOut,
        .Aborted = snapshot.Aborted,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr Ipc::WaitSetThreadDispatcherFailure MakeCompletionUnavailableFailure() noexcept
{
    return Ipc::WaitSetThreadDispatcherFailure{
        .WaitSet = {},
        .Queue = {},
        .WaitSetPin = Object::ObjectPinError::Ok,
        .ThreadPin = Object::ObjectPinError::Ok,
        .SchedulingContextPin = Object::ObjectPinError::Ok,
        .Thread = Scheduler::ThreadError::None,
        .QueueTransition = Scheduler::ReadyQueueError::None,
        .Error = Ipc::WaitSetThreadDispatcherError::CompletionUnavailable,
    };
}
} // namespace

bool WaitSetSystemCallDispatcher::ValidateActiveBinding(const Scheduler::ProcessorActiveExecutionBinding& active,
                                                        Object::ObjectPin& threadPin,
                                                        Object::ObjectPin& schedulingContextPin,
                                                        WaitSetSystemCallDispatchFailure& failure) noexcept
{
    if ((active.ThreadStorage == nullptr) || (active.SchedulingContextStorage == nullptr) ||
        !active.ThreadIdentity.IsValid() || !active.SchedulingContextIdentity.IsValid() ||
        !active.Processor.IsValid() || (active.RunSequence == 0U) ||
        !active.ThreadIdentity.IsFromRegistry(m_objectRegistry) ||
        !active.SchedulingContextIdentity.IsFromRegistry(m_objectRegistry))
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::InvalidActiveBinding);
        return false;
    }

    Object::ObjectPinError pin =
        m_objectRegistry.PinObject(active.ThreadIdentity.GetObjectId(), Object::ObjectType::Thread, threadPin);
    if (pin != Object::ObjectPinError::Ok)
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::ThreadPinRejected, {}, Object::ObjectPinError::Ok, pin);
        return false;
    }
    pin = m_objectRegistry.PinObject(active.SchedulingContextIdentity.GetObjectId(),
                                     Object::ObjectType::SchedulingContext, schedulingContextPin);
    if (pin != Object::ObjectPinError::Ok)
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::SchedulingContextPinRejected, {},
                              Object::ObjectPinError::Ok, Object::ObjectPinError::Ok, pin);
        return false;
    }
    if (&threadPin.GetObject() != static_cast<Object::IKernelObject*>(active.ThreadStorage))
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::ThreadInstanceMismatch);
        return false;
    }
    if (&schedulingContextPin.GetObject() != static_cast<Object::IKernelObject*>(active.SchedulingContextStorage))
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::SchedulingContextInstanceMismatch);
        return false;
    }

    const Scheduler::ThreadSnapshot thread = active.ThreadStorage->GetSnapshot();
    if ((thread.State != Scheduler::ThreadState::Running) || !thread.RunningProcessor.IsValid() ||
        (thread.RunningProcessor.GetValue() != active.Processor.GetValue()) ||
        (thread.RunSequence != active.RunSequence) || thread.BlockToken.IsValid() || (thread.ExitCode != 0U))
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::ActiveThreadStateMismatch);
        return false;
    }

    const Scheduler::SchedulingContextSnapshot context = active.SchedulingContextStorage->GetSnapshot();
    if ((context.State != Scheduler::SchedulingContextState::Running) || !context.RunningProcessor.IsValid() ||
        (context.RunningProcessor.GetValue() != active.Processor.GetValue()) ||
        !context.CurrentThread.IsEqualTo(active.ThreadIdentity))
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::ActiveSchedulingContextStateMismatch);
        return false;
    }
    return true;
}

namespace Detail
{
WaitSetSystemCallDispatchError ValidatePreparedReceipt(const Ipc::WaitSetThreadDispatcher& dispatcher,
                                                       const Ipc::WaitSetThreadDispatcherSnapshot& current,
                                                       const Ipc::WaitSetThreadDispatcherReceipt& receipt,
                                                       const Scheduler::ProcessorActiveExecutionBinding& active,
                                                       const Ipc::WaitSetId waitSet) noexcept
{
    if ((receipt.Dispatcher != &dispatcher) || (current.Dispatcher != &dispatcher) ||
        (receipt.TransactionGeneration == 0U) || (receipt.TransactionGeneration != current.TransactionGeneration) ||
        !receipt.WaitSet.IsEqualTo(waitSet) || !receipt.Thread.IsEqualTo(active.ThreadIdentity) ||
        !receipt.SchedulingContext.IsEqualTo(active.SchedulingContextIdentity) ||
        (receipt.RunSequence != active.RunSequence))
    {
        return WaitSetSystemCallDispatchError::DispatcherReceiptMismatch;
    }

    if (receipt.State == Ipc::WaitSetThreadDispatcherState::Prepared)
    {
        if (receipt.Notification.IsValid() || (receipt.RegistrationSequence != 0U) || (receipt.BlockSequence != 0U) ||
            (receipt.Tag != 0U) || (receipt.QueueSequence != 0U))
        {
            return WaitSetSystemCallDispatchError::DispatcherReceiptMismatch;
        }
        return WaitSetSystemCallDispatchError::None;
    }
    if (receipt.State == Ipc::WaitSetThreadDispatcherState::Completed)
    {
        if (!receipt.Notification.IsValid() || (receipt.RegistrationSequence == 0U) || (receipt.BlockSequence != 0U) ||
            (receipt.QueueSequence != 0U))
        {
            return WaitSetSystemCallDispatchError::DispatcherReceiptMismatch;
        }
        return WaitSetSystemCallDispatchError::None;
    }
    return WaitSetSystemCallDispatchError::UnexpectedDispatcherState;
}

WaitSetSystemCallDispatchError
ValidateCompletionSnapshot(const Ipc::WaitSetThreadDispatcher& dispatcher,
                           const Foundation::UInt64 transactionGeneration,
                           const Ipc::WaitSetThreadDispatcherSnapshot& snapshot,
                           const Scheduler::ProcessorActiveExecutionBinding& active) noexcept
{
    // The blocked thread runs again only once this completion woke it, so
    // every later run of it is the resumption. An admission rolled back
    // before entry (its run stop passed) consumes a run sequence without
    // running, so the resumed run need not be the immediate successor.
    if ((snapshot.State != Ipc::WaitSetThreadDispatcherState::Completed) || (snapshot.Dispatcher != &dispatcher) ||
        (transactionGeneration == 0U) || (snapshot.TransactionGeneration != transactionGeneration) ||
        !snapshot.WaitSet.IsValid() || !snapshot.Thread.IsEqualTo(active.ThreadIdentity) ||
        !snapshot.SchedulingContext.IsEqualTo(active.SchedulingContextIdentity)
        // A readiness wake names its registration; an expiry names none.
        || (snapshot.Notification.IsValid() == snapshot.TimedOut) ||
        ((snapshot.RegistrationSequence == 0U) != snapshot.TimedOut)
        // An abort completes as an expiry does.
        || (snapshot.Aborted && !snapshot.TimedOut) || (snapshot.BlockSequence == 0U) ||
        (snapshot.QueueSequence == 0U) || (snapshot.RetainedClaimSequence != 0U) ||
        (snapshot.RetainedQueueReservationSequence != 0U) || (snapshot.RunSequence == 0U) ||
        (active.RunSequence <= snapshot.RunSequence))
    {
        return WaitSetSystemCallDispatchError::DispatcherReceiptMismatch;
    }
    return WaitSetSystemCallDispatchError::None;
}

WaitSetSystemCallDispatchFailure
RejectUnavailableCompletion(const Ipc::WaitSetThreadDispatcherSnapshot& snapshot) noexcept
{
    return MakeFailure(WaitSetSystemCallDispatchError::DispatcherRejected,
                       snapshot.State == Ipc::WaitSetThreadDispatcherState::Faulted
                           ? snapshot.Diagnostic
                           : MakeCompletionUnavailableFailure());
}
} // namespace Detail

bool WaitSetSystemCallDispatcher::ValidatePreparedReceipt(const Ipc::WaitSetThreadDispatcherReceipt& receipt,
                                                          const Scheduler::ProcessorActiveExecutionBinding& active,
                                                          const Ipc::WaitSetId waitSet,
                                                          WaitSetSystemCallDispatchFailure& failure) noexcept
{
    const WaitSetSystemCallDispatchError error =
        Detail::ValidatePreparedReceipt(m_dispatcher, m_dispatcher.GetSnapshot(), receipt, active, waitSet);
    if (error != WaitSetSystemCallDispatchError::None)
    {
        failure = MakeFailure(error);
        return false;
    }
    return true;
}

bool WaitSetSystemCallDispatcher::ValidateCompletionSnapshot(const Ipc::WaitSetThreadDispatcherSnapshot& snapshot,
                                                             const Scheduler::ProcessorActiveExecutionBinding& active,
                                                             WaitSetSystemCallDispatchFailure& failure) noexcept
{
    const WaitSetSystemCallDispatchError error =
        Detail::ValidateCompletionSnapshot(m_dispatcher, m_transactionGeneration, snapshot, active);
    if (error != WaitSetSystemCallDispatchError::None)
    {
        failure = MakeFailure(error);
        return false;
    }
    return true;
}

DispatchResult WaitSetSystemCallDispatcher::Dispatch(Object::CapabilitySpace& caller,
                                                     const Scheduler::ProcessorActiveExecutionBinding& active,
                                                     const SystemCallRequest& request) noexcept
{
    WaitSetSystemCallDispatchResult result{};
    WaitSetSystemCallDispatchFailure failure{};
    if (!DispatchTransaction(caller, active, request, result, failure))
    {
        return DispatchResult::Failure(failure);
    }
    return DispatchResult::Success(result);
}

bool WaitSetSystemCallDispatcher::DispatchTransaction(Object::CapabilitySpace& caller,
                                                      const Scheduler::ProcessorActiveExecutionBinding& active,
                                                      const SystemCallRequest& request,
                                                      WaitSetSystemCallDispatchResult& result,
                                                      WaitSetSystemCallDispatchFailure& failure) noexcept
{
    if (!IsWaitSetSystemCall(request.Number))
    {
        WriteCompleted(result, SystemCallError::UnknownSystemCall);
        return true;
    }
    const SystemCallError timeout = DecodeTimeout(request);
    if (timeout != SystemCallError::None)
    {
        WriteCompleted(result, timeout);
        return true;
    }
    if (!caller.IsFromRegistry(m_objectRegistry))
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::InvalidConfiguration);
        return false;
    }

    Object::ObjectPin threadPin;
    Object::ObjectPin schedulingContextPin;
    if (!ValidateActiveBinding(active, threadPin, schedulingContextPin, failure))
    {
        return false;
    }
    if (!DispatchAuthorized(caller, active, request, result, failure))
    {
        return false;
    }
    return true;
}

bool WaitSetSystemCallDispatcher::DispatchAuthorized(Object::CapabilitySpace& caller,
                                                     const Scheduler::ProcessorActiveExecutionBinding& active,
                                                     const SystemCallRequest& request,
                                                     WaitSetSystemCallDispatchResult& result,
                                                     WaitSetSystemCallDispatchFailure& failure) noexcept
{
    const Object::CapabilityHandle handle{request.Arguments[0U], request.Arguments[1U]};
    const Object::CapabilityLookupResult authorized =
        caller.Lookup(handle, Object::ObjectType::WaitSet, Object::CapabilityOperation::Wait);
    if (!authorized.HasValue())
    {
        WriteCompleted(result, MapCapabilityError(authorized.GetError()));
        return true;
    }

    const Object::CapabilityLease& lease = authorized.GetValue();
    const Object::CapabilityDescriptor& descriptor = lease.GetDescriptor();
    if (!descriptor.Object.IsValid() || (descriptor.Type != Object::ObjectType::WaitSet) ||
        (lease.GetObject().GetObjectType() != Object::ObjectType::WaitSet))
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::WaitSetInstanceMismatch);
        return false;
    }

    Object::ObjectPin waitSetPin;
    const Object::ObjectPinError pin =
        m_objectRegistry.PinObject(descriptor.Object, Object::ObjectType::WaitSet, waitSetPin);
    if (pin != Object::ObjectPinError::Ok)
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::WaitSetPinRejected, {}, pin);
        return false;
    }
    if ((waitSetPin.GetObjectId().GetValue() != descriptor.Object.GetValue()) ||
        (&waitSetPin.GetObject() != &lease.GetObject()))
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::WaitSetInstanceMismatch);
        return false;
    }

    const Ipc::WaitSetId waitSet{m_objectRegistry, descriptor.Object};
    return PrepareAuthorized(waitSet, active, result, failure);
}

bool WaitSetSystemCallDispatcher::PrepareAuthorized(const Ipc::WaitSetId waitSet,
                                                    const Scheduler::ProcessorActiveExecutionBinding& active,
                                                    WaitSetSystemCallDispatchResult& result,
                                                    WaitSetSystemCallDispatchFailure& failure) noexcept
{
    const auto prepared = m_dispatcher.Prepare(Ipc::WaitSetThreadPrepareRequest{
        .WaitSet = waitSet,
        .Thread = active.ThreadIdentity,
        .SchedulingContext = active.SchedulingContextIdentity,
        .DeadlineTicks = m_deadline,
    });
    if (!prepared.HasValue())
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::DispatcherRejected, prepared.GetErrorReference());
        return false;
    }

    const Ipc::WaitSetThreadDispatcherReceipt& receipt = prepared.GetValue();
    if (!ValidatePreparedReceipt(receipt, active, waitSet, failure))
    {
        RejectDispatcherReceipt(receipt, failure);
        return false;
    }
    m_transactionGeneration = receipt.TransactionGeneration;
    if (receipt.State == Ipc::WaitSetThreadDispatcherState::Completed)
    {
        return ConsumeImmediateCompletion(receipt, result, failure);
    }
    WriteSuspendPrepared(result, receipt);
    return true;
}

bool WaitSetSystemCallDispatcher::ConsumeImmediateCompletion(const Ipc::WaitSetThreadDispatcherReceipt& prepared,
                                                             WaitSetSystemCallDispatchResult& result,
                                                             WaitSetSystemCallDispatchFailure& failure) noexcept
{
    const auto consumed = m_dispatcher.ConsumeCompletion(prepared);
    if (!consumed.HasValue())
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::DispatcherRejected, consumed.GetErrorReference());
        return false;
    }
    const Ipc::WaitSetThreadDispatcherReceipt& receipt = consumed.GetValue();
    WriteCompleted(result, SystemCallError::None, receipt.Tag, receipt.RegistrationSequence, receipt);
    m_transactionGeneration = 0U;
    return true;
}

// Argument 2 is a relative timeout in microseconds, zero for none; arguments
// 3 to 5 are zero. Sets the deadline the wait is prepared with.
SystemCallError WaitSetSystemCallDispatcher::DecodeTimeout(const SystemCallRequest& request) noexcept
{
    const Foundation::UInt64 timeout = request.Arguments[2U];
    if ((timeout > MaximumTimeoutMicroseconds) || (request.Arguments[3U] != 0U) || (request.Arguments[4U] != 0U) ||
        (request.Arguments[5U] != 0U))
    {
        return SystemCallError::InvalidArguments;
    }
    if ((timeout != 0U) && (m_clock == nullptr))
    {
        return SystemCallError::InvalidConfiguration;
    }
    m_deadline = (timeout != 0U) ? (m_clock->GetTicks() + (timeout * m_ticksPerMicrosecond)) : 0U;
    return SystemCallError::None;
}

bool WaitSetSystemCallDispatcher::ConfigureTimeouts(const Hal::Time::IMonotonicClock& clock,
                                                    const Foundation::UInt64 ticksPerMicrosecond) noexcept
{
    // At most 2^16 ticks per microsecond keeps the longest timeout's ticks
    // far below the clock's range.
    if ((m_clock != nullptr) || (ticksPerMicrosecond == 0U) || (ticksPerMicrosecond > 0x1'0000U))
    {
        return false;
    }
    m_clock = &clock;
    m_ticksPerMicrosecond = ticksPerMicrosecond;
    return true;
}

DispatchResult
WaitSetSystemCallDispatcher::ConsumeCompletion(const Scheduler::ProcessorActiveExecutionBinding& active) noexcept
{
    WaitSetSystemCallDispatchResult result{};
    WaitSetSystemCallDispatchFailure failure{};
    Object::ObjectPin threadPin;
    Object::ObjectPin schedulingContextPin;
    if (!ValidateActiveBinding(active, threadPin, schedulingContextPin, failure))
    {
        return DispatchResult::Failure(failure);
    }
    if (!ConsumeValidatedCompletion(active, result, failure))
    {
        return DispatchResult::Failure(failure);
    }
    return DispatchResult::Success(result);
}

bool WaitSetSystemCallDispatcher::ConsumeValidatedCompletion(const Scheduler::ProcessorActiveExecutionBinding& active,
                                                             WaitSetSystemCallDispatchResult& result,
                                                             WaitSetSystemCallDispatchFailure& failure) noexcept
{
    const Ipc::WaitSetThreadDispatcherSnapshot snapshot = m_dispatcher.GetSnapshot();
    if (snapshot.State != Ipc::WaitSetThreadDispatcherState::Completed)
    {
        return ConsumeUnavailableCompletion(snapshot, failure);
    }
    if (!ValidateCompletionSnapshot(snapshot, active, failure))
    {
        RejectDispatcherReceipt(ReceiptFromSnapshot(snapshot), failure);
        return false;
    }
    return ConsumeExactCompletion(snapshot, result, failure);
}

bool WaitSetSystemCallDispatcher::ConsumeExactCompletion(const Ipc::WaitSetThreadDispatcherSnapshot& snapshot,
                                                         WaitSetSystemCallDispatchResult& result,
                                                         WaitSetSystemCallDispatchFailure& failure) noexcept
{
    const auto consumed = m_dispatcher.ConsumeCompletion(ReceiptFromSnapshot(snapshot));
    if (!consumed.HasValue())
    {
        failure = MakeFailure(WaitSetSystemCallDispatchError::DispatcherRejected, consumed.GetErrorReference());
        return false;
    }
    const Ipc::WaitSetThreadDispatcherReceipt& receipt = consumed.GetValue();
    WriteCompleted(result, receipt.TimedOut ? SystemCallError::TimedOut : SystemCallError::None, receipt.Tag,
                   receipt.RegistrationSequence, receipt);
    m_transactionGeneration = 0U;
    return true;
}

bool WaitSetSystemCallDispatcher::ConsumeUnavailableCompletion(const Ipc::WaitSetThreadDispatcherSnapshot& snapshot,
                                                               WaitSetSystemCallDispatchFailure& failure) noexcept
{
    failure = Detail::RejectUnavailableCompletion(snapshot);
    return false;
}

void WaitSetSystemCallDispatcher::RejectDispatcherReceipt(const Ipc::WaitSetThreadDispatcherReceipt& receipt,
                                                          WaitSetSystemCallDispatchFailure& failure) noexcept
{
    failure.Dispatcher = m_dispatcher.RejectReceiptInvariant(receipt);
}
} // namespace Asteria::Kernel::System
