// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_PAGE_FAULT_ENTRY_CONTRACT_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_PAGE_FAULT_ENTRY_CONTRACT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FaultToken.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
constexpr Foundation::Size Ia64PageFaultGeneralRegisterCount{31U};
constexpr Foundation::Size Ia64PageFaultBranchRegisterCount{8U};
constexpr Foundation::Size Ia64PageFaultFloatingRegisterCount{126U};
constexpr Foundation::UInt64 Ia64PageFaultMaximumRseLoadSizeBytes{16U * 1024U};
constexpr Foundation::UInt64 Ia64PageFaultCoordinatorStackSizeBytes{16U * 1024U};
constexpr Foundation::UInt64 Ia64PageFaultCoordinatorRseTransitionSizeBytes{16U * 1024U};
constexpr Foundation::UInt64 Ia64PageFaultCoordinatorRseBackingStoreSizeBytes{32U * 1024U};
constexpr Foundation::UInt64 Ia64PageFaultStoragePageSizeBytes{4096U};
constexpr Foundation::UInt64 Ia64PageFaultMaximumProcessorCount{4U};
/// The Alternate Data TLB vector, through which the refill handler delivers a
/// PL3 ledger miss to the user-fault entry (Ia64UserFaultEntry.hpp).
constexpr Foundation::UInt64 Ia64PageFaultAlternateDataTlbVectorOffset{0x1000U};
constexpr Foundation::UInt64 Ia64PageFaultPageNotPresentVectorOffset{0x5000U};
constexpr Foundation::UInt64 Ia64PageFaultKeyPermissionVectorOffset{0x5100U};
constexpr Foundation::UInt64 Ia64PageFaultInstructionAccessRightsVectorOffset{0x5200U};
constexpr Foundation::UInt64 Ia64PageFaultDataAccessRightsVectorOffset{0x5300U};

/// Interruption evidence that must be checked before executing cover. A
/// native leaf must enforce the same contract while using only static
/// registers. The checkpoint deliberately rejects incomplete frames; cover
/// itself completes them (Intel SDM Vol. 2, §6.8), as the user entry requires.
struct Ia64PageFaultPreCoverObservation
{
    Foundation::UInt64 InterruptionStatus;
    Foundation::UInt64 VectorOffset;
};

/// Fixed-layout decoded interruption evidence. CoveredInterruptionFunctionState
/// must be produced after the pre-cover contract succeeds and cover executes;
/// it is not the hardware IFS sampled on entry. The capture-only native checkpoint
/// fail-stops before cover and does not produce this completed record.
struct Ia64PageFaultEntryObservation
{
    Foundation::UInt64 FaultAddress;
    Foundation::UInt64 InstructionPointer;
    Foundation::UInt64 InterruptionStatus;
    Foundation::UInt64 InterruptionProcessorStatus;
    Foundation::UInt64 CoveredInterruptionFunctionState;
    Foundation::UInt64 VectorOffset;
};

/// Exact interruption controls to restore after the pager has made the fault
/// runnable. The interrupted IIP, IPSR, and covered IFS are copied unchanged;
/// unlike a break-based system call, no instruction or slot is advanced.
struct Ia64PageFaultResumeTarget
{
    Foundation::UInt64 InstructionPointer;
    Foundation::UInt64 InterruptionProcessorStatus;
    Foundation::UInt64 CoveredInterruptionFunctionState;
};

struct Ia64PageFaultEntryOutput
{
    Kernel::Memory::FaultRecord Fault;
    Ia64PageFaultResumeTarget ResumeTarget;
};

enum class Ia64PageFaultEntryContractError : Foundation::UInt8
{
    None,
    UnsupportedInterruptionVector,
    UnsupportedInstructionSet,
    UnsupportedPrivilegeLevel,
    UnsupportedRegisterBank,
    UnsupportedTranslationState,
    InterruptionCollectionDisabled,
    ProtectionKeyDisabled,
    InvalidInstructionPointer,
    InvalidInstructionSlot,
    InterruptionSlotMismatch,
    NestedInterruption,
    UnsupportedAccessState,
    UnsupportedInterruptionCode,
    InvalidInterruptionStatus,
    InvalidAccessEncoding,
    InvalidCoveredInterruptionFunctionState,
    DecoderContractViolation,
};

