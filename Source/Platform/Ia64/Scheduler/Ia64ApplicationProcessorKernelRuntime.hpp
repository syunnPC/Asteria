// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_APPLICATION_PROCESSOR_KERNEL_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_APPLICATION_PROCESSOR_KERNEL_RUNTIME_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Platform/Ia64/Diagnostics/Ia64StopStage.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Memory/TlbShootdownDrain.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Scheduler/ProcessorRunController.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/Scheduler/Thread.hpp"
#include "Kernel/Time/DeferredTimerWorker.hpp"
#include "Kernel/Time/Timer.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorRuntimeReleaseCoordinator.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeLayout.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Memory/Ia64LocalTlbInvalidator.hpp"
#include "Platform/Ia64/Memory/Ia64PolledTlbShootdownNotifier.hpp"
#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"
#include "Platform/Ia64/Scheduler/Ia64ProcessorRunLoop.hpp"
#include "Platform/Ia64/Time/Ia64LocalTimer.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Ia64::Scheduler
{
enum class Ia64ApplicationProcessorKernelRuntimeState : Foundation::UInt8
{
    Fresh,
    Failed,
    Ready,
};

enum class Ia64ApplicationProcessorKernelRuntimeProcessorState : Foundation::UInt8
{
    Unused,
    AwaitingRelease,
    PhysicalClaimed,
    RuntimeEntered,
    SchedulerWaiting,
    IdleServing,
    Failed,
};

enum class Ia64ApplicationProcessorLocalTimerProofPhase : Foundation::UInt8
{
    Unconfigured,
    Configured,
    Armed,
    Completed,
    Failed,
};

enum class Ia64ApplicationProcessorPortableSchedulerPhase : Foundation::UInt8
{
    Unconfigured,
    Prepared,
    Starting,
    Waiting,
    Online,
    Failed,
};

enum class Ia64ApplicationProcessorPortableSchedulerServiceState : Foundation::UInt8
{
    Idle,
    Publishing,
    Pending,
    Executing,
    Failed,
};

enum class Ia64ApplicationProcessorPortableSchedulerServiceResult : Foundation::UInt8
{
    None,
    Completed,
};

enum class Ia64ApplicationProcessorTimerDeliveryState : Foundation::UInt8
{
    Ready,
    Publishing,
    Pending,
    Executing,
    Completed,
    Failed,
};

enum class Ia64ApplicationProcessorTimerDeliveryResult : Foundation::UInt8
{
    None,
    Delivered,
};

enum class Ia64ApplicationProcessorKernelRuntimeError : Foundation::UInt32
{
    None,
    AlreadyPrepared,
    InvalidAddressProfile,
    InvalidLayout,
    ReleaseCoordinatorRejected,
    WorkerContextRejected,
    RuntimeNotReady,
    InvalidProcessor,
    InvalidGeneration,
    NonzeroReserved,
    InvalidProcessorState,
    InvalidRegisterEvidence,
    ReleaseClaimRejected,
    RuntimeEnteredPublicationRejected,
    SchedulerWaitingPublicationRejected,
    IdleServingPublicationRejected,
    InvalidPhysicalFailure,
    PhysicalRootAdmissionRejected,
    RuntimePivotRejected,
    TranslatedContinuationReturned,
    InvalidWorkerArgument,
    LocalTimerProofContextPublicationRejected,
    LocalTimerProofContextAccessRejected,
    InvalidLocalTimerProofEvidence,
    InvalidLocalTimerProofState,
    LocalTimerInitializationRejected,
    LocalTimerTaskPriorityRejected,
    LocalTimerCounterUnavailable,
    LocalTimerDeadlineOverflow,
    LocalTimerProgramRejected,
    LocalTimerPollRejected,
    LocalTimerEvidenceRejected,
    LocalTimerCancelRejected,
    LocalTimerPostconditionRejected,
    LocalTimerProofNotCompleted,
    PortableSchedulerAlreadyConfigured,
    PortableSchedulerDependenciesRejected,
    PortableSchedulerNotConfigured,
    PortableSchedulerRootBorrowRejected,
    PortableSchedulerRootRejected,
    PortableSchedulerProtectionDomainInitializationRejected,
    PortableSchedulerProtectionDomainRegistrationRejected,
    PortableSchedulerProtectionDomainIdentityRejected,
    PortableSchedulerThreadInitializationRejected,
    PortableSchedulerThreadRegistrationRejected,
    PortableSchedulerSchedulingContextInitializationRejected,
    PortableSchedulerSchedulingContextRegistrationRejected,
    PortableSchedulerSchedulingContextBindingRejected,
    PortableSchedulerRunLoopBindingRejected,
    PortableSchedulerThreadReadyRejected,
    PortableSchedulerEnqueueRejected,
    PortableSchedulerDispatchRejected,
    PortableSchedulerActiveBindingRejected,
    PortableSchedulerFinishRejected,
    PortableSchedulerServiceUnavailable,
    PortableSchedulerServiceAlreadyPending,
    PortableSchedulerServiceSequenceExhausted,
    PortableSchedulerServiceWakeReservationRejected,
    PortableSchedulerServiceWakeRejected,
    PortableSchedulerServiceWakeCommitRejected,
    PortableSchedulerServiceCompletionRejected,
    PortableSchedulerPostconditionRejected,
    PortableSchedulerObjectCapacityRejected,
    PortableSchedulerTimerInitializationRejected,
    PortableSchedulerTimerRegistrationRejected,
    PortableSchedulerNotificationInitializationRejected,
    PortableSchedulerNotificationRegistrationRejected,
    PortableSchedulerDeferredTimerBindingRejected,
    TimerDeliveryUnavailable,
    TimerDeliveryAlreadyRequested,
    TimerDeliveryGateConflict,
    TimerDeliverySequenceExhausted,
    TimerDeliveryEvidenceRejected,
    TimerDeliveryCounterUnavailable,
    TimerDeliveryDeadlineOverflow,
    TimerDeliveryArmRejected,
    TimerDeliveryDeadlineProgramRejected,
    TimerDeliveryPollRejected,
    TimerDeliveryDrainRejected,
    TimerDeliveryNotificationClaimRejected,
    TimerDeliveryNotificationCommitRejected,
    TimerDeliveryDeadlineReconcileRejected,
    TimerDeliveryPostconditionRejected,
    TlbShootdownAlreadyConfigured,
    TlbShootdownDependenciesRejected,
    TlbShootdownNotConfigured,
    TlbShootdownLocalInvalidatorInitializationRejected,
    TlbShootdownServiceUnavailable,
    TlbShootdownFixedRootRejected,
    TlbShootdownPollRejected,
    TlbShootdownDrainRejected,
};

enum class Ia64ApplicationProcessorKernelRuntimeOperation : Foundation::UInt8
{
    None,
    Prepare,
    ClaimPhysicalRelease,
    PublishPhysicalFailure,
    EnterTranslatedIdle,
    PublishWorkerEntry,
    PublishSchedulerWaiting,
    RequestPortableSchedulerService,
    ServicePortableScheduler,
    PublishIdleServing,
    WorkerAdapter,
    ConfigureLocalTimerInterruptProof,
    BeginLocalTimerInterruptProof,
    CompleteLocalTimerInterruptProof,
    ConfigurePortableSchedulerDependencies,
    PreparePortableScheduler,
    DispatchPortableScheduler,
    FinishPortableScheduler,
    RequestTimerDelivery,
    BeginTimerDelivery,
    CompleteTimerDelivery,
    ConfigureTlbShootdownDependencies,
    PrepareTlbShootdown,
    ServiceTlbShootdown,
};

enum class Ia64ApplicationProcessorKernelRuntimePhysicalFailure : Foundation::UInt8
{
    None,
    RootAdmissionRejected,
    PivotRejected,
    TranslatedContinuationReturned,
    LocalTimerProofContextPublicationRejected,
    LocalTimerProofContextAccessRejected,
};

enum class Ia64ApplicationProcessorKernelRuntimeAccessError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidProcessor,
    PublicationPending,
    PublicationSequenceExhausted,
};

enum class Ia64ApplicationProcessorKernelRuntimeFailurePoll : Foundation::UInt8
{
    None,
    PublicationPending,
    Available,
};

