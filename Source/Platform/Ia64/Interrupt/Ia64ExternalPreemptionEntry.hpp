// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_EXTERNAL_PREEMPTION_ENTRY_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_EXTERNAL_PREEMPTION_ENTRY_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionFrame.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
constexpr Foundation::UInt64 Ia64ExternalPreemptionCoordinatorStackSizeBytes{16U * 1024U};
constexpr Foundation::UInt64 Ia64ExternalPreemptionCoordinatorRseTransitionSizeBytes{16U * 1024U};
constexpr Foundation::UInt64 Ia64ExternalPreemptionCoordinatorRseBackingStoreSizeBytes{32U * 1024U};
constexpr Foundation::UInt64 Ia64ExternalPreemptionStoragePageSizeBytes{4096U};
/// The exact sampling bundle is restartable only at RI 0 and RI 1. RI 0
/// re-executes its idempotent srlz.d while delivery is masked; RI 1 completes
/// the no-op suffix while masked. RI 2 is outside this finite restart proof.
constexpr Foundation::UInt64 Ia64ExternalPreemptionSentinelFirstRestartInstructionSlot{0U};
constexpr Foundation::UInt64 Ia64ExternalPreemptionSentinelLastRestartInstructionSlot{1U};

[[nodiscard]] constexpr bool
IsIa64ExternalPreemptionSentinelRestartInstructionSlot(const Foundation::UInt64 instructionSlot) noexcept
{
    return instructionSlot <= Ia64ExternalPreemptionSentinelLastRestartInstructionSlot;
}

/// Release-selected behavior behind one immutable external-entry context.
/// LegacyEvidence preserves the polling mailbox. BspOneShotPreemption is
/// admitted only for the BSP's exact no-lock PL0 sentinel.
enum class Ia64ExternalPreemptionMode : Foundation::UInt64
{
    LegacyEvidence = 0U,
    BspOneShotPreemption = 1U,
};

/// One-processor control ownership. Mode becomes native only after Armed is
/// complete. Entry changes Armed to Capturing before touching a frame. The
/// normal-context coordinator disarms Mode before ResumeCommitted so a second
/// delivered vector cannot observe a corridor being reset for restore.
enum class Ia64ExternalPreemptionControlState : Foundation::UInt64
{
    Fresh = 0U,
    Configured = 1U,
    Armed = 2U,
    Capturing = 3U,
    Coordinator = 4U,
    ResumeCommitted = 5U,
    Complete = 6U,
    Faulted = 7U,
};

enum class Ia64ExternalPreemptionControlError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyConfigured,
    InvalidState,
    FrameOwned,
    InvalidSentinelState,
    DirtyPartitionTooLarge,
    TokenMismatch,
    FrameStateMismatch,
};

enum class Ia64ExternalPreemptionCoveredRseError : Foundation::UInt8
{
    None,
    InvalidFunctionState,
    InvalidPointer,
    CoveredPointerOutsideWorkerRange,
    CoveredPointerMismatch,
    InvalidLoadConfiguration,
};

/// Immutable addresses installed while legacy evidence remains selected.
/// CoordinatorRseBackingStoreStart names the 16-KiB transition headroom;
/// normal P64 coordinator frames may use the following 16 KiB only after the
/// entry has rebound BSPSTORE and measured the covered dirty partition.
struct Ia64ExternalPreemptionControlConfiguration
{
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 TimerVector;
    Foundation::UInt64 FrameAddress;
    Foundation::UInt64 WorkerMemoryStackStart;
    Foundation::UInt64 WorkerMemoryStackEnd;
    Foundation::UInt64 WorkerRseBackingStoreStart;
    Foundation::UInt64 WorkerRseBackingStoreEnd;
    Foundation::UInt64 CoordinatorMemoryStackStart;
    Foundation::UInt64 CoordinatorMemoryStackEnd;
    Foundation::UInt64 CoordinatorRseBackingStoreStart;
    Foundation::UInt64 CoordinatorRseBackingStoreEnd;
    Foundation::UInt64 CoordinatorEntryPoint;
    Foundation::UInt64 CoordinatorGlobalPointer;
    Foundation::UInt64 CoordinatorArgument;
    Foundation::UInt64 SentinelEntryPoint;
    Foundation::UInt64 SentinelEnd;
};

