// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Ipc/ReplyToken.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Ipc
{
namespace
{
[[nodiscard]] constexpr ReplyTokenError MapCallerPinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::Ok:
        return ReplyTokenError::None;
    case Object::ObjectPinError::InvalidArgument:
        return ReplyTokenError::InvalidCaller;
    case Object::ObjectPinError::ObjectNotFound:
        return ReplyTokenError::CallerNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return ReplyTokenError::CallerTypeMismatch;
    case Object::ObjectPinError::OutputOccupied:
    case Object::ObjectPinError::ReferenceExhausted:
        return ReplyTokenError::CallerPinUnavailable;
    }
    return ReplyTokenError::CallerPinUnavailable;
}

[[nodiscard]] constexpr ReplyTokenError MapSchedulingContextPinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::Ok:
        return ReplyTokenError::None;
    case Object::ObjectPinError::InvalidArgument:
        return ReplyTokenError::InvalidSchedulingContext;
    case Object::ObjectPinError::ObjectNotFound:
        return ReplyTokenError::SchedulingContextNotFound;
    case Object::ObjectPinError::TypeMismatch:
        return ReplyTokenError::SchedulingContextTypeMismatch;
    case Object::ObjectPinError::OutputOccupied:
    case Object::ObjectPinError::ReferenceExhausted:
        return ReplyTokenError::SchedulingContextPinUnavailable;
    }
    return ReplyTokenError::SchedulingContextPinUnavailable;
}

[[nodiscard]] constexpr bool IsUsableContextState(const Scheduler::SchedulingContextState state) noexcept
{
    switch (state)
    {
    case Scheduler::SchedulingContextState::Bound:
    case Scheduler::SchedulingContextState::Donated:
    case Scheduler::SchedulingContextState::BudgetExhausted:
        return true;

    case Scheduler::SchedulingContextState::Uninitialized:
    case Scheduler::SchedulingContextState::Unbound:
    case Scheduler::SchedulingContextState::Running:
        return false;
    }
    return false;
}
} // namespace

ReplyTokenFailure ReplyToken::MakeFailure(const ReplyTokenError error, const Object::ObjectPinError pinError,
                                          const Scheduler::SchedulingContextError schedulingError) noexcept
{
    return ReplyTokenFailure{
        .Error = error,
        .PinError = pinError,
        .SchedulingError = schedulingError,
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    };
}

ReplyTokenFailure ReplyToken::Initialize(Object::ObjectRegistry& registry, const Object::ResourceDomainId chargeOwner,
                                         const Scheduler::ThreadId caller,
                                         const Scheduler::SchedulingContextId schedulingContext,
                                         const Scheduler::ThreadBlockToken blockToken,
                                         const Foundation::UInt64 transactionIdentifier) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != ReplyTokenState::Uninitialized)
    {
        return MakeFailure(ReplyTokenError::AlreadyInitialized);
    }
    if (!chargeOwner.IsValid())
    {
        return MakeFailure(ReplyTokenError::InvalidConfiguration);
    }
    const ReplyTokenFailure armed = ArmLocked(registry, caller, schedulingContext, blockToken, transactionIdentifier);
    if (armed.Error != ReplyTokenError::None)
    {
        return armed;
    }
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return MakeFailure(ReplyTokenError::None);
}

ReplyTokenFailure ReplyToken::Configure(const Object::ResourceDomainId chargeOwner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != ReplyTokenState::Uninitialized)
    {
        return MakeFailure(ReplyTokenError::AlreadyInitialized);
    }
    if (!chargeOwner.IsValid())
    {
        return MakeFailure(ReplyTokenError::InvalidConfiguration);
    }

    m_state = ReplyTokenState::Available;
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return MakeFailure(ReplyTokenError::None);
}

ReplyTokenFailure ReplyToken::Arm(Object::ObjectRegistry& registry, const Scheduler::ThreadId caller,
                                  const Scheduler::SchedulingContextId schedulingContext,
                                  const Scheduler::ThreadBlockToken blockToken,
                                  const Foundation::UInt64 transactionIdentifier) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == ReplyTokenState::Uninitialized)
    {
        return MakeFailure(ReplyTokenError::NotInitialized);
    }
    if (m_state != ReplyTokenState::Available)
    {
        return MakeFailure(ReplyTokenError::AlreadyInitialized);
    }
    return ArmLocked(registry, caller, schedulingContext, blockToken, transactionIdentifier);
}

