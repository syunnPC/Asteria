// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_APPLICATION_PROCESSOR_RUNTIME_RELEASE_COORDINATOR_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_APPLICATION_PROCESSOR_RUNTIME_RELEASE_COORDINATOR_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorTakeoverCoordinator.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
enum class Ia64ApplicationProcessorRuntimeReleaseCoordinatorState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    AllServing,
    Faulted,
};

enum class Ia64ApplicationProcessorRuntimeStage : Foundation::UInt8
{
    Unused,
    AwaitingRelease,
    Released,
    Claimed,
    RuntimeEntered,
    SchedulerWaiting,
    IdleServing,
    Invalid = 0xFFU,
};

enum class Ia64ApplicationProcessorRuntimeInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    Faulted,
    TakeoverNotReady,
    InvalidProcessorCount,
    InvalidEnabledProcessorMask,
    EnabledProcessorMaskMismatch,
    InvalidTakeoverMasks,
    InvalidTakeoverLifecycle,
    InvalidTakeoverReserved,
    InvalidCoordinatorStateEncoding,
};

enum class Ia64ApplicationProcessorRuntimeTransitionError : Foundation::UInt8
{
    None,
    CoordinatorNotReady,
    CoordinatorAllServing,
    Faulted,
    InvalidProcessor,
    BootstrapProcessor,
    ProcessorNotEnabled,
    InvalidGeneration,
    StaleGeneration,
    ForeignGeneration,
    OutOfOrderProcessor,
    DuplicateRelease,
    ServingNotObserved,
    ProcessorNotReleased,
    DuplicateClaim,
    DuplicateStage,
    InvalidPrecedingStage,
    InvalidEvidence,
    ServingPending,
    InvalidCoordinatorStateEncoding,
    InvalidStageEncoding,
    InvalidActiveReleaseEncoding,
};

enum class Ia64ApplicationProcessorRuntimeFaultOperation : Foundation::UInt8
{
    None,
    Release,
    Claim,
    PublishRuntimeEntered,
    PublishSchedulerWaiting,
    PublishIdleServing,
    ObserveServing,
};

/// Exact AP-produced payload retained after IdleServing publication. The
/// coordinator interprets no address or result field; the runtime assigns the
/// values while preserving this publication contract.
struct Ia64ApplicationProcessorRuntimeServingEvidence
{
    Foundation::UInt64 RuntimeEntryValue;
    Foundation::UInt64 FirstWorkerEntryValue;
    Foundation::UInt64 ResumedWorkerEntryValue;
    Foundation::UInt64 ServingIdleValue;
    Foundation::UInt8 Reserved[8U];
};

/// One exact BSP release token. The AP must acquire-claim the same processor
/// and generation before publishing any later runtime stage.
struct Ia64ApplicationProcessorRuntimeRelease
{
    Foundation::UInt64 Generation;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 Reserved[7U];
};

/// Exact serving record returned only after the BSP acquire-observes
/// IdleServing. Evidence is copied after that acquire observation.
struct Ia64ApplicationProcessorRuntimeServingObservation
{
    Ia64ApplicationProcessorRuntimeServingEvidence Evidence;
    Foundation::UInt64 Generation;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 Reserved[7U];
};

/// First committed-state contradiction, or a fail-closed encoding diagnostic
/// synthesized by GetSnapshot with Operation None. ExpectedGeneration and
/// ObservedGeneration retain the exact request relationship for committed
/// faults; ProcessorValue is 0xFF only when no valid processor was associated
/// with the fault.
struct Ia64ApplicationProcessorRuntimeFault
{
    Foundation::UInt64 ExpectedGeneration;
    Foundation::UInt64 ObservedGeneration;
    Ia64ApplicationProcessorRuntimeTransitionError Error;
    Ia64ApplicationProcessorRuntimeFaultOperation Operation;
    Ia64ApplicationProcessorRuntimeStage ExpectedStage;
    Ia64ApplicationProcessorRuntimeStage ObservedStage;
    Foundation::UInt8 ProcessorValue;
    Foundation::UInt8 Reserved[3U];
};

struct Ia64ApplicationProcessorRuntimeProcessorSnapshot
{
    Ia64ApplicationProcessorRuntimeServingEvidence Evidence;
    Foundation::UInt64 Generation;
    Ia64ApplicationProcessorRuntimeStage Stage;
    bool ServingObserved;
    Foundation::UInt8 Reserved[6U];
};