/// Exact target observations made by the sentinel before enabling delivery.
/// ProgrammedDeadlineTicks comes from cr.itm. RSE values are sampled at the
/// leaf boundary whose code range is the only native preemption admission.
struct Ia64ExternalPreemptionSentinelObservation
{
    Foundation::UInt64 InstructionPointer;
    Foundation::UInt64 CurrentProcessorStatus;
    Foundation::UInt64 StackPointer;
    Foundation::UInt64 RseConfiguration;
    Foundation::UInt64 RseBackingStorePointer;
    Foundation::UInt64 RseBackingStoreStorePointer;
    Foundation::UInt64 ProgrammedDeadlineTicks;
};

/// Assembly-visible control stored in the locked Region-0 interrupt corridor.
/// It owns no stack or frame bytes. All address ranges are immutable after
/// configuration; Mode and State are the release/acquire handoff words.
struct alignas(64U) Ia64ExternalPreemptionControl
{
    volatile Foundation::UInt64 Mode;
    volatile Foundation::UInt64 State;
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 TimerVector;
    Foundation::UInt64 FrameAddress;
    Foundation::UInt64 WorkerMemoryStackStart;
    Foundation::UInt64 WorkerMemoryStackEnd;
    Foundation::UInt64 WorkerRseBackingStoreStart;
    Foundation::UInt64 WorkerRseBackingStoreEnd;
    Foundation::UInt64 CoordinatorMemoryStackStart;
    Foundation::UInt64 CoordinatorMemoryStackEnd;
    Foundation::UInt64 CoordinatorRseBackingStoreStart;
    Foundation::UInt64 CoordinatorRseBackingStoreEnd;
    Foundation::UInt64 CoordinatorEntryPoint;
    Foundation::UInt64 CoordinatorGlobalPointer;
    Foundation::UInt64 CoordinatorArgument;
    Foundation::UInt64 SentinelEntryPoint;
    Foundation::UInt64 SentinelEnd;
    Foundation::UInt64 ArmedDeadlineTicks;
    Foundation::UInt64 CapturedFrameToken;
    Foundation::UInt64 IdleContinuationGeneration;
    Foundation::UInt64 CaptureCount;
    Foundation::UInt64 RestoreCount;
    Foundation::UInt64 LastFailure;
};

struct Ia64ExternalPreemptionControlSnapshot
{
    Ia64ExternalPreemptionControlConfiguration Configuration;
    Foundation::UInt64 ArmedDeadlineTicks;
    Foundation::UInt64 CapturedFrameToken;
    Foundation::UInt64 IdleContinuationGeneration;
    Foundation::UInt64 CaptureCount;
    Foundation::UInt64 RestoreCount;
    Foundation::UInt64 LastFailure;
    Ia64ExternalPreemptionMode Mode;
    Ia64ExternalPreemptionControlState State;
};

struct Ia64ExternalPreemptionCodeRange
{
    Foundation::UInt64 Start;
    Foundation::UInt64 End;
};

/// Configures one fresh processor control without selecting native delivery.
/// Failure changes no byte. The caller separately proves all ranges have
/// locked target translation coverage before it exposes the control pointer.
[[nodiscard]] Ia64ExternalPreemptionControlError
ConfigureIa64ExternalPreemptionControl(Ia64ExternalPreemptionControl& control,
                                       const Ia64ExternalPreemptionControlConfiguration& configuration) noexcept;