struct Ia64ApplicationProcessorKernelRuntimeRegisterEvidence
{
    Foundation::UInt64 StackPointer;
    /// Direct sample of ar.bsp.
    Foundation::UInt64 RseBackingStorePointer;
    /// Direct sample of ar.bspstore.
    Foundation::UInt64 RseBackingStoreBase;
    Foundation::UInt64 RseNatCollection;
    Foundation::UInt64 RseConfiguration;
    Foundation::UInt64 ProcessorStatus;
    Foundation::UInt8 CaptureComplete;
    Foundation::UInt8 Reserved[7U];
};

struct Ia64ApplicationProcessorKernelRuntimeFailure
{
    Foundation::UInt64 Generation;
    Foundation::UInt64 Detail;
    Ia64ApplicationProcessorKernelRuntimeError Error;
    Ia64ApplicationProcessorKernelRuntimeOperation Operation;
    Ia64ApplicationProcessorKernelRuntimeProcessorState ObservedState;
    Interrupt::Ia64ApplicationProcessorRuntimeTransitionError ReleaseError;
    Ia64KernelContextError ContextError;
    Foundation::UInt8 ProcessorValue;
    Foundation::UInt8 Reserved[7U];
};

struct Ia64ApplicationProcessorKernelRuntimeSummary
{
    Foundation::UInt64 WorkerEntryPoint;
    Foundation::UInt64 WorkerEntryGlobalPointer;
    Foundation::UInt8 ProcessorCount;
    Foundation::UInt8 EnabledProcessorMask;
    Foundation::UInt8 PreparedContextCount;
    Foundation::UInt8 CommonPhysicalAddressBits;
    Foundation::UInt8 CommonImplementedVirtualAddressMostSignificantBit;
    Ia64ApplicationProcessorKernelRuntimeState State;
    Foundation::UInt8 Reserved[2U];
};

struct Ia64ApplicationProcessorPortableSchedulerServiceRequest
{
    Foundation::UInt64 Sequence;
    Foundation::UInt64 Cookie;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 Reserved[7U];
};

/// Independently atomic progress for the reusable BSP-to-AP service mailbox.
/// CompletionSequence is the release publication for the corresponding
/// completion cookie, result, evidence value, and run sequence.
struct Ia64ApplicationProcessorPortableSchedulerServiceSnapshot
{
    Foundation::UInt64 RequestSequence;
    Foundation::UInt64 CompletionSequence;
    Foundation::UInt64 RequestCookie;
    Foundation::UInt64 CompletionCookie;
    Foundation::UInt64 CompletionEvidenceValue;
    Foundation::UInt64 CompletionRunSequence;
    Foundation::UInt64 DispatchCount;
    Foundation::UInt64 WakeCount;
    Foundation::UInt64 ServiceCount;
    Ia64ApplicationProcessorPortableSchedulerServiceState State;
    Ia64ApplicationProcessorPortableSchedulerServiceResult Result;
    Foundation::UInt8 Reserved[6U];
};

struct Ia64ApplicationProcessorTimerDeliveryRequest
{
    Foundation::UInt64 Sequence;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 Reserved[7U];
};

/// Independently atomic progress for the one-shot BSP-to-AP timer-delivery
/// gate. CompletionSequence is the release publication for Record.
struct Ia64ApplicationProcessorTimerDeliverySnapshot
{
    Foundation::UInt64 RequestSequence;
    Foundation::UInt64 CompletionSequence;
    Ia64ApplicationProcessorTimerDeliveryState State;
    Ia64ApplicationProcessorTimerDeliveryResult Result;
    Foundation::UInt8 Reserved[6U];
};

/// Acquire-published only after the AP has completed its bounded local-timer
/// proof. This is passive diagnostic evidence: the runtime does not
/// acknowledge sequence one. The BSP validates and acknowledges it before
/// publishing the distinct gate that permits AP-local sequence-two reuse.
struct Ia64ApplicationProcessorLocalTimerProofRecord
{
    Interrupt::Ia64LocalTimerInterruptEvidence InterruptEvidence;
    Time::Ia64LocalTimerSnapshot FinalTimerSnapshot;
    Interrupt::Ia64ExternalInterruptPollCodeRange PollCodeRange;
    Foundation::UInt64 EvidenceAddress;
    Foundation::UInt64 ProgrammedDeadlineTicks;
    Foundation::UInt64 PollDeadlineTicks;
    Foundation::UInt64 TaskPriority;
    Interrupt::Ia64ExternalInterruptPollStatus PollStatus;
    Interrupt::Ia64LocalTimerInterruptEvidenceValidationError ValidationResult;
    Ia64ApplicationProcessorLocalTimerProofPhase Phase;
    Foundation::UInt8 Vector;
    Foundation::UInt8 Reserved[5U];
};

/// Immutable acquire-published evidence for the first complete cooperative
/// block, wake, resume, and re-block cycle. Changing service progress is kept
/// in a separate all-atomic snapshot.
struct Ia64ApplicationProcessorPortableSchedulerRecord
{
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
    Kernel::Protection::ProtectionDomainId ProtectionDomainIdentity;
    Kernel::Scheduler::ThreadId ThreadIdentity;
    Kernel::Scheduler::SchedulingContextId SchedulingContextIdentity;
    Foundation::UInt64 InitialRunSequence;
    Foundation::UInt64 FirstResumedRunSequence;
    Foundation::UInt64 InitialBlockToken;
    Foundation::UInt64 ServingBlockToken;
    Kernel::Scheduler::CpuId Processor;
    Ia64ApplicationProcessorPortableSchedulerPhase Phase;
    Kernel::Scheduler::ThreadState ThreadState;
    Kernel::Scheduler::SchedulingContextState SchedulingContextState;
    Kernel::Scheduler::ProcessorRunState ControllerState;
    Kernel::Scheduler::ProcessorKernelRunLoopState RunLoopState;
    Kernel::Time::ProcessorDeadlineState DeadlineState;
    Time::Ia64LocalTimerState LocalTimerState;
    Foundation::UInt8 TimerQueueScheduledEntries;
    Foundation::UInt8 TimerQueuePendingEntries;
    Foundation::UInt8 TimerQueueClaimedEntries;
    Foundation::UInt8 RunLoopBindingCount;
    Foundation::UInt8 Reserved[3U];
};

/// Bounded best-effort settlement evidence retained when a timer-delivery
/// transaction fails after its TimerQueue arm succeeded. Cleanup errors are
/// deliberately separate from the causal failure for the rejected operation.
struct Ia64ApplicationProcessorTimerDeliverySettlement
{
    Kernel::Time::TimerQueueState FinalQueue;
    Kernel::Time::TimerSnapshot FinalTimer;
    Kernel::Time::ProcessorDeadlineReceipt ControllerRefresh;
    Time::Ia64LocalTimerSnapshot FinalLocalTimer;
    Kernel::Time::TimerQueueError QueueCancelError;
    Kernel::Scheduler::ProcessorRunError ControllerRefreshError;
    Hal::Time::LocalTimerError LocalTimerCancelError;
    Foundation::UInt8 QueueCancelAttempted;
    Foundation::UInt8 ControllerRefreshAttempted;
    Foundation::UInt8 LocalTimerCancelAttempted;
    Foundation::UInt8 QueueEmptyProven;
    Foundation::UInt8 TimerDisarmedProven;
    Foundation::UInt8 ControllerDisarmedProven;
    Foundation::UInt8 LocalTimerDisarmedProven;
    Foundation::UInt8 Reserved[6U];
};