/// Persistent ownership state for one synchronous user page fault. Capturing
/// is fail-stop once native state may have been displaced. Retained is owned
/// by the scheduler/pager transaction. ResumeCommitted is cleared only by the
/// final nonfaulting restore tail immediately before rfi.
enum class Ia64PageFaultFrameState : Foundation::UInt64
{
    Inactive = 0U,
    Capturing = 1U,
    Retained = 2U,
    ResumeCommitted = 3U,
};

/// Ordered native capture progress. Publication remains zero until the full
/// architectural image has been captured and the entry contract has decoded
/// one stable pager record and exact retry target.
enum class Ia64PageFaultCaptureStage : Foundation::UInt64
{
    None = 0U,
    PreCoverValidated = 1U,
    InterruptionState = 2U,
    IntegerState = 3U,
    ApplicationState = 4U,
    FloatingState = 5U,
    RseTransition = 6U,
    Decoded = 7U,
};

enum class Ia64PageFaultFrameError : Foundation::UInt8
{
    None,
    InvalidPreCoverObservation,
    TokenOutputAliasesFrame,
    AlreadyOwned,
    TokenExhausted,
    InvalidState,
    TokenMismatch,
    InvalidCaptureStage,
    ObservationMismatch,
    EntryContractRejected,
    InvalidRseFunctionState,
    InvalidRsePointer,
    CoveredRsePointerMismatch,
    InvalidRseLoadConfiguration,
    CapturedEntryMismatch,
    NotRetained,
    NotCommitted,
};

struct alignas(16U) Ia64PageFaultFloatingRegister
{
    Foundation::UInt64 Low;
    Foundation::UInt64 High;
};

/// Complete retained native PL3 image for one synchronous page fault.
/// GeneralRegisters maps r1-r15 followed by interrupted-bank r16-r31,
/// BranchRegisters maps b0-b7, and FloatingRegisters maps f2-f127. Native
/// spill/fill code keeps static and banked spill-UNAT values separate from the
/// interrupted ar.unat image. IA-32-only application state is outside the
/// validated native fault subset.
///
/// The frame contains no coordinator memory stack or RSE backing store. A
/// later native continuation uses separately retained processor-owned
/// transition storage while this context-owned image and the interrupted user
/// RBS remain stable for the entire blocked pager transaction.
struct alignas(64U) Ia64PageFaultFrame
{
    Foundation::UInt64 PublishedToken;
    Foundation::UInt64 CaptureToken;
    Foundation::UInt64 NextToken;
    Ia64PageFaultFrameState State;
    Ia64PageFaultCaptureStage CaptureStage;
    Foundation::UInt64 PreCoverInterruptionStatus;
    Foundation::UInt64 VectorOffset;

    Foundation::UInt64 CurrentProcessorStatus;
    Foundation::UInt64 InterruptedInstructionPointer;
    Foundation::UInt64 InterruptedProcessorStatus;
    Foundation::UInt64 HardwareInterruptionFunctionState;
    Foundation::UInt64 CoveredInterruptionFunctionState;
    Foundation::UInt64 InterruptionStatus;
    Foundation::UInt64 InterruptionFaultingAddress;
    Foundation::UInt64 InterruptionTranslationInformation;
    Foundation::UInt64 InterruptionPreviousInstructionPointer;
    Foundation::UInt64 InterruptionImmediate;

    Foundation::UInt64 PreviousFunctionState;
    Foundation::UInt64 RseConfiguration;
    Foundation::UInt64 RseBackingStorePointer;
    Foundation::UInt64 RseBackingStoreStorePointer;
    Foundation::UInt64 RseNatCollection;
    Foundation::UInt64 RseDirtyPartitionLoadConfiguration;
    Foundation::UInt64 OriginalNatCollection;
    Foundation::UInt64 StaticGeneralRegisterSpillNatCollection;
    Foundation::UInt64 BankedGeneralRegisterSpillNatCollection;
    Foundation::UInt64 LoopCounter;
    Foundation::UInt64 EpilogCounter;
    Foundation::UInt64 CompareAndStoreData;
    Foundation::UInt64 CompareExchangeValue;
    Foundation::UInt64 FloatingPointStatus;
    Foundation::UInt64 Predicates;

