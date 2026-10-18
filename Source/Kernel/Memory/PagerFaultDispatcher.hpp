// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_PAGER_FAULT_DISPATCHER_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_PAGER_FAULT_DISPATCHER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Memory/FaultToken.hpp"
#include "Kernel/Memory/PagerProtocol.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Scheduler/ProcessorScheduler.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Memory
{
enum class PagerFaultDispatcherState : Foundation::UInt8
{
    Idle,
    Delivered,
    Completed,
    Faulted,
};

enum class PagerFaultDispatcherError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidToken,
    TokenPinRejected,
    TokenNotPending,
    InvalidSchedulingContext,
    SchedulingContextRegistryMismatch,
    ThreadPinRejected,
    SchedulingContextPinRejected,
    CorrelatedStateChanged,
    InvalidSender,
    InvalidCapability,
    CapabilityLookupRejected,
    CapabilityObjectMismatch,
    ProtocolRejected,
    EndpointRejected,
    NotDelivered,
    AlreadyDelivered,
    InvalidResolution,
    QueueReservationRejected,
    TokenRejected,
    ThreadRejected,
    QueueCommitRejected,
    ReservationRollbackRejected,
    AlreadyCompleted,
    Faulted,
};

struct PagerFaultDispatcherFailure
{
    Foundation::UInt64 TransactionIdentifier;
    FaultTokenError Token;
    Object::ObjectPinError Pin;
    Object::CapabilityError Capability;
    PagerProtocolError Protocol;
    Ipc::EndpointError Endpoint;
    Scheduler::ReadyQueueFailure Queue;
    Scheduler::ThreadError Thread;
    Scheduler::ReadyQueueError QueueTransition;
    PagerFaultDispatcherError Error;
    Foundation::UInt8 Reserved;
};

struct PagerFaultDispatcherReceipt
{
    Scheduler::ThreadId Thread;
    Object::ObjectId FaultToken;
    Foundation::UInt64 TransactionIdentifier;
    Foundation::UInt64 EndpointSequence;
    Foundation::UInt64 QueueSequence;
    FaultResolutionKind Resolution;
    PagerFaultDispatcherState State;
    Foundation::UInt8 Reserved[6];
};

struct PagerFaultDispatcherSnapshot
{
    Scheduler::ThreadId Thread;
    Scheduler::SchedulingContextId SchedulingContext;
    Object::ObjectId FaultToken;
    Foundation::UInt64 TransactionIdentifier;
    Foundation::UInt64 EndpointSequence;
    FaultResolutionKind Resolution;
    PagerFaultDispatcherState State;
    Foundation::UInt8 Reserved[6];
};

struct PagerFaultDeliveryRequest
{
    Object::CapabilitySpace* SourceSpace;
    Object::ObjectId FaultToken;
    Scheduler::SchedulingContextId SchedulingContext;
    Scheduler::ThreadBlockToken BlockToken;
    Object::ResourceDomainId Sender;
    Object::CapabilityHandle FaultTokenCapability;
    Ipc::TraceContext Trace;
    Foundation::UInt64 TransactionIdentifier;
    Object::MonotonicTime Deadline;
    /// The faulting domain's supervision tag.
    Foundation::UInt64 SupervisionTag;
    // Registry slot and generation, assigned by the kernel.
    Foundation::UInt64 ProtectionDomainIdentity{0U};
};

struct PagerFaultCompletionRequest
{
    FaultResolutionClaimToken Claim;
    Foundation::UInt64 ExitCode;
    FaultResolutionKind Resolution;
    Foundation::UInt8 Reserved[7];
};

