// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_EXTERNAL_PREEMPTION_FRAME_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_EXTERNAL_PREEMPTION_FRAME_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
constexpr Foundation::Size Ia64ExternalPreemptionGeneralRegisterCount{31U};
constexpr Foundation::Size Ia64ExternalPreemptionBranchRegisterCount{8U};
constexpr Foundation::Size Ia64ExternalPreemptionFloatingRegisterCount{126U};

/// Persistent ownership state for one interrupted native context. Capturing is
/// terminal unless the native entry completes publication; Retained is owned
/// by the common scheduler; ResumeCommitted may be cleared only in the final,
/// nonfaulting restore window immediately before rfi.
enum class Ia64ExternalPreemptionFrameState : Foundation::UInt64
{
    Inactive = 0U,
    Capturing = 1U,
    Retained = 2U,
    ResumeCommitted = 3U,
};

/// Ordered target-written capture progress. The token remains unpublished
/// until every stage, including the hardware acknowledgement, is complete.
enum class Ia64ExternalPreemptionCaptureStage : Foundation::UInt64
{
    None = 0U,
    InterruptionState = 1U,
    IntegerState = 2U,
    ApplicationState = 3U,
    FloatingState = 4U,
    RseTransition = 5U,
    TimerAcknowledged = 6U,
};

enum class Ia64ExternalPreemptionFrameError : Foundation::UInt8
{
    None,
    InvalidRequest,
    AlreadyOwned,
    TokenExhausted,
    InvalidState,
    TokenMismatch,
    InvalidCaptureStage,
    InvalidTimerEvidence,
    NotRetained,
    NotCommitted,
};

struct Ia64ExternalPreemptionCaptureRequest
{
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 TimerVector;
    Foundation::UInt64 ProgrammedDeadlineTicks;
    Foundation::UInt64 Reserved;
};

struct Ia64ExternalPreemptionTimerEvidence
{
    Foundation::UInt64 ClaimedVector;
    Foundation::UInt64 ObservedTicks;
    Foundation::UInt64 MaskedTimerVector;
    Foundation::UInt64 EoiCount;
};

struct alignas(16U) Ia64ExternalPreemptionFloatingRegister
{
    Foundation::UInt64 Low;
    Foundation::UInt64 High;
};

/// Complete persistent architectural image for one arbitrary PL0 interruption.
/// GeneralRegisters maps r1-r15 followed by interrupted-bank r16-r31,
/// BranchRegisters maps b0-b7, and FloatingRegisters maps f2-f127. General
/// registers are stored with spill/fill operations; the static and banked
/// spill-UNAT images remain distinct from the interrupted ar.unat value.
/// CSD is the implicit high half used by native 16-byte memory operations.
/// IA-32-only application registers, including SSD, are outside this native
/// PL0 proof and are neither captured nor restored.
///
/// This frame deliberately contains no coordinator memory stack or RSE backing
/// store. Native entry leaves the interrupted stack/RBS in this image and uses
/// one separately owned per-CPU coordinator corridor.
struct alignas(64U) Ia64ExternalPreemptionFrame
{
    volatile Foundation::UInt64 PublishedToken;
    Foundation::UInt64 CaptureToken;
    Foundation::UInt64 NextToken{1U};
    Ia64ExternalPreemptionFrameState State;
    Ia64ExternalPreemptionCaptureStage CaptureStage;
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 TimerVector;
    Foundation::UInt64 ClaimedVector;

    Foundation::UInt64 ProgrammedDeadlineTicks;
    Foundation::UInt64 ObservedTicks;
    Foundation::UInt64 MaskedTimerVector;
    Foundation::UInt64 EoiCount;

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
    Foundation::UInt64 InterruptionHashAddress;

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

    Foundation::UInt64 BranchRegisters[Ia64ExternalPreemptionBranchRegisterCount];
    Foundation::UInt64 GeneralRegisters[Ia64ExternalPreemptionGeneralRegisterCount];
    Ia64ExternalPreemptionFloatingRegister FloatingRegisters[Ia64ExternalPreemptionFloatingRegisterCount];
    /// ar.bsp sampled immediately after cover. The pre-cover BSP above remains
    /// the exact interrupted diagnostic; restore/loadrs must converge here.
    Foundation::UInt64 CoveredRseBackingStorePointer;
};

/// Hosted reference for the stackless entry/restore assembly's frame transitions.
/// These helpers access no IA-64 register or interrupt hardware.
///
/// Starts one target capture and reserves a lifetime-monotonic, nonzero token.
/// Failure changes no frame byte or token output. Capturing cannot be cancelled:
/// after hardware state may have been displaced, any failure is fail-stop.
[[nodiscard]] Ia64ExternalPreemptionFrameError
BeginIa64ExternalPreemptionCapture(Ia64ExternalPreemptionFrame& frame,
                                   const Ia64ExternalPreemptionCaptureRequest& request,
                                   Foundation::UInt64& captureToken) noexcept;

/// Advances exactly one capture stage. Skips, repeats, stale tokens, and calls
/// after hardware acknowledgement are rejected without mutation.
[[nodiscard]] Ia64ExternalPreemptionFrameError
AdvanceIa64ExternalPreemptionCapture(Ia64ExternalPreemptionFrame& frame, Foundation::UInt64 captureToken,
                                     Ia64ExternalPreemptionCaptureStage completedStage) noexcept;