    Foundation::UInt64 BranchRegisters[Ia64PageFaultBranchRegisterCount];
    Foundation::UInt64 GeneralRegisters[Ia64PageFaultGeneralRegisterCount];
    Ia64PageFaultFloatingRegister FloatingRegisters[Ia64PageFaultFloatingRegisterCount];
    /// ar.bsp sampled immediately after cover. The earlier BSP remains the
    /// interrupted diagnostic; restore/loadrs must converge on this value.
    Foundation::UInt64 CoveredRseBackingStorePointer;
    Ia64PageFaultEntryOutput CapturedEntry;
};

/// One immutable bank-zero root published through offset 32 of the installed
/// ar.k3 context. Each immutable heavyweight IVT trampoline supplies a distinct
/// selector; the native capture entry checks all four offsets and retains
/// exactly the offset selected by that trampoline.
struct alignas(16U) Ia64PageFaultEntryContext
{
    Foundation::UInt64 ControlAddress;
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 PageNotPresentVectorOffset;
    Foundation::UInt64 KeyPermissionVectorOffset;
    Foundation::UInt64 InstructionAccessRightsVectorOffset;
    Foundation::UInt64 DataAccessRightsVectorOffset;
    Foundation::UInt64 Reserved[2U];
};

/// One-shot processor ownership. Configuration and IVT publication occur
/// before user execution. Claim first reserves Claiming, owns the frame, writes
/// all capture correlation, then release-publishes Capturing. Coordinator is
/// release-published only after the complete Ia64PageFaultFrame token is
/// visible. Complete is terminal for this control; no storage is silently
/// recycled under an old IVT publication.
enum class Ia64PageFaultEntryControlState : Foundation::UInt64
{
    Fresh = 0U,
    Configured = 1U,
    Claiming = 2U,
    Capturing = 3U,
    Coordinator = 4U,
    ResumeCommitted = 5U,
    Complete = 6U,
    Faulted = 7U,
    Invalid = 8U,
};

enum class Ia64PageFaultEntryControlError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyConfigured,
    InvalidState,
    InvalidPreCoverObservation,
    FrameOwned,
    EntryCaptureTokenExhausted,
    EntryCaptureTokenMismatch,
    FrameCaptureRejected,
    FrameStateMismatch,
    VectorMismatch,
    RsePointerOutsideInterruptedRange,
    ResumeGenerationMismatch,
};

/// Immutable Region-0 geometry for one processor's one-shot native capture.
/// InterruptedRseBackingStore is the exact prepared user-context range and is
/// not required to reside in Region 0. The capture-only entry does not
/// dereference it; a later native translation proof must permit that access.
/// BSP and BSPSTORE
/// remain strictly inside its half-open range; covered BSP may equal the end
/// as its one-past convergence point. The first 16 KiB of the coordinator RBS
/// is reserved for the bounded dirty-partition transition.
struct Ia64PageFaultEntryControlConfiguration
{
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 FrameAddress;
    Foundation::UInt64 InterruptedRseBackingStoreStart;
    Foundation::UInt64 InterruptedRseBackingStoreEnd;
    Foundation::UInt64 CoordinatorMemoryStackStart;
    Foundation::UInt64 CoordinatorMemoryStackEnd;
    Foundation::UInt64 CoordinatorRseBackingStoreStart;
    Foundation::UInt64 CoordinatorRseBackingStoreEnd;
    Foundation::UInt64 CoordinatorEntryPoint;
    Foundation::UInt64 CoordinatorGlobalPointer;
    Foundation::UInt64 CoordinatorArgument;
};

/// Assembly-visible storage retained in the locked Region-0 entry corridor.
/// Address fields become immutable when State is release-set to Configured.
/// EntryCaptureToken is a processor-control generation and is deliberately
/// distinct from CapturedFrameToken and ResumeGeneration.
struct alignas(64U) Ia64PageFaultEntryControl
{
    Foundation::UInt64 State;
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 FrameAddress;
    Foundation::UInt64 InterruptedRseBackingStoreStart;
    Foundation::UInt64 InterruptedRseBackingStoreEnd;
    Foundation::UInt64 CoordinatorMemoryStackStart;
    Foundation::UInt64 CoordinatorMemoryStackEnd;
    Foundation::UInt64 CoordinatorRseBackingStoreStart;
    Foundation::UInt64 CoordinatorRseBackingStoreEnd;
    Foundation::UInt64 CoordinatorEntryPoint;
    Foundation::UInt64 CoordinatorGlobalPointer;
    Foundation::UInt64 CoordinatorArgument;
    Foundation::UInt64 EntryCaptureToken;
    Foundation::UInt64 NextEntryCaptureToken;
    Foundation::UInt64 CapturedFrameToken;
    Foundation::UInt64 CapturedVectorOffset;
    Foundation::UInt64 ResumeGeneration;
    Foundation::UInt64 CaptureCount;
    Foundation::UInt64 RestoreCount;
    Foundation::UInt64 LastFailure;
};