ReplyTokenFailure ReplyToken::ArmLocked(Object::ObjectRegistry& registry, const Scheduler::ThreadId caller,
                                        const Scheduler::SchedulingContextId schedulingContext,
                                        const Scheduler::ThreadBlockToken blockToken,
                                        const Foundation::UInt64 transactionIdentifier) noexcept
{
    if (transactionIdentifier == 0U)
    {
        return MakeFailure(ReplyTokenError::InvalidTransaction);
    }
    if (!caller.IsValid())
    {
        return MakeFailure(ReplyTokenError::InvalidCaller);
    }
    if (!caller.IsFromRegistry(registry))
    {
        return MakeFailure(ReplyTokenError::CallerRegistryMismatch);
    }
    if (!schedulingContext.IsValid())
    {
        return MakeFailure(ReplyTokenError::InvalidSchedulingContext);
    }
    if (!schedulingContext.IsFromRegistry(registry))
    {
        return MakeFailure(ReplyTokenError::SchedulingContextRegistryMismatch);
    }
    if (!blockToken.IsValid())
    {
        return MakeFailure(ReplyTokenError::InvalidBlockToken);
    }

    const Object::ObjectPinError callerPinError =
        registry.PinObject(caller.GetObjectId(), Object::ObjectType::Thread, m_callerPin);
    if (callerPinError != Object::ObjectPinError::Ok)
    {
        return MakeFailure(MapCallerPinError(callerPinError), callerPinError);
    }

    const Object::ObjectPinError contextPinError = registry.PinObject(
        schedulingContext.GetObjectId(), Object::ObjectType::SchedulingContext, m_schedulingContextPin);
    if (contextPinError != Object::ObjectPinError::Ok)
    {
        m_callerPin.Reset();
        return MakeFailure(MapSchedulingContextPinError(contextPinError), contextPinError);
    }

    Scheduler::Thread& callerObject = static_cast<Scheduler::Thread&>(m_callerPin.GetObject());
    const Scheduler::ThreadSnapshot callerSnapshot = callerObject.GetSnapshot();
    if (callerSnapshot.State != Scheduler::ThreadState::Blocked)
    {
        m_schedulingContextPin.Reset();
        m_callerPin.Reset();
        return MakeFailure(ReplyTokenError::CallerNotBlocked);
    }
    if (!callerSnapshot.BlockToken.IsEqualTo(blockToken))
    {
        m_schedulingContextPin.Reset();
        m_callerPin.Reset();
        return MakeFailure(ReplyTokenError::InvalidBlockToken);
    }

    Scheduler::SchedulingContext& contextObject =
        static_cast<Scheduler::SchedulingContext&>(m_schedulingContextPin.GetObject());
    const Scheduler::SchedulingContextSnapshot contextSnapshot = contextObject.GetSnapshot();
    if (!IsUsableContextState(contextSnapshot.State))
    {
        m_schedulingContextPin.Reset();
        m_callerPin.Reset();
        return MakeFailure(ReplyTokenError::SchedulingContextNotReady);
    }
    if (!contextSnapshot.CurrentThread.IsEqualTo(caller))
    {
        m_schedulingContextPin.Reset();
        m_callerPin.Reset();
        return MakeFailure(ReplyTokenError::SchedulingContextCallerMismatch);
    }

    m_registry = &registry;
    m_caller = caller;
    m_schedulingContext = schedulingContext;
    m_server = Scheduler::ThreadId{};
    m_blockToken = blockToken;
    m_transactionIdentifier = transactionIdentifier;
    m_state = ReplyTokenState::Pending;
    return MakeFailure(ReplyTokenError::None);
}

ReplyTokenFailure ReplyToken::Accept(const Scheduler::ThreadId server) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state == ReplyTokenState::Uninitialized) || (m_state == ReplyTokenState::Available))
    {
        return MakeFailure(ReplyTokenError::NotInitialized);
    }
    if (IsCompletedLocked())
    {
        return MakeFailure(ReplyTokenError::AlreadyCompleted);
    }
    if (m_state == ReplyTokenState::Accepted)
    {
        return MakeFailure(ReplyTokenError::AlreadyAccepted);
    }
    if (!server.IsValid())
    {
        return MakeFailure(ReplyTokenError::InvalidServer);
    }
    if ((m_registry == nullptr) || !server.IsFromRegistry(*m_registry))
    {
        return MakeFailure(ReplyTokenError::ServerRegistryMismatch);
    }

    Scheduler::SchedulingContext& context =
        static_cast<Scheduler::SchedulingContext&>(m_schedulingContextPin.GetObject());
    const Scheduler::SchedulingContextError schedulingError = context.Donate(m_caller, server);
    if (schedulingError != Scheduler::SchedulingContextError::None)
    {
        return MakeFailure(ReplyTokenError::DonationRejected, Object::ObjectPinError::Ok, schedulingError);
    }

    m_server = server;
    m_state = ReplyTokenState::Accepted;
    return MakeFailure(ReplyTokenError::None);
}