/// Joins one already-blocked Thread fault to reserved Endpoint delivery and the terminal
/// wake-or-terminate transition. The token factory and processor exception path are separate
/// owners. Before Deliver, they register and mint one Pending FaultToken, finish the exact active
/// Thread as Blocked, and transfer exclusive lifecycle access to this dispatcher. A successful
/// Deliver moves the Invoke-only token authority into the pager Endpoint. Complete is called only
/// after an authorized resolver has finished the Map, Redirect, or Terminate transaction while
/// retaining the exact claim. The registry, endpoint, wake facade's scheduler, capability spaces,
/// token storage, Thread, and SchedulingContext outlive the active dispatcher transaction.
/// Operations may spin, are forbidden in interrupt context, and require same-CPU reentry and direct
/// lifecycle operations to be excluded.
class PagerFaultDispatcher final
{
public:
    /// Preconditions:
    /// - objectRegistry is the registry used by pagerEndpoint's receiver
    /// space, source spaces used by Deliver, wakeFacade, and every request;
    /// - objectRegistry, pagerEndpoint, and wakeFacade's scheduler outlive this
    /// object;
    /// - wakeFacade and all referenced objects remain alive and immovable for
    /// this lifetime.
    constexpr PagerFaultDispatcher(Object::ObjectRegistry& objectRegistry, Ipc::Endpoint& pagerEndpoint,
                                   Scheduler::ProcessorSchedulerWakeFacade& wakeFacade) noexcept
        : m_objectRegistry{objectRegistry}, m_pagerEndpoint{pagerEndpoint}, m_wakeFacade{wakeFacade}
    {
    }

    /// Preconditions:
    /// - State is Idle or Completed;
    /// - no concurrent operation accesses this object.
    ~PagerFaultDispatcher() noexcept = default;

    PagerFaultDispatcher(const PagerFaultDispatcher&) = delete;
    PagerFaultDispatcher& operator=(const PagerFaultDispatcher&) = delete;
    PagerFaultDispatcher(PagerFaultDispatcher&&) = delete;
    PagerFaultDispatcher& operator=(PagerFaultDispatcher&&) = delete;

    /// Publishes one canonical fault notification through ReservedControl.
    /// A publication-phase failure from Idle leaves this dispatcher Idle,
    /// keeps the FaultToken Pending, leaves the source capability in place,
    /// and changes no Endpoint entry. State/replay failures preserve their
    /// already-published state.
    [[nodiscard]] Foundation::Result<PagerFaultDispatcherReceipt, PagerFaultDispatcherFailure>
    Deliver(const PagerFaultDeliveryRequest& request) noexcept;

    /// Commits the exact active claim. Map and Redirect wake and publish the
    /// retained Thread through wakeFacade's scheduler-owned queue. Terminate
    /// exits it without enqueue. Before FaultToken commitment, failures remain
    /// retryable and preserve the Delivered state. Any failure after
    /// commitment is terminal Faulted.
    [[nodiscard]] Foundation::Result<PagerFaultDispatcherReceipt, PagerFaultDispatcherFailure>
    Complete(const PagerFaultCompletionRequest& request) noexcept;

    /// Ends a Completed transaction so the next fault can Deliver. Completion
    /// already released every pin; this clears the retained message and
    /// identities. Any other state is left unchanged and reports
    /// NotDelivered.
    [[nodiscard]] PagerFaultDispatcherError Reset() noexcept;