struct Ia64PageFaultEntryControlSnapshot
{
    Ia64PageFaultEntryControlConfiguration Configuration;
    Foundation::UInt64 EntryCaptureToken;
    Foundation::UInt64 NextEntryCaptureToken;
    Foundation::UInt64 CapturedFrameToken;
    Foundation::UInt64 CapturedVectorOffset;
    Foundation::UInt64 ResumeGeneration;
    Foundation::UInt64 CaptureCount;
    Foundation::UInt64 RestoreCount;
    Foundation::UInt64 LastFailure;
    Ia64PageFaultEntryControlState State;
};

/// Side-effect-free revalidation of an already configured control. This does
/// not prove locked translation coverage; the retaining runtime owner adds
/// that containment proof before IVT publication.
[[nodiscard]] Ia64PageFaultEntryControlError
ValidateConfiguredIa64PageFaultEntryControl(const Ia64PageFaultEntryControl& control) noexcept;

/// Configures one fresh processor control. The control, frame, and coordinator
/// ranges are validated for Region-0 placement, exact alignment, bounded size,
/// and pairwise non-overlap. The separately prepared interrupted RBS has
/// bounded aligned geometry but is not required to be in Region 0. Retryable
/// validation failure while Fresh changes no control byte. Invalid or
/// release-published live ownership is a terminal contradiction; duplicate,
/// Claiming, Complete, and Faulted observations remain unchanged.
[[nodiscard]] Ia64PageFaultEntryControlError
ConfigureIa64PageFaultEntryControl(Ia64PageFaultEntryControl& control,
                                   const Ia64PageFaultEntryControlConfiguration& configuration) noexcept;

/// Hosted semantic model of the stackless claim used by native entry. All
/// retryable checks occur before the Configured-to-Claiming reservation. The
/// reservation owns initialization and therefore competing observers must not
/// fault it. Success starts the committed frame lifecycle, records separate
/// nonzero control and frame tokens, then release-publishes Capturing. Any
/// contradiction after that publication faults the control and retains frame
/// ownership for fail-stop diagnosis.
[[nodiscard]] Ia64PageFaultEntryControlError
ClaimIa64PageFaultEntryControlCapture(Ia64PageFaultEntryControl& control, Ia64PageFaultFrame& frame,
                                      const Ia64PageFaultPreCoverObservation& observation) noexcept;

/// Release-publishes Coordinator only after the exact frame token and vector
/// are retained and the covered interrupted-RBS pointers fit the configured
/// range.
/// Failure after claim release-faults the one-shot control.
[[nodiscard]] Ia64PageFaultEntryControlError
ModelPublishIa64PageFaultEntryControlCapture(Ia64PageFaultEntryControl& control, const Ia64PageFaultFrame& frame,
                                             Foundation::UInt64 entryCaptureToken) noexcept;

/// Records the distinct scheduler resume generation after both common
/// retained-frame ownership and the Ia64PageFaultFrame have committed resume.
[[nodiscard]] Ia64PageFaultEntryControlError
CommitIa64PageFaultEntryControlForResume(Ia64PageFaultEntryControl& control, const Ia64PageFaultFrame& frame,
                                         Foundation::UInt64 entryCaptureToken, Foundation::UInt64 frameToken,
                                         Foundation::UInt64 resumeGeneration) noexcept;

/// Models the final control store before the native restore performs its last
/// frame accesses. The caller must subsequently release the frame with
/// ModelReleaseIa64PageFaultFrameForRfi and execute rfi; this helper does not
/// weaken that frame-level leaf-window contract.
[[nodiscard]] Ia64PageFaultEntryControlError
ModelCompleteIa64PageFaultEntryControlForRfi(Ia64PageFaultEntryControl& control, const Ia64PageFaultFrame& frame,
                                             Foundation::UInt64 entryCaptureToken, Foundation::UInt64 frameToken,
                                             Foundation::UInt64 resumeGeneration) noexcept;

