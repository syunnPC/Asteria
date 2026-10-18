// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_PAGE_FAULT_INGRESS_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_PAGE_FAULT_INGRESS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/IPagerResolution.hpp"
#include "Kernel/Memory/FaultTokenFactory.hpp"
#include "Kernel/Memory/PagerFaultDispatcher.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Memory
{
enum class PageFaultIngressState : Foundation::UInt8
{
    Idle,
    Prepared,
    BlockedPendingToken,
    TokenReady,
    Delivered,
    Faulted,
};

enum class PageFaultIngressError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyPrepared,
    ThreadPinRejected,
    AddressSpacePinRejected,
    SchedulingContextPinRejected,
    CorrelatedStateChanged,
    InvalidBlockToken,
    InvalidState,
    FactoryRejected,
    DeliveryRejected,
    RollbackRejected,
    Faulted,
};

struct PageFaultIngressRequest
{
    Object::CapabilitySpace* SourceSpace;
    Scheduler::ThreadId Thread;
    AddressSpaceId AddressSpace;
    Scheduler::SchedulingContextId SchedulingContext;
    FaultRecord Fault;
    Ipc::TraceContext Trace;
    Object::ResourceDomainId ChargeOwner;
    Object::ResourceDomainId Sender;
    Object::RevocationDomainId RevocationDomain;
    Object::MonotonicTime Deadline;
    Foundation::UInt64 TransactionIdentifier;
    /// The faulting domain's supervision tag.
    Foundation::UInt64 SupervisionTag;
    // Registry slot and generation, assigned by the kernel.
    Foundation::UInt64 ProtectionDomainIdentity{0U};
    IPagerResolution* Resolution{nullptr};
    Object::ObjectId ChannelIdentity{};
};

struct PageFaultIngressSnapshot
{
    Scheduler::ThreadId Thread;
    AddressSpaceId AddressSpace;
    Scheduler::SchedulingContextId SchedulingContext;
    Object::ObjectId FaultToken;
    Foundation::UInt64 TransactionIdentifier;
    Foundation::UInt64 EndpointSequence;
    PageFaultIngressState State;
    PageFaultIngressError Error;
    FaultTokenFactoryError Factory;
    FaultTokenError Token;
    PagerFaultDispatcherError Dispatcher;
    Ipc::EndpointErrorCode Endpoint;
    PagerProtocolError Protocol;
    Object::ObjectPinError Pin;
};

/// Owns the bounded transition from one running user fault to one blocked pager transaction.
/// Prepare pins and correlates the running Thread, SchedulingContext, and AddressSpace without
/// allocating or changing their lifecycle. CommitBlocked is called after the scheduler has
/// irreversibly completed the exact Thread as Blocked and before its native context is switched
/// away. Token or Endpoint pressure leaves a retryable blocked transaction; it never resumes the
/// faulting Thread without a pager resolution. The caller serializes direct lifecycle changes from
/// Prepare through Delivered or terminal rollback. A permanent pre-publication failure closes any
/// source capability created by this transaction, retires its token, and releases the ingress pins;
/// the higher owner then collects the token and resolves the still-blocked Thread. Operations may
/// spin and run only from the synchronous post-capture kernel-fault path, never a hard ISR.
class PageFaultIngress final
{
public:
    /// Preconditions:
    /// - objectRegistry is the registry used by factory and dispatcher;
    /// - factory, dispatcher, their collaborators, and every request object
    /// outlive this transaction;
    /// - dispatcher is Idle and is dedicated to this transaction.
    constexpr PageFaultIngress(Object::ObjectRegistry& objectRegistry, FaultTokenFactory& factory,
                               PagerFaultDispatcher& dispatcher) noexcept
        : m_objectRegistry{objectRegistry}, m_factory{factory}, m_dispatcher{dispatcher}
    {
    }

    /// Preconditions:
    /// - State is Idle, Delivered, or terminal Faulted with Error other than
    /// RollbackRejected;
    /// - no concurrent operation accesses this object;
    /// - a Delivered token's external retirement remains owned by the caller.
    ~PageFaultIngress() noexcept = default;