/// Complete retained diagnostics for one AP's portable scheduler attempt.
/// Readers receive a stabilized value copy from an atomic word publication;
/// they never borrow the AP-owned mutable tree.
struct Ia64ApplicationProcessorPortableSchedulerFailure
{
    Kernel::Scheduler::ProcessorRunFailure Controller;
    Kernel::Scheduler::ProcessorKernelRunLoopFailure RunLoop;
    Kernel::Scheduler::ProcessorActiveExecutionBindingFailure ActiveBinding;
    Kernel::Protection::ProtectionDomainFailure ProtectionDomain;
    Kernel::Object::ObjectRegistryError ObjectRegistry;
    Kernel::Object::ObjectPinError ObjectPin;
    Kernel::Scheduler::ThreadError Thread;
    Kernel::Scheduler::SchedulingContextError SchedulingContext;
    Kernel::Scheduler::ProcessorKernelRunLoopError RunLoopBinding;
    Kernel::Scheduler::ReadyQueueFailure WakeQueue;
    Kernel::Scheduler::ReadyQueueError WakeCancellation;
    Kernel::Time::TimerQueueFailure TimerQueue;
    Kernel::Time::DeferredTimerWorkerFailure DeferredTimerWorker;
    Kernel::Ipc::NotificationFailure Notification;
    Kernel::Time::TimerError Timer;
    Ia64ApplicationProcessorTimerDeliverySettlement TimerDeliverySettlement;
    Kernel::Memory::ProcessorAddressSpaceControllerSnapshot TlbShootdownController;
    Kernel::Memory::ProcessorAddressSpaceControllerSnapshot TlbShootdownControllerAfter;
    Kernel::Memory::AddressSpaceSnapshot TlbShootdownAddressSpaceBefore;
    Kernel::Memory::AddressSpaceSnapshot TlbShootdownAddressSpaceAfter;
    Kernel::Memory::TlbShootdownDrainSummary TlbShootdownDrain;
    Memory::Ia64BspRuntimeApplicationProcessorAdmissionError RootBorrow;
    Memory::Ia64LocalTlbInvalidatorInitializationError TlbShootdownInvalidatorInitializationError;
    Memory::Ia64PolledTlbShootdownPollResult TlbShootdownPollResult;
    Foundation::UInt8 TlbShootdownPollAttempted;
    Foundation::UInt8 Reserved[6U];
};

/// Immutable acquire-published evidence for the AP-local sequence-two timer
/// transaction. Every nested value is the exact portable or IA-64 receipt or
/// snapshot produced at the named boundary. Storage addresses are retained
/// diagnostics for the runtime's boot-lifetime objects; identity correlation
/// remains the caller's responsibility through the registry-backed records.
struct Ia64ApplicationProcessorTimerDeliveryRecord
{
    Kernel::Time::DeferredTimerRegistrationReceipt Registration;
    Kernel::Time::TimerQueueArmReceipt Arm;
    Kernel::Time::TimerQueueState ArmedQueue;
    Kernel::Time::ProcessorDeadlineReceipt ArmedDeadline;
    Time::Ia64LocalTimerSnapshot ArmedLocalTimer;
    Interrupt::Ia64LocalTimerInterruptEvidence InterruptEvidence;
    Interrupt::Ia64ExternalInterruptPollCodeRange PollCodeRange;
    Kernel::Time::DeferredTimerDrainReceipt Drain;
    Kernel::Time::TimerNotificationDeliveryReceipt Delivery;
    Kernel::Ipc::NotificationClaimToken NotificationClaim;
    Kernel::Ipc::NotificationSnapshot ClaimedNotification;
    Kernel::Ipc::NotificationSnapshot FinalNotification;
    Kernel::Time::TimerSnapshot FinalTimer;
    Kernel::Time::DeferredTimerWorkerSnapshot FinalWorker;
    Kernel::Time::ProcessorDeadlineReceipt FinalDeadline;
    Kernel::Scheduler::ProcessorRunSnapshot FinalController;
    Kernel::Scheduler::ProcessorKernelRunLoopSnapshot FinalRunLoop;
    Kernel::Scheduler::ThreadSnapshot FinalThread;
    Kernel::Scheduler::SchedulingContextSnapshot FinalSchedulingContext;
    Ia64ApplicationProcessorPortableSchedulerServiceSnapshot FinalService;
    Time::Ia64LocalTimerSnapshot FinalLocalTimer;
    Foundation::UInt64 TimerStorageAddress;
    Foundation::UInt64 NotificationStorageAddress;
    Foundation::UInt64 EvidenceAddress;
    Foundation::UInt64 PollDeadlineTicks;
    Kernel::Scheduler::CpuId Processor;
    Interrupt::Ia64ExternalInterruptPollStatus PollStatus;
    Ia64ApplicationProcessorTimerDeliveryResult Result;
    Foundation::UInt8 Reserved[6U];
};

struct Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot
{
    Memory::Ia64ApplicationProcessorRuntimeProcessorLayout Layout;
    Ia64ApplicationProcessorKernelRuntimeRegisterEvidence RuntimeEntryEvidence;
    Ia64ApplicationProcessorKernelRuntimeRegisterEvidence FirstWorkerEntryEvidence;
    Ia64ApplicationProcessorKernelRuntimeRegisterEvidence ResumedWorkerEntryEvidence;
    Ia64ApplicationProcessorKernelRuntimeRegisterEvidence ServingIdleEvidence;
    Ia64ApplicationProcessorLocalTimerProofRecord LocalTimerProof;
    Ia64ApplicationProcessorPortableSchedulerRecord PortableScheduler;
    Ia64ApplicationProcessorPortableSchedulerServiceSnapshot Service;
    Ia64ApplicationProcessorTimerDeliverySnapshot TimerDelivery;
    Ia64ApplicationProcessorTimerDeliveryRecord TimerDeliveryRecord;
    Ia64KernelContextSnapshot IdleContext;
    Ia64KernelContextSnapshot WorkerContext;
    Foundation::UInt64 Generation;
    Kernel::Scheduler::CpuId Processor;
    Ia64ApplicationProcessorKernelRuntimeProcessorState State;
    Foundation::UInt8 EvidenceMask;
    Foundation::UInt8 Reserved[5U];
};

/// Fixed-capacity, boot-lifetime owner of the IA-64 AP translated idle path and persistent
/// cooperative scheduler. Prepare is BSP-serialized and single-use. It copies the complete layout
/// and release-coordinator preparation state before release, prepares worker contexts over their
/// Region-1 aliases, and publishes immutable configuration with one release store. Retained shared
/// dependencies are configured before release; each AP then constructs its scheduler graph in fixed
/// in-object storage. Each AP owns only its corresponding slot after ClaimPhysicalRelease. Plain
/// evidence is written once and made visible by a per-slot release publication; the first failure
/// record has a separate first-writer-wins publication word. Same-CPU re-entry into a claimed slot
/// is excluded. Immutable online evidence is copied only after its release publication; changing
/// mailbox progress is represented only by atomic words. No operation dynamically allocates
/// storage.
class Ia64ApplicationProcessorKernelRuntime final
{
public:
    constexpr Ia64ApplicationProcessorKernelRuntime() noexcept = default;

    ~Ia64ApplicationProcessorKernelRuntime() noexcept = default;

    Ia64ApplicationProcessorKernelRuntime(const Ia64ApplicationProcessorKernelRuntime&) = delete;
    Ia64ApplicationProcessorKernelRuntime& operator=(const Ia64ApplicationProcessorKernelRuntime&) = delete;
    Ia64ApplicationProcessorKernelRuntime(Ia64ApplicationProcessorKernelRuntime&&) = delete;
    Ia64ApplicationProcessorKernelRuntime& operator=(Ia64ApplicationProcessorKernelRuntime&&) = delete;

    /// Preconditions:
    /// - coordinator has the exact freshly initialized topology represented by
    /// plan and no AP release has started;
    /// - every alias in plan is already writable in the common translated root;
    /// - this owner and coordinator remain alive for the complete boot.
    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorKernelRuntimeSummary,
                                     Ia64ApplicationProcessorKernelRuntimeFailure>
    Prepare(const Memory::Ia64ApplicationProcessorRuntimeLayoutPlan& plan, Foundation::UInt8 commonPhysicalAddressBits,
            Foundation::UInt8 commonImplementedVirtualAddressMostSignificantBit,
            Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinator& coordinator) noexcept;