[[nodiscard]] Ia64PageFaultEntryControlState
AcquireIa64PageFaultEntryControlState(const Ia64PageFaultEntryControl& control) noexcept;

/// Acquire-decodes State and returns only fields release-published by that
/// state. Fresh and Invalid expose no payload; Claiming exposes immutable
/// configuration but hides correlation still being initialized by its owner.
[[nodiscard]] Ia64PageFaultEntryControlSnapshot
AcquireIa64PageFaultEntryControlSnapshot(const Ia64PageFaultEntryControl& control) noexcept;

/// Pure validation boundary between native interruption capture and the
/// architecture-neutral pager record. It accepts only
/// non-RSE, non-speculative native PL3 faults. Failure leaves output
/// unchanged. The page-fault frame lifecycle below owns capture and retention;
/// native entry and restore code must mirror its ordered transitions.
/// ValidateBeforeCover admits the four heavyweight vectors of that lifecycle;
/// ValidateAndDecode also admits the Alternate Data TLB vector of the
/// user-fault entry, whose frame never enters that lifecycle.
class Ia64PageFaultEntryContract final
{
public:
    /// Checks the architectural CFM fields of the IFS produced by cover.
    [[nodiscard]] static bool IsCoveredFunctionStateValid(Foundation::UInt64 functionState) noexcept;

    [[nodiscard]] static Ia64PageFaultEntryContractError
    ValidateBeforeCover(const Ia64PageFaultPreCoverObservation& observation) noexcept;

    [[nodiscard]] static Ia64PageFaultEntryContractError
    ValidateAndDecode(const Ia64PageFaultEntryObservation& observation, Ia64PageFaultEntryOutput& output) noexcept;

private:
    Ia64PageFaultEntryContract() = delete;
};

/// Hosted semantic model of stackless capture and restore; these helpers
/// access no IA-64 hardware. The native capture-only checkpoint mirrors Begin
/// and advances through InterruptionState before fail-stopping. Restore is
/// model-only in this contract. Begin validates the evidence
/// that is safe to inspect before cover and reserves one lifetime-monotonic,
/// nonzero token. An exact all-zero frame starts at generation one; zero
/// NextToken with any stale state is exhausted. Token output storage may not
/// overlap the frame. A failed Begin changes no frame byte or token output.
///
/// Every mutation helper and ReadIa64PageFaultCapture is serialized by the
/// owning CPU with local interrupts disabled. Acquire publication proves that
/// capture completed before observation; it does not pin the frame or permit a
/// cross-CPU reader to race resume commit, final release, or slot reuse.
[[nodiscard]] Ia64PageFaultFrameError BeginIa64PageFaultCapture(Ia64PageFaultFrame& frame,
                                                                const Ia64PageFaultPreCoverObservation& observation,
                                                                Foundation::UInt64& captureToken) noexcept;

/// Advances exactly one target-written capture stage through RseTransition.
/// Skips, repeats, stale tokens, and advancement to Decoded are rejected.
[[nodiscard]] Ia64PageFaultFrameError AdvanceIa64PageFaultCapture(Ia64PageFaultFrame& frame,
                                                                  Foundation::UInt64 captureToken,
                                                                  Ia64PageFaultCaptureStage completedStage) noexcept;

/// Proves the covered CFM-to-BSP advance, including skipped RNAT slots, and
/// validates that loadrs represents every logical dirty register from the
/// original BSPSTORE through the post-cover BSP after rebasing to a
/// page-aligned coordinator RBS. The raw old and new byte spans can differ
/// when their RNAT phases differ. Native per-CPU control storage must
/// additionally prove the captured pointers lie in the retained user RBS and
/// bound loadrs by its coordinator transition RBS.
[[nodiscard]] Ia64PageFaultFrameError ValidateIa64PageFaultCoveredRseState(const Ia64PageFaultFrame& frame) noexcept;

/// Validates that the post-cover ISR still names the accepted pre-cover fault,
/// decodes the frame-resident interruption controls, and retains both the
/// pager record and exact retry target. Failure publishes nothing and leaves a
/// started capture fail-stop owned.
[[nodiscard]] Ia64PageFaultFrameError CompleteIa64PageFaultCapture(Ia64PageFaultFrame& frame,
                                                                   Foundation::UInt64 captureToken) noexcept;