    PageFaultIngress(const PageFaultIngress&) = delete;
    PageFaultIngress& operator=(const PageFaultIngress&) = delete;
    PageFaultIngress(PageFaultIngress&&) = delete;
    PageFaultIngress& operator=(PageFaultIngress&&) = delete;

    /// Correlates one currently Running Thread and SchedulingContext. Every
    /// failure leaves State Idle, acquires no lasting pin, and may be retried.
    [[nodiscard]] PageFaultIngressError Prepare(const PageFaultIngressRequest& request) noexcept;

    /// Records the scheduler-produced block generation and attempts token
    /// creation plus pager publication. A retryable failure is retained in
    /// BlockedPendingToken or TokenReady and is visible through GetSnapshot.
    void CommitBlocked(Scheduler::ThreadBlockToken blockToken) noexcept;

    /// Retries the retained token-creation or delivery phase. This never
    /// changes the Thread lifecycle and is inert outside a pending state.
    void Retry() noexcept;

    /// Ends a Delivered transaction so the next fault can Prepare. Delivery
    /// already released every ingress pin; the delivered token and the
    /// blocked Thread now belong to the pager transaction, whose owner
    /// retires the token. Any other state is left unchanged and reports
    /// InvalidState.
    [[nodiscard]] PageFaultIngressError Reset() noexcept;

    [[nodiscard]] PageFaultIngressSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] static bool FaultRecordIsValid(const FaultRecord& fault) noexcept;
    [[nodiscard]] static bool TraceIsValid(const Ipc::TraceContext& trace) noexcept;
    [[nodiscard]] static bool ContextCanResume(Scheduler::SchedulingContextState state) noexcept;
    [[nodiscard]] static bool FactoryFailureIsRetryable(const FaultTokenFactoryFailure& failure) noexcept;
    [[nodiscard]] static bool DeliveryFailureIsRetryable(const PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool RunningStateIsCorrelatedLocked() const noexcept;
    [[nodiscard]] bool BlockedStateIsCorrelatedLocked() const noexcept;
    void ProgressLocked() noexcept;
    void SetPrepareFailureLocked(PageFaultIngressError error,
                                 Object::ObjectPinError pin = Object::ObjectPinError::Ok) noexcept;
    void SetFactoryFailureLocked(const FaultTokenFactoryFailure& failure) noexcept;
    void SetDeliveryFailureLocked(const PagerFaultDispatcherFailure& failure) noexcept;
    [[nodiscard]] bool RollbackCreationLocked() noexcept;
    void ClearFailureLocked() noexcept;
    void ReleaseIngressPinsLocked() noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    FaultTokenFactory& m_factory;
    PagerFaultDispatcher& m_dispatcher;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Object::ObjectPin m_threadPin;
    Object::ObjectPin m_addressSpacePin;
    Object::ObjectPin m_contextPin;
    Scheduler::Thread* m_thread{nullptr};
    Scheduler::SchedulingContext* m_context{nullptr};
    PageFaultIngressRequest m_request{};
    FaultTokenCreation m_creation{};
    Scheduler::ThreadBlockToken m_blockToken{};
    Foundation::UInt64 m_endpointSequence{0U};
    PageFaultIngressState m_state{PageFaultIngressState::Idle};
    PageFaultIngressError m_error{PageFaultIngressError::None};
    FaultTokenFactoryError m_factoryError{FaultTokenFactoryError::None};
    FaultTokenError m_tokenError{FaultTokenError::None};
    PagerFaultDispatcherError m_dispatcherError{PagerFaultDispatcherError::None};
    Ipc::EndpointErrorCode m_endpointError{Ipc::EndpointErrorCode::None};
    PagerProtocolError m_protocolError{PagerProtocolError::None};
    Object::ObjectPinError m_pinError{Object::ObjectPinError::Ok};
};

static_assert(sizeof(PageFaultIngressState) == 1U);
static_assert(sizeof(PageFaultIngressError) == 1U);
static_assert(sizeof(PageFaultIngressRequest) == 184U);
static_assert(alignof(PageFaultIngressRequest) == 8U);
static_assert(sizeof(PageFaultIngressSnapshot) == 80U);
static_assert(alignof(PageFaultIngressSnapshot) == 8U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_PAGE_FAULT_INGRESS_HPP_INCLUDED