/// Hosted semantic reference for the exact sentinel-side arm operation.
/// Success release-publishes Armed before BspOneShotPreemption. Target code
/// performs the same checks and enables PSR.i only after this transition.
[[nodiscard]] Ia64ExternalPreemptionControlError
ArmIa64ExternalPreemptionControlForSentinel(Ia64ExternalPreemptionControl& control,
                                            const Ia64ExternalPreemptionFrame& frame,
                                            const Ia64ExternalPreemptionSentinelObservation& observation) noexcept;

/// Hosted semantic reference for the stackless native-entry claim. The
/// interrupted IIP and restart slot must identify RI 0 or RI 1 in the
/// configured sentinel sampling bundle. Success is the sole
/// Armed-to-Capturing transition; failure changes no byte.
[[nodiscard]] Ia64ExternalPreemptionControlError
ClaimIa64ExternalPreemptionControlCapture(Ia64ExternalPreemptionControl& control,
                                          Foundation::UInt64 interruptedInstructionPointer,
                                          Foundation::UInt64 interruptedProcessorStatus) noexcept;

/// Hosted model of entry's final publication after the frame token itself is
/// visible. Native entry mirrors these stores and tail-transfers only after
/// Coordinator is release-published.
[[nodiscard]] Ia64ExternalPreemptionControlError
ModelPublishIa64ExternalPreemptionControlCapture(Ia64ExternalPreemptionControl& control,
                                                 const Ia64ExternalPreemptionFrame& frame) noexcept;

/// Disarms the one-shot after common and frame ownership have both committed.
/// Legacy mode is release-published first. Failure retains both control words.
[[nodiscard]] Ia64ExternalPreemptionControlError
CommitIa64ExternalPreemptionControlForRestore(Ia64ExternalPreemptionControl& control,
                                              const Ia64ExternalPreemptionFrame& frame, Foundation::UInt64 frameToken,
                                              Foundation::UInt64 idleContinuationGeneration) noexcept;

/// Models the sole intentional deviation from byte-exact interruption-state
/// restoration in the finite BSP sentinel proof. The retained frame remains
/// unchanged, including its original IPSR.i=1. The outgoing rfi carrier clears
/// only IPSR.i after proving an exact return to RI 0 or RI 1 of the sentinel's
/// `srlz.d` sampling bundle. Resume either re-executes the idempotent srlz.d or
/// completes its no-op suffix with delivery masked before the immediately
/// following idempotent `rsm psr.i`, so a matured readmission timer cannot
/// nest. This operation is not a general arbitrary-context restore policy.
/// Failure changes neither the frame nor resumeProcessorStatus.
[[nodiscard]] Ia64ExternalPreemptionControlError
PrepareIa64ExternalPreemptionSentinelResumeProcessorStatus(const Ia64ExternalPreemptionControl& control,
                                                           const Ia64ExternalPreemptionFrame& frame,
                                                           Foundation::UInt64& resumeProcessorStatus) noexcept;

/// Hosted model of the final leaf stores immediately before rfi. Native
/// restore mirrors this transition directly after its final frame access.
[[nodiscard]] Ia64ExternalPreemptionControlError
ModelCompleteIa64ExternalPreemptionControlForRfi(Ia64ExternalPreemptionControl& control, Foundation::UInt64 frameToken,
                                                 Foundation::UInt64 idleContinuationGeneration) noexcept;

[[nodiscard]] Ia64ExternalPreemptionMode
AcquireIa64ExternalPreemptionMode(const Ia64ExternalPreemptionControl& control) noexcept;

[[nodiscard]] Ia64ExternalPreemptionControlState
AcquireIa64ExternalPreemptionControlState(const Ia64ExternalPreemptionControl& control) noexcept;

[[nodiscard]] Ia64ExternalPreemptionControlSnapshot
AcquireIa64ExternalPreemptionControlSnapshot(const Ia64ExternalPreemptionControl& control) noexcept;

/// Returns the exact target bundle extent containing dispatch, capture, and
/// raw restore. Hosted builds return an empty extent. Native activation must
/// prove locked instruction-translation coverage for the complete range.
[[nodiscard]] Ia64ExternalPreemptionCodeRange GetIa64ExternalPreemptionCodeRange() noexcept;

