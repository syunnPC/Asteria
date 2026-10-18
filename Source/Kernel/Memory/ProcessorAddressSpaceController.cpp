// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/ProcessorAddressSpaceController.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
[[nodiscard]] constexpr Foundation::ProcessorMask ProcessorBit(const Scheduler::CpuId processor) noexcept
{
    return static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processor.GetValue());
}
} // Namespace

bool ProcessorAddressSpaceController::AdoptIdleRoot(AddressSpace& destination) noexcept
{
    if (m_state != ProcessorAddressSpaceControllerState::Ready || m_active != m_idle || m_previous != nullptr)
    {
        return false;
    }
    const auto switched = SwitchTo(destination);
    if (!switched.HasValue())
    {
        return false;
    }
    m_idle = &destination;
    m_idleIdentity = m_activeIdentity;
    return true;
}

ProcessorAddressSpaceControllerFailure ProcessorAddressSpaceController::MakeFailure(
    const ProcessorAddressSpaceControllerError error, const AddressSpaceActivationFailure& activation,
    const AddressSpaceError detach, const AddressSpaceError transition, const bool admissionRollbackSafe) noexcept
{
    return ProcessorAddressSpaceControllerFailure{
        .Error = error,
        .Activation = activation,
        .Transition = transition,
        .Detach = detach,
        .AdmissionRollbackSafe = admissionRollbackSafe,
        .Reserved = {},
    };
}

ProcessorAddressSpaceControllerReceipt
ProcessorAddressSpaceController::MakeReceipt(const AddressSpaceId previous,
                                             const bool nativeActivationPerformed) const noexcept
{
    return ProcessorAddressSpaceControllerReceipt{
        .Previous = previous,
        .Active = m_activeIdentity,
        .Processor = m_processor,
        .NativeActivationPerformed = nativeActivationPerformed,
        .Reserved = {},
    };
}

bool ProcessorAddressSpaceController::DestinationInvariantIsValid(
    const AddressSpace& destination, const AddressSpaceId identity,
    const AddressSpaceTransitionReceipt transition) const noexcept
{
    if (!m_processor.IsValid() || !identity.IsValid() || !transition.IsValid() || !transition.IsFor(destination) ||
        (transition.GetProcessor().GetValue() != m_processor.GetValue()))
    {
        return false;
    }
    const AddressSpaceSnapshot snapshot = destination.GetSnapshot();
    return (snapshot.State == AddressSpaceState::Ready) && snapshot.Identity.IsEqualTo(identity) &&
           destination.IsCurrentProcessorTransition(transition) &&
           ((snapshot.ResidentProcessorMask & ProcessorBit(m_processor)) != 0U);
}

bool ProcessorAddressSpaceController::ActiveInvariantIsValid(
    const AddressSpaceTransitionReceipt transition) const noexcept
{
    return (m_active != nullptr) && m_activeIdentity.IsValid() && (m_previous == nullptr) &&
           !m_previousIdentity.IsValid() && DestinationInvariantIsValid(*m_active, m_activeIdentity, transition);
}

void ProcessorAddressSpaceController::ClearPrevious() noexcept
{
    m_previous = nullptr;
    m_previousIdentity = {};
}