/// Release-publishes the token only after a complete decoded capture.
/// The RSE proof and entry decode are repeated immediately before publication;
/// the freshly decoded result must exactly equal CapturedEntry.
[[nodiscard]] Ia64PageFaultFrameError PublishIa64PageFaultCapture(Ia64PageFaultFrame& frame,
                                                                  Foundation::UInt64 captureToken) noexcept;

[[nodiscard]] Foundation::UInt64 AcquireIa64PageFaultToken(const Ia64PageFaultFrame& frame) noexcept;

/// Copies a Retained decoded entry under its exact acquired generation before
/// resume commit. Failure leaves output unchanged. ResumeCommitted is rejected
/// because acquire publication is visibility, not a reader lifetime pin.
[[nodiscard]] Ia64PageFaultFrameError ReadIa64PageFaultCapture(const Ia64PageFaultFrame& frame,
                                                               Foundation::UInt64 publishedToken,
                                                               Ia64PageFaultEntryOutput& output) noexcept;

/// Records the scheduler's exact retained-frame resume commit without
/// releasing any architecture storage. The acquired frame publication must
/// still identify a complete Decoded capture.
[[nodiscard]] Ia64PageFaultFrameError CommitIa64PageFaultResume(Ia64PageFaultFrame& frame,
                                                                Foundation::UInt64 publishedToken) noexcept;

/// Semantic model of the final ownership clear. Decoded remains mandatory;
/// native restore must mirror its release store in a proven leaf window and
/// execute rfi next.
[[nodiscard]] Ia64PageFaultFrameError ModelReleaseIa64PageFaultFrameForRfi(Ia64PageFaultFrame& frame,
                                                                           Foundation::UInt64 publishedToken) noexcept;

/// Read-only admission predicate for never-used frame storage. Publication is
/// acquire-read; the exact ABI-fresh representation has every scalar, array
/// element, and decoded-output field zero, including NextToken. Begin maps
/// that representation to generation one.
[[nodiscard]] bool Ia64PageFaultFrameIsPristine(const Ia64PageFaultFrame& frame) noexcept;

[[nodiscard]] inline bool Ia64PageFaultFrameIsOwned(const Ia64PageFaultFrame& frame) noexcept
{
    return (frame.State != Ia64PageFaultFrameState::Inactive) ||
           (__atomic_load_n(&frame.PublishedToken, __ATOMIC_ACQUIRE) != 0U);
}

/// Raw bank-zero target reached only from the four heavyweight IVT
/// trampolines. r16 carries their immutable selector (0 through 3). This is
/// not a C++ ABI entry and never returns: the current checkpoint owns the
/// pristine one-shot frame, release-publishes Capturing after retaining the
/// interruption tuple, and then fail-stops before cover or any user-RBS access.
extern "C" [[noreturn]] void AsteriaIa64PageFaultNativeEntry() noexcept;

static_assert(sizeof(Ia64PageFaultPreCoverObservation) == 16U);
static_assert(alignof(Ia64PageFaultPreCoverObservation) == 8U);
static_assert(__is_aggregate(Ia64PageFaultPreCoverObservation));
static_assert(__is_standard_layout(Ia64PageFaultPreCoverObservation));
static_assert(__is_trivially_copyable(Ia64PageFaultPreCoverObservation));
static_assert(__builtin_offsetof(Ia64PageFaultPreCoverObservation, InterruptionStatus) == 0U);
static_assert(__builtin_offsetof(Ia64PageFaultPreCoverObservation, VectorOffset) == 8U);

static_assert(sizeof(Ia64PageFaultEntryObservation) == 48U);
static_assert(alignof(Ia64PageFaultEntryObservation) == 8U);
static_assert(__is_aggregate(Ia64PageFaultEntryObservation));
static_assert(__is_standard_layout(Ia64PageFaultEntryObservation));
static_assert(__is_trivially_copyable(Ia64PageFaultEntryObservation));
static_assert(__builtin_offsetof(Ia64PageFaultEntryObservation, FaultAddress) == 0U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryObservation, InstructionPointer) == 8U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryObservation, InterruptionStatus) == 16U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryObservation, InterruptionProcessorStatus) == 24U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryObservation, CoveredInterruptionFunctionState) == 32U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryObservation, VectorOffset) == 40U);