struct Ia64ApplicationProcessorRuntimeReleaseSnapshot
{
    Ia64ApplicationProcessorRuntimeProcessorSnapshot Processors[Kernel::Scheduler::MaximumProcessorCount];
    Ia64ApplicationProcessorRuntimeFault Fault;
    Foundation::UInt64 ActiveGeneration;
    Kernel::Scheduler::CpuId ActiveProcessor;
    Foundation::UInt8 ProcessorCount;
    Foundation::UInt8 EnabledProcessorMask;
    Foundation::UInt8 ReleasedProcessorMask;
    Foundation::UInt8 OnlineProcessorMask;
    Foundation::UInt8 ObservedProcessorMask;
    Ia64ApplicationProcessorRuntimeReleaseCoordinatorState State;
    Foundation::UInt8 Reserved[1U];
};

/// Fixed-capacity, pure-C++ bridge from established IA-64 AP takeover into one sequential runtime
/// release generation per enabled AP. The BSP externally serializes Initialize, Release, and
/// ObserveServing. Each released AP calls Claim and the three publication operations only for
/// itself. A generation begins at one and advances exactly with contiguous CpuId order, so the
/// four-CPU capacity cannot approach UInt64 wrap. Initialization validates its complete input
/// before publishing state and is retryable after every validation failure. After a release is
/// committed, duplicate, stale, or out-of-order release/claim/stage operations latch the first
/// fault. The exact next AP remains retryable before Release and while Release is publishing its
/// active token. Invalid identifiers, generation zero, reserved evidence, incomplete observation,
/// and calls before readiness reject without partial mutation. Unrecognized coordinator, per-CPU
/// stage, and active-release encodings fail closed. Serving observation reserves the coordinator
/// state before changing its masks, so AllServing and fault publication have one atomic winner.
/// Allocation: - Does not allocate. Blocking: - Every operation is wait-free or bounded by four
/// processor slots. No operation waits for an AP or hardware event.
class Ia64ApplicationProcessorRuntimeReleaseCoordinator final
{
public:
    constexpr Ia64ApplicationProcessorRuntimeReleaseCoordinator() noexcept = default;

    ~Ia64ApplicationProcessorRuntimeReleaseCoordinator() noexcept = default;

    Ia64ApplicationProcessorRuntimeReleaseCoordinator(const Ia64ApplicationProcessorRuntimeReleaseCoordinator&) =
        delete;
    Ia64ApplicationProcessorRuntimeReleaseCoordinator&
    operator=(const Ia64ApplicationProcessorRuntimeReleaseCoordinator&) = delete;
    Ia64ApplicationProcessorRuntimeReleaseCoordinator(Ia64ApplicationProcessorRuntimeReleaseCoordinator&&) = delete;
    Ia64ApplicationProcessorRuntimeReleaseCoordinator&
    operator=(Ia64ApplicationProcessorRuntimeReleaseCoordinator&&) = delete;

    /// Accepts an exact contiguous enabled mask including CPU0. Every enabled
    /// AP must already be acquire-observed as InitialTranslationsInstalled in
    /// the matching takeover snapshot. Validation failure publishes nothing
    /// and may be retried. A valid CPU0-only snapshot is AllServing
    /// immediately.
    [[nodiscard]] Ia64ApplicationProcessorRuntimeInitializationError
    Initialize(const Ia64ApplicationProcessorTakeoverProgressSnapshot& takeover,
               Foundation::UInt8 enabledProcessorMask) noexcept;