void ProcessorAddressSpaceController::EnterFaulted() noexcept
{
    m_state = ProcessorAddressSpaceControllerState::Faulted;
}

Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
ProcessorAddressSpaceController::Initialize(const Scheduler::CpuId processor, AddressSpace& idleAddressSpace) noexcept
{
    using ResultType =
        Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>;

    if (m_state != ProcessorAddressSpaceControllerState::Uninitialized)
    {
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::AlreadyInitialized));
    }
    if (!processor.IsValid())
    {
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::InvalidConfiguration,
                                               AddressSpaceActivationFailure{}, AddressSpaceError::None,
                                               AddressSpaceError::None, true));
    }

    m_processor = processor;
    const auto transition = idleAddressSpace.ReserveProcessorTransition(processor);
    if (!transition.HasValue())
    {
        m_processor = Scheduler::CpuId{};
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionRejected,
                                               AddressSpaceActivationFailure{}, AddressSpaceError::None,
                                               transition.GetError(), true));
    }

    m_destinationTransition = transition.GetValue();
    return ActivateInitialRoot(idleAddressSpace);
}

Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
ProcessorAddressSpaceController::ActivateInitialRoot(AddressSpace& idleAddressSpace) noexcept
{
    using ResultType =
        Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>;

    const auto activation = idleAddressSpace.ActivateReservedProcessor(m_destinationTransition);
    if (!activation.HasValue())
    {
        const AddressSpaceActivationFailure failure = activation.GetError();
        if (failure.ProcessorMayBeResident)
        {
            m_idle = &idleAddressSpace;
            m_active = &idleAddressSpace;
            m_idleIdentity = idleAddressSpace.GetIdentity();
            m_activeIdentity = m_idleIdentity;
            EnterFaulted();
        }
        else
        {
            const AddressSpaceError release = idleAddressSpace.ReleaseProcessorTransition(m_destinationTransition);
            m_destinationTransition = AddressSpaceTransitionReceipt{};
            if (release != AddressSpaceError::None)
            {
                m_idle = &idleAddressSpace;
                m_idleIdentity = idleAddressSpace.GetIdentity();
                EnterFaulted();
                return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionReleaseRejected,
                                                       failure, AddressSpaceError::None, release));
            }
            m_processor = Scheduler::CpuId{};
        }
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::ActivationRejected, failure,
                                               AddressSpaceError::None, AddressSpaceError::None,
                                               !failure.ProcessorMayBeResident));
    }

    const AddressSpaceId identity = idleAddressSpace.GetIdentity();
    m_idle = &idleAddressSpace;
    m_active = &idleAddressSpace;
    m_idleIdentity = identity;
    m_activeIdentity = identity;
    if (!DestinationInvariantIsValid(idleAddressSpace, identity, m_destinationTransition))
    {
        EnterFaulted();
        return ResultType::Failure(
            MakeFailure(ProcessorAddressSpaceControllerError::DestinationRootInvariantViolation));
    }

    const AddressSpaceError release = idleAddressSpace.ReleaseProcessorTransition(m_destinationTransition);
    m_destinationTransition = AddressSpaceTransitionReceipt{};
    if (release != AddressSpaceError::None)
    {
        EnterFaulted();
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionReleaseRejected,
                                               AddressSpaceActivationFailure{}, AddressSpaceError::None, release));
    }

    m_state = ProcessorAddressSpaceControllerState::Ready;
    return ResultType::Success(MakeReceipt(AddressSpaceId{}, true));
}

Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
ProcessorAddressSpaceController::SwitchTo(AddressSpace& destination) noexcept
{
    using ResultType =
        Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>;

    if (m_state == ProcessorAddressSpaceControllerState::Faulted)
    {
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::Faulted));
    }
    if (m_state != ProcessorAddressSpaceControllerState::Ready)
    {
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::NotReady,
                                               AddressSpaceActivationFailure{}, AddressSpaceError::None,
                                               AddressSpaceError::None, true));
    }
    if ((m_active == nullptr) || m_sourceTransition.IsValid() || m_destinationTransition.IsValid())
    {
        EnterFaulted();
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::ActiveRootInvariantViolation));
    }

    const auto sourceTransition = m_active->ReserveProcessorTransition(m_processor);
    if (!sourceTransition.HasValue())
    {
        const AddressSpaceError transitionError = sourceTransition.GetError();
        if ((transitionError == AddressSpaceError::ProcessorTransitionPending) ||
            (transitionError == AddressSpaceError::ActivationPending) ||
            (transitionError == AddressSpaceError::ShootdownPending))
        {
            return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionRejected,
                                                   AddressSpaceActivationFailure{}, AddressSpaceError::None,
                                                   transitionError, true));
        }

        EnterFaulted();
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::ActiveRootInvariantViolation,
                                               AddressSpaceActivationFailure{}, AddressSpaceError::None,
                                               transitionError));
    }
    m_sourceTransition = sourceTransition.GetValue();
    if (m_active->GetSnapshot().State == AddressSpaceState::ShootdownPending)
    {
        // Drain the actual mailbox work before leaving this
        // resident source. An early CR3 acknowledgement could otherwise complete a mapping before
        // its queued work arrives. No guard is held; the run loop's bounded idle-root retry
        // progresses this CPU's mailbox until the publisher completes the transaction.
        const auto released = m_active->ReleaseProcessorTransition(m_sourceTransition);
        m_sourceTransition = {};
        if (released != AddressSpaceError::None)
        {
            EnterFaulted();
            return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionReleaseRejected, {},
                                                   AddressSpaceError::None, released));
        }
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionRejected, {},
                                               AddressSpaceError::None, AddressSpaceError::ShootdownPending, true));
    }

    if (!ActiveInvariantIsValid(m_sourceTransition))
    {
        EnterFaulted();
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::ActiveRootInvariantViolation));
    }

    AddressSpace* const previousStorage = m_active;
    const AddressSpaceId previousIdentity = m_activeIdentity;
    if (&destination == previousStorage)
    {
        const AddressSpaceError release = previousStorage->ReleaseProcessorTransition(m_sourceTransition);
        m_sourceTransition = AddressSpaceTransitionReceipt{};
        ClearPrevious();
        if (release != AddressSpaceError::None)
        {
            EnterFaulted();
            return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionReleaseRejected,
                                                   AddressSpaceActivationFailure{}, AddressSpaceError::None, release));
        }
        return ResultType::Success(MakeReceipt(previousIdentity, false));
    }

    return ReserveDestinationAndSwitch(destination, *previousStorage, previousIdentity);
}

Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
ProcessorAddressSpaceController::ReserveDestinationAndSwitch(AddressSpace& destination, AddressSpace& previousStorage,
                                                             const AddressSpaceId previousIdentity) noexcept
{
    using ResultType =
        Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>;

    const auto destinationTransition = destination.ReserveProcessorTransition(m_processor);
    if (!destinationTransition.HasValue())
    {
        const AddressSpaceError transitionError = destinationTransition.GetError();
        const AddressSpaceError release = previousStorage.ReleaseProcessorTransition(m_sourceTransition);
        m_sourceTransition = AddressSpaceTransitionReceipt{};
        ClearPrevious();
        if (release != AddressSpaceError::None)
        {
            EnterFaulted();
            return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionReleaseRejected,
                                                   AddressSpaceActivationFailure{}, AddressSpaceError::None, release));
        }
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionRejected,
                                               AddressSpaceActivationFailure{}, AddressSpaceError::None,
                                               transitionError, true));
    }

    m_destinationTransition = destinationTransition.GetValue();
    m_previous = &previousStorage;
    m_previousIdentity = previousIdentity;
    return ActivateDestinationAndFinish(destination, previousStorage, previousIdentity);
}

Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
ProcessorAddressSpaceController::ActivateDestinationAndFinish(AddressSpace& destination, AddressSpace& previousStorage,
                                                              const AddressSpaceId previousIdentity) noexcept
{
    using ResultType =
        Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>;

    const auto activation = destination.ActivateReservedProcessor(m_destinationTransition);
    if (!activation.HasValue())
    {
        const AddressSpaceActivationFailure failure = activation.GetError();
        if (failure.ProcessorMayBeResident)
        {
            m_active = &destination;
            m_activeIdentity = destination.GetSnapshot().Identity;
            EnterFaulted();
        }
        else
        {
            const AddressSpaceError destinationRelease =
                destination.ReleaseProcessorTransition(m_destinationTransition);
            m_destinationTransition = AddressSpaceTransitionReceipt{};
            const AddressSpaceError sourceRelease = previousStorage.ReleaseProcessorTransition(m_sourceTransition);
            m_sourceTransition = AddressSpaceTransitionReceipt{};
            ClearPrevious();
            const AddressSpaceError firstReleaseError =
                (destinationRelease != AddressSpaceError::None) ? destinationRelease : sourceRelease;
            if (firstReleaseError != AddressSpaceError::None)
            {
                EnterFaulted();
                return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionReleaseRejected,
                                                       failure, AddressSpaceError::None, firstReleaseError));
            }
        }
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::ActivationRejected, failure,
                                               AddressSpaceError::None, AddressSpaceError::None,
                                               !failure.ProcessorMayBeResident));
    }

    m_active = &destination;
    m_activeIdentity = destination.GetSnapshot().Identity;
    if (!DestinationInvariantIsValid(destination, m_activeIdentity, m_destinationTransition))
    {
        EnterFaulted();
        return ResultType::Failure(
            MakeFailure(ProcessorAddressSpaceControllerError::DestinationRootInvariantViolation));
    }

    const AddressSpaceError detach = previousStorage.DetachReservedProcessor(m_sourceTransition);
    if (detach != AddressSpaceError::None)
    {
        EnterFaulted();
        return ResultType::Failure(
            MakeFailure(ProcessorAddressSpaceControllerError::DetachRejected, AddressSpaceActivationFailure{}, detach));
    }

    const ProcessorAddressSpaceControllerReceipt receipt = MakeReceipt(previousIdentity, true);
    const AddressSpaceError sourceRelease = previousStorage.ReleaseProcessorTransition(m_sourceTransition);
    m_sourceTransition = AddressSpaceTransitionReceipt{};
    ClearPrevious();
    const AddressSpaceError destinationRelease = destination.ReleaseProcessorTransition(m_destinationTransition);
    m_destinationTransition = AddressSpaceTransitionReceipt{};
    const AddressSpaceError firstReleaseError =
        (sourceRelease != AddressSpaceError::None) ? sourceRelease : destinationRelease;
    if (firstReleaseError != AddressSpaceError::None)
    {
        EnterFaulted();
        return ResultType::Failure(MakeFailure(ProcessorAddressSpaceControllerError::TransitionReleaseRejected,
                                               AddressSpaceActivationFailure{}, AddressSpaceError::None,
                                               firstReleaseError));
    }
    return ResultType::Success(receipt);
}

Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>
ProcessorAddressSpaceController::SwitchToIdle() noexcept
{
    using ResultType =
        Foundation::Result<ProcessorAddressSpaceControllerReceipt, ProcessorAddressSpaceControllerFailure>;

    if (m_idle == nullptr)
    {
        // As in SwitchTo, an uninitialized controller changed nothing.
        const bool faulted = m_state == ProcessorAddressSpaceControllerState::Faulted;
        return ResultType::Failure(MakeFailure(
            faulted ? ProcessorAddressSpaceControllerError::Faulted : ProcessorAddressSpaceControllerError::NotReady,
            AddressSpaceActivationFailure{}, AddressSpaceError::None, AddressSpaceError::None, !faulted));
    }
    return SwitchTo(*m_idle);
}
} // Namespace Asteria::Kernel::Memory