static_assert(sizeof(Ia64PageFaultResumeTarget) == 24U);
static_assert(alignof(Ia64PageFaultResumeTarget) == 8U);
static_assert(__is_aggregate(Ia64PageFaultResumeTarget));
static_assert(__is_standard_layout(Ia64PageFaultResumeTarget));
static_assert(__is_trivially_copyable(Ia64PageFaultResumeTarget));
static_assert(__builtin_offsetof(Ia64PageFaultResumeTarget, InstructionPointer) == 0U);
static_assert(__builtin_offsetof(Ia64PageFaultResumeTarget, InterruptionProcessorStatus) == 8U);
static_assert(__builtin_offsetof(Ia64PageFaultResumeTarget, CoveredInterruptionFunctionState) == 16U);

static_assert(sizeof(Ia64PageFaultEntryOutput) == 56U);
static_assert(alignof(Ia64PageFaultEntryOutput) == 8U);
static_assert(__is_aggregate(Ia64PageFaultEntryOutput));
static_assert(__is_standard_layout(Ia64PageFaultEntryOutput));
static_assert(__is_trivially_copyable(Ia64PageFaultEntryOutput));
static_assert(__builtin_offsetof(Ia64PageFaultEntryOutput, Fault) == 0U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryOutput, ResumeTarget) == 32U);
static_assert(sizeof(Ia64PageFaultEntryContractError) == 1U);
static_assert(Ia64PageFaultMaximumRseLoadSizeBytes == 16U * 1024U);
static_assert(Ia64PageFaultCoordinatorStackSizeBytes == 16U * 1024U);
static_assert(Ia64PageFaultCoordinatorRseTransitionSizeBytes == 16U * 1024U);
static_assert(Ia64PageFaultMaximumRseLoadSizeBytes == Ia64PageFaultCoordinatorRseTransitionSizeBytes);
static_assert(Ia64PageFaultCoordinatorRseBackingStoreSizeBytes == 32U * 1024U);
static_assert(Ia64PageFaultStoragePageSizeBytes == 4096U);
static_assert(Ia64PageFaultMaximumProcessorCount == 4U);
static_assert(Ia64PageFaultAlternateDataTlbVectorOffset == 0x1000U);
static_assert(Ia64PageFaultPageNotPresentVectorOffset == 0x5000U);
static_assert(Ia64PageFaultKeyPermissionVectorOffset == 0x5100U);
static_assert(Ia64PageFaultInstructionAccessRightsVectorOffset == 0x5200U);
static_assert(Ia64PageFaultDataAccessRightsVectorOffset == 0x5300U);
static_assert(sizeof(Ia64PageFaultFrameState) == 8U);
static_assert(sizeof(Ia64PageFaultCaptureStage) == 8U);
static_assert(sizeof(Ia64PageFaultFrameError) == 1U);
static_assert(sizeof(Ia64PageFaultFloatingRegister) == 16U);
static_assert(alignof(Ia64PageFaultFloatingRegister) == 16U);
static_assert(__is_aggregate(Ia64PageFaultFrame));
static_assert(__is_standard_layout(Ia64PageFaultFrame));
static_assert(__is_trivially_copyable(Ia64PageFaultFrame));
static_assert(alignof(Ia64PageFaultFrame) == 64U);
static_assert(sizeof(Ia64PageFaultFrame) == 2688U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, PublishedToken) == 0U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, CaptureToken) == 8U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, NextToken) == 16U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, State) == 24U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, CaptureStage) == 32U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, PreCoverInterruptionStatus) == 40U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, VectorOffset) == 48U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, CurrentProcessorStatus) == 56U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, InterruptedInstructionPointer) == 64U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, InterruptedProcessorStatus) == 72U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, HardwareInterruptionFunctionState) == 80U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, CoveredInterruptionFunctionState) == 88U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, InterruptionStatus) == 96U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, InterruptionFaultingAddress) == 104U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, InterruptionTranslationInformation) == 112U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, InterruptionPreviousInstructionPointer) == 120U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, InterruptionImmediate) == 128U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, PreviousFunctionState) == 136U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, RseConfiguration) == 144U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, RseBackingStorePointer) == 152U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, RseBackingStoreStorePointer) == 160U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, RseNatCollection) == 168U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, RseDirtyPartitionLoadConfiguration) == 176U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, OriginalNatCollection) == 184U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, StaticGeneralRegisterSpillNatCollection) == 192U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, BankedGeneralRegisterSpillNatCollection) == 200U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, LoopCounter) == 208U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, EpilogCounter) == 216U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, CompareAndStoreData) == 224U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, CompareExchangeValue) == 232U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, FloatingPointStatus) == 240U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, Predicates) == 248U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, BranchRegisters) == 256U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, GeneralRegisters) == 320U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, FloatingRegisters) == 576U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, CoveredRseBackingStorePointer) == 2592U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, CapturedEntry) == 2600U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, GeneralRegisters) + ((16U - 1U) * sizeof(Foundation::UInt64)) ==
              440U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, GeneralRegisters) + ((31U - 1U) * sizeof(Foundation::UInt64)) ==
              560U);