Foundation::Result<ReplyTokenCompletion, ReplyTokenFailure>
ReplyToken::CompleteReply(const Scheduler::ThreadId server, const Foundation::UInt64 transactionIdentifier) noexcept
{
    using ResultType = Foundation::Result<ReplyTokenCompletion, ReplyTokenFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state == ReplyTokenState::Uninitialized) || (m_state == ReplyTokenState::Available))
    {
        return ResultType::Failure(MakeFailure(ReplyTokenError::NotInitialized));
    }
    if (IsCompletedLocked())
    {
        return ResultType::Failure(MakeFailure(ReplyTokenError::AlreadyCompleted));
    }
    if (m_state != ReplyTokenState::Accepted)
    {
        return ResultType::Failure(MakeFailure(ReplyTokenError::NotAccepted));
    }
    if (!server.IsValid() || !m_server.IsEqualTo(server))
    {
        return ResultType::Failure(MakeFailure(ReplyTokenError::WrongServer));
    }
    if (transactionIdentifier != m_transactionIdentifier)
    {
        return ResultType::Failure(MakeFailure(ReplyTokenError::TransactionMismatch));
    }

    Scheduler::SchedulingContext& context =
        static_cast<Scheduler::SchedulingContext&>(m_schedulingContextPin.GetObject());
    const Scheduler::SchedulingContextError schedulingError = context.ReturnDonation(server);
    if (schedulingError != Scheduler::SchedulingContextError::None)
    {
        return ResultType::Failure(
            MakeFailure(ReplyTokenError::ReturnRejected, Object::ObjectPinError::Ok, schedulingError));
    }

    m_state = ReplyTokenState::Replied;
    const ReplyTokenCompletion completion = MakeCompletionLocked(m_state);
    return ResultType::Success(completion);
}

Foundation::Result<ReplyTokenCompletion, ReplyTokenFailure> ReplyToken::Cancel() noexcept
{
    using ResultType = Foundation::Result<ReplyTokenCompletion, ReplyTokenFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state == ReplyTokenState::Uninitialized) || (m_state == ReplyTokenState::Available))
    {
        return ResultType::Failure(MakeFailure(ReplyTokenError::NotInitialized));
    }
    if (IsCompletedLocked())
    {
        return ResultType::Failure(MakeFailure(ReplyTokenError::AlreadyCompleted));
    }

    if (m_state == ReplyTokenState::Accepted)
    {
        Scheduler::SchedulingContext& context =
            static_cast<Scheduler::SchedulingContext&>(m_schedulingContextPin.GetObject());
        const Scheduler::SchedulingContextError schedulingError = context.ReturnDonation(m_server);
        if (schedulingError != Scheduler::SchedulingContextError::None)
        {
            return ResultType::Failure(
                MakeFailure(ReplyTokenError::ReturnRejected, Object::ObjectPinError::Ok, schedulingError));
        }
    }

    m_state = ReplyTokenState::Cancelled;
    const ReplyTokenCompletion completion = MakeCompletionLocked(m_state);
    return ResultType::Success(completion);
}

bool ReplyToken::IsInitialized() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state != ReplyTokenState::Uninitialized;
}

ReplyTokenSnapshot ReplyToken::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return ReplyTokenSnapshot{
        .Caller = m_caller,
        .SchedulingContext = m_schedulingContext,
        .Server = m_server,
        .BlockToken = m_blockToken,
        .TransactionIdentifier = m_transactionIdentifier,
        .State = m_state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

Object::ObjectType ReplyToken::GetObjectType() const noexcept
{
    return Object::ObjectType::ReplyToken;
}

Object::ResourceDomainId ReplyToken::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

ReplyTokenCompletion ReplyToken::MakeCompletionLocked(const ReplyTokenState state) const noexcept
{
    return ReplyTokenCompletion{
        .Caller = m_caller,
        .BlockToken = m_blockToken,
        .TransactionIdentifier = m_transactionIdentifier,
        .State = state,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}

bool ReplyToken::IsCompletedLocked() const noexcept
{
    return (m_state == ReplyTokenState::Replied) || (m_state == ReplyTokenState::Cancelled);
}
} // namespace Asteria::Kernel::Ipc
