// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_BSP_KERNEL_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_BSP_KERNEL_RUNTIME_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Memory/ITlbShootdownTransport.hpp"
#include "Kernel/Diagnostics/DiagnosticChannel.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/DirectUserMemoryAccess.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/TlbShootdownDrain.hpp"
#include "Kernel/Scheduler/CrossProcessorWaitBudget.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Memory/PinnedFrameMapping.hpp"
#include "Kernel/Memory/ProcessorAddressSpaceController.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Kernel/Object/CapabilityKeyDerivation.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Scheduler/ProcessorRunController.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/Scheduler/Thread.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Kernel/System/WaitSetSystemCallDispatcher.hpp"
#include "Kernel/System/WaitSetSystemCallIngress.hpp"
#include "Kernel/Time/DeferredTimerWorker.hpp"
#include "Kernel/Time/TimerNotificationDelivery.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Platform/Firmware/DiagnosticLineSink.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionEntry.hpp"
#include "Platform/Ia64/Interrupt/Ia64InterruptLines.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Memory/Ia64LocalTlbInvalidator.hpp"
#include "Platform/Ia64/Scheduler/Ia64InitialUserImageLaunchOwner.hpp"
#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"
#include "Platform/Ia64/System/Ia64UserPreemption.hpp"
#include "Platform/Ia64/Scheduler/Ia64ProcessorRunLoop.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserExecutionProbe.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserExecutionProbePayload.hpp"
#include "Platform/Ia64/System/Ia64SystemCallWaitContinuation.hpp"
#include "Platform/Ia64/Time/Ia64LocalTimer.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

extern "C" [[noreturn]] void AsteriaIa64KernelIdle() noexcept;

namespace Asteria::Platform::Ia64::System
{
class Ia64PagerRuntime;
class Ia64UserThreadTable;
struct Ia64UserThreadTableCollaborators;
struct Ia64PagerCollaborators;
}