static_assert(__builtin_offsetof(Ia64PageFaultFrame, FloatingRegisters) +
                  ((127U - 2U) * sizeof(Ia64PageFaultFloatingRegister)) ==
              2576U);

static_assert(sizeof(Ia64PageFaultEntryContext) == 64U);
static_assert(alignof(Ia64PageFaultEntryContext) == 16U);
static_assert(__is_aggregate(Ia64PageFaultEntryContext));
static_assert(__is_standard_layout(Ia64PageFaultEntryContext));
static_assert(__is_trivially_copyable(Ia64PageFaultEntryContext));
static_assert(__builtin_offsetof(Ia64PageFaultEntryContext, ControlAddress) == 0U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryContext, ProcessorIndex) == 8U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryContext, PageNotPresentVectorOffset) == 16U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryContext, KeyPermissionVectorOffset) == 24U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryContext, InstructionAccessRightsVectorOffset) == 32U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryContext, DataAccessRightsVectorOffset) == 40U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryContext, Reserved) == 48U);
static_assert(sizeof(Ia64PageFaultEntryControlState) == 8U);
static_assert(sizeof(Ia64PageFaultEntryControlError) == 1U);
static_assert(sizeof(Ia64PageFaultEntryControlConfiguration) == 88U);
static_assert(alignof(Ia64PageFaultEntryControlConfiguration) == 8U);
static_assert(__is_aggregate(Ia64PageFaultEntryControlConfiguration));
static_assert(__is_standard_layout(Ia64PageFaultEntryControlConfiguration));
static_assert(__is_trivially_copyable(Ia64PageFaultEntryControlConfiguration));
static_assert(sizeof(Ia64PageFaultEntryControl) == 192U);
static_assert(alignof(Ia64PageFaultEntryControl) == 64U);
static_assert(__is_aggregate(Ia64PageFaultEntryControl));
static_assert(__is_standard_layout(Ia64PageFaultEntryControl));
static_assert(__is_trivially_copyable(Ia64PageFaultEntryControl));
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, State) == 0U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, ProcessorIndex) == 8U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, FrameAddress) == 16U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, InterruptedRseBackingStoreStart) == 24U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, InterruptedRseBackingStoreEnd) == 32U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, CoordinatorMemoryStackStart) == 40U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, CoordinatorMemoryStackEnd) == 48U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, CoordinatorRseBackingStoreStart) == 56U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, CoordinatorRseBackingStoreEnd) == 64U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, CoordinatorEntryPoint) == 72U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, CoordinatorGlobalPointer) == 80U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, CoordinatorArgument) == 88U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, EntryCaptureToken) == 96U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, NextEntryCaptureToken) == 104U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, CapturedFrameToken) == 112U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, CapturedVectorOffset) == 120U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, ResumeGeneration) == 128U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, CaptureCount) == 136U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, RestoreCount) == 144U);
static_assert(__builtin_offsetof(Ia64PageFaultEntryControl, LastFailure) == 152U);
static_assert(sizeof(Ia64PageFaultEntryControlSnapshot) == 160U);
static_assert(alignof(Ia64PageFaultEntryControlSnapshot) == 8U);
static_assert(__is_aggregate(Ia64PageFaultEntryControlSnapshot));
static_assert(__is_standard_layout(Ia64PageFaultEntryControlSnapshot));
static_assert(__is_trivially_copyable(Ia64PageFaultEntryControlSnapshot));
} // namespace Asteria::Platform::Ia64::Interrupt

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_PAGE_FAULT_ENTRY_CONTRACT_HPP_INCLUDED