    /// Release-publishes exactly the next ascending AP and generation. The
    /// first pair is CPU1/generation1. Later releases require the preceding
    /// serving state to have been explicitly acquire-observed by the BSP.
    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorRuntimeRelease,
                                     Ia64ApplicationProcessorRuntimeTransitionError>
    Release(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// AP-side acquire observation of an exact currently Released token.
    /// Processors without the active token receive ProcessorNotReleased. The
    /// operation never claims a token, changes a stage, or latches a fault, so
    /// every enabled AP may poll it without guessing another AP's generation.
    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorRuntimeRelease,
                                     Ia64ApplicationProcessorRuntimeTransitionError>
    TryObserveRelease(Kernel::Scheduler::CpuId processor) const noexcept;

    /// AP acquire-claim of the one active exact release token.
    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorRuntimeRelease,
                                     Ia64ApplicationProcessorRuntimeTransitionError>
    Claim(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    [[nodiscard]] Ia64ApplicationProcessorRuntimeTransitionError
    PublishRuntimeEntered(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// AP release-publication after the retained scheduler first returns to its
    /// idle context with its worker blocked and ready for cooperative work.
    [[nodiscard]] Ia64ApplicationProcessorRuntimeTransitionError
    PublishSchedulerWaiting(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// AP publication after later work has resumed and the retained worker has
    /// blocked again. Validates and copies every evidence field before the
    /// IdleServing release-store. Nonzero reserved bytes publish nothing.
    [[nodiscard]] Ia64ApplicationProcessorRuntimeTransitionError
    PublishIdleServing(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation,
                       const Ia64ApplicationProcessorRuntimeServingEvidence& evidence) noexcept;

    /// BSP acquire-observation of the exact active serving state. Pending is a
    /// retryable result and performs no wait. Success clears the active release,
    /// gates the next Release, and publishes AllServing after the final AP.
    [[nodiscard]] Foundation::Result<Ia64ApplicationProcessorRuntimeServingObservation,
                                     Ia64ApplicationProcessorRuntimeTransitionError>
    ObserveServing(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation) noexcept;

    /// Acquire-loads all publication words. Evidence is copied only for a
    /// serving slot. A committed fault is copied only after Faulted publication;
    /// malformed encodings instead synthesize a fail-closed snapshot diagnostic
    /// with Operation None and never expose an invalid active processor.
    [[nodiscard]] Ia64ApplicationProcessorRuntimeReleaseSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] Ia64ApplicationProcessorRuntimeTransitionError
    ValidateProcessorAndState(Kernel::Scheduler::CpuId processor, Foundation::UInt64 generation,
                              Foundation::UInt8& processorValue) const noexcept;

    [[nodiscard]] Ia64ApplicationProcessorRuntimeTransitionError
    ResolveMissingActiveRelease(Ia64ApplicationProcessorRuntimeFaultOperation operation,
                                Foundation::UInt8 processorValue, Foundation::UInt64 generation,
                                Ia64ApplicationProcessorRuntimeStage expectedStage,
                                Ia64ApplicationProcessorRuntimeStage duplicateStage,
                                Ia64ApplicationProcessorRuntimeTransitionError duplicateError) noexcept;

    [[nodiscard]] Ia64ApplicationProcessorRuntimeTransitionError
    CommitFault(Ia64ApplicationProcessorRuntimeTransitionError error,
                Ia64ApplicationProcessorRuntimeFaultOperation operation, Foundation::UInt8 processorValue,
                Foundation::UInt64 expectedGeneration, Foundation::UInt64 observedGeneration,
                Ia64ApplicationProcessorRuntimeStage expectedStage,
                Ia64ApplicationProcessorRuntimeStage observedStage) noexcept;

    Ia64ApplicationProcessorRuntimeServingEvidence m_evidence[Kernel::Scheduler::MaximumProcessorCount]{};
    Foundation::UInt64 m_generations[Kernel::Scheduler::MaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_stages[Kernel::Scheduler::MaximumProcessorCount]{};
    Synchronization::AtomicUInt64 m_servingObserved[Kernel::Scheduler::MaximumProcessorCount]{};
    Ia64ApplicationProcessorRuntimeFault m_fault{};
    Synchronization::AtomicUInt64 m_activeRelease{};
    Synchronization::AtomicUInt64 m_state{};
    Foundation::UInt8 m_processorCount{0U};
    Foundation::UInt8 m_enabledProcessorMask{0U};
    [[maybe_unused]] Foundation::UInt8 m_reserved[6U]{};
};

/// Internal publication-word rules of
/// Ia64ApplicationProcessorRuntimeReleaseCoordinator. Besides its committed
/// states and stages, the coordinator stores transient words while a
/// publication is in progress. Every rule fails closed: a word the
/// coordinator never stores is not interpreted as a usable state, stage, or
/// processor index.
namespace Detail
{
/// Initialize is admitted only from Uninitialized. Committed and transient
/// words report Faulted or AlreadyInitialized; any other word reports
/// InvalidCoordinatorStateEncoding.
[[nodiscard]] Ia64ApplicationProcessorRuntimeInitializationError
ValidateInitializationState(Foundation::UInt64 state) noexcept;

/// Transitions are admitted only from Ready. Every other committed or
/// transient word reports its retryable or terminal error; any other word
/// reports InvalidCoordinatorStateEncoding.
[[nodiscard]] Ia64ApplicationProcessorRuntimeTransitionError ValidateTransitionState(Foundation::UInt64 state) noexcept;

/// Decision for an AP transition that found no active release word.
struct Ia64ApplicationProcessorRuntimeMissingReleaseResolution
{
    Foundation::UInt64 ExpectedGeneration;
    Ia64ApplicationProcessorRuntimeTransitionError Error;
    Ia64ApplicationProcessorRuntimeStage ObservedStage;
    bool CommitsFault;
};

/// Classifies a transition by processorValue at generation that found no
/// active release while expectedProcessor is the next AP to release; its
/// generation equals its CpuId. Release stores Released before the active
/// word, so the exact next AP observing AwaitingRelease, the transient
/// release word, or Released reports retryable ProcessorNotReleased. Any
/// other stage word is a contradiction to commit: an unknown word as
/// InvalidStageEncoding, a stale, foreign, or out-of-order request as such,
/// duplicateStage as duplicateError, and anything else as
/// InvalidPrecedingStage.
[[nodiscard]] Ia64ApplicationProcessorRuntimeMissingReleaseResolution
ResolveMissingActiveRelease(Foundation::UInt8 expectedProcessor, Foundation::UInt8 processorValue,
                            Foundation::UInt64 generation, Foundation::UInt64 stage,
                            Ia64ApplicationProcessorRuntimeStage duplicateStage,
                            Ia64ApplicationProcessorRuntimeTransitionError duplicateError) noexcept;

/// Records the coordinator state word in snapshot. An unknown word replaces
/// snapshot with a Faulted diagnostic that has Operation None, no processor,
/// and no active release, and returns false so that no other word is read.
[[nodiscard]] bool RecordCoordinatorState(Ia64ApplicationProcessorRuntimeReleaseSnapshot& snapshot,
                                          Foundation::UInt64 state) noexcept;

/// Records one processor's stage word and its released and online mask bits
/// in snapshot, returning the decoded stage. An unknown word is Invalid; it
/// faults snapshot, retains any earlier diagnostic, and sets neither bit.
/// Precondition: processor is below MaximumProcessorCount.
[[nodiscard]] Ia64ApplicationProcessorRuntimeStage
RecordProcessorStage(Ia64ApplicationProcessorRuntimeReleaseSnapshot& snapshot, Foundation::UInt8 processor,
                     Foundation::UInt64 stage) noexcept;

/// Records the active release word in snapshot. Only zero or an exact
/// encoding of an enabled AP at its own generation is published; a malformed
/// word faults snapshot, retains any earlier diagnostic, and never exposes an
/// active processor or generation.
void RecordActiveRelease(Ia64ApplicationProcessorRuntimeReleaseSnapshot& snapshot, Foundation::UInt64 activeRelease,
                         Foundation::UInt8 processorCount, Foundation::UInt8 enabledProcessorMask) noexcept;
} // Namespace Detail

static_assert(sizeof(Ia64ApplicationProcessorRuntimeReleaseCoordinatorState) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeStage) == 1U);
static_assert(static_cast<Foundation::UInt8>(Ia64ApplicationProcessorRuntimeStage::Invalid) == 0xFFU);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeInitializationError) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeTransitionError) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeFaultOperation) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeServingEvidence) == 40U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeServingEvidence) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeServingEvidence));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeServingEvidence));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeServingEvidence));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeRelease) == 16U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeRelease) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeRelease));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeRelease));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeRelease));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeServingObservation) == 56U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeServingObservation) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeServingObservation));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeServingObservation));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeServingObservation));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeFault) == 24U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeFault) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeFault));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeFault));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeFault));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeProcessorSnapshot) == 56U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeProcessorSnapshot) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeProcessorSnapshot));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeProcessorSnapshot));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeProcessorSnapshot));
static_assert(sizeof(Ia64ApplicationProcessorRuntimeReleaseSnapshot) == 264U);
static_assert(alignof(Ia64ApplicationProcessorRuntimeReleaseSnapshot) == 8U);
static_assert(__is_aggregate(Ia64ApplicationProcessorRuntimeReleaseSnapshot));
static_assert(__is_standard_layout(Ia64ApplicationProcessorRuntimeReleaseSnapshot));
static_assert(__is_trivially_copyable(Ia64ApplicationProcessorRuntimeReleaseSnapshot));
static_assert(alignof(Ia64ApplicationProcessorRuntimeReleaseCoordinator) == 8U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeReleaseCoordinator) == 304U);
} // Namespace Asteria::Platform::Ia64::Interrupt

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_APPLICATION_PROCESSOR_RUNTIME_RELEASE_COORDINATOR_HPP_INCLUDED