/// Records the fixed timer-vector mask/readback and exactly one EOI after all
/// architectural state and the RSE transition are complete.
[[nodiscard]] Ia64ExternalPreemptionFrameError
CompleteIa64ExternalPreemptionTimerAcknowledgement(Ia64ExternalPreemptionFrame& frame, Foundation::UInt64 captureToken,
                                                   const Ia64ExternalPreemptionTimerEvidence& evidence) noexcept;

/// Release-publishes the retained token only after the complete capture and
/// hardware acknowledgement. The returned token is the only common-scheduler
/// ownership key for this frame generation.
[[nodiscard]] Ia64ExternalPreemptionFrameError
PublishIa64ExternalPreemptionCapture(Ia64ExternalPreemptionFrame& frame, Foundation::UInt64 captureToken) noexcept;

/// Acquires the currently retained or committed token. Zero means that no
/// complete frame has been published; Capturing must still be treated as
/// terminal target ownership rather than a reusable context.
[[nodiscard]] Foundation::UInt64 AcquireIa64ExternalPreemptionToken(const Ia64ExternalPreemptionFrame& frame) noexcept;

/// Records the exact common-scheduler commit. This does not release any frame
/// storage: the target restore tail still owns the complete image.
[[nodiscard]] Ia64ExternalPreemptionFrameError
CommitIa64ExternalPreemptionResume(Ia64ExternalPreemptionFrame& frame, Foundation::UInt64 publishedToken) noexcept;

/// Hosted semantic model of the final ownership clear. Native restore cannot
/// call this ordinary C++ helper after recovering registers: its proven leaf
/// assembly tail must mirror the release store directly, with no later
/// fault/refill opportunity, and execute exact rfi next or fail-stop.
[[nodiscard]] Ia64ExternalPreemptionFrameError
ModelReleaseIa64ExternalPreemptionFrameForRfi(Ia64ExternalPreemptionFrame& frame,
                                              Foundation::UInt64 publishedToken) noexcept;

[[nodiscard]] inline bool Ia64ExternalPreemptionFrameIsOwned(const Ia64ExternalPreemptionFrame& frame) noexcept
{
    return (frame.State != Ia64ExternalPreemptionFrameState::Inactive) ||
           (__atomic_load_n(&frame.PublishedToken, __ATOMIC_ACQUIRE) != 0U);
}

static_assert(sizeof(Ia64ExternalPreemptionFrameState) == 8U);
static_assert(sizeof(Ia64ExternalPreemptionCaptureStage) == 8U);
static_assert(sizeof(Ia64ExternalPreemptionFrameError) == 1U);
static_assert(sizeof(Ia64ExternalPreemptionCaptureRequest) == 32U);
static_assert(sizeof(Ia64ExternalPreemptionTimerEvidence) == 32U);
static_assert(sizeof(Ia64ExternalPreemptionFloatingRegister) == 16U);
static_assert(alignof(Ia64ExternalPreemptionFloatingRegister) == 16U);
static_assert(__is_aggregate(Ia64ExternalPreemptionFrame));
static_assert(__is_standard_layout(Ia64ExternalPreemptionFrame));
static_assert(__is_trivially_copyable(Ia64ExternalPreemptionFrame));
static_assert(alignof(Ia64ExternalPreemptionFrame) == 64U);
static_assert(sizeof(Ia64ExternalPreemptionFrame) == 2688U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, PublishedToken) == 0U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, CaptureToken) == 8U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, NextToken) == 16U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, State) == 24U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, CaptureStage) == 32U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, ProcessorIndex) == 40U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, TimerVector) == 48U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, ClaimedVector) == 56U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, ProgrammedDeadlineTicks) == 64U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, ObservedTicks) == 72U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, MaskedTimerVector) == 80U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, EoiCount) == 88U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, CurrentProcessorStatus) == 96U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptedInstructionPointer) == 104U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptedProcessorStatus) == 112U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, HardwareInterruptionFunctionState) == 120U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, CoveredInterruptionFunctionState) == 128U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionStatus) == 136U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionFaultingAddress) == 144U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionTranslationInformation) == 152U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionPreviousInstructionPointer) == 160U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionImmediate) == 168U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionHashAddress) == 176U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, PreviousFunctionState) == 184U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, RseConfiguration) == 192U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, RseBackingStorePointer) == 200U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, RseBackingStoreStorePointer) == 208U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, RseNatCollection) == 216U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, RseDirtyPartitionLoadConfiguration) == 224U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, OriginalNatCollection) == 232U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, StaticGeneralRegisterSpillNatCollection) == 240U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, BankedGeneralRegisterSpillNatCollection) == 248U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, LoopCounter) == 256U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, EpilogCounter) == 264U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, CompareAndStoreData) == 272U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, CompareExchangeValue) == 280U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, FloatingPointStatus) == 288U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, Predicates) == 296U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, BranchRegisters) == 304U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, GeneralRegisters) == 368U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, FloatingRegisters) == 624U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, GeneralRegisters) +
                  ((16U - 1U) * sizeof(Foundation::UInt64)) ==
              488U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, GeneralRegisters) +
                  ((31U - 1U) * sizeof(Foundation::UInt64)) ==
              608U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, FloatingRegisters) +
                  ((127U - 2U) * sizeof(Ia64ExternalPreemptionFloatingRegister)) ==
              2624U);
static_assert(__builtin_offsetof(Ia64ExternalPreemptionFrame, CoveredRseBackingStorePointer) == 2640U);
} // namespace Asteria::Platform::Ia64::Interrupt

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_EXTERNAL_PREEMPTION_FRAME_HPP_INCLUDED