    /// Retains the shared boot object graph used to construct each AP's
    /// scheduler objects. This is BSP-serialized and must complete after
    /// Prepare and before any AP release is claimed.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    ConfigurePortableSchedulerDependencies(Memory::Ia64BspRuntimeMemory& runtimeMemory,
                                           Kernel::Object::ObjectRegistry& objectRegistry,
                                           Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                           Kernel::Resource::ResourceDomain& resourceDomain,
                                           Kernel::Object::RootCapabilityIssuer& issuer) noexcept;

    /// Retains the boot-owned cooperative notifier, fixed mailbox transport,
    /// and each AP's exact frozen PAL record before any AP release is claimed.
    /// The notifier and mailbox outlive this runtime. Their masks must exactly
    /// match the prepared shared-root processor mask and the notifier must have
    /// no pending publication. CPU zero remains owned by the BSP path.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError ConfigureTlbShootdownDependencies(
        Memory::Ia64PolledTlbShootdownNotifier& notifier, Kernel::Memory::TlbShootdownMailboxTransport& mailbox,
        const Memory::PalVirtualMemoryCapabilities (
            &processorCapabilities)[Kernel::Scheduler::MaximumProcessorCount]) noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeState GetState() const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeAccessError
    GetSummary(Ia64ApplicationProcessorKernelRuntimeSummary& output) const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeAccessError
    GetPlan(Memory::Ia64ApplicationProcessorRuntimeLayoutPlan& output) const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeAccessError
    GetProcessorSnapshot(Kernel::Scheduler::CpuId processor,
                         Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot& output) const noexcept;

    /// Copies the most recently completed, coherent diagnostics publication.
    /// The AP-owned mutable tree is never exposed. PublicationPending means a
    /// bounded observation overlapped an AP update and the caller may retry.
    /// PublicationSequenceExhausted permanently reports the theoretical
    /// saturation of the 63-bit publication generation without returning stale
    /// diagnostics.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeAccessError GetPortableSchedulerFailure(
        Kernel::Scheduler::CpuId processor, Ia64ApplicationProcessorPortableSchedulerFailure& output) const noexcept;

    /// Returns without waiting. Available acquire-publishes the complete first
    /// record; PublicationPending is the bounded first-writer commit window.
    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeFailurePoll
    PollFirstFailure(Ia64ApplicationProcessorKernelRuntimeFailure& output) const noexcept;

    /// Physical-side validation and exact coordinator claim. This is called
    /// before shared-root admission and the no-return translated pivot.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    ClaimPhysicalRelease(const Interrupt::Ia64ApplicationProcessorRuntimeRelease& release) noexcept;

    /// Records an external physical-root admission or pivot failure after a
    /// successful claim. detail is the caller's exact target-specific status.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError PublishPhysicalFailure(
        Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation,
        Ia64ApplicationProcessorKernelRuntimePhysicalFailure failure, Foundation::UInt64 detail) noexcept;

    /// Testable translated state-machine steps. Native RunTranslatedLanding
    /// supplies register evidence captured at the direct call boundary.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    EnterTranslatedIdle(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation,
                        const Ia64ApplicationProcessorKernelRuntimeRegisterEvidence& evidence) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    PublishWorkerEntry(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation,
                       const Ia64ApplicationProcessorKernelRuntimeRegisterEvidence& evidence) noexcept;

    /// Configures one AP-owned, one-shot external-interrupt mailbox while that
    /// AP is PhysicalClaimed. The complete 64-byte Region-0 mailbox must be
    /// naturally aligned and initially zero, including both sequence words.
    /// On success, this runtime retains the non-owning const pointer for the
    /// complete boot, through non-returning shutdown or reset. The pointed-to
    /// object must remain alive and immobile at that exact Region-0 address and
    /// is permanently bound to this processor slot; it cannot be relocated,
    /// reused, or rebound. Const applies only to the runtime's retained view:
    /// the native external-interrupt entry mutates the underlying mailbox via
    /// its separately established ISR access path before release-publication.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    ConfigureLocalTimerInterruptProof(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation,
                                      const Interrupt::Ia64LocalTimerInterruptEvidence* evidence) noexcept;

    /// Begins the bounded AP-local proof after RuntimeEntered and before any
    /// scheduler worker dispatch. The caller is
    /// the owning AP at CPL0 with PSR.vm=0, PSR.ic=1 and PSR.i=0. It initializes
    /// the E8 source masked before installing the processor-wide cr.tpr=0xD0
    /// policy, samples that AP's ITC, and arms one finite deadline. A deadline
    /// that passed before cr.itm was written (the AP did not run in between)
    /// arms nothing; the proof samples again with twice the lead, and only a
    /// timer that rejects a bounded number of attempts fails it.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    BeginLocalTimerInterruptProof(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// Completes one poll return while still on the owning AP with PSR.i=0.
    /// The timer is cancelled on every returning path after an accepted arm.
    /// No EOI, IVR/IRR drain, mailbox acknowledgement, scheduler loop,
    /// preemption, IPI, or general interrupt routing is performed here.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    CompleteLocalTimerInterruptProof(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation,
                                     Interrupt::Ia64ExternalInterruptPollStatus pollStatus,
                                     Interrupt::Ia64ExternalInterruptPollCodeRange pollCodeRange) noexcept;

    /// Constructs and registers one AP's fixed-storage portable scheduler
    /// graph after the local-timer proof has left E8 Ready, masked, and
    /// disarmed. This normal-context path keeps PSR.i clear.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    PreparePortableScheduler(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// Makes the retained worker Ready, enqueues it, and enters it through the
    /// portable run loop. The worker completes RunSequence one as Blocked and
    /// this call returns to idle only with the exact canonical no-runnable
    /// result. SchedulerWaiting is then published to the release coordinator.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    DispatchPortableSchedulerFromIdle(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// BSP-serialized request publication. Exactly one sequence may be pending
    /// or executing per AP. The caller does not touch the scheduler or blocked
    /// Thread; only that AP consumes the release-published mailbox request.
    /// ServiceUnavailable before SchedulerWaiting is retryable and leaves all
    /// request, completion, and counter words unchanged.
    /// After the one-shot timer transaction completes, a later service is
    /// accepted only while E8 stably reports sequence/acknowledgement 2/2;
    /// this condition is revalidated after the idle-work owner is acquired
    /// and again when the AP consumes the pending service.
    /// SequenceExhausted is returned without publishing once the completed
    /// sequence reaches UInt64 maximum minus two. This leaves the portable
    /// ReadyQueue's terminal sequence unconsumed rather than accepting a
    /// request that its wake transaction cannot commit.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Foundation::Result<Ia64ApplicationProcessorPortableSchedulerServiceRequest,
                                                            Ia64ApplicationProcessorKernelRuntimeError>
    RequestPortableSchedulerService(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation,
                                    Foundation::UInt64 cookie) noexcept;

    /// BSP-serialized, one-shot publication. The request is accepted only
    /// after service sequence one completed in IdleServing and the reusable E8
    /// mailbox stably reports sequence/acknowledgement 1/1 with no payload.
    /// Service and timer publications share one level-triggered exclusion
    /// owner, so neither operation can overtake the other's Pending or
    /// Executing transaction.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA
        Foundation::Result<Ia64ApplicationProcessorTimerDeliveryRequest, Ia64ApplicationProcessorKernelRuntimeError>
        RequestTimerDelivery(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// One bounded AP-idle service step. With no published request it returns
    /// success without mutation. Otherwise the AP reserves blocked admission,
    /// wakes the exact token, commits the reservation, and dispatches. It
    /// returns only after the same worker records the request and blocks again.
    /// The first completed request publishes immutable IdleServing evidence.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    ServicePortableSchedulerFromIdle(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// Performs one bounded clear-before-drain cooperative shootdown poll. Preconditions: - The
    /// caller is the owning, non-migrating AP in IdleServing with the retained shared root still
    /// active and same-CPU re-entry excluded. - This method is called before other work in each
    /// IdleServing loop pass. Empty notification state performs no drain and does not require
    /// global AddressSpace quiescence while later APs are admitted. Pending state runs one
    /// mailbox-capacity pass, which must complete every request it receives with no receive,
    /// invalidation, acknowledgement, or remote rejection error, and the shared root must be a
    /// fixed-root state (Ready, or ShootdownPending that targets exactly the enabled processors)
    /// before and after the pass. The pass may receive nothing or several requests: the notifier
    /// coalesces back-to-back notifications and may leave one latched whose work an earlier pass
    /// already drained, so the root's shootdown sequence and this AP's acknowledgement bit are not
    /// related to the work of one pass. Every other result is terminal for the AP slot and is
    /// retained in the failure publication.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    ServiceTlbShootdownFromIdle(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// AP-owned first half of the sequence-two transaction. Arms the exact
    /// AP Timer in its TimerQueue and installs the corresponding processor
    /// deadline while the blocked scheduler worker remains untouched.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    BeginTimerDeliveryFromIdle(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// AP-owned second half after the stackless E8 poll. Drains exactly one
    /// due Timer into its Notification, claims and commits that notification,
    /// reconciles the now-empty deadline, and release-publishes immutable
    /// completion evidence. It deliberately does not acknowledge E8 sequence
    /// two; the BSP owns that acknowledgement after validation.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    CompleteTimerDeliveryFromIdle(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation,
                                  Interrupt::Ia64ExternalInterruptPollStatus pollStatus,
                                  Interrupt::Ia64ExternalInterruptPollCodeRange pollCodeRange) noexcept;

    /// Native translated landing. Success enters the boot-lifetime cooperative
    /// idle service loop and never parks or returns. PSR.i remains clear; no
    /// timer, IPI, or interrupt-preemption behavior is claimed.
    [[noreturn]] ASTERIA_NOINLINE_NOIPA void RunTranslatedLanding(Kernel::Scheduler::CpuId processor) noexcept;

    /// Entry used by the raw P64 descriptor. This public adapter exists only so
    /// the platform assembly can enter a private retained worker argument.
    [[noreturn]] ASTERIA_NOINLINE_NOIPA static void RunWorkerAdapter(void* argument) noexcept;