namespace Asteria::Platform::Ia64::Scheduler
{
inline constexpr Foundation::UInt64 Ia64BspKernelWorkerExitCode{0x4941'3634'4253'5001ULL};
inline constexpr Foundation::UInt64 Ia64BspUserProbeLaunchFailureExitCode{0x4941'3634'5550'0001ULL};
inline constexpr Foundation::UInt64 Ia64BspInitialUserImageLaunchFailureExitCode{0x4941'3634'4955'0001ULL};
inline constexpr Foundation::UInt64 Ia64BspLocalTimerProbeLeadTicks{Kernel::Scheduler::CrossProcessorMinimumPolls};
// A native WaitSet wait crosses the CPL3 syscall, blocking, and context-switch
// path before the deadline is refreshed. Keep that bounded lead at ten times
// the immediate interrupt probe lead and one tenth of the polling grace.
inline constexpr Foundation::UInt64 Ia64BspUserProbeWaitTimerLeadTicks{100'000'000U};
inline constexpr Foundation::UInt64 Ia64BspLocalTimerProbeGraceTicks{1'000'000'000U};
/// Finite admitted run budget for the opt-in native proof. The larger period
/// supplies a separate initialization/admission margin before the absolute
/// deadline; the firmware harness timeout remains independent.
inline constexpr Foundation::UInt64 Ia64BspNativePreemptionLeadTicks{50'000'000U};
inline constexpr Foundation::UInt64 Ia64BspNativePreemptionPeriodTicks{100'000'000U};
inline constexpr Foundation::UInt64 Ia64BspNativePreemptionExitCode{0x4941'3634'4E50'0001ULL};
inline constexpr Foundation::UInt64 Ia64BspSharedRootShootdownVirtualStart{0x2000'0000'0500'0000ULL};
/// Budget of the BSP's wait for the APs' shared-root shootdown
/// acknowledgements (Ia64CrossProcessorWaitBudget): it gives up only after
/// both this many polls and this much ITC, 20 s at the emulator's 400 MHz.
inline constexpr Foundation::Size Ia64BspSharedRootShootdownMinimumPolls{Kernel::Scheduler::CrossProcessorMinimumPolls};
inline constexpr Foundation::UInt64 Ia64BspSharedRootShootdownMinimumTicks{
    Kernel::Scheduler::Ia64CrossProcessorMinimumTicks};

/// Keeps the first hardware run-stop boundary relative to StartRun rather
/// than shortening it by work performed after the context's initial clock
/// sample. The period-sized relative deadline leaves the explicit admission
/// margin above; the smaller budget remains the selected run-stop source.
[[nodiscard]] constexpr Kernel::Scheduler::SchedulingContextConfiguration
MakeIa64BspNativePreemptionSchedulingConfiguration(const Foundation::ProcessorMask affinityMask) noexcept
{
    return Kernel::Scheduler::SchedulingContextConfiguration{
        .BudgetTicks = Ia64BspNativePreemptionLeadTicks,
        .PeriodTicks = Ia64BspNativePreemptionPeriodTicks,
        .RelativeDeadlineTicks = Ia64BspNativePreemptionPeriodTicks,
        .Weight = 1U,
        .Priority = 0xFFU,
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Emergency,
        .CpuAffinityMask = affinityMask,
        .Class = Kernel::Scheduler::SchedulingClass::EmergencyRecovery,
        .Reserved = 0U,
    };
}

namespace Detail
{
/// Internal rule of Ia64BspKernelRuntime. Derives the user WaitSet probe
/// timer deadline, observedTicks plus Ia64BspUserProbeWaitTimerLeadTicks, and
/// its polling timeout one Ia64BspLocalTimerProbeGraceTicks later. Every
/// sample whose timeout would reach UInt64 maximum is rejected before any
/// addition; failure leaves both outputs unchanged.
[[nodiscard]] bool TryComputeUserProbeWaitDeadlines(Foundation::UInt64 observedTicks, Foundation::UInt64& deadlineTicks,
                                                    Foundation::UInt64& timeoutTicks) noexcept;
} // Namespace Detail

enum class Ia64BspKernelRuntimeState : Foundation::UInt8
{
    Fresh,
    Prepared,
    Dispatching,
    Complete,
    Failed,
};

enum class Ia64BspKernelRuntimeError : Foundation::UInt8
{
    None,
    AlreadyPrepared,
    PreviousPreparationFailed,
    InvalidPlacement,
    RuntimeRootBorrowRejected,
    RuntimeRootRejected,
    InitialComponentRootBorrowRejected,
    InitialComponentRootRejected,
    ResourceDomainRejected,
    LocalTlbInvalidatorRejected,
    LocalTimerRejected,
    TimerInitializationRejected,
    TimerRegistrationRejected,
    NotificationInitializationRejected,
    NotificationRegistrationRejected,
    WaitSetInitializationRejected,
    WaitSetRegistrationRejected,
    ProtectionDomainInitializationRejected,
    ProtectionDomainRegistrationRejected,
    ProtectionDomainIdentityRejected,
    ThreadInitializationRejected,
    ThreadRegistrationRejected,
    SchedulingContextInitializationRejected,
    SchedulingContextRegistrationRejected,
    SchedulingContextBindingRejected,
    WorkerContextRejected,
    SystemCallPublicationRejected,
    PreparationPostconditionRejected,
    NotPrepared,
    DispatchAlreadyAttempted,
    ThreadReadyRejected,
    RunLoopBindingRejected,
    EnqueueRejected,
    DispatchResultRejected,
    DispatchPostconditionRejected,
    NativePreemptionUnavailable,
    NativePreemptionAlreadyAttempted,
    NativePreemptionPrerequisiteRejected,
    NativePreemptionSchedulingContextRejected,
    NativePreemptionStorageRejected,
    NativePreemptionControlRejected,
    NativePreemptionPublicationRejected,
    NativePreemptionContinuationRejected,
    NativePreemptionArmRejected,
    NativePreemptionCoordinatorRejected,
    NativePreemptionRestoreRejected,
};

enum class Ia64BspKernelRuntimeUserProbeState : Foundation::UInt8
{
    Fresh,
    Prepared,
    Dispatching,
    PreparationFailed,
    DispatchFailed,
};

enum class Ia64BspKernelRuntimeUserProbeError : Foundation::UInt8
{
    None,
    RuntimeNotPrepared,
    AlreadyPrepared,
    PreviousPreparationFailed,
    ConfigurationConflict,
    InvalidRetainedDependencies,
    InvalidProbePlacement,
    PrivateRootActivationRejected,
    SoleResidentRootRejected,
    LocalTlbInvalidatorRejected,
    ProbePreparationRejected,
    CapabilityAuthorityRejected,
    EndpointInitializationRejected,
    EndpointRegistrationRejected,
    EndpointCapabilityRejected,
    WaitSetCapabilityRejected,
    WaitContinuationRejected,
    WaitPathRejected,
    WaitTimerRejected,
    WaitSuspendRejected,
    WaitInterruptRejected,
    WaitDeliveryRejected,
    WaitWakeRejected,
    WaitResumeRejected,
    UserMemoryInitializationRejected,
    BootstrapEncodingRejected,
    BootstrapPublicationRejected,
    CommonRootRestorationRejected,
    PreparationPostconditionRejected,
    DispatchNotPrepared,
    DispatchAlreadyAttempted,
    ThreadReadyRejected,
    RunLoopBindingRejected,
    EnqueueRejected,
    DispatchResultRejected,
    LaunchRejected,
    DispatchPostconditionRejected,
};

enum class Ia64BspKernelRuntimeInitialUserImageState : Foundation::UInt8
{
    Fresh,
    Prepared,
    Dispatching,
    PreparationFailed,
    DispatchFailed,
};

enum class Ia64BspKernelRuntimeInitialUserImageError : Foundation::UInt8
{
    None,
    RuntimeNotPrepared,
    AlreadyPrepared,
    PreviousPreparationFailed,
    ConfigurationConflict,
    InvalidRetainedDependencies,
    ResourceReservationRejected,
    EndpointInitializationRejected,
    EndpointRegistrationRejected,
    DiagnosticChannelInitializationRejected,
    DiagnosticChannelRegistrationRejected,
    UserMemoryInitializationRejected,
    LaunchPreparationRejected,
    PreparationPostconditionRejected,
    DispatchNotPrepared,
    DispatchAlreadyAttempted,
    ThreadReadyRejected,
    RunLoopBindingRejected,
    EnqueueRejected,
    LaunchRejected,
    DispatchResultRejected,
    DispatchPostconditionRejected,
};

enum class Ia64BspKernelRuntimeLocalTimerProbeState : Foundation::UInt8
{
    Fresh,
    Complete,
    Failed,
};

enum class Ia64BspKernelRuntimeLocalTimerProbeError : Foundation::UInt8
{
    None,
    RuntimeNotPrepared,
    UserMemoryNotPrepared,
    AlreadyAttempted,
    ExternalInterruptPublicationRejected,
    ExternalInterruptPublicationMismatch,
    ClockExhausted,
    DeadlineOverflow,
    TimerArmRejected,
    DeadlineRefreshRejected,
    PollRejected,
    TimerRollbackRejected,
    DeadlineRollbackRejected,
    TimerDeliveryRejected,
    NotificationClaimRejected,
    NotificationCommitRejected,
    EvidenceRejected,
    EvidenceAcknowledgementRejected,
    WaitPathRejected,
    PostconditionRejected,
};

enum class Ia64BspSharedRootShootdownState : Foundation::UInt8
{
    Fresh,
    Mapping,
    Complete,
    Failed,
};

enum class Ia64BspSharedRootShootdownError : Foundation::UInt8
{
    None,
    AlreadyAttempted,
    RuntimeNotPrepared,
    InvalidDependencies,
    InvalidPhysicalPage,
    InvalidProcessorProfile,
    RootRejected,
    RegistryCapacityExceeded,
    ResourceCapacityExceeded,
    MappingCapacityExceeded,
    VirtualRangeOccupied,
    PhysicalStorageAlreadyMapped,
    FrameInitializationRejected,
    MemoryObjectInitializationRejected,
    ObjectRegistrationRejected,
    RegistryBindingRejected,
    MappingPreparationRejected,
    AddressSpaceMappingRejected,
    ShootdownRejected,
    ShootdownReceiptMismatch,
    ShootdownStateMismatch,
    ShootdownTimeout,
    ShootdownSettlementRejected,
    MappingCompletionRejected,
    PostconditionRejected,
};

/// Register evidence captured by the bootstrap worker before it exits.
struct Ia64BspKernelWorkerEvidence
{
    Foundation::UInt64 StackPointer;
    Foundation::UInt64 RseBackingStorePointer;
    Foundation::UInt64 RseBackingStoreBase;
    Foundation::UInt64 RseNatCollection;
    Foundation::UInt64 RseConfiguration;
    Foundation::UInt64 ProcessorStatus;
    Foundation::UInt8 CaptureComplete;
    Foundation::UInt8 Reserved[7U];
};

/// Architecture-neutral TimerQueue-to-Notification proof published beside the
/// unchanged IA-64 external-entry evidence. Sequence is assigned last in the
/// retained normal-context transaction and is one only after every field and
/// final postcondition has been validated.
struct Ia64BspKernelRuntimeTimerQueueEvidence
{
    Foundation::UInt64 Sequence;
    Foundation::UInt64 TimerObjectIdentity;
    Foundation::UInt64 NotificationObjectIdentity;
    Foundation::UInt64 TimerArmSequence;
    Foundation::UInt64 DeadlineTicks;
    Foundation::UInt64 ObservedTicks;
    Foundation::UInt64 QueueClaimSequence;
    Foundation::UInt64 NotificationClaimSequence;
    Foundation::UInt8 InitialDeadlineState;
    Foundation::UInt8 SelectedDeadlineSources;
    Foundation::UInt8 InitialDeadlineReprogrammed;
    Foundation::UInt8 ClaimedTimerState;
    Foundation::UInt8 NotificationPendingAfterSignal;
    Foundation::UInt8 FinalTimerState;
    Foundation::UInt8 FinalNotificationState;
    Foundation::UInt8 FinalNotificationPending;
    Foundation::UInt8 FinalDeadlineState;
    Foundation::UInt8 FinalLocalTimerState;
    Foundation::UInt8 FinalLocalTimerArmed;
    Foundation::UInt8 FinalScheduledEntries;
    Foundation::UInt8 FinalPendingEntries;
    Foundation::UInt8 FinalClaimedEntries;
    Foundation::UInt8 Reserved[2U];
};

struct Ia64BspKernelRuntimePreparationSummary
{
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
    Kernel::Protection::ProtectionDomainId ProtectionDomainIdentity;
    Kernel::Scheduler::ThreadId ThreadIdentity;
    Kernel::Scheduler::SchedulingContextId SchedulingContextIdentity;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 TimerVector;
    Foundation::UInt8 Reserved[6U];
};

struct Ia64BspKernelRuntimeDispatchSummary
{
    Foundation::UInt64 ExitCode;
    Kernel::Scheduler::CpuId Processor;
    Ia64BspKernelRuntimeState State;
    Foundation::UInt8 Reserved[6U];
};

struct Ia64BspKernelRuntimeUserProbePreparationSummary
{
    Kernel::Scheduler::ThreadId ThreadIdentity;
    Kernel::Scheduler::CpuId Processor;
    Ia64BspKernelRuntimeUserProbeState State;
    Foundation::UInt8 Reserved[6U];
};

struct Ia64BspKernelRuntimeUserProbeDispatchSummary
{
    Foundation::UInt64 ExitCode;
    Kernel::Scheduler::CpuId Processor;
    Ia64BspKernelRuntimeUserProbeState State;
    Foundation::UInt8 Reserved[6U];
};

struct Ia64BspKernelRuntimeInitialUserImagePreparationSummary
{
    Kernel::Scheduler::ThreadId ThreadIdentity;
    Kernel::Scheduler::CpuId Processor;
    Ia64BspKernelRuntimeInitialUserImageState State;
    Foundation::UInt8 Reserved[6U];
};

struct Ia64BspKernelRuntimeInitialUserImageDispatchSummary
{
    Foundation::UInt64 ExitCode;
    Kernel::Scheduler::CpuId Processor;
    Ia64BspKernelRuntimeInitialUserImageState State;
    Foundation::UInt8 Reserved[6U];
};

struct Ia64BspKernelRuntimeLocalTimerProbeSummary
{
    Foundation::UInt64 ProgrammedDeadlineTicks;
    Foundation::UInt64 ObservedIntervalTimeCounter;
    Kernel::Scheduler::CpuId Processor;
    Ia64BspKernelRuntimeLocalTimerProbeState State;
    Foundation::UInt8 Reserved[6U];
};

struct Ia64BspSharedRootShootdownSummary
{
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
    Kernel::Memory::FrameId FrameIdentity;
    Kernel::Memory::MemoryObjectId MemoryObjectIdentity;
    Kernel::Memory::AddressSpaceMappingReceipt Mapping;
    Foundation::UInt64 ShootdownSequence;
    Foundation::ProcessorMask TargetProcessorMask;
    Foundation::ProcessorMask QueuedProcessorMask;
    Foundation::ProcessorMask AcknowledgedProcessorMask;
    Ia64BspSharedRootShootdownState State;
    Foundation::UInt8 Reserved[4U];
};

/// Retained compact diagnostics. The larger common controller and run-loop
/// failures remain in the owner so no public result copies them on IA-64's
/// bounded memory stack.
struct Ia64BspKernelRuntimeFailure
{
    Ia64BspKernelRuntimeError Error;
    Memory::Ia64BspRuntimeRootBorrowError RootBorrowError;
    Memory::Ia64LocalTlbInvalidatorInitializationError LocalInvalidator;
    Time::Ia64LocalTimerInitializationError LocalTimerError;
    Kernel::Time::TimerError TimerError;
    Kernel::Ipc::NotificationError NotificationError;
    Kernel::Ipc::WaitSetError WaitSetError;
    Kernel::Protection::ProtectionDomainError ProtectionDomainError;
    Kernel::Object::ObjectRegistryError ObjectRegistryError;
    Kernel::Object::ObjectPinError ObjectPinError;
    Kernel::Scheduler::ThreadError ThreadError;
    Kernel::Scheduler::SchedulingContextError SchedulingContextError;
    Ia64KernelContextError NativeContextError;
    Kernel::Scheduler::ProcessorKernelRunLoopError RunLoopError;
    Kernel::Scheduler::ProcessorRunError ControllerError;
    Kernel::Scheduler::ProcessorSchedulerError SchedulerError;
    Kernel::Time::ProcessorDeadlineError DeadlineError;
    Kernel::Memory::ProcessorAddressSpaceControllerError AddressSpaceControllerError;
    Memory::Ia64BspRuntimeSystemCallPublicationError SystemCallPublicationError;
    Memory::Ia64BspRuntimeExternalPreemptionError ExternalPreemptionStorageError;
    Interrupt::Ia64ExternalPreemptionControlError ExternalPreemptionControlError;
    Interrupt::Ia64ExternalPreemptionFrameError ExternalPreemptionFrameError;
    Ia64ExternalFrameIdleContinuationError ExternalPreemptionContinuationError;
};

/// Retained target evidence for the terminal BSP native-preemption proof.
/// Sequence becomes one only in the fresh idle landing after the resumed
/// worker has exited through the common run loop.
struct Ia64BspNativePreemptionEvidence
{
    Foundation::UInt64 Sequence;
    Foundation::UInt64 FrameToken;
    Foundation::UInt64 ProgrammedDeadlineTicks;
    Foundation::UInt64 ObservedTicks;
    Foundation::UInt64 InterruptedInstructionPointer;
    Foundation::UInt64 InterruptedProcessorStatus;
    Foundation::UInt64 StoppedRunSequence;
    Foundation::UInt64 AdmissionRunSequence;
    Foundation::UInt64 AdmissionDeadlineTicks;
    Foundation::UInt64 IdleContinuationGeneration;
    Foundation::UInt64 CaptureCount;
    Foundation::UInt64 RestoreCount;
};

/// Retained user-probe diagnostics. Public probe operations return only the
/// compact runtime error so this larger common-mapping record is never copied
/// through the selected worker's bounded memory stack.
struct Ia64BspKernelRuntimeUserProbeFailure
{
    Foundation::UInt64 ExitCode;
    Ia64UserExecutionProbeFailure Probe;
    Ia64BspKernelRuntimeUserProbeError Error;
    Memory::Ia64LocalTlbInvalidatorInitializationError LocalInvalidator;
    Kernel::Object::RevocationError Revocation;
    Kernel::Object::CapabilityError Capability;
    Ia64UserModeLaunchError Launch;
    Kernel::Scheduler::ThreadError Thread;
    Kernel::Scheduler::ProcessorKernelRunLoopError RunLoop;
    Kernel::Scheduler::ProcessorRunError Controller;
    Kernel::Scheduler::ProcessorSchedulerError Scheduler;
    Kernel::Time::ProcessorDeadlineError Deadline;
    Kernel::Memory::ProcessorAddressSpaceControllerError AddressSpaceController;
    Kernel::Memory::DirectUserMemoryAccessError UserMemory;
    Kernel::Object::ObjectRegistryError EndpointRegistry;
    Kernel::Object::ObjectPinError EndpointPin;
    Kernel::Object::ObjectRegistryError WaitSetRegistry;
    Kernel::Object::ObjectPinError WaitSetPin;
    Kernel::Ipc::WaitSetFailure WaitSet;
    Kernel::Time::DeferredTimerWorkerFailure DeferredTimer;
    Kernel::Scheduler::ProcessorRunFailure WaitController;
    Kernel::Ipc::WaitSetThreadDispatcherFailure WaitDispatcher;
    Kernel::System::WaitSetSystemCallIngressFailure WaitIngress;
    Interrupt::Ia64ExternalInterruptPollStatus WaitPoll;
    Interrupt::Ia64LocalTimerInterruptEvidenceValidationError WaitEvidenceValidation;
    Memory::Ia64BspRuntimeExternalInterruptAcknowledgementError WaitAcknowledgement;
    System::Ia64SystemCallWaitContinuationError WaitContinuation;
    Kernel::Ipc::MessageCodecError BootstrapCodec;
    Kernel::Memory::UserMemoryRangeError BootstrapRange;
    Kernel::Memory::UserMemoryAccessError BootstrapAccess;
};

struct Ia64BspKernelRuntimeInitialUserImageFailure
{
    Foundation::UInt64 ExitCode;
    Ia64InitialUserImageLaunchFailure LaunchOwner;
    Ia64BspKernelRuntimeInitialUserImageError Error;
    Ia64UserModeLaunchError Launch;
    Kernel::Scheduler::ThreadError Thread;
    Kernel::Scheduler::ProcessorKernelRunLoopError RunLoop;
    Kernel::Scheduler::ProcessorRunError Controller;
    Kernel::Scheduler::ProcessorSchedulerError Scheduler;
    Kernel::Time::ProcessorDeadlineError Deadline;
    Kernel::Memory::ProcessorAddressSpaceControllerError AddressSpaceController;
    Kernel::Memory::DirectUserMemoryAccessError UserMemory;
    Kernel::Resource::ResourceDomainError ResourceDomain;
    Kernel::Object::ObjectPinError ResourceDomainPin;
    Kernel::Object::ObjectRegistryError EndpointRegistry;
    Kernel::Object::ObjectPinError EndpointPin;
    Kernel::Object::ObjectRegistryError DiagnosticChannelRegistry;
    Kernel::Object::ObjectPinError DiagnosticChannelPin;
    Kernel::Object::ObjectPinError UserMemoryPin;
};

struct Ia64BspKernelRuntimeLocalTimerProbeFailure
{
    Foundation::UInt64 InitialTicks;
    Foundation::UInt64 ProgrammedDeadlineTicks;
    Foundation::UInt64 AbsoluteTimeoutTicks;
    Interrupt::Ia64ExternalInterruptPollStatus PollStatus;
    Kernel::Time::TimerQueueFailure TimerQueue;
    Kernel::Scheduler::ProcessorRunFailure Controller;
    Kernel::Time::TimerNotificationDeliveryFailure Delivery;
    Kernel::Ipc::NotificationFailure Notification;
    Ia64BspKernelRuntimeLocalTimerProbeError Error;
    Memory::Ia64BspRuntimeExternalInterruptPublicationError Publication;
    Memory::Ia64RuntimeExternalInterruptPublicationError InterruptVectorTable;
    Interrupt::Ia64LocalTimerInterruptEvidenceValidationError EvidenceValidation;
    Memory::Ia64BspRuntimeExternalInterruptAcknowledgementError Acknowledgement;
    Foundation::UInt8 Reserved[3U];
};

struct Ia64BspSharedRootShootdownFailure
{
    Kernel::Memory::AddressSpaceFailure AddressSpace;
    Kernel::Memory::TlbShootdownFailure Shootdown;
    Kernel::Memory::MemoryObjectFailure MemoryObject;
    Ia64BspSharedRootShootdownError Error;
    Kernel::Memory::FrameError Frame;
    Kernel::Object::ObjectRegistryError Registry;
    Kernel::Object::ObjectPinError Pin;
    Kernel::Memory::PinnedFrameMappingError MappingPreparation;
    Kernel::Memory::AddressSpaceError Settlement;
    Foundation::UInt8 Reserved[3U];
};

/// Boot-lifetime owner for the BSP's one cooperative kernel bootstrap worker. Placement and
/// lifetime: - The complete object starts on a 4-KiB boundary in boot-lifetime storage, remains
/// immovable, and is never destroyed after Prepare accepts a root. - The borrowed BSP
/// runtime-memory owner, registry, its synchronization, issuer, ResourceDomain, and every
/// registered generation outlive this object and all continuations. The owned revocation registry
/// and initially empty capability space share that synchronization and this object's local timer. -
/// The exact borrowed root maps this complete object read/write at its numerical address. The
/// worker stack and RSE store are separate embedded 16-KiB, page-aligned ranges. Serialization: -
/// The non-preemptible BSP calls every operation with interruption delivery disabled and excludes
/// migration, ISR use, re-entry, and concurrent access. Allocation: - Does not allocate.
/// ProcessorRunController and Ia64ProcessorRunLoop are placement-constructed in retained inline
/// storage after root borrowing.
class alignas(Kernel::Memory::MappingPageSizeBytes) Ia64BspKernelRuntime final : public System::IIa64UserPreemption
{
public:
    /// Preconditions:
    /// - objectRegistrySynchronization is the exact SpinLock retained by
    /// objectRegistry and outlives this runtime.
    /// - capabilityValidationKey is this boot's BootstrapSpace key
    /// (DeriveCapabilityValidationKey), never a constant.
    Ia64BspKernelRuntime(Kernel::Object::ObjectRegistry& objectRegistry,
                         Platform::Synchronization::SpinLock& objectRegistrySynchronization,
                         const Kernel::Object::CapabilityValidationKey& capabilityValidationKey) noexcept;

    /// Safe only before any Prepare call has passed its read-only preflight.
    ~Ia64BspKernelRuntime() noexcept = default;

    Ia64BspKernelRuntime(const Ia64BspKernelRuntime&) = delete;
    Ia64BspKernelRuntime& operator=(const Ia64BspKernelRuntime&) = delete;
    Ia64BspKernelRuntime(Ia64BspKernelRuntime&&) = delete;
    Ia64BspKernelRuntime& operator=(Ia64BspKernelRuntime&&) = delete;

    /// Initializes and registers the user-capable scheduling objects but leaves
    /// the empty authority space unchanged and the worker Dormant and unqueued.
    /// Every accepted failure is terminal and retains all objects already
    /// registered; no rollback or retry occurs.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA
        Foundation::Result<Ia64BspKernelRuntimePreparationSummary, Ia64BspKernelRuntimeError>
        Prepare(Memory::Ia64BspRuntimeMemory& runtimeMemory,
                Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                Kernel::Resource::ResourceDomain& resourceDomain, Kernel::Object::RootCapabilityIssuer& issuer,
                const Memory::PalVirtualMemoryCapabilities& palVirtualMemoryCapabilities,
                Foundation::UInt8 timerVector) noexcept;

    /// Installs one already-staged probe into the exact root and exact Thread
    /// prepared above, mints its Inspect capability into r4/r5 (r6/r7 stay
    /// zero), constructs one self-receiving Endpoint with Invoke and Read
    /// authority, and stages its byte-oriented bootstrap record into the
    /// mapped user stack on native IA-64. This is a distinct terminal
    /// configuration path and does not make the worker runnable. The probe is
    /// retained by non-owning boot-lifetime reference after any accepted
    /// mapping or registration side effect.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA
        Foundation::Result<Ia64BspKernelRuntimeUserProbePreparationSummary, Ia64BspKernelRuntimeUserProbeError>
        PrepareUserExecutionProbe(Ia64UserExecutionProbe& userExecutionProbe) noexcept;

    /// Derives the real embedded image's launch request from its retained
    /// object and mapping owners, constructs one self-receiving boot Endpoint
    /// and one DiagnosticChannel, publishes the complete pinned image mapping
    /// set to user-copy ingress, and issues one Inspect/Invoke/Read Endpoint
    /// capability in r4/r5 and one Inspect/Write DiagnosticChannel capability
    /// in r6/r7, each in its own revocation root. This path and the synthetic
    /// user probe are mutually exclusive. It neither makes the worker runnable
    /// nor changes the installed image mappings.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Foundation::Result<Ia64BspKernelRuntimeInitialUserImagePreparationSummary,
                                                            Ia64BspKernelRuntimeInitialUserImageError>
    PrepareInitialUserImage(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
                            const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept;

    /// Installs one boot-lifetime supervisor read/write page into the fixed
    /// shared root after every profiled processor is resident and serving its
    /// bounded idle loop. The caller supplies the exact shared remote-work
    /// transport. Every accepted attempt is terminal: success retains the
    /// mapping and pins, while any failure retains all accepted transaction
    /// state and never retries, removes, remaps, or reclaims the page.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA
        Foundation::Result<Ia64BspSharedRootShootdownSummary, Ia64BspSharedRootShootdownError>
        MapSharedRootShootdownPage(const Kernel::Memory::PhysicalPageRange& physicalPage,
                                   Foundation::UInt8 profiledProcessorMask,
                                   Hal::Memory::ITlbShootdownTransport& transport) noexcept;

    /// Publishes the BSP external-interrupt context and proves one native
    /// local-ITM delivery while the prepared user probe is still dormant.
    /// The caller enters with PSR.i clear and may make only one accepted
    /// attempt. Hosted execution retains NativeUnavailable diagnostics and
    /// leaves the prepared dispatch path usable.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA
        Foundation::Result<Ia64BspKernelRuntimeLocalTimerProbeSummary, Ia64BspKernelRuntimeLocalTimerProbeError>
        RunLocalTimerInterruptProbe() noexcept;

    /// Makes the prepared worker Ready, binds and enqueues it, and performs
    /// one real common-run-loop native switch to the worker and back to idle.
    /// Success is returned only after all retained postconditions are exact.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA
        Foundation::Result<Ia64BspKernelRuntimeDispatchSummary, Ia64BspKernelRuntimeError>
        DispatchBootstrapWorker() noexcept;

    /// Executes the opt-in BSP-only native timer-preemption proof. Native
    /// success never returns here: the raw interrupt/coordinator call chain
    /// unwinds before restore, while its fresh dedicated idle continuation
    /// replaces the cooperative bootstrap dispatch continuation.
    /// Exact rfi resumes the sentinel, and the worker later switches to that
    /// dedicated idle landing. That landing validates the restored state,
    /// records the proof in the kernel evidence record, reports the
    /// NATIVE-PREEMPTION-RECORDED stage, and halts. Hosted and unsupported
    /// targets return NativePreemptionUnavailable
    /// without borrowing or publishing a corridor.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA
        Foundation::Result<Ia64BspKernelRuntimeDispatchSummary, Ia64BspKernelRuntimeError>
        DispatchNativeExternalPreemptionProbe() noexcept;

    /// P64 bridge targets used only by the configured native corridor. The
    /// global C ABI adapters pass this exact boot-lifetime runtime as argument.
    /// Coordinator returns zero only after common/frame/control commit; the
    /// assembly caller has unwound every C++ frame before entering restore.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Foundation::UInt64 CoordinateNativeExternalPreemption() noexcept;

    [[noreturn]] ASTERIA_NOINLINE_NOIPA void LandNativeExternalPreemptionIdle() noexcept;

    /// Selects the same real worker through the common run loop, then invokes
    /// the resumable CPL3 launch from RunWorker. Query/send/receive return
    /// directly to PL3; Wait restores this worker, blocks it, and later resumes
    /// the retained syscall after exact wake/readmission. Architecturally
    /// defined pre-commit launch rejection exits the worker cooperatively and
    /// is returned as a retained terminal failure.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA
        Foundation::Result<Ia64BspKernelRuntimeUserProbeDispatchSummary, Ia64BspKernelRuntimeUserProbeError>
        DispatchUserExecutionProbe() noexcept;

    /// The collaborators the boot pager borrows after PrepareInitialUserImage (System:
    /// Ia64PagerRuntime). Prepared only.
    [[nodiscard]] System::Ia64PagerCollaborators GetPagerCollaborators() noexcept;
    void BindShootdownMailbox(Kernel::Memory::TlbShootdownMailboxTransport& mailbox) noexcept;

    /// Attaches the initialized boot pager, which DispatchInitialUserImage
    /// admits ahead of the initial thread so it waits for faults first.
    [[nodiscard]] bool AttachPager(System::Ia64PagerRuntime& pager) noexcept;

    /// The collaborators the created-thread table borrows. Prepared only.
    [[nodiscard]] System::Ia64UserThreadTableCollaborators GetThreadTableCollaborators() noexcept;

    /// Attaches the configured created-thread table: its threads' ticks end
    /// their runs through it, and the initial-image dispatch's idle loop
    /// reaps it.
    [[nodiscard]] bool AttachThreads(System::Ia64UserThreadTable& threads) noexcept;

    /// Attaches the INTx lines routed to drivers' Interrupts: the idle loops, which run with PSR.i
    /// clear, dispatch the device vectors that pend meanwhile.
    [[nodiscard]] bool AttachDeviceInterrupts(Interrupt::Ia64InterruptLines& lines) noexcept;

    /// A boot thread's PL3 timer tick (IIa64UserPreemption): acknowledges
    /// the deadline AsteriaIa64UserInterruptClaim masked, progresses the
    /// pager like idle, refreshes the active run's deadline and, when it
    /// fired, ends the run Ready on the thread's own context, then selects
    /// the domain's entry contexts again.
    [[nodiscard]] bool PreemptUser(Memory::Ia64BspRuntimeUserDomain domain) noexcept override;

    /// The same for a created thread's tick.
    [[nodiscard]] bool PreemptThread(Foundation::Size slot) noexcept override;

    /// A boot thread's fault deferred behind another: ends its run Ready and
    /// selects its entry contexts again once it runs.
    [[nodiscard]] bool YieldUser(Memory::Ia64BspRuntimeUserDomain domain) noexcept override;

    /// Selects the prepared BSP worker and enters the real initial image at
    /// PL3. Native success does not return; a defined pre-commit rejection is
    /// retained and returned after the worker exits cooperatively.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Foundation::Result<Ia64BspKernelRuntimeInitialUserImageDispatchSummary,
                                                            Ia64BspKernelRuntimeInitialUserImageError>
    DispatchInitialUserImage() noexcept;

    [[nodiscard]] constexpr Ia64BspKernelRuntimeState GetState() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] constexpr const Ia64BspKernelRuntimeFailure& GetFailure() const noexcept
    {
        return m_failure;
    }

    [[nodiscard]] constexpr const Ia64BspKernelWorkerEvidence& GetWorkerEvidence() const noexcept
    {
        return m_workerEvidence;
    }

    [[nodiscard]] constexpr const Ia64BspNativePreemptionEvidence& GetNativeExternalPreemptionEvidence() const noexcept
    {
        return m_nativePreemptionEvidence;
    }

    [[nodiscard]] constexpr Ia64BspKernelRuntimeUserProbeState GetUserExecutionProbeState() const noexcept
    {
        return m_userProbeState;
    }

    [[nodiscard]] constexpr const Ia64BspKernelRuntimeUserProbeFailure& GetUserExecutionProbeFailure() const noexcept
    {
        return m_userProbeFailure;
    }

    [[nodiscard]] constexpr Ia64BspKernelRuntimeInitialUserImageState GetInitialUserImageState() const noexcept
    {
        return m_initialUserImageState;
    }

    [[nodiscard]] constexpr const Ia64BspKernelRuntimeInitialUserImageFailure&
    GetInitialUserImageFailure() const noexcept
    {
        return m_initialUserImageFailure;
    }

    [[nodiscard]] constexpr const Ia64InitialUserImageLaunchOwner& GetInitialUserImageLaunchOwner() const noexcept
    {
        return m_initialUserImageLaunchOwner;
    }

    [[nodiscard]] constexpr Ia64BspKernelRuntimeLocalTimerProbeState GetLocalTimerInterruptProbeState() const noexcept
    {
        return m_localTimerProbeState;
    }

    [[nodiscard]] constexpr const Ia64BspKernelRuntimeLocalTimerProbeSummary&
    GetLocalTimerInterruptProbeSummary() const noexcept
    {
        return m_localTimerProbeSummary;
    }

    [[nodiscard]] constexpr const Ia64BspKernelRuntimeLocalTimerProbeFailure&
    GetLocalTimerInterruptProbeFailure() const noexcept
    {
        return m_localTimerProbeFailure;
    }

    [[nodiscard]] constexpr Ia64BspSharedRootShootdownState GetSharedRootShootdownState() const noexcept
    {
        return m_sharedRootShootdownState;
    }

    [[nodiscard]] constexpr const Ia64BspSharedRootShootdownSummary& GetSharedRootShootdownSummary() const noexcept
    {
        return m_sharedRootShootdownSummary;
    }

    [[nodiscard]] constexpr const Ia64BspSharedRootShootdownFailure& GetSharedRootShootdownFailure() const noexcept
    {
        return m_sharedRootShootdownFailure;
    }

    [[nodiscard]] constexpr const Interrupt::Ia64LocalTimerInterruptEvidence&
    GetLocalTimerInterruptEvidence() const noexcept
    {
        return m_localTimerProbeEvidence;
    }

    [[nodiscard]] constexpr const Ia64BspKernelRuntimeTimerQueueEvidence&
    GetLocalTimerInterruptTimerQueueEvidence() const noexcept
    {
        return m_localTimerProbeTimerQueueEvidence;
    }

    [[nodiscard]] constexpr Kernel::Time::TimerId GetLocalTimerInterruptTimerIdentity() const noexcept
    {
        return m_localTimerProbeTimerIdentity;
    }

    [[nodiscard]] constexpr Kernel::Ipc::NotificationId GetLocalTimerInterruptNotificationIdentity() const noexcept
    {
        return m_localTimerProbeNotificationIdentity;
    }

    /// Returns the exact retained endpoint authority for boot diagnostics.
    /// The owning CapabilitySpace remains the authority-validation boundary.
    [[nodiscard]] constexpr const Kernel::Object::CapabilityHandle&
    GetUserExecutionProbeEndpointCapability() const noexcept
    {
        return m_userProbeEndpointCapability;
    }

    [[nodiscard]] constexpr const Kernel::Object::CapabilityHandle&
    GetUserExecutionProbeWaitSetCapability() const noexcept
    {
        return m_userProbeWaitSetCapability;
    }

    /// Returns the exact retained record used for native stack publication.
    [[nodiscard]] constexpr const Ia64UserExecutionProbeIpcBootstrapRecord&
    GetUserExecutionProbeIpcBootstrapRecord() const noexcept
    {
        return m_userProbeBootstrapRecord;
    }

private:
    using PreparationResult = Foundation::Result<Ia64BspKernelRuntimePreparationSummary, Ia64BspKernelRuntimeError>;
    using DispatchResult = Foundation::Result<Ia64BspKernelRuntimeDispatchSummary, Ia64BspKernelRuntimeError>;
    using UserProbePreparationResult =
        Foundation::Result<Ia64BspKernelRuntimeUserProbePreparationSummary, Ia64BspKernelRuntimeUserProbeError>;
    using UserProbeDispatchResult =
        Foundation::Result<Ia64BspKernelRuntimeUserProbeDispatchSummary, Ia64BspKernelRuntimeUserProbeError>;
    using InitialUserImagePreparationResult = Foundation::Result<Ia64BspKernelRuntimeInitialUserImagePreparationSummary,
                                                                 Ia64BspKernelRuntimeInitialUserImageError>;
    using InitialUserImageDispatchResult = Foundation::Result<Ia64BspKernelRuntimeInitialUserImageDispatchSummary,
                                                              Ia64BspKernelRuntimeInitialUserImageError>;
    using LocalTimerProbeResult =
        Foundation::Result<Ia64BspKernelRuntimeLocalTimerProbeSummary, Ia64BspKernelRuntimeLocalTimerProbeError>;
    using SharedRootShootdownResult =
        Foundation::Result<Ia64BspSharedRootShootdownSummary, Ia64BspSharedRootShootdownError>;

    enum class WorkerPurpose : Foundation::UInt8
    {
        None,
        BootstrapRoundTrip,
        NativeExternalPreemption,
        UserExecutionProbe,
        InitialUserImage,
    };

    [[nodiscard]] PreparationResult FailPreparation(Ia64BspKernelRuntimeError error) noexcept;
    [[nodiscard]] DispatchResult FailDispatch(Ia64BspKernelRuntimeError error) noexcept;
    [[nodiscard]] UserProbePreparationResult
    FailUserProbePreparation(Ia64BspKernelRuntimeUserProbeError error) noexcept;
    [[nodiscard]] UserProbeDispatchResult FailUserProbeDispatch(Ia64BspKernelRuntimeUserProbeError error) noexcept;
    [[nodiscard]] InitialUserImagePreparationResult
    FailInitialUserImagePreparation(Ia64BspKernelRuntimeInitialUserImageError error, bool terminal) noexcept;
    [[nodiscard]] InitialUserImageDispatchResult
    FailInitialUserImageDispatch(Ia64BspKernelRuntimeInitialUserImageError error) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspKernelRuntimeInitialUserImageError
    PrepareInitialUserImageLaunchOwner(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
                                       const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspKernelRuntimeInitialUserImageError
    CaptureInitialUserImageMemoryMappings(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
                                          const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspKernelRuntimeInitialUserImageError CaptureInitialUserImageMemoryMapping(
        Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
        const Executable::Ia64InitialUserImageMappingOwner& mappingOwner,
        const Executable::Ia64InitialUserImageMemorySummary& memorySummary,
        const Executable::Ia64InitialUserImageMappingSummary& mappingSummary, Foundation::Size index,
        Foundation::UInt64& previousEnd, Kernel::Memory::Frame*& aggregateFrame,
        Kernel::Memory::DirectUserMemoryMapping& directMapping) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidateInitialUserImageMappingObjects(const Executable::Ia64InitialUserImageMemoryRangeView& range,
                                           const Executable::Ia64InitialUserImageMemorySummary& memorySummary) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspKernelRuntimeInitialUserImageError
    ReserveInitialUserImageResources() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateInitialUserImageResourceDomainRegistration() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateInitialUserImageResourceReservation() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspKernelRuntimeInitialUserImageError
    InitializeInitialUserImageEndpoint() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspKernelRuntimeInitialUserImageError
    InitializeInitialUserImageDiagnosticChannel() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool IsInitialUserImageDiagnosticChannelRegistered() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspKernelRuntimeInitialUserImageError
    InitializeInitialUserImageMemory() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateInitialUserImageIpcPostconditions() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateInitialUserImageDiagnosticChannelPostconditions() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspKernelRuntimeInitialUserImageError
    RetainInitialUserImageLaunchRequest() noexcept;
    [[nodiscard]] LocalTimerProbeResult FailLocalTimerProbe(Ia64BspKernelRuntimeLocalTimerProbeError error) noexcept;
    [[nodiscard]] SharedRootShootdownResult FailSharedRootShootdown(Ia64BspSharedRootShootdownError error) noexcept;
    [[nodiscard]] SharedRootShootdownResult
    FailPendingSharedRootShootdown(Ia64BspSharedRootShootdownError error) noexcept;
    [[nodiscard]] bool ValidateExternalInterruptPublication(
        const Memory::Ia64BspRuntimeExternalInterruptPublicationSnapshot& publication) const noexcept;
    /// Copies the committed first-interrupt and TimerQueue evidence into
    /// the kernel evidence record (Ia64UserProbeEvidence).
    void RecordLocalTimerProbeEvidence() const noexcept;
    [[nodiscard]] bool ValidateLocalTimerProbeCommitPostconditions() const noexcept;
    [[nodiscard]] bool ValidateLocalTimerProbeObjects() const noexcept;
    [[nodiscard]] bool InitializeLocalTimerProbeObjects(Kernel::Object::ResourceDomainId chargeOwner,
                                                        Kernel::Object::RootCapabilityIssuer& issuer) noexcept;
    [[nodiscard]] bool InitializeUserProbeWaitSet(Kernel::Object::ResourceDomainId chargeOwner,
                                                  Kernel::Object::RootCapabilityIssuer& issuer) noexcept;
    [[nodiscard]] bool HasPossiblyAcceptedLocalTimerProbeArm() const noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool RefreshLocalTimerProbeRollbackDeadline() noexcept;
    [[nodiscard]] bool RollbackLocalTimerProbeBeforeCompletion() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateLocalTimerProbeDeliveryPostconditions() const noexcept;
    [[nodiscard]] bool DeliverLocalTimerProbeExpiration() noexcept;
    [[nodiscard]] bool ConsumeLocalTimerProbeNotification() noexcept;
    [[nodiscard]] bool RefreshEmptyLocalTimerProbeDeadline() noexcept;
    [[nodiscard]] bool PrepareTimerQueueEvidence() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareLocalTimerProbePublication() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    RefreshLocalTimerProbeInitialDeadline(Foundation::UInt64 deadlineTicks) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareLocalTimerProbeAttempt() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool CompleteLocalTimerProbeAttempt() noexcept;
    [[nodiscard]] bool ValidatePlacementAndMapping() const noexcept;
    [[nodiscard]] bool ValidateBorrowedRoot() const noexcept;
    [[nodiscard]] bool ValidateBorrowedInitialComponentRoot() const noexcept;
    [[nodiscard]] bool SwitchToInitialComponentRoot() noexcept;
    [[nodiscard]] bool SwitchToCommonRoot() noexcept;
    [[nodiscard]] bool ValidateResourceDomain(Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                              Kernel::Resource::ResourceDomain& resourceDomain) noexcept;
    [[nodiscard]] bool RegistrationMatches(Kernel::Object::ObjectId identity, Kernel::Object::ObjectType type,
                                           Kernel::Object::IKernelObject& storage) noexcept;
    [[nodiscard]] bool InitializeProtectionDomain(Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                  Kernel::Resource::ResourceDomain& resourceDomain,
                                                  Kernel::Object::RootCapabilityIssuer& issuer) noexcept;
    [[nodiscard]] bool InitializeThread(Kernel::Object::ResourceDomainId chargeOwner,
                                        Kernel::Object::RootCapabilityIssuer& issuer) noexcept;
    [[nodiscard]] bool InitializeSchedulingContext(Kernel::Object::ResourceDomainId chargeOwner,
                                                   Kernel::Object::RootCapabilityIssuer& issuer) noexcept;
    [[nodiscard]] bool InitializeNativePreemptionSchedulingContext() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ResolveNativePreemptionDescriptors() noexcept;
    [[nodiscard]] bool PrepareNativePreemptionCorridor() noexcept;
    [[nodiscard]] bool ArmNativePreemptionSentinel() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool
    ValidateNativePreemptionCapture(Interrupt::Ia64ExternalPreemptionFrame& frame, Foundation::UInt64& frameToken,
                                    Foundation::UInt64& stoppedRunSequence) noexcept;
    ASTERIA_NOINLINE_NOIPA void RefreshNativePreemptionControlSnapshot() noexcept;
    [[nodiscard]] bool ValidateNativePreemptionResume() noexcept;
    [[noreturn]] void FinishNativePreemptionWorker() noexcept;
    [[nodiscard]] bool PrepareWorkerContext(Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept;
    [[nodiscard]] bool ValidateUserProbePlacement(const Ia64UserExecutionProbe& userExecutionProbe) const noexcept;
    [[nodiscard]] bool ValidateSoleResidentRoot() const noexcept;
    [[nodiscard]] bool ConstructTlbShootdownDispatcher() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspSharedRootShootdownError PreflightSharedRootShootdown(
        const Kernel::Memory::PhysicalPageRange& physicalPage, Foundation::UInt8 profiledProcessorMask) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspSharedRootShootdownError
    InitializeSharedRootShootdownObjects(const Kernel::Memory::PhysicalPageRange& physicalPage) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspSharedRootShootdownError RegisterSharedRootShootdownObjects() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspSharedRootShootdownError PrepareSharedRootShootdownMapping() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspSharedRootShootdownError
    DispatchSharedRootShootdown(Hal::Memory::ITlbShootdownTransport& transport) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64BspSharedRootShootdownError WaitForSharedRootShootdown() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateSharedRootShootdownPostconditions() noexcept;
    [[nodiscard]] bool PrepareUserProbeLaunchRecord(Ia64UserExecutionProbe& userExecutionProbe) noexcept;
    [[nodiscard]] bool PrepareUserProbeCapability() noexcept;
    [[nodiscard]] bool InitializeUserProbeEndpoint() noexcept;
    [[nodiscard]] bool PrepareUserProbeWaitSetCapability() noexcept;
    [[nodiscard]] bool InitializeSystemCallWaitContinuation() noexcept;
    [[nodiscard]] bool PrepareUserProbeWaitPath() noexcept;
    [[nodiscard]] bool ArmUserProbeWaitTimer() noexcept;
    /// Copies the validated wait-timer interrupt evidence into the kernel
    /// evidence record (Ia64UserProbeEvidence).
    void RecordUserProbeWaitTimerEvidence() const noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ValidateUserProbeWaitSuspension() const noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PollUserProbeWaitInterrupt() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool DeliverUserProbeWaitTimer() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool RefreshUserProbeWaitDeadline() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool WakeUserProbeWaitThread() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool CompleteUserProbeWaitFromIdle() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool ConsumeUserProbeWaitCompletion() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PrepareUserProbeSystemCallResume() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA System::Ia64SystemCallWaitSupervisorReceipt
    ResumeUserProbeSystemCall() noexcept;
    [[nodiscard]] bool InitializeUserMemory() noexcept;
    [[nodiscard]] bool PrepareUserProbeBootstrap() noexcept;
    [[nodiscard]] bool ValidateRetainedUserProbeLaunch() const noexcept;
    [[nodiscard]] bool ValidateUserMemoryMappingLedger() const noexcept;
    [[nodiscard]] bool ValidateUserMemoryPostconditions() const noexcept;
    [[nodiscard]] bool ValidateUserProbeBootstrapRecord() const noexcept;
    [[nodiscard]] bool ValidateUserProbeIpcPostconditions() noexcept;
    void ConstructControllerAndRunLoop() noexcept;
    [[nodiscard]] bool ValidatePreparationPostconditions() const noexcept;
    [[nodiscard]] bool ValidatePreparedObjects() const noexcept;
    [[nodiscard]] bool ValidatePreparedDispatchers() const noexcept;
    [[nodiscard]] bool BindWorker() noexcept;
    [[nodiscard]] bool EnqueueWorker() noexcept;
    [[nodiscard]] bool DispatchWorkerFromIdle() noexcept;
    [[nodiscard]] bool
    ValidateExpectedDispatchFailure(const Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) const noexcept;
    [[nodiscard]] bool ValidateDispatchOwnerState(Foundation::UInt64 expectedExitCode) const noexcept;
    [[nodiscard]] bool ValidateUserProbeDispatchOwnerState() const noexcept;
    void RetainUserProbeDispatchFailure() noexcept;
    [[nodiscard]] bool ValidateInitialUserImageDispatchOwnerState() const noexcept;
    void RetainInitialUserImageDispatchFailure() noexcept;
    [[nodiscard]] static bool IsRecoverableUserProbeLaunchError(Ia64UserModeLaunchError error) noexcept;
    [[nodiscard]] bool ValidateWorkerEvidence() const noexcept;
    [[nodiscard]] Kernel::Scheduler::ProcessorRunController& Controller() noexcept;
    [[nodiscard]] const Kernel::Scheduler::ProcessorRunController& Controller() const noexcept;
    [[nodiscard]] Ia64ProcessorRunLoop& RunLoop() noexcept;
    [[nodiscard]] const Ia64ProcessorRunLoop& RunLoop() const noexcept;
    /// The active run's deadline refresh at a user tick: 1 when its deadline
    /// fired, 0 when it was re-armed, -1 on a controller failure.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA int RefreshUserRunStop() noexcept;
    /// The scheduling point of a PL3 tick of the thread on entry slot `slot`.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool PreemptScheduledUser(Memory::Ia64BspRuntimeEntrySlot slot) noexcept;
    /// Ends the running thread's run Ready; returns once it runs again.
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA bool FinishUserRun(Memory::Ia64BspRuntimeEntrySlot slot) noexcept;
    /// Writes ASTERIA:IA64-USER-TICK:FAIL:<stage> and returns false.
    [[nodiscard]] static bool RejectUserTick(Foundation::UInt8 stage) noexcept;
    /// Writes an unexpected idle dispatch failure as a diagnostic line: the
    /// run-loop, controller and scheduler errors, the deadline receipt's
    /// state and sources, and whether the run is retained, one byte each.
    static void ReportDispatchFailure(const Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept;
    /// Whether an idle dispatch found no runnable thread because every Ready
    /// one has spent its budget until the armed scheduler-retry deadline.
    [[nodiscard]] static bool
    IsReplenishmentWait(const Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept;
    /// Polls the ITC until deadlineTicks, or until the pager's timed wait is due if that comes
    /// first, or until a device interrupt was dispatched; false if a replenishment deadline lies
    /// further ahead than any can.
    [[nodiscard]] bool WaitForReplenishmentFromIdle(Foundation::UInt64 deadlineTicks) noexcept;
    /// What an idle wait with nothing runnable ended on.
    enum class IdleWait : Foundation::UInt8
    {
        /// Nothing can end it: no timed wait and no Interrupt bound.
        None,
        /// The pager's or a created thread's timed wait fell due.
        Deadline,
        /// A device interrupt was dispatched.
        DeviceInterrupt,
    };
    /// Polls the ITC until the pager's timed wait is due, dispatching device
    /// interrupts meanwhile, and returns early once one was dispatched;
    /// without a timed wait, waits for a device interrupt while an Interrupt
    /// is bound. A wait timeout is at most 0xFFFF'FFFF microseconds away.
    [[nodiscard]] IdleWait WaitForPagerDeadlineFromIdle() noexcept;
    /// Dispatches the device vectors pending at idle; whether any was. Another vector fail-stops.
    [[nodiscard]] bool PollDeviceInterruptsFromIdle() noexcept;
    /// Whether an idle dispatch failed only because a deadline fell due
    /// before it could be programmed or the admitted run entered.
    [[nodiscard]] static bool IsLateDispatch(const Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept;
    [[noreturn]] ASTERIA_NOINLINE_NOIPA static void WorkerEntry(void* argument) noexcept;
    [[noreturn]] ASTERIA_NOINLINE_NOIPA void RunInitialUserImageWorker() noexcept;
    [[noreturn]] ASTERIA_NOINLINE_NOIPA void RunWorker() noexcept;

    Kernel::Object::ObjectRegistry& m_objectRegistry;
    Time::Ia64LocalTimer m_localTimer;
    Kernel::Time::Timer m_localTimerProbeTimer;
    Kernel::Ipc::Notification m_localTimerProbeNotification;
    Kernel::Object::RevocationRegistry m_revocationRegistry;
    Kernel::Object::CapabilitySpace m_capabilitySpace;
    Memory::Ia64LocalTlbInvalidator m_localTlbInvalidator;
    Kernel::Memory::BootFrame m_sharedRootShootdownFrame;
    Kernel::Memory::MemoryObject m_sharedRootShootdownMemoryObject;
    Kernel::Memory::PinnedFrameMappingPreparation m_sharedRootShootdownPreparation;
    Kernel::Time::TimerQueue m_timerQueue;
    Kernel::Time::DeferredTimerWorker m_deferredTimerWorker;
    Kernel::Time::TimerNotificationDelivery m_localTimerProbeDelivery;
    Kernel::Ipc::WaitSet m_userProbeWaitSet;
    Kernel::Protection::ProtectionDomain m_protectionDomain;
    Kernel::Scheduler::Thread m_thread;
    Kernel::Scheduler::SchedulingContext m_schedulingContext;
    Kernel::Scheduler::SchedulingContext m_nativePreemptionSchedulingContext;
    Ia64InitialUserImageLaunchOwner m_initialUserImageLaunchOwner;
    Kernel::Memory::DirectUserMemoryAccess m_userMemory;
    Kernel::System::SystemCallScratch m_systemCallScratch;
    System::Ia64SystemCallBridgeContext m_systemCallBridgeContext{};
    System::Ia64SystemCallWaitContinuation m_systemCallWaitContinuation;
    Ia64KernelContext m_idleContext;
    Ia64KernelContext m_workerContext;
    // The initial image thread's f32-f127 across switches.
    Ia64HighFloatingArea m_workerHighFloating{};
    alignas(Ia64KernelStoragePageSizeBytes)
        Foundation::UInt8 m_workerMemoryStack[MinimumIa64KernelMemoryStackSizeBytes]{};
    alignas(Ia64KernelStoragePageSizeBytes)
        Foundation::UInt8 m_workerRseBackingStore[MinimumIa64KernelRseBackingStoreSizeBytes]{};
    alignas(Kernel::Scheduler::ProcessorRunController)
        Foundation::UInt8 m_controllerStorage[sizeof(Kernel::Scheduler::ProcessorRunController)]{};
    alignas(Ia64ProcessorRunLoop) Foundation::UInt8 m_runLoopStorage[sizeof(Ia64ProcessorRunLoop)]{};
    alignas(Kernel::Ipc::WaitSetThreadDispatcher)
        Foundation::UInt8 m_waitSetThreadDispatcherStorage[sizeof(Kernel::Ipc::WaitSetThreadDispatcher)]{};
    alignas(Kernel::System::WaitSetSystemCallDispatcher)
        Foundation::UInt8 m_waitSetSystemCallDispatcherStorage[sizeof(Kernel::System::WaitSetSystemCallDispatcher)]{};
    alignas(Kernel::Memory::TlbShootdownDispatcher)
        Foundation::UInt8 m_tlbShootdownDispatcherStorage[sizeof(Kernel::Memory::TlbShootdownDispatcher)]{};
    Kernel::Scheduler::ProcessorSchedulerWakeFacade* m_homeWakes[Kernel::Scheduler::MaximumProcessorCount]{};
    Kernel::Scheduler::ProcessorRunController* m_controller{nullptr};
    Ia64ProcessorRunLoop* m_runLoop{nullptr};
    Kernel::Ipc::WaitSetThreadDispatcher* m_waitSetThreadDispatcher{nullptr};
    Kernel::System::WaitSetSystemCallDispatcher* m_waitSetSystemCallDispatcher{nullptr};
    System::Ia64SystemCallEntryFrame* m_systemCallEntryFrame{nullptr};
    alignas(Kernel::Memory::TlbShootdownDrain)
        Foundation::UInt8 m_initiatorDrainStorage[sizeof(Kernel::Memory::TlbShootdownDrain)]{};
    Kernel::Memory::TlbShootdownDrain* m_initiatorDrain{};
    Kernel::Memory::TlbShootdownDispatcher* m_tlbShootdownDispatcher{nullptr};
    Memory::Ia64BspRuntimeMemory* m_runtimeMemory{nullptr};
    Kernel::Resource::ResourceDomain* m_resourceDomain{nullptr};
    Kernel::Object::RootCapabilityIssuer* m_issuer{nullptr};
    Ia64UserExecutionProbe* m_userExecutionProbe{nullptr};
    System::Ia64PagerRuntime* m_pager{nullptr};
    System::Ia64UserThreadTable* m_threads{nullptr};
    Interrupt::Ia64InterruptLines* m_deviceInterrupts{nullptr};
    // PL3 ticks that ended a created thread's run.
    Foundation::UInt64 m_threadRunStops{0U};
    Memory::Ia64BspRuntimeRootView m_root{};
    Memory::Ia64BspRuntimeRootView m_userRoot{};
    Kernel::Resource::ResourceDomainObjectId m_resourceDomainIdentity{};
    Memory::PalVirtualMemoryCapabilities m_palVirtualMemoryCapabilities{};
    Kernel::Protection::ProtectionDomainId m_protectionDomainIdentity{};
    Kernel::Scheduler::ThreadId m_threadIdentity{};
    Kernel::Scheduler::SchedulingContextId m_schedulingContextIdentity{};
    Kernel::Scheduler::SchedulingContextId m_nativePreemptionSchedulingContextIdentity{};
    Kernel::Time::TimerId m_localTimerProbeTimerIdentity{};
    Kernel::Ipc::NotificationId m_localTimerProbeNotificationIdentity{};
    Kernel::Ipc::WaitSetId m_userProbeWaitSetIdentity{};
    Ia64BspKernelWorkerEvidence m_workerEvidence{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_dispatchReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_dispatchFailure{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_workerReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_workerFailure{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_userPreemptReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_userPreemptFailure{};
    // PL3 timer ticks taken; the idle loop's progress signal.
    Foundation::UInt64 m_userTicks{0U};
    Ia64BspKernelRuntimeFailure m_failure{};
    Ia64BspNativePreemptionEvidence m_nativePreemptionEvidence{};
    Interrupt::Ia64ExternalPreemptionControlSnapshot m_nativePreemptionControlSnapshot{};
    Ia64BspKernelRuntimeUserProbeFailure m_userProbeFailure{};
    Ia64BspKernelRuntimeInitialUserImageFailure m_initialUserImageFailure{};
    Ia64BspKernelRuntimeLocalTimerProbeSummary m_localTimerProbeSummary{};
    Ia64BspKernelRuntimeLocalTimerProbeFailure m_localTimerProbeFailure{};
    Ia64BspSharedRootShootdownSummary m_sharedRootShootdownSummary{};
    Ia64BspSharedRootShootdownFailure m_sharedRootShootdownFailure{};
    Kernel::Memory::AddressSpaceShootdownRequest m_sharedRootShootdownRequest{};
    Kernel::Memory::AddressSpaceSnapshot m_sharedRootShootdownInitialRoot{};
    Kernel::Memory::AddressSpaceSnapshot m_sharedRootShootdownObservedRoot{};
    Kernel::Resource::ResourceDomainSnapshot m_sharedRootShootdownInitialResources{};
    Kernel::Resource::ResourceDomainSnapshot m_sharedRootShootdownFinalResources{};
    Kernel::Memory::PhysicalPageRange m_sharedRootShootdownPhysicalPage{};
    Kernel::Memory::TlbShootdownReceipt m_sharedRootShootdownReceipt{};
    Foundation::Size m_sharedRootShootdownInitialRegistryCount{0U};
    Interrupt::Ia64LocalTimerInterruptEvidence m_localTimerProbeEvidence{};
    Interrupt::Ia64LocalTimerInterruptEvidence m_userProbeWaitTimerEvidence{};
    Ia64BspKernelRuntimeTimerQueueEvidence m_localTimerProbeTimerQueueEvidence{};
    Kernel::Time::TimerQueueArmReceipt m_localTimerProbeArmReceipt{};
    Kernel::Time::TimerQueueArmReceipt m_userProbeWaitArmReceipt{};
    Kernel::System::WaitSetSystemCallDispatchResult m_userProbeWaitCompletion{};
    Kernel::Time::DeferredTimerRegistrationToken m_deferredTimerRegistration{};
    Kernel::Ipc::WaitSetRegistrationToken m_userProbeWaitRegistration{};
    Kernel::Time::ProcessorDeadlineReceipt m_localTimerProbeInitialDeadlineReceipt{};
    Kernel::Time::TimerNotificationDeliveryReceipt m_localTimerProbeDeliveryReceipt{};
    Kernel::Ipc::NotificationSnapshot m_localTimerProbeFinalNotificationSnapshot{};
    Kernel::Time::ProcessorDeadlineReceipt m_localTimerProbeFinalDeadlineReceipt{};
    Memory::Ia64BspRuntimeExternalInterruptPublicationSnapshot m_localTimerProbePublication{};
    Interrupt::Ia64ExternalInterruptPollCodeRange m_localTimerProbePollingRange{};
    Memory::Ia64BspRuntimeExternalPreemptionStorageView m_nativePreemptionStorage{};
    Interrupt::Ia64ExternalPreemptionControl* m_nativePreemptionControl{nullptr};
    Foundation::UInt64 m_nativePreemptionCoordinatorEntryPoint{0U};
    Foundation::UInt64 m_nativePreemptionCoordinatorGlobalPointer{0U};
    Foundation::UInt64 m_nativePreemptionSentinelEntryPoint{0U};
    Foundation::UInt64 m_nativePreemptionSentinelEnd{0U};
    Foundation::UInt64 m_localTimerProbePublishedContextAddress{0U};
    Foundation::UInt64 m_localTimerProbePublishedEvidenceAddress{0U};
    Foundation::UInt64 m_localTimerProbeNotificationClaimSequence{0U};
    Foundation::UInt64 m_userProbeWaitDeadlineTicks{0U};
    Foundation::UInt64 m_userProbeWaitTimeoutTicks{0U};
    Foundation::UInt64 m_userProbeWaitSequence{0U};
    Ia64UserModeLaunchRequest m_userProbeLaunchRequest{};
    Ia64UserModeLaunchRequest m_initialUserImageLaunchRequest{};
    Kernel::Object::CapabilityHandle m_userProbeCapability{};
    Kernel::Object::RevocationDomainId m_userProbeRevocationDomain{};
    Ia64BspKernelRuntimeState m_state{Ia64BspKernelRuntimeState::Fresh};
    Ia64BspKernelRuntimeUserProbeState m_userProbeState{Ia64BspKernelRuntimeUserProbeState::Fresh};
    Ia64BspKernelRuntimeInitialUserImageState m_initialUserImageState{Ia64BspKernelRuntimeInitialUserImageState::Fresh};
    Ia64BspKernelRuntimeLocalTimerProbeState m_localTimerProbeState{Ia64BspKernelRuntimeLocalTimerProbeState::Fresh};
    Ia64BspSharedRootShootdownState m_sharedRootShootdownState{Ia64BspSharedRootShootdownState::Fresh};
    WorkerPurpose m_workerPurpose{WorkerPurpose::None};
    bool m_initialComponentWaitIdleStageReported{false};
    bool m_initialComponentResumeStageReported{false};
    Foundation::UInt8 m_timerVector{0U};
    [[maybe_unused]] Foundation::UInt8 m_reserved[4U]{};
    [[maybe_unused]] alignas(Foundation::UInt64) Foundation::UInt64 m_workerEntryDescriptor[2U]{};
    alignas(Foundation::UInt64) Foundation::UInt64 m_nativePreemptionIdleDescriptor[2U]{};
    alignas(Kernel::Ipc::Endpoint) Foundation::UInt8 m_userProbeEndpointStorage[sizeof(Kernel::Ipc::Endpoint)]{};
    Kernel::Ipc::Endpoint* m_userProbeEndpoint{nullptr};
    Kernel::Object::ObjectId m_userProbeEndpointIdentity{};
    Kernel::Object::CapabilityHandle m_userProbeEndpointCapability{};
    Kernel::Object::CapabilityHandle m_userProbeWaitSetCapability{};
    Ia64UserExecutionProbeIpcBootstrapRecord m_userProbeBootstrapRecord{};
    alignas(Kernel::Ipc::Endpoint) Foundation::UInt8 m_initialUserImageEndpointStorage[sizeof(Kernel::Ipc::Endpoint)]{};
    Kernel::Ipc::Endpoint* m_initialUserImageEndpoint{nullptr};
    Kernel::Object::ObjectId m_initialUserImageEndpointIdentity{};
    Platform::Firmware::DiagnosticLineSink m_initialUserImageDiagnosticSink{};
    alignas(Kernel::Diagnostics::DiagnosticChannel)
        Foundation::UInt8 m_initialUserImageDiagnosticChannelStorage[sizeof(Kernel::Diagnostics::DiagnosticChannel)]{};
    Kernel::Diagnostics::DiagnosticChannel* m_initialUserImageDiagnosticChannel{nullptr};
    Kernel::Object::ObjectId m_initialUserImageDiagnosticChannelIdentity{};
    Kernel::Memory::DirectUserMemoryMapping
        m_initialUserImageMemoryMappings[Kernel::Memory::MaximumDirectUserMemoryMappings]{};
    Kernel::Resource::ResourceDomainUsage m_initialUserImageResourceUsageBefore{};
    Foundation::Size m_initialUserImageMemoryMappingCount{0U};
    bool m_initialUserImageResourceReservationAccepted{false};
};

static_assert(Ia64BspKernelWorkerExitCode != 0U);
static_assert(Ia64BspUserProbeLaunchFailureExitCode != 0U);
static_assert(Ia64BspUserProbeLaunchFailureExitCode != Ia64BspKernelWorkerExitCode);
static_assert(Ia64BspInitialUserImageLaunchFailureExitCode != 0U);
static_assert(Ia64BspInitialUserImageLaunchFailureExitCode != Ia64BspKernelWorkerExitCode);
static_assert(Ia64BspInitialUserImageLaunchFailureExitCode != Ia64BspUserProbeLaunchFailureExitCode);
static_assert(Ia64BspLocalTimerProbeLeadTicks == 10'000'000U);
static_assert(Ia64BspUserProbeWaitTimerLeadTicks == 100'000'000U);
static_assert(Ia64BspLocalTimerProbeGraceTicks == 1'000'000'000U);
static_assert(Ia64BspUserProbeWaitTimerLeadTicks > Ia64BspLocalTimerProbeLeadTicks);
static_assert(Ia64BspUserProbeWaitTimerLeadTicks == (Ia64BspLocalTimerProbeGraceTicks / 10U));
static_assert(Ia64BspNativePreemptionLeadTicks != 0U);
static_assert(Ia64BspNativePreemptionPeriodTicks > Ia64BspNativePreemptionLeadTicks);
static_assert((Ia64BspNativePreemptionPeriodTicks - Ia64BspNativePreemptionLeadTicks) >=
              Ia64BspNativePreemptionLeadTicks);
static_assert(MakeIa64BspNativePreemptionSchedulingConfiguration(1U).BudgetTicks == Ia64BspNativePreemptionLeadTicks);
static_assert(MakeIa64BspNativePreemptionSchedulingConfiguration(1U).RelativeDeadlineTicks ==
              Ia64BspNativePreemptionPeriodTicks);
static_assert(Ia64BspNativePreemptionExitCode != 0U);
static_assert(sizeof(Ia64BspKernelRuntimeState) == 1U);
static_assert(sizeof(Ia64BspKernelRuntimeError) == 1U);
static_assert(sizeof(Ia64BspKernelRuntimeUserProbeState) == 1U);
static_assert(sizeof(Ia64BspKernelRuntimeUserProbeError) == 1U);
static_assert(sizeof(Ia64BspKernelRuntimeLocalTimerProbeState) == 1U);
static_assert(sizeof(Ia64BspKernelRuntimeLocalTimerProbeError) == 1U);
static_assert(sizeof(Ia64BspSharedRootShootdownState) == 1U);
static_assert(sizeof(Ia64BspSharedRootShootdownError) == 1U);
static_assert(sizeof(Ia64BspSharedRootShootdownSummary) == 120U);
static_assert(alignof(Ia64BspSharedRootShootdownSummary) == 8U);
static_assert(__is_aggregate(Ia64BspSharedRootShootdownSummary));
static_assert(__is_standard_layout(Ia64BspSharedRootShootdownSummary));
static_assert(__is_trivially_copyable(Ia64BspSharedRootShootdownSummary));
static_assert(sizeof(Ia64BspSharedRootShootdownFailure) == 33U);
static_assert(alignof(Ia64BspSharedRootShootdownFailure) == 1U);
static_assert(__is_aggregate(Ia64BspSharedRootShootdownFailure));
static_assert(__is_standard_layout(Ia64BspSharedRootShootdownFailure));
static_assert(__is_trivially_copyable(Ia64BspSharedRootShootdownFailure));
static_assert((Ia64BspSharedRootShootdownVirtualStart & (Kernel::Memory::MappingPageSizeBytes - 1U)) == 0U);
static_assert((Ia64BspSharedRootShootdownVirtualStart >> 61U) == 1U);
static_assert(Ia64BspSharedRootShootdownMinimumPolls != 0U);
static_assert(sizeof(Ia64BspKernelWorkerEvidence) == 56U);
static_assert(alignof(Ia64BspKernelWorkerEvidence) == 8U);
static_assert(__is_aggregate(Ia64BspKernelWorkerEvidence));
static_assert(__is_standard_layout(Ia64BspKernelWorkerEvidence));
static_assert(__is_trivially_copyable(Ia64BspKernelWorkerEvidence));
static_assert(__builtin_offsetof(Ia64BspKernelWorkerEvidence, StackPointer) == 0U);
static_assert(__builtin_offsetof(Ia64BspKernelWorkerEvidence, RseBackingStorePointer) == 8U);
static_assert(__builtin_offsetof(Ia64BspKernelWorkerEvidence, RseBackingStoreBase) == 16U);
static_assert(__builtin_offsetof(Ia64BspKernelWorkerEvidence, RseNatCollection) == 24U);
static_assert(__builtin_offsetof(Ia64BspKernelWorkerEvidence, RseConfiguration) == 32U);
static_assert(__builtin_offsetof(Ia64BspKernelWorkerEvidence, ProcessorStatus) == 40U);
static_assert(__builtin_offsetof(Ia64BspKernelWorkerEvidence, CaptureComplete) == 48U);
static_assert(sizeof(Ia64BspKernelRuntimeTimerQueueEvidence) == 80U);
static_assert(alignof(Ia64BspKernelRuntimeTimerQueueEvidence) == 8U);
static_assert(__is_aggregate(Ia64BspKernelRuntimeTimerQueueEvidence));
static_assert(__is_standard_layout(Ia64BspKernelRuntimeTimerQueueEvidence));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeTimerQueueEvidence));
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, Sequence) == 0U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, TimerObjectIdentity) == 8U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, NotificationObjectIdentity) == 16U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, TimerArmSequence) == 24U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, DeadlineTicks) == 32U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, ObservedTicks) == 40U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, QueueClaimSequence) == 48U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, NotificationClaimSequence) == 56U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, InitialDeadlineState) == 64U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, SelectedDeadlineSources) == 65U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, InitialDeadlineReprogrammed) == 66U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, ClaimedTimerState) == 67U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, NotificationPendingAfterSignal) == 68U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, FinalTimerState) == 69U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, FinalNotificationState) == 70U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, FinalNotificationPending) == 71U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, FinalDeadlineState) == 72U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, FinalLocalTimerState) == 73U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, FinalLocalTimerArmed) == 74U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, FinalScheduledEntries) == 75U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, FinalPendingEntries) == 76U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, FinalClaimedEntries) == 77U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeTimerQueueEvidence, Reserved) == 78U);
static_assert(sizeof(Ia64BspKernelRuntimePreparationSummary) == 72U);
static_assert(alignof(Ia64BspKernelRuntimePreparationSummary) == 8U);
static_assert(__is_aggregate(Ia64BspKernelRuntimePreparationSummary));
static_assert(__is_standard_layout(Ia64BspKernelRuntimePreparationSummary));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimePreparationSummary));
static_assert(__builtin_offsetof(Ia64BspKernelRuntimePreparationSummary, AddressSpaceIdentity) == 0U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimePreparationSummary, ProtectionDomainIdentity) == 16U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimePreparationSummary, ThreadIdentity) == 32U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimePreparationSummary, SchedulingContextIdentity) == 48U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimePreparationSummary, Processor) == 64U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimePreparationSummary, TimerVector) == 65U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimePreparationSummary, Reserved) == 66U);
static_assert(sizeof(Ia64BspKernelRuntimeDispatchSummary) == 16U);
static_assert(alignof(Ia64BspKernelRuntimeDispatchSummary) == 8U);
static_assert(__is_aggregate(Ia64BspKernelRuntimeDispatchSummary));
static_assert(__is_standard_layout(Ia64BspKernelRuntimeDispatchSummary));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeDispatchSummary));
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeDispatchSummary, ExitCode) == 0U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeDispatchSummary, Processor) == 8U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeDispatchSummary, State) == 9U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeDispatchSummary, Reserved) == 10U);
static_assert(sizeof(Ia64BspKernelRuntimeUserProbePreparationSummary) == 24U);
static_assert(alignof(Ia64BspKernelRuntimeUserProbePreparationSummary) == 8U);
static_assert(__is_aggregate(Ia64BspKernelRuntimeUserProbePreparationSummary));
static_assert(__is_standard_layout(Ia64BspKernelRuntimeUserProbePreparationSummary));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeUserProbePreparationSummary));
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeUserProbePreparationSummary, ThreadIdentity) == 0U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeUserProbePreparationSummary, Processor) == 16U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeUserProbePreparationSummary, State) == 17U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeUserProbePreparationSummary, Reserved) == 18U);
static_assert(sizeof(Ia64BspKernelRuntimeUserProbeDispatchSummary) == 16U);
static_assert(alignof(Ia64BspKernelRuntimeUserProbeDispatchSummary) == 8U);
static_assert(__is_aggregate(Ia64BspKernelRuntimeUserProbeDispatchSummary));
static_assert(__is_standard_layout(Ia64BspKernelRuntimeUserProbeDispatchSummary));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeUserProbeDispatchSummary));
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeUserProbeDispatchSummary, ExitCode) == 0U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeUserProbeDispatchSummary, Processor) == 8U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeUserProbeDispatchSummary, State) == 9U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeUserProbeDispatchSummary, Reserved) == 10U);
static_assert(sizeof(Ia64BspKernelRuntimeInitialUserImagePreparationSummary) == 24U);
static_assert(sizeof(Ia64BspKernelRuntimeInitialUserImageDispatchSummary) == 16U);
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeInitialUserImagePreparationSummary));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeInitialUserImageDispatchSummary));
static_assert(sizeof(Ia64BspKernelRuntimeLocalTimerProbeSummary) == 24U);
static_assert(alignof(Ia64BspKernelRuntimeLocalTimerProbeSummary) == 8U);
static_assert(__is_aggregate(Ia64BspKernelRuntimeLocalTimerProbeSummary));
static_assert(__is_standard_layout(Ia64BspKernelRuntimeLocalTimerProbeSummary));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeLocalTimerProbeSummary));
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeLocalTimerProbeSummary, ProgrammedDeadlineTicks) == 0U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeLocalTimerProbeSummary, ObservedIntervalTimeCounter) == 8U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeLocalTimerProbeSummary, Processor) == 16U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeLocalTimerProbeSummary, State) == 17U);
static_assert(__builtin_offsetof(Ia64BspKernelRuntimeLocalTimerProbeSummary, Reserved) == 18U);
static_assert(__is_standard_layout(Ia64BspKernelRuntimeFailure));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeFailure));
static_assert(sizeof(Ia64BspNativePreemptionEvidence) == 96U);
static_assert(alignof(Ia64BspNativePreemptionEvidence) == 8U);
static_assert(__is_aggregate(Ia64BspNativePreemptionEvidence));
static_assert(__is_standard_layout(Ia64BspNativePreemptionEvidence));
static_assert(__is_trivially_copyable(Ia64BspNativePreemptionEvidence));
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, Sequence) == 0U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, FrameToken) == 8U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, ProgrammedDeadlineTicks) == 16U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, ObservedTicks) == 24U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, InterruptedInstructionPointer) == 32U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, InterruptedProcessorStatus) == 40U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, StoppedRunSequence) == 48U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, AdmissionRunSequence) == 56U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, AdmissionDeadlineTicks) == 64U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, IdleContinuationGeneration) == 72U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, CaptureCount) == 80U);
static_assert(__builtin_offsetof(Ia64BspNativePreemptionEvidence, RestoreCount) == 88U);
static_assert(__is_standard_layout(Ia64BspKernelRuntimeUserProbeFailure));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeUserProbeFailure));
static_assert(__is_standard_layout(Ia64BspKernelRuntimeInitialUserImageFailure));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeInitialUserImageFailure));
static_assert(__is_standard_layout(Ia64BspKernelRuntimeLocalTimerProbeFailure));
static_assert(__is_trivially_copyable(Ia64BspKernelRuntimeLocalTimerProbeFailure));
static_assert(Kernel::Memory::MaximumDirectUserMemoryMappings >=
              Executable::Ia64InitialUserImageMaximumMemoryObjectCount);
static_assert(alignof(Ia64BspKernelRuntime) == Kernel::Memory::MappingPageSizeBytes);
static_assert((sizeof(Ia64BspKernelRuntime) % Kernel::Memory::MappingPageSizeBytes) == 0U);
} // Namespace Asteria::Platform::Ia64::Scheduler

#endif // ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_BSP_KERNEL_RUNTIME_HPP_INCLUDED