/// Hosted semantic reference for cover's CFM-to-BSP transition and the
/// coordinator loadrs encoding. Native capture performs the same bounded
/// checks before publishing a token.
[[nodiscard]] Ia64ExternalPreemptionCoveredRseError
ValidateIa64ExternalPreemptionCoveredRseState(const Ia64ExternalPreemptionControl& control,
                                              const Ia64ExternalPreemptionFrame& frame) noexcept;

static_assert(Ia64ExternalPreemptionCoordinatorStackSizeBytes == 16U * 1024U);
static_assert(Ia64ExternalPreemptionCoordinatorRseTransitionSizeBytes == 16U * 1024U);
static_assert(Ia64ExternalPreemptionCoordinatorRseBackingStoreSizeBytes == 32U * 1024U);
static_assert(sizeof(Ia64ExternalPreemptionMode) == 8U);
static_assert(sizeof(Ia64ExternalPreemptionControlState) == 8U);
static_assert(sizeof(Ia64ExternalPreemptionControlError) == 1U);
static_assert(sizeof(Ia64ExternalPreemptionCoveredRseError) == 1U);
static_assert(sizeof(Ia64ExternalPreemptionControlConfiguration) == 128U);
static_assert(sizeof(Ia64ExternalPreemptionSentinelObservation) == 56U);
static_assert(__is_aggregate(Ia64ExternalPreemptionControl));
static_assert(__is_standard_layout(Ia64ExternalPreemptionControl));
static_assert(__is_trivially_copyable(Ia64ExternalPreemptionControl));
static_assert(sizeof(Ia64ExternalPreemptionControl) == 192U);
static_assert(alignof(Ia64ExternalPreemptionControl) == 64U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, Mode) == 0U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, State) == 8U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, ProcessorIndex) == 16U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, TimerVector) == 24U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, FrameAddress) == 32U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, WorkerMemoryStackStart) == 40U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, WorkerMemoryStackEnd) == 48U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, WorkerRseBackingStoreStart) == 56U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, WorkerRseBackingStoreEnd) == 64U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, CoordinatorMemoryStackStart) == 72U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, CoordinatorMemoryStackEnd) == 80U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, CoordinatorRseBackingStoreStart) == 88U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, CoordinatorRseBackingStoreEnd) == 96U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, CoordinatorEntryPoint) == 104U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, CoordinatorGlobalPointer) == 112U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, CoordinatorArgument) == 120U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, SentinelEntryPoint) == 128U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, SentinelEnd) == 136U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, ArmedDeadlineTicks) == 144U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, CapturedFrameToken) == 152U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, IdleContinuationGeneration) == 160U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, CaptureCount) == 168U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, RestoreCount) == 176U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionControl, LastFailure) == 184U);
static_assert(sizeof(Ia64ExternalPreemptionCodeRange) == 16U);
static_assert(alignof(Ia64ExternalPreemptionCodeRange) == 8U);
static_assert(__is_aggregate(Ia64ExternalPreemptionCodeRange));
static_assert(__is_standard_layout(Ia64ExternalPreemptionCodeRange));
static_assert(__is_trivially_copyable(Ia64ExternalPreemptionCodeRange));
} // namespace Asteria::Platform::Ia64::Interrupt

/// Target-only stackless IVT front end. It preserves the legacy entry contract
/// when the context's native-control publication is zero or selects legacy
/// mode, and transfers to native capture before IVR or IFS.v rejection only
/// for an acquire-observed BSP one-shot control.
extern "C" [[noreturn]] void AsteriaIa64ExternalInterruptDispatchEntry() noexcept;

/// Target-only stackless continuation selected by the dispatch front end while
/// the exact BSP one-shot mode is acquire-visible. It never returns to the
/// legacy evidence path.
extern "C" [[noreturn]] void AsteriaIa64ExternalPreemptionNativeEntry() noexcept;

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_EXTERNAL_PREEMPTION_ENTRY_HPP_INCLUDED
