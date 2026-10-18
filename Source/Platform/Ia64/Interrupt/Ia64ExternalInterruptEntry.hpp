// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_EXTERNAL_INTERRUPT_ENTRY_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_EXTERNAL_INTERRUPT_ENTRY_HPP_INCLUDED

#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64::Interrupt
{
constexpr Foundation::UInt8 Ia64ExternalInterruptSpuriousVector{15U};
constexpr Foundation::UInt8 Ia64ExternalInterruptTimerVector{0xE8U};
constexpr Foundation::UInt64 Ia64ExternalInterruptMaskBit{Foundation::UInt64{1U} << 16U};
constexpr Foundation::UInt64 Ia64ExternalInterruptMaskedTimerVector{Ia64ExternalInterruptMaskBit |
                                                                    Ia64ExternalInterruptTimerVector};
constexpr Foundation::UInt64 Ia64ExternalInterruptIfsValidMask{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 Ia64ExternalInterruptNestedMask{Foundation::UInt64{1U} << 39U};
constexpr Foundation::UInt64 Ia64ExternalInterruptInstructionSlotMask{Foundation::UInt64{3U} << 41U};
/// IPSR retains the interrupted UP/AC/MFL/MFH user state. These fields do not
/// change this entry's safety preconditions, and rfi restores them without the
/// handler interpreting them. Big-endian state remains checked separately.
constexpr Foundation::UInt64 Ia64ExternalInterruptInterruptedRestoredUserStatusMask{Foundation::UInt64{0xFU} << 2U};
constexpr Foundation::UInt64 Ia64ExternalInterruptInterruptedProcessorStatusMask{
    ~(Ia64ExternalInterruptInstructionSlotMask | Ia64ExternalInterruptInterruptedRestoredUserStatusMask)};
constexpr Foundation::UInt64 Ia64ExternalInterruptInterruptedProcessorStatusValue{0x0000'1010'0802'6000ULL};
constexpr Foundation::UInt64 Ia64ExternalInterruptCurrentProcessorStatusMask{0x0000'0010'080E'E002ULL};
constexpr Foundation::UInt64 Ia64ExternalInterruptCurrentProcessorStatusValue{0x0000'0010'0802'0000ULL};
constexpr Foundation::UInt64 Ia64ExternalInterruptPollProcessorStatusMask{0x0000'0010'080E'E002ULL};
constexpr Foundation::UInt64 Ia64ExternalInterruptPollProcessorStatusValue{0x0000'0010'0802'2000ULL};
constexpr Foundation::UInt64 Ia64ExternalInterruptRegionZeroLimit{Foundation::UInt64{1U} << 61U};
constexpr Foundation::UInt64 Ia64ExternalInterruptMaximumProcessorCount{4U};
constexpr Foundation::Size Ia64LocalTimerInterruptLegacyEvidenceSizeBytes{56U};
static_assert(__atomic_always_lock_free(sizeof(Foundation::UInt64), nullptr),
              "IA-64 timer mailbox sequences require lock-free 64-bit atomics");

/// Bank-zero input reached through offset 24 of the ar.k3 context. The owner
/// release-publishes its address only after the complete context and zero
/// evidence storage are retained inside the locked Region-0 corridor. The
/// final word may release-publish one immutable, locked native-preemption
/// control for the BSP proof. Only the sole IVT dispatcher interprets it; the
/// legacy continuation deliberately ignores it after dispatch.
struct alignas(16U) Ia64ExternalInterruptEntryContext
{
    Foundation::UInt64 EvidenceAddress;
    Foundation::UInt64 ProcessorIndex;
    Foundation::UInt64 TimerVector;
    Foundation::UInt64 ExternalPreemptionControlAddress;
};

/// Reusable timer-evidence mailbox. The first 56 bytes retain the Phase-D
/// one-shot payload byte for byte. The entry overwrites that payload only when
/// Sequence and AcknowledgedSequence prove that no record is outstanding, then
/// release-publishes the next nonwrapping Sequence after masking and EOI.
struct Ia64LocalTimerInterruptEvidence
{
    Foundation::UInt64 Sequence;
    Foundation::UInt64 ProgrammedDeadlineTicks;
    Foundation::UInt64 ObservedIntervalTimeCounter;
    Foundation::UInt64 InterruptedInstructionPointer;
    Foundation::UInt64 InterruptedProcessorStatus;
    Foundation::UInt64 InterruptedIfsValid;
    Foundation::UInt8 ProcessorIndex;
    Foundation::UInt8 Vector;
    Foundation::UInt8 Reserved[6U];
    Foundation::UInt64 AcknowledgedSequence;
};

struct Ia64ExternalInterruptPollCodeRange
{
    Foundation::UInt64 Start;
    Foundation::UInt64 End;
};

/// Pure input for hosted semantic validation. CurrentProcessorStatus contains
/// only the fields architecturally observable through mov-from-PSR. No model
/// call executes an interrupt, accesses a control register, or acknowledges a
/// source.
struct Ia64ExternalInterruptModelInput
{
    Foundation::UInt64 CurrentProcessorStatus;
    Foundation::UInt64 InterruptedInstructionPointer;
    Foundation::UInt64 InterruptedProcessorStatus;
    Foundation::UInt64 InterruptedFunctionState;
    Foundation::UInt64 InterruptionStatus;
    Foundation::UInt64 ClaimedVector;
    Foundation::UInt64 IntervalTimerMatch;
    Foundation::UInt64 IntervalTimeCounter;
};

enum class Ia64ExternalInterruptDisposition : Foundation::UInt8
{
    ResumeSpurious,
    ResumeTimer,
};

enum class Ia64ExternalInterruptModelError : Foundation::UInt8
{
    None,
    IncompatibleCurrentState,
    IncompatibleInterruptedState,
    MisalignedInterruptedInstructionPointer,
    IfsValid,
    NestedInterruption,
    InvalidContextAddress,
    InvalidContext,
    InvalidEvidenceAddress,
    EvidenceOutstanding,
    EvidenceSequenceExhausted,
    UnexpectedClaimedVector,
    InvalidTimerState,
};

struct Ia64ExternalInterruptModelAction
{
    Ia64LocalTimerInterruptEvidence Evidence;
    Foundation::UInt64 TimerVectorRegister;
    Ia64ExternalInterruptDisposition Disposition;
    Foundation::UInt8 EoiCount;
    Foundation::UInt8 Reserved[6U];
};

enum class Ia64LocalTimerInterruptEvidenceValidationError : Foundation::UInt8
{
    None,
    InvalidExpectedProcessor,
    InvalidExpectedDeadline,
    InvalidPollingRange,
    Incomplete,
    DeadlineMismatch,
    CounterBeforeDeadline,
    InstructionPointerOutsidePollingRange,
    InvalidInterruptedProcessorStatus,
    IfsValid,
    ProcessorMismatch,
    VectorMismatch,
    ReservedNotZero,
};

enum class Ia64ExternalInterruptPollStatus : Foundation::UInt64
{
    Completed = 0U,
    TimedOut = 1U,
    InvalidEvidence = 2U,
    IncompatibleProcessorState = 3U,
    NativeUnavailable = 4U,
};

enum class Ia64LocalTimerInterruptAcknowledgementError : Foundation::UInt8
{
    None,
    InvalidSequence,
    StaleSequence,
    ForeignSequence,
    AlreadyAcknowledged,
    SequenceExhausted,
    InvalidMailboxState,
};

/// Pure observations at the three decision points of the exact-sequence poll
/// leaf. A call models one return path, so DeadlineReached must be true when
/// SequenceBeforeDisable still names the preceding acknowledged generation.
struct Ia64ExternalInterruptSequencePollModelInput
{
    Foundation::UInt64 ExpectedSequence;
    Foundation::UInt64 InitialPublishedSequence;
    Foundation::UInt64 InitialAcknowledgedSequence;
    Foundation::UInt64 SequenceBeforeDisable;
    Foundation::UInt64 SequenceAfterDisable;
    bool DeadlineReached;
    Foundation::UInt8 Reserved[7U];
};

/// Models all state accepted by the stackless native entry. Unknown claimed
/// vectors are reported as errors; the native entry performs one serialized
/// EOI before fail-stop for that case.
[[nodiscard]] Foundation::Result<Ia64ExternalInterruptModelAction, Ia64ExternalInterruptModelError>
ModelIa64ExternalInterrupt(Foundation::UInt64 contextAddress, const Ia64ExternalInterruptEntryContext& context,
                           Foundation::UInt64 evidenceAddress, const Ia64LocalTimerInterruptEvidence& evidence,
                           Ia64ExternalInterruptModelInput input) noexcept;

/// Validates the acquire-observed one-shot record without accessing native
/// state. pollingStart and pollingEnd form one exact half-open code interval.
[[nodiscard]] Ia64LocalTimerInterruptEvidenceValidationError ValidateIa64LocalTimerInterruptEvidence(
    const Ia64LocalTimerInterruptEvidence& evidence, Foundation::UInt8 expectedProcessorIndex,
    Foundation::UInt64 expectedDeadlineTicks, Foundation::UInt64 pollingStart, Foundation::UInt64 pollingEnd) noexcept;

/// Sequence-aware counterpart for a currently outstanding exact generation.
/// The expected generation must be nonzero and its predecessor must remain
/// acknowledged in the acquired snapshot.
[[nodiscard]] Ia64LocalTimerInterruptEvidenceValidationError ValidateIa64LocalTimerInterruptEvidenceForSequence(
    const Ia64LocalTimerInterruptEvidence& evidence, Foundation::UInt64 expectedSequence,
    Foundation::UInt8 expectedProcessorIndex, Foundation::UInt64 expectedDeadlineTicks, Foundation::UInt64 pollingStart,
    Foundation::UInt64 pollingEnd) noexcept;

/// Acquire-observes Sequence and copies the remaining fields only when the
/// native release publication is exactly one. Other sequence values are
/// returned with every non-sequence field zero, so diagnostics never consume
/// a partially written entry record.
[[nodiscard]] Ia64LocalTimerInterruptEvidence
AcquireIa64LocalTimerInterruptEvidence(const Ia64LocalTimerInterruptEvidence& evidence) noexcept;

/// Acquire-observes the currently published generation without consuming its
/// payload. Callers may pass this exact value to the sequence-aware acquire so
/// a reusable mailbox snapshot is never hard-coded to the legacy generation.
[[nodiscard]] Foundation::UInt64
AcquireIa64LocalTimerInterruptEvidenceSequence(const Ia64LocalTimerInterruptEvidence& evidence) noexcept;

/// Acquire-copies one exact outstanding generation. A mismatch returns only
/// the acquire-observed publication and acknowledgement words; every legacy
/// payload field is zero so a caller cannot consume an overwritten record.
[[nodiscard]] Ia64LocalTimerInterruptEvidence
AcquireIa64LocalTimerInterruptEvidenceForSequence(const Ia64LocalTimerInterruptEvidence& evidence,
                                                  Foundation::UInt64 expectedSequence) noexcept;

/// Normal-context exact acknowledgement. The caller keeps the timer source
/// masked and externally serializes consumers. The release store makes every
/// preceding payload use complete before the ISR may reuse the mailbox.
[[nodiscard]] Ia64LocalTimerInterruptAcknowledgementError
AcknowledgeIa64LocalTimerInterruptEvidence(Ia64LocalTimerInterruptEvidence& evidence,
                                           Foundation::UInt64 sequence) noexcept;

/// Pure return-path model for the generalized native exact-sequence poll.
[[nodiscard]] Ia64ExternalInterruptPollStatus
ModelIa64ExternalInterruptSequencePoll(Ia64ExternalInterruptSequencePollModelInput input) noexcept;

/// Returns the raw bundle-address interval used by the native poll leaf. This
/// deliberately avoids interpreting an IA-64 P64 function descriptor as code.
/// Hosted builds return an empty range.
[[nodiscard]] Ia64ExternalInterruptPollCodeRange GetIa64ExternalInterruptPollCodeRange() noexcept;

static_assert(Ia64ExternalInterruptTimerVector >= 16U);
static_assert(Ia64ExternalInterruptMaskedTimerVector == 0x1'00E8U);
static_assert(Ia64ExternalInterruptInterruptedRestoredUserStatusMask == 0x3CU);
static_assert(Ia64ExternalInterruptInterruptedProcessorStatusMask == 0xFFFF'F9FF'FFFF'FFC3ULL);
static_assert((Ia64ExternalInterruptInterruptedProcessorStatusValue &
               (Ia64ExternalInterruptInstructionSlotMask | Ia64ExternalInterruptInterruptedRestoredUserStatusMask)) ==
              0U);
static_assert(sizeof(Ia64ExternalInterruptEntryContext) == 32U);
static_assert(alignof(Ia64ExternalInterruptEntryContext) == 16U);
static_assert(__is_aggregate(Ia64ExternalInterruptEntryContext));
static_assert(__is_standard_layout(Ia64ExternalInterruptEntryContext));
static_assert(__is_trivially_copyable(Ia64ExternalInterruptEntryContext));
static_assert(__builtin_offsetof(Ia64ExternalInterruptEntryContext, EvidenceAddress) == 0U);
static_assert(__builtin_offsetof(Ia64ExternalInterruptEntryContext, ProcessorIndex) == 8U);
static_assert(__builtin_offsetof(Ia64ExternalInterruptEntryContext, TimerVector) == 16U);
static_assert(__builtin_offsetof(Ia64ExternalInterruptEntryContext, ExternalPreemptionControlAddress) == 24U);
static_assert(sizeof(Ia64LocalTimerInterruptEvidence) == 64U);
static_assert(alignof(Ia64LocalTimerInterruptEvidence) == 8U);
static_assert(__is_aggregate(Ia64LocalTimerInterruptEvidence));
static_assert(__is_standard_layout(Ia64LocalTimerInterruptEvidence));
static_assert(__is_trivially_copyable(Ia64LocalTimerInterruptEvidence));
static_assert(__builtin_offsetof(Ia64LocalTimerInterruptEvidence, Sequence) == 0U);
static_assert(__builtin_offsetof(Ia64LocalTimerInterruptEvidence, ProgrammedDeadlineTicks) == 8U);
static_assert(__builtin_offsetof(Ia64LocalTimerInterruptEvidence, ObservedIntervalTimeCounter) == 16U);
static_assert(__builtin_offsetof(Ia64LocalTimerInterruptEvidence, InterruptedInstructionPointer) == 24U);
static_assert(__builtin_offsetof(Ia64LocalTimerInterruptEvidence, InterruptedProcessorStatus) == 32U);
static_assert(__builtin_offsetof(Ia64LocalTimerInterruptEvidence, InterruptedIfsValid) == 40U);
static_assert(__builtin_offsetof(Ia64LocalTimerInterruptEvidence, ProcessorIndex) == 48U);
static_assert(__builtin_offsetof(Ia64LocalTimerInterruptEvidence, Vector) == 49U);
static_assert(__builtin_offsetof(Ia64LocalTimerInterruptEvidence, Reserved) == 50U);
static_assert(__builtin_offsetof(Ia64LocalTimerInterruptEvidence, AcknowledgedSequence) ==
              Ia64LocalTimerInterruptLegacyEvidenceSizeBytes);
static_assert(sizeof(Ia64ExternalInterruptModelInput) == 64U);
static_assert(sizeof(Ia64ExternalInterruptModelAction) == 80U);
static_assert(sizeof(Ia64ExternalInterruptSequencePollModelInput) == 48U);
static_assert(alignof(Ia64ExternalInterruptSequencePollModelInput) == 8U);
static_assert(__is_aggregate(Ia64ExternalInterruptSequencePollModelInput));
static_assert(__is_standard_layout(Ia64ExternalInterruptSequencePollModelInput));
static_assert(__is_trivially_copyable(Ia64ExternalInterruptSequencePollModelInput));
static_assert(sizeof(Ia64ExternalInterruptPollCodeRange) == 16U);
static_assert(alignof(Ia64ExternalInterruptPollCodeRange) == 8U);
static_assert(__is_aggregate(Ia64ExternalInterruptPollCodeRange));
static_assert(__is_standard_layout(Ia64ExternalInterruptPollCodeRange));
static_assert(__is_trivially_copyable(Ia64ExternalInterruptPollCodeRange));
static_assert(sizeof(Ia64ExternalInterruptPollStatus) == 8U);
} // namespace Asteria::Platform::Ia64::Interrupt

extern "C" void AsteriaIa64ExternalInterruptEntry() noexcept;

/// Target-only bounded PL0 leaf. CPL0, PSR.vm=0, native IA-64, bank one, and
/// PSR.ic=1 are caller preconditions because mov-from-PSR cannot observe all of
/// them. The leaf requires PSR.i=0, enables delivery only while polling, and
/// disables delivery plus acquire-rechecks Sequence before every return.
/// absoluteDeadlineTicks is an absolute ar.itc deadline, not a duration.
extern "C" [[nodiscard]] ASTERIA_NOINLINE_NOIPA Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus
AsteriaIa64PollForExternalInterrupt(Asteria::Foundation::UInt64 evidenceAddress,
                                    Asteria::Foundation::UInt64 absoluteDeadlineTicks) noexcept;

/// Exact-sequence counterpart used after a prior record has been acknowledged.
/// It accepts only expectedSequence-1 as the initial acknowledged publication,
/// waits for exactly expectedSequence, and performs the same disabled acquire
/// recheck before returning. Other native-state preconditions are unchanged.
extern "C" [[nodiscard]] ASTERIA_NOINLINE_NOIPA Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus
AsteriaIa64PollForExternalInterruptSequence(Asteria::Foundation::UInt64 evidenceAddress,
                                            Asteria::Foundation::UInt64 expectedSequence,
                                            Asteria::Foundation::UInt64 absoluteDeadlineTicks) noexcept;

#endif // ASTERIA_PLATFORM_IA64_INTERRUPT_IA64_EXTERNAL_INTERRUPT_ENTRY_HPP_INCLUDED