    [[nodiscard]] bool IsDelivered() const noexcept;
    [[nodiscard]] PagerFaultDispatcherSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] static PagerFaultDispatcherFailure
    MakeFailure(PagerFaultDispatcherError error, Foundation::UInt64 transactionIdentifier = 0U,
                FaultTokenError token = FaultTokenError::None, Object::ObjectPinError pin = Object::ObjectPinError::Ok,
                Object::CapabilityError capability = Object::CapabilityError::Ok,
                PagerProtocolError protocol = PagerProtocolError::None, const Ipc::EndpointError& endpoint = {},
                const Scheduler::ReadyQueueFailure& queue = {},
                Scheduler::ThreadError thread = Scheduler::ThreadError::None,
                Scheduler::ReadyQueueError queueTransition = Scheduler::ReadyQueueError::None) noexcept;
    [[nodiscard]] static bool ContextCanResume(Scheduler::SchedulingContextState state) noexcept;
    [[nodiscard]] bool ValidateAndPinDeliveryLocked(const PagerFaultDeliveryRequest& request,
                                                    PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool SourceCapabilityIsExactLocked(const PagerFaultDeliveryRequest& request,
                                                     PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool BuildDeliveryMessageLocked(const PagerFaultDeliveryRequest& request,
                                                  PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool PublishDeliveryLocked(const PagerFaultDeliveryRequest& request,
                                             PagerFaultDispatcherReceipt& receipt,
                                             PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool ValidateCompletionLocked(const PagerFaultCompletionRequest& request,
                                                PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool CompleteTerminationLocked(const PagerFaultCompletionRequest& request,
                                                 PagerFaultDispatcherReceipt& receipt,
                                                 PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool ReserveWakeLocked(Scheduler::ReadyQueueEnqueueReservation& reservation,
                                         PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool CommitWakeLocked(const PagerFaultCompletionRequest& request,
                                        Scheduler::ReadyQueueEnqueueReservation reservation,
                                        PagerFaultDispatcherReceipt& receipt,
                                        PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool CorrelatedStateIsValidLocked() const noexcept;
    [[nodiscard]] bool CancelReservationLocked(Scheduler::ReadyQueueEnqueueReservation reservation,
                                               PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] PagerFaultDispatcherReceipt MakeReceiptLocked(Foundation::UInt64 queueSequence,
                                                                FaultResolutionKind resolution,
                                                                PagerFaultDispatcherState state) const noexcept;
    void ClearMessageLocked() noexcept;
    void ResetDeliveryLocked() noexcept;
    void ReleaseTerminalPinsLocked() noexcept;
    void EnterFaultedLocked() noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Ipc::Endpoint& m_pagerEndpoint;
    Scheduler::ProcessorSchedulerWakeFacade& m_wakeFacade;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Object::ObjectPin m_tokenPin;
    Object::ObjectPin m_threadPin;
    Object::ObjectPin m_contextPin;
    FaultToken* m_token{nullptr};
    Scheduler::Thread* m_thread{nullptr};
    Scheduler::SchedulingContext* m_context{nullptr};
    Scheduler::ThreadId m_threadIdentity{};
    Scheduler::SchedulingContextId m_contextIdentity{};
    Scheduler::ThreadBlockToken m_blockToken{};
    Object::ObjectId m_tokenIdentity{};
    FaultRecord m_fault{};
    Foundation::UInt64 m_transactionIdentifier{0U};
    Foundation::UInt64 m_endpointSequence{0U};
    FaultResolutionKind m_resolution{FaultResolutionKind::None};
    PagerFaultDispatcherState m_state{PagerFaultDispatcherState::Idle};
    Ipc::MessageEnvelope m_message{};
};

static_assert(sizeof(PagerFaultDispatcherState) == 1U);
static_assert(sizeof(PagerFaultDispatcherError) == 1U);
static_assert(sizeof(PagerFaultDispatcherFailure) == 24U);
static_assert(alignof(PagerFaultDispatcherFailure) == 8U);
static_assert(sizeof(PagerFaultDispatcherReceipt) == 56U);
static_assert(alignof(PagerFaultDispatcherReceipt) == 8U);
static_assert(sizeof(PagerFaultDispatcherSnapshot) == 64U);
static_assert(alignof(PagerFaultDispatcherSnapshot) == 8U);
static_assert(sizeof(PagerFaultDeliveryRequest) == 128U);
static_assert(alignof(PagerFaultDeliveryRequest) == 8U);
static_assert(sizeof(PagerFaultCompletionRequest) == 32U);
static_assert(alignof(PagerFaultCompletionRequest) == 8U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_PAGER_FAULT_DISPATCHER_HPP_INCLUDED
