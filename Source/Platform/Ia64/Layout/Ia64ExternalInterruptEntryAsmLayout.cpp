// SPDX-License-Identifier: Apache-2.0

// External-interrupt constants and the entry-context, timer-evidence and ar.k3
// context layouts for Interrupt/Ia64ExternalInterruptEntry.S, which
// Interrupt/Ia64ExternalPreemptionEntry.S shares (see Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp"

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptCurrentProcessorStatusMask;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptCurrentProcessorStatusValue;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptEntryContext;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptInterruptedProcessorStatusMask;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptInterruptedProcessorStatusValue;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptMaskedTimerVector;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptMaximumProcessorCount;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollCodeRange;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollProcessorStatusMask;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollProcessorStatusValue;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollStatus;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptRegionZeroLimit;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptSpuriousVector;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
using Asteria::Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableContext;
} // namespace

extern "C" void AsteriaIa64ExternalInterruptEntryAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_SPURIOUS_VECTOR, UInt64{Ia64ExternalInterruptSpuriousVector});
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_TIMER_VECTOR, UInt64{Ia64ExternalInterruptTimerVector});
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_MASKED_TIMER_VECTOR, Ia64ExternalInterruptMaskedTimerVector);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_INTERRUPTED_PROCESSOR_STATUS_MASK,
                       Ia64ExternalInterruptInterruptedProcessorStatusMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_INTERRUPTED_PROCESSOR_STATUS_VALUE,
                       Ia64ExternalInterruptInterruptedProcessorStatusValue);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_CURRENT_PROCESSOR_STATUS_MASK,
                       Ia64ExternalInterruptCurrentProcessorStatusMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_CURRENT_PROCESSOR_STATUS_VALUE,
                       Ia64ExternalInterruptCurrentProcessorStatusValue);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_PROCESSOR_STATUS_MASK,
                       Ia64ExternalInterruptPollProcessorStatusMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_PROCESSOR_STATUS_VALUE,
                       Ia64ExternalInterruptPollProcessorStatusValue);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_REGION_ZERO_LIMIT, Ia64ExternalInterruptRegionZeroLimit);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_MAXIMUM_PROCESSOR_COUNT,
                       Ia64ExternalInterruptMaximumProcessorCount);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_STATUS_TIMED_OUT,
                       static_cast<UInt64>(Ia64ExternalInterruptPollStatus::TimedOut));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_STATUS_INVALID_EVIDENCE,
                       static_cast<UInt64>(Ia64ExternalInterruptPollStatus::InvalidEvidence));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_STATUS_INCOMPATIBLE_PROCESSOR_STATE,
                       static_cast<UInt64>(Ia64ExternalInterruptPollStatus::IncompatibleProcessorState));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_SIZE,
                       sizeof(Ia64RuntimeInterruptVectorTableContext));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_ALIGNMENT_MASK,
                       alignof(Ia64RuntimeInterruptVectorTableContext) - 1U);
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_ADDRESS,
        __builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, ExternalInterruptEntryContextAddress));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_SIZE, sizeof(Ia64ExternalInterruptEntryContext));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_ALIGNMENT_MASK,
                       alignof(Ia64ExternalInterruptEntryContext) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_EVIDENCE_ADDRESS,
                       __builtin_offsetof(Ia64ExternalInterruptEntryContext, EvidenceAddress));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_SIZE, sizeof(Ia64LocalTimerInterruptEvidence));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_ALIGNMENT_MASK,
                       alignof(Ia64LocalTimerInterruptEvidence) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_PROGRAMMED_DEADLINE_TICKS,
                       __builtin_offsetof(Ia64LocalTimerInterruptEvidence, ProgrammedDeadlineTicks));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_OBSERVED_INTERVAL_TIME_COUNTER,
                       __builtin_offsetof(Ia64LocalTimerInterruptEvidence, ObservedIntervalTimeCounter));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_INTERRUPTED_INSTRUCTION_POINTER,
                       __builtin_offsetof(Ia64LocalTimerInterruptEvidence, InterruptedInstructionPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_INTERRUPTED_PROCESSOR_STATUS,
                       __builtin_offsetof(Ia64LocalTimerInterruptEvidence, InterruptedProcessorStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_INTERRUPTED_IFS_VALID,
                       __builtin_offsetof(Ia64LocalTimerInterruptEvidence, InterruptedIfsValid));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_PROCESSOR_INDEX,
                       __builtin_offsetof(Ia64LocalTimerInterruptEvidence, ProcessorIndex));
    // The legacy entry stores ProcessorIndex and Vector as one 8-byte word.
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_VECTOR_SHIFT,
                       8U * (__builtin_offsetof(Ia64LocalTimerInterruptEvidence, Vector) -
                             __builtin_offsetof(Ia64LocalTimerInterruptEvidence, ProcessorIndex)));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_ACKNOWLEDGED_SEQUENCE,
                       __builtin_offsetof(Ia64LocalTimerInterruptEvidence, AcknowledgedSequence));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_CODE_RANGE_ALIGNMENT_MASK,
                       alignof(Ia64ExternalInterruptPollCodeRange) - 1U);
}