private:
    enum class EvidenceKind : Foundation::UInt8
    {
        Idle,
        Worker,
    };

    struct WorkerArgument
    {
        Ia64ApplicationProcessorKernelRuntime* Owner;
        Foundation::UInt64 Generation;
        Kernel::Scheduler::CpuId Processor;
        Foundation::UInt8 Reserved[7U];
    };

    struct PendingServiceCompletion
    {
        Ia64ApplicationProcessorKernelRuntimeRegisterEvidence Evidence;
        Foundation::UInt64 Sequence;
        Foundation::UInt64 Cookie;
        Foundation::UInt64 RunSequence;
        Ia64ApplicationProcessorPortableSchedulerServiceResult Result;
        Foundation::UInt8 Reserved[7U];
    };

    static constexpr Foundation::UInt64 PublishingFailure{1U};
    static constexpr Foundation::UInt64 PublishedFailure{2U};
    static constexpr Foundation::UInt64 PublishingPhysicalClaim{0x8000'0000'0000'0001ULL};
    static constexpr Foundation::UInt64 PublishingRuntimeEntry{0x8000'0000'0000'0002ULL};
    static constexpr Foundation::UInt64 DispatchingPortableWorker{0x8000'0000'0000'0003ULL};
    static constexpr Foundation::UInt64 PublishingWorkerEntry{0x8000'0000'0000'0004ULL};
    static constexpr Foundation::UInt64 PublishingSchedulerWaiting{0x8000'0000'0000'0005ULL};
    static constexpr Foundation::UInt64 PublishingIdleServing{0x8000'0000'0000'0006ULL};
    static constexpr Foundation::UInt64 RuntimeEntryEvidencePublished{1U};
    static constexpr Foundation::UInt64 FirstWorkerEvidencePublished{2U};
    static constexpr Foundation::UInt64 ResumedWorkerEvidencePublished{4U};
    static constexpr Foundation::UInt64 ServingIdleEvidencePublished{8U};
    static constexpr Foundation::UInt64 PublishingLocalTimerConfiguration{0x8000'0000'0000'0101ULL};
    static constexpr Foundation::UInt64 PublishingLocalTimerArm{0x8000'0000'0000'0102ULL};
    static constexpr Foundation::UInt64 PublishingLocalTimerCompletion{0x8000'0000'0000'0103ULL};
    static constexpr Foundation::UInt64 PublishingPortableSchedulerPreparation{0x8000'0000'0000'0201ULL};
    static constexpr Foundation::UInt64 PublishingPortableSchedulerDispatch{0x8000'0000'0000'0202ULL};
    static constexpr Foundation::UInt64 PublishingPortableSchedulerWaiting{0x8000'0000'0000'0203ULL};
    static constexpr Foundation::UInt64 PublishingPortableSchedulerOnline{0x8000'0000'0000'0204ULL};
    static constexpr Foundation::UInt64 PortableSchedulerDependenciesReady{1U};
    static constexpr Foundation::UInt64 TlbShootdownDependenciesReady{1U};
    static constexpr Foundation::UInt64 ServiceMailboxIdle{0U};
    static constexpr Foundation::UInt64 ServiceMailboxPublishing{1U};
    static constexpr Foundation::UInt64 ServiceMailboxPending{2U};
    static constexpr Foundation::UInt64 ServiceMailboxExecuting{3U};
    static constexpr Foundation::UInt64 ServiceMailboxFailed{4U};
    static constexpr Foundation::UInt64 TimerDeliveryReady{0U};
    static constexpr Foundation::UInt64 TimerDeliveryPublishing{1U};
    static constexpr Foundation::UInt64 TimerDeliveryPending{2U};
    static constexpr Foundation::UInt64 TimerDeliveryExecuting{3U};
    static constexpr Foundation::UInt64 TimerDeliveryCompleted{4U};
    static constexpr Foundation::UInt64 TimerDeliveryFailed{5U};
    static constexpr Foundation::UInt64 IdleWorkAvailable{0U};
    static constexpr Foundation::UInt64 IdleWorkService{1U};
    static constexpr Foundation::UInt64 IdleWorkTimerDelivery{2U};
    static constexpr Foundation::UInt64 IdleWorkFailed{3U};
    static constexpr Foundation::Size PortableSchedulerObjectsPerProcessor{5U};
    static constexpr Foundation::Size PortableSchedulerFailureWordCount{
        sizeof(Ia64ApplicationProcessorPortableSchedulerFailure) / sizeof(Foundation::UInt64)};
    static constexpr Foundation::Size PortableSchedulerFailureSnapshotAttemptLimit{4U};

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidatePlan(const Memory::Ia64ApplicationProcessorRuntimeLayoutPlan& plan,
                 Foundation::UInt8 commonPhysicalAddressBits,
                 Foundation::UInt8 commonImplementedVirtualAddressMostSignificantBit) const noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool CaptureAndValidateReleaseCoordinator(
        Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinator& coordinator) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64KernelContextError PrepareWorkerContext(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidateEvidence(Foundation::Size index, const Ia64ApplicationProcessorKernelRuntimeRegisterEvidence& evidence,
                     EvidenceKind kind) const noexcept;

    [[nodiscard]] bool TryGetProcessorIndex(Kernel::Scheduler::CpuId processor, Foundation::Size& index) const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeError
    ValidateProcessorGeneration(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation,
                                Foundation::Size& index,
                                Ia64ApplicationProcessorKernelRuntimeOperation operation) noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeProcessorState
    DecodeProcessorState(Foundation::UInt64 state) const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorLocalTimerProofPhase
    DecodeLocalTimerProofPhase(Foundation::UInt64 state) const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorPortableSchedulerPhase
    DecodePortableSchedulerPhase(Foundation::UInt64 state) const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorPortableSchedulerServiceState
    DecodePortableSchedulerServiceState(Foundation::UInt64 state) const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorTimerDeliveryState
    DecodeTimerDeliveryState(Foundation::UInt64 state) const noexcept;

    [[nodiscard]] bool
    ValidateLocalTimerEvidenceStorage(const Interrupt::Ia64LocalTimerInterruptEvidence* evidence) const noexcept;

    [[nodiscard]] bool RegistrationMatches(Kernel::Object::ObjectRegistry& registry, Kernel::Object::ObjectId identity,
                                           Kernel::Object::ObjectType type, Kernel::Object::IKernelObject& storage,
                                           Kernel::Object::ObjectPinError& pinError) noexcept;

    [[nodiscard]] bool ValidateResourceDomain(Kernel::Object::ObjectRegistry& registry,
                                              Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                              Kernel::Resource::ResourceDomain& resourceDomain,
                                              Kernel::Object::ObjectPinError& pinError) noexcept;

    [[nodiscard]] bool ValidatePortableSchedulerRoot(Foundation::Size index,
                                                     const Memory::Ia64BspRuntimeRootView& root) const noexcept;

    [[nodiscard]] bool InitializePortableSchedulerObjects(Foundation::Size index,
                                                          const Memory::Ia64BspRuntimeRootView& root) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool InitializePortableTimerDeliveryObjects(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    InitializeTlbShootdownObjects(Foundation::Size index, Kernel::Scheduler::CpuId processor) noexcept;

    void ConstructPortableSchedulerDispatchers(Foundation::Size index,
                                               const Memory::Ia64BspRuntimeRootView& root) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidatePreparedPortableSchedulerDispatcherPostconditions(Foundation::Size index) const noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidatePreparedTimerDeliveryPostconditions(Foundation::Size index) const noexcept;

    [[nodiscard]] bool ValidateExpectedPortableSchedulerIdleFailure(Foundation::Size index) const noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidatePortableSchedulerIdleTimerPostconditions(Foundation::Size index) const noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidatePortableSchedulerIdleRunPostconditions(Foundation::Size index) const noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidatePortableSchedulerBlockedThreadPostconditions(Foundation::Size index, Foundation::UInt64 expectedRunSequence,
                                                         bool requireDistinctToken) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidatePortableSchedulerBlockedPostconditions(Foundation::Size index, Foundation::UInt64 expectedRunSequence,
                                                   bool requireDistinctToken) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    ValidatePortableSchedulerActiveBinding(Foundation::Size index, Kernel::Scheduler::CpuId processor,
                                           Foundation::UInt64 expectedRunSequence,
                                           Ia64ApplicationProcessorKernelRuntimeOperation operation) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError PublishSchedulerWaiting(
        Foundation::Size index, Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    WakePortableSchedulerWorker(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    RecordPortableSchedulerServiceCompletion(Foundation::Size index, Kernel::Scheduler::CpuId processor) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    PublishPortableSchedulerServiceCompletion(Foundation::Size index, Kernel::Scheduler::CpuId processor,
                                              Foundation::UInt64 generation) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidateTimerDeliveryRequestPreconditions(Foundation::Size index) const noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidateAcknowledgedTimerDeliveryGate(Foundation::Size index) const noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidateAcknowledgedTimerDeliveryCompletion(Foundation::Size index) const noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidateTimerDeliverySchedulerState(Foundation::Size index,
                                        Foundation::UInt64 expectedNextClaimSequence) const noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ArmTimerDelivery(Foundation::Size index) noexcept;

    ASTERIA_NOINLINE_NOIPA void SettleArmedTimerDelivery(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError FailArmedTimerDelivery(
        Foundation::Size index, Ia64ApplicationProcessorKernelRuntimeError error,
        Ia64ApplicationProcessorKernelRuntimeOperation operation, Foundation::UInt64 detail) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    CaptureTimerDeliveryInterruptEvidence(Foundation::Size index, Interrupt::Ia64ExternalInterruptPollStatus pollStatus,
                                          Interrupt::Ia64ExternalInterruptPollCodeRange pollCodeRange) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool DrainTimerDelivery(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ReconcileTimerDeliveryDeadline(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool CaptureTimerDeliveryFinalSchedulerState(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    PublishTimerDeliveryCompletion(Foundation::Size index, Kernel::Scheduler::CpuId processor) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError PublishIdleServing(
        Foundation::Size index, Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool CaptureServingIdleEvidence(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    CapturePortableSchedulerOnlineThreadRecord(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool CapturePortableSchedulerOnlineRunRecord(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    CapturePortableSchedulerOnlineTimerRecord(Foundation::Size index) noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeError
    FailPortableScheduler(Foundation::Size index, Ia64ApplicationProcessorKernelRuntimeError error,
                          Ia64ApplicationProcessorKernelRuntimeOperation operation, Foundation::UInt64 detail) noexcept;

    ASTERIA_NOINLINE_NOIPA void PublishPortableSchedulerFailureSnapshot(Foundation::Size index) noexcept;

    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError
    FinishPortableScheduler(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    [[noreturn]] ASTERIA_NOINLINE_NOIPA void RunPortableSchedulerIdleLoop(Kernel::Scheduler::CpuId processor,
                                                                          Foundation::UInt64 generation) noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeSummary MakeSummary() const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeFailure
    MakeFailure(Ia64ApplicationProcessorKernelRuntimeError error,
                Ia64ApplicationProcessorKernelRuntimeOperation operation, Foundation::UInt8 processorValue,
                Foundation::UInt64 generation, Ia64ApplicationProcessorKernelRuntimeProcessorState observedState,
                Interrupt::Ia64ApplicationProcessorRuntimeTransitionError releaseError,
                Ia64KernelContextError contextError, Foundation::UInt64 detail) const noexcept;

    void PublishFirstFailure(const Ia64ApplicationProcessorKernelRuntimeFailure& failure) noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeError
    FailProcessor(Foundation::Size index, Ia64ApplicationProcessorKernelRuntimeError error,
                  Ia64ApplicationProcessorKernelRuntimeOperation operation,
                  Interrupt::Ia64ApplicationProcessorRuntimeTransitionError releaseError,
                  Ia64KernelContextError contextError, Foundation::UInt64 detail) noexcept;

    [[nodiscard]] Ia64ApplicationProcessorKernelRuntimeError
    FailLocalTimerProof(Foundation::Size index, Ia64ApplicationProcessorKernelRuntimeError error,
                        Ia64ApplicationProcessorKernelRuntimeOperation operation, Foundation::UInt64 detail) noexcept;

    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorKernelRuntimeSummary,
                                     Ia64ApplicationProcessorKernelRuntimeFailure>
    FailPreparation(Ia64ApplicationProcessorKernelRuntimeError error, Foundation::UInt8 processorValue,
                    Interrupt::Ia64ApplicationProcessorRuntimeTransitionError releaseError,
                    Ia64KernelContextError contextError, Foundation::UInt64 detail) noexcept;

    [[noreturn]] ASTERIA_NOINLINE_NOIPA void RunWorker(Kernel::Scheduler::CpuId processor,
                                                       Foundation::UInt64 generation) noexcept;

    [[noreturn]] ASTERIA_NOINLINE_NOIPA static void Park(Diagnostics::Ia64ApplicationProcessorStage stage) noexcept;

    Ia64KernelContext m_idleContexts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64KernelContext m_workerContexts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64KernelContextSnapshot m_workerContextSnapshots[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Time::Ia64LocalTimer m_localTimers[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Protection::ProtectionDomain
        m_schedulerProtectionDomains[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Scheduler::Thread m_schedulerThreads[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    alignas(Kernel::Scheduler::SchedulingContext)
        Foundation::UInt8 m_schedulerContextStorage[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]
                                                   [sizeof(Kernel::Scheduler::SchedulingContext)]{};
    alignas(Kernel::Time::TimerQueue)
        Foundation::UInt8 m_schedulerTimerQueueStorage[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]
                                                      [sizeof(Kernel::Time::TimerQueue)]{};
    alignas(Kernel::Time::Timer)
        Foundation::UInt8 m_timerDeliveryTimerStorage[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]
                                                     [sizeof(Kernel::Time::Timer)]{};
    alignas(Kernel::Ipc::Notification) Foundation::UInt8
        m_timerDeliveryNotificationStorage[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]
                                          [sizeof(Kernel::Ipc::Notification)]{};
    alignas(Kernel::Time::DeferredTimerWorker)
        Foundation::UInt8 m_deferredTimerWorkerStorage[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]
                                                      [sizeof(Kernel::Time::DeferredTimerWorker)]{};
    alignas(Kernel::Scheduler::ProcessorRunController)
        Foundation::UInt8 m_schedulerControllerStorage[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]
                                                      [sizeof(Kernel::Scheduler::ProcessorRunController)]{};
    alignas(Ia64ProcessorRunLoop)
        Foundation::UInt8 m_schedulerRunLoopStorage[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]
                                                   [sizeof(Ia64ProcessorRunLoop)]{};
    Kernel::Scheduler::SchedulingContext*
        m_schedulerContexts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Time::TimerQueue* m_schedulerTimerQueues[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Time::Timer* m_timerDeliveryTimers[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Ipc::Notification*
        m_timerDeliveryNotifications[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Time::DeferredTimerWorker*
        m_deferredTimerWorkers[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Scheduler::ProcessorRunController*
        m_schedulerControllers[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64ProcessorRunLoop* m_schedulerRunLoops[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Memory::Ia64LocalTlbInvalidator
        m_tlbShootdownInvalidators[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    alignas(Kernel::Memory::TlbShootdownDispatcher)
        Foundation::UInt8 m_tlbShootdownDispatcherStorage[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]
                                                         [sizeof(Kernel::Memory::TlbShootdownDispatcher)]{};
    alignas(Kernel::Memory::TlbShootdownDrain)
        Foundation::UInt8 m_tlbShootdownDrainStorage[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]
                                                    [sizeof(Kernel::Memory::TlbShootdownDrain)]{};
    Kernel::Memory::TlbShootdownDispatcher*
        m_tlbShootdownDispatchers[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Memory::TlbShootdownDrain*
        m_tlbShootdownDrains[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Memory::PalVirtualMemoryCapabilities
        m_tlbShootdownCapabilities[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Memory::Ia64ApplicationProcessorRuntimeLayoutPlan m_plan{};
    Interrupt::Ia64ApplicationProcessorRuntimeReleaseSnapshot m_releasePreparationSnapshot{};
    Ia64ApplicationProcessorKernelRuntimeRegisterEvidence
        m_runtimeEntryEvidence[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64ApplicationProcessorKernelRuntimeRegisterEvidence
        m_firstWorkerEntryEvidence[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64ApplicationProcessorKernelRuntimeRegisterEvidence
        m_resumedWorkerEntryEvidence[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64ApplicationProcessorKernelRuntimeRegisterEvidence
        m_servingIdleEvidence[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    const Interrupt::Ia64LocalTimerInterruptEvidence*
        m_localTimerEvidence[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64ApplicationProcessorLocalTimerProofRecord
        m_localTimerProofRecords[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64ApplicationProcessorPortableSchedulerRecord
        m_portableSchedulerRecords[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64ApplicationProcessorTimerDeliveryRecord
        m_timerDeliveryRecords[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Ia64ApplicationProcessorPortableSchedulerFailure
        m_portableSchedulerFailures[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_portableSchedulerFailureWords[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]
                                       [PortableSchedulerFailureWordCount]{};
    Synchronization::AtomicUInt64
        m_portableSchedulerFailurePublicationSequences[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Memory::Ia64BspRuntimeRootView m_schedulerRoots[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Protection::ProtectionDomainId
        m_schedulerProtectionDomainIdentities[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Scheduler::ThreadId
        m_schedulerThreadIdentities[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Scheduler::SchedulingContextId
        m_schedulerContextIdentities[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Time::TimerId
        m_timerDeliveryTimerIdentities[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Ipc::NotificationId
        m_timerDeliveryNotificationIdentities[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Time::DeferredTimerRegistrationReceipt
        m_timerDeliveryRegistrations[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Time::TimerQueueArmReceipt
        m_timerDeliveryArmReceipts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Time::ProcessorDeadlineReceipt
        m_timerDeliveryArmedDeadlineReceipts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Time::ProcessorDeadlineReceipt
        m_timerDeliveryFinalDeadlineReceipts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt
        m_schedulerIdleReceipts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure
        m_schedulerIdleFailures[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt
        m_schedulerWorkerReceipts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure
        m_schedulerWorkerFailures[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Scheduler::ThreadBlockToken
        m_schedulerBlockTokens[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Kernel::Scheduler::ThreadBlockToken
        m_initialSchedulerBlockTokens[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    PendingServiceCompletion
        m_pendingServiceCompletions[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Foundation::UInt64 m_localTimerProgrammedDeadlines[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Foundation::UInt64 m_localTimerPollDeadlines[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Foundation::UInt64 m_timerDeliveryPollDeadlines[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    WorkerArgument m_workerArguments[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Foundation::UInt64 m_generations[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_processorStates[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_evidencePublications[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_idleContextPreparedPublications[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_localTimerProofStates[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_portableSchedulerStates[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_serviceMailboxStates[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_serviceRequestSequences[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_serviceRequestCookies[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_serviceCompletionSequences[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_serviceCompletionCookies[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_serviceCompletionResults[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_serviceCompletionEvidenceValues[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_serviceCompletionRunSequences[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_serviceDispatchCounts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_serviceWakeCounts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_serviceCounts[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_idleWorkOwners[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_timerDeliveryStates[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_timerDeliveryRequestSequences[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_timerDeliveryCompletionSequences[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Synchronization::AtomicUInt64
        m_timerDeliveryResults[Memory::Ia64ApplicationProcessorRuntimeMaximumProcessorCount]{};
    Interrupt::Ia64ApplicationProcessorRuntimeReleaseCoordinator* m_releaseCoordinator{nullptr};
    Memory::Ia64BspRuntimeMemory* m_runtimeMemory{nullptr};
    Memory::Ia64PolledTlbShootdownNotifier* m_tlbShootdownNotifier{nullptr};
    Kernel::Memory::TlbShootdownMailboxTransport* m_tlbShootdownMailbox{nullptr};
    Kernel::Object::ObjectRegistry* m_objectRegistry{nullptr};
    Kernel::Resource::ResourceDomain* m_resourceDomain{nullptr};
    Kernel::Object::RootCapabilityIssuer* m_issuer{nullptr};
    Memory::Ia64BspRuntimeRootView m_expectedSharedRoot{};
    Kernel::Resource::ResourceDomainObjectId m_resourceDomainIdentity{};
    Ia64ApplicationProcessorKernelRuntimeFailure m_firstFailure{};
    Synchronization::AtomicUInt64 m_firstFailurePublication{};
    Synchronization::AtomicUInt64 m_state{};
    Synchronization::AtomicUInt64 m_portableSchedulerConfiguration{};
    Synchronization::AtomicUInt64 m_tlbShootdownConfiguration{};
    alignas(16U) Foundation::UInt64 m_workerEntryDescriptor[2U]{};
    Foundation::UInt8 m_commonPhysicalAddressBits{0U};
    Foundation::UInt8 m_commonImplementedVirtualAddressMostSignificantBit{0U};
    Foundation::UInt8 m_preparedContextCount{0U};
    [[maybe_unused]] Foundation::UInt8 m_reserved[5U]{};
};

/// Internal rules of Ia64ApplicationProcessorKernelRuntime. Each rule is a
/// function of the values it is given and neither reads nor mutates runtime
/// state; the runtime applies them to its own retained objects and words.
namespace Detail
{
/// Accepts one AP drain pass only when it received at most mailbox-capacity
/// requests for processor, completed every one, and retained no receive,
/// remote, local-invalidation, AddressSpace, or reserved-byte contradiction.
/// An empty pass is valid: a coalesced notification may remain latched after
/// an earlier pass drained its work.
[[nodiscard]] constexpr bool ValidateTlbShootdownDrainSummary(const Kernel::Memory::TlbShootdownDrainSummary& summary,
                                                              const Kernel::Scheduler::CpuId processor) noexcept
{
    return processor.IsValid() && summary.Processor.IsValid() &&
           (summary.Processor.GetValue() == processor.GetValue()) &&
           (summary.ReceivedCount <= Kernel::Memory::TlbShootdownMailboxSlotCount) &&
           (summary.ReceivedCount == summary.CompletedCount) && (summary.RejectedCount == 0U) &&
           (summary.ReceiveError == Kernel::Memory::TlbShootdownReceiveError::None) &&
           (summary.FirstRemoteError == Kernel::Memory::TlbShootdownError::None) &&
           (summary.FirstLocalError == Hal::Memory::LocalTlbInvalidationError::None) &&
           (summary.FirstAddressSpaceError == Kernel::Memory::AddressSpaceError::None) &&
           (summary.Reserved[0U] == 0U) && (summary.Reserved[1U] == 0U) && (summary.Reserved[2U] == 0U) &&
           (summary.Reserved[3U] == 0U) && (summary.Reserved[4U] == 0U) && (summary.Reserved[5U] == 0U) &&
           (summary.Reserved[6U] == 0U) && (summary.Reserved[7U] == 0U);
}

/// Accepts a processor-local controller snapshot only as the exact fixed-root
/// proof for processor: Ready, with root's AddressSpace as both its idle and
/// Active storage and identity, and with no previous root, transition, or nonzero reserved byte.
[[nodiscard]] bool FixedRootControllerMatches(const Kernel::Memory::ProcessorAddressSpaceControllerSnapshot& snapshot,
                                              const Memory::Ia64BspRuntimeRootView& root,
                                              Kernel::Scheduler::CpuId processor) noexcept;

/// Derives the one-shot timer-delivery deadline, observedTicks plus the
/// delivery lead, and its poll deadline one poll grace later. The Timer is
/// armed once, so the lead itself must outlast the AP losing the processor
/// between this sample and programming cr.itm. Every sample whose
/// poll deadline would reach UInt64 maximum is rejected before any addition;
/// failure leaves both outputs unchanged.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
TryComputeTimerDeliveryDeadlines(Foundation::UInt64 observedTicks, Foundation::UInt64& deadlineTicks,
                                 Foundation::UInt64& pollDeadlineTicks) noexcept;

/// Allocates the BSP-to-AP service request sequence that follows
/// completedSequence. Admission ends once the completed sequence reaches
/// UInt64 maximum minus two, which leaves the portable ReadyQueue's terminal
/// sequence unconsumed rather than accepting a request whose wake transaction
/// cannot commit. Failure leaves nextSequence unchanged.
[[nodiscard]] constexpr bool TryAdvancePortableSchedulerServiceSequence(const Foundation::UInt64 completedSequence,
                                                                        Foundation::UInt64& nextSequence) noexcept
{
    if (completedSequence >= (~Foundation::UInt64{0U} - 2U))
    {
        return false;
    }
    nextSequence = completedSequence + 1U;
    return true;
}

/// Whether independently acquire-loaded timer-delivery words form a stable
/// observation. Before completion, each state admits only its own request
/// sequence and no result. Completion sequence one is exposed only with
/// request one, Completed, and Delivered; every other combination, including
/// an unknown encoding, is a publication still in progress.
[[nodiscard]] bool TimerDeliveryPublicationIsCoherent(Foundation::UInt64 completionSequence, Foundation::UInt64 state,
                                                      Foundation::UInt64 requestSequence,
                                                      Foundation::UInt64 result) noexcept;

/// Claims and commits the timer-delivery notification that the retained
/// drain has published once, recording the claim and both notification
/// snapshots in record. Returns None only when the exact first claim commits
/// and leaves the notification Ready with nothing pending. A rejected claim or
/// commit retains its Notification failure in failure and returns
/// TimerDeliveryNotificationClaimRejected or
/// TimerDeliveryNotificationCommitRejected; any other contradiction returns
/// TimerDeliveryPostconditionRejected.
[[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64ApplicationProcessorKernelRuntimeError ConsumeTimerDeliveryNotification(
    Kernel::Ipc::Notification& notification, Ia64ApplicationProcessorTimerDeliveryRecord& record,
    Kernel::Ipc::NotificationFailure& failure) noexcept;
} // Namespace Detail

static_assert(sizeof(Ia64ApplicationProcessorKernelRuntimeState) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorKernelRuntimeProcessorState) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorLocalTimerProofPhase) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorPortableSchedulerPhase) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorPortableSchedulerServiceState) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorPortableSchedulerServiceResult) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorTimerDeliveryState) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorTimerDeliveryResult) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorKernelRuntimeError) == 4U);
static_assert(sizeof(Ia64ApplicationProcessorKernelRuntimeOperation) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorKernelRuntimePhysicalFailure) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorKernelRuntimeAccessError) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorKernelRuntimeFailurePoll) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence) == 56U);
static_assert(alignof(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence));
static_assert(__is_standard_layout(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence));
static_assert(__builtin_offsetof(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence, StackPointer) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence, RseBackingStorePointer) == 8U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence, RseBackingStoreBase) == 16U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence, RseNatCollection) == 24U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence, RseConfiguration) == 32U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence, ProcessorStatus) == 40U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorKernelRuntimeRegisterEvidence, CaptureComplete) == 48U);
static_assert(sizeof(Ia64ApplicationProcessorKernelRuntimeFailure) == 32U);
static_assert(alignof(Ia64ApplicationProcessorKernelRuntimeFailure) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorKernelRuntimeFailure));
static_assert(__is_standard_layout(Ia64ApplicationProcessorKernelRuntimeFailure));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorKernelRuntimeFailure));
static_assert(sizeof(Ia64ApplicationProcessorKernelRuntimeSummary) == 24U);
static_assert(alignof(Ia64ApplicationProcessorKernelRuntimeSummary) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorKernelRuntimeSummary));
static_assert(__is_standard_layout(Ia64ApplicationProcessorKernelRuntimeSummary));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorKernelRuntimeSummary));
static_assert(sizeof(Ia64ApplicationProcessorPortableSchedulerServiceRequest) == 24U);
static_assert(alignof(Ia64ApplicationProcessorPortableSchedulerServiceRequest) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorPortableSchedulerServiceRequest));
static_assert(__is_standard_layout(Ia64ApplicationProcessorPortableSchedulerServiceRequest));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorPortableSchedulerServiceRequest));
static_assert(sizeof(Ia64ApplicationProcessorPortableSchedulerServiceSnapshot) == 80U);
static_assert(alignof(Ia64ApplicationProcessorPortableSchedulerServiceSnapshot) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorPortableSchedulerServiceSnapshot));
static_assert(__is_standard_layout(Ia64ApplicationProcessorPortableSchedulerServiceSnapshot));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorPortableSchedulerServiceSnapshot));
static_assert(sizeof(Ia64ApplicationProcessorTimerDeliveryRequest) == 16U);
static_assert(alignof(Ia64ApplicationProcessorTimerDeliveryRequest) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorTimerDeliveryRequest));
static_assert(__is_standard_layout(Ia64ApplicationProcessorTimerDeliveryRequest));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorTimerDeliveryRequest));
static_assert(sizeof(Ia64ApplicationProcessorTimerDeliverySnapshot) == 24U);
static_assert(alignof(Ia64ApplicationProcessorTimerDeliverySnapshot) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorTimerDeliverySnapshot));
static_assert(__is_standard_layout(Ia64ApplicationProcessorTimerDeliverySnapshot));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorTimerDeliverySnapshot));
static_assert(sizeof(Ia64ApplicationProcessorLocalTimerProofRecord) == 144U);
static_assert(alignof(Ia64ApplicationProcessorLocalTimerProofRecord) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorLocalTimerProofRecord));
static_assert(__is_standard_layout(Ia64ApplicationProcessorLocalTimerProofRecord));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorLocalTimerProofRecord));
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, InterruptEvidence) == 0U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, FinalTimerSnapshot) == 64U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, PollCodeRange) == 80U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, EvidenceAddress) == 96U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, ProgrammedDeadlineTicks) == 104U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, PollDeadlineTicks) == 112U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, TaskPriority) == 120U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, PollStatus) == 128U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, ValidationResult) == 136U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, Phase) == 137U);
static_assert(__builtin_offsetof(Ia64ApplicationProcessorLocalTimerProofRecord, Vector) == 138U);
static_assert(__is_aggregate(Ia64ApplicationProcessorPortableSchedulerRecord));
static_assert(__is_standard_layout(Ia64ApplicationProcessorPortableSchedulerRecord));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorPortableSchedulerRecord));
static_assert(alignof(Ia64ApplicationProcessorTimerDeliverySettlement) == 8U);
static_assert(sizeof(Ia64ApplicationProcessorTimerDeliverySettlement) == 112U);
static_assert(__is_aggregate(Ia64ApplicationProcessorTimerDeliverySettlement));
static_assert(__is_standard_layout(Ia64ApplicationProcessorTimerDeliverySettlement));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorTimerDeliverySettlement));
static_assert(__is_aggregate(Ia64ApplicationProcessorPortableSchedulerFailure));
static_assert(__is_standard_layout(Ia64ApplicationProcessorPortableSchedulerFailure));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorPortableSchedulerFailure));
static_assert((sizeof(Ia64ApplicationProcessorPortableSchedulerFailure) % sizeof(Foundation::UInt64)) == 0U);
static_assert(alignof(Ia64ApplicationProcessorTimerDeliveryRecord) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorTimerDeliveryRecord));
static_assert(__is_standard_layout(Ia64ApplicationProcessorTimerDeliveryRecord));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorTimerDeliveryRecord));
static_assert(alignof(Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot));
static_assert(__is_standard_layout(Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorKernelRuntimeProcessorSnapshot));
static_assert(Memory::Ia64ApplicationProcessorRuntimeIdleStackSizeBytes == 32U * 1024U);
static_assert(Memory::Ia64ApplicationProcessorRuntimeIdleRseSizeBytes == 32U * 1024U);
static_assert(Memory::Ia64ApplicationProcessorRuntimeWorkerStackSizeBytes == 16U * 1024U);
static_assert(Memory::Ia64ApplicationProcessorRuntimeWorkerRseSizeBytes == 16U * 1024U);
static_assert(alignof(Ia64ApplicationProcessorKernelRuntime) == 64U);
} // Namespace Asteria::Platform::Ia64::Scheduler

#endif // ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_APPLICATION_PROCESSOR_KERNEL_RUNTIME_HPP_INCLUDED
