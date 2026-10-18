// SPDX-License-Identifier: Apache-2.0

// Ia64ExternalPreemptionControl and Ia64ExternalPreemptionFrame offsets, their
// state and capture-stage values and the preemption constants for
// Interrupt/Ia64ExternalPreemptionEntry.S (see Platform/AsmLayout.hpp). The
// shared external-interrupt constants are in Ia64ExternalInterruptEntryAsmLayout.

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionEntry.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionFrame.hpp"

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionCaptureStage;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionCodeRange;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionControl;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionControlState;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionCoordinatorRseTransitionSizeBytes;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionFrame;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionFrameState;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionMode;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionSentinelLastRestartInstructionSlot;
} // namespace

extern "C" void AsteriaIa64ExternalPreemptionEntryAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_COORDINATOR_RSE_TRANSITION_SIZE_BYTES,
                       Ia64ExternalPreemptionCoordinatorRseTransitionSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_SENTINEL_LAST_RESTART_INSTRUCTION_SLOT,
                       Ia64ExternalPreemptionSentinelLastRestartInstructionSlot);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_MODE_BSP_ONE_SHOT_PREEMPTION,
                       static_cast<UInt64>(Ia64ExternalPreemptionMode::BspOneShotPreemption));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_ARMED,
                       static_cast<UInt64>(Ia64ExternalPreemptionControlState::Armed));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_CAPTURING,
                       static_cast<UInt64>(Ia64ExternalPreemptionControlState::Capturing));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_COORDINATOR,
                       static_cast<UInt64>(Ia64ExternalPreemptionControlState::Coordinator));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_RESUME_COMMITTED,
                       static_cast<UInt64>(Ia64ExternalPreemptionControlState::ResumeCommitted));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_COMPLETE,
                       static_cast<UInt64>(Ia64ExternalPreemptionControlState::Complete));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATE_CAPTURING,
                       static_cast<UInt64>(Ia64ExternalPreemptionFrameState::Capturing));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATE_RETAINED,
                       static_cast<UInt64>(Ia64ExternalPreemptionFrameState::Retained));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATE_RESUME_COMMITTED,
                       static_cast<UInt64>(Ia64ExternalPreemptionFrameState::ResumeCommitted));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_INTERRUPTION_STATE,
                       static_cast<UInt64>(Ia64ExternalPreemptionCaptureStage::InterruptionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_INTEGER_STATE,
                       static_cast<UInt64>(Ia64ExternalPreemptionCaptureStage::IntegerState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_APPLICATION_STATE,
                       static_cast<UInt64>(Ia64ExternalPreemptionCaptureStage::ApplicationState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_FLOATING_STATE,
                       static_cast<UInt64>(Ia64ExternalPreemptionCaptureStage::FloatingState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_RSE_TRANSITION,
                       static_cast<UInt64>(Ia64ExternalPreemptionCaptureStage::RseTransition));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_TIMER_ACKNOWLEDGED,
                       static_cast<UInt64>(Ia64ExternalPreemptionCaptureStage::TimerAcknowledged));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_SIZE, sizeof(Ia64ExternalPreemptionControl));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_ALIGNMENT_MASK,
                       alignof(Ia64ExternalPreemptionControl) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, State));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_PROCESSOR_INDEX,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, ProcessorIndex));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_FRAME_ADDRESS,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, FrameAddress));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_WORKER_MEMORY_STACK_START,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, WorkerMemoryStackStart));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_WORKER_RSE_BACKING_STORE_START,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, WorkerRseBackingStoreStart));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_WORKER_RSE_BACKING_STORE_END,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, WorkerRseBackingStoreEnd));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_COORDINATOR_MEMORY_STACK_END,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, CoordinatorMemoryStackEnd));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_COORDINATOR_RSE_BACKING_STORE_START,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, CoordinatorRseBackingStoreStart));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_COORDINATOR_ENTRY_POINT,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, CoordinatorEntryPoint));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_SENTINEL_ENTRY_POINT,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, SentinelEntryPoint));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_ARMED_DEADLINE_TICKS,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, ArmedDeadlineTicks));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_CAPTURED_FRAME_TOKEN,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, CapturedFrameToken));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_CAPTURE_COUNT,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, CaptureCount));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_RESTORE_COUNT,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, RestoreCount));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_SIZE, sizeof(Ia64ExternalPreemptionFrame));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_ALIGNMENT_MASK,
                       alignof(Ia64ExternalPreemptionFrame) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_TOKEN,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, CaptureToken));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_NEXT_TOKEN,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, NextToken));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATE,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, State));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_STAGE,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, CaptureStage));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CLAIMED_VECTOR,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, ClaimedVector));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_PROGRAMMED_DEADLINE_TICKS,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, ProgrammedDeadlineTicks));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_OBSERVED_TICKS,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, ObservedTicks));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_MASKED_TIMER_VECTOR,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, MaskedTimerVector));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_EOI_COUNT,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, EoiCount));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CURRENT_PROCESSOR_STATUS,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, CurrentProcessorStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTED_INSTRUCTION_POINTER,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptedInstructionPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTED_PROCESSOR_STATUS,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptedProcessorStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_HARDWARE_INTERRUPTION_FUNCTION_STATE,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, HardwareInterruptionFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_COVERED_INTERRUPTION_FUNCTION_STATE,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, CoveredInterruptionFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_STATUS,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_FAULTING_ADDRESS,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionFaultingAddress));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_TRANSLATION_INFORMATION,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionTranslationInformation));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_PREVIOUS_INSTRUCTION_POINTER,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionPreviousInstructionPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_IMMEDIATE,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionImmediate));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_HASH_ADDRESS,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, InterruptionHashAddress));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_PREVIOUS_FUNCTION_STATE,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, PreviousFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_CONFIGURATION,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, RseConfiguration));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_BACKING_STORE_POINTER,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, RseBackingStorePointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_BACKING_STORE_STORE_POINTER,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, RseBackingStoreStorePointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_NAT_COLLECTION,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, RseNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_DIRTY_PARTITION_LOAD_CONFIGURATION,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, RseDirtyPartitionLoadConfiguration));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_ORIGINAL_NAT_COLLECTION,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, OriginalNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATIC_GENERAL_REGISTER_SPILL_NAT_COLLECTION,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, StaticGeneralRegisterSpillNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BANKED_GENERAL_REGISTER_SPILL_NAT_COLLECTION,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, BankedGeneralRegisterSpillNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_LOOP_COUNTER,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, LoopCounter));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_EPILOG_COUNTER,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, EpilogCounter));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_COMPARE_AND_STORE_DATA,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, CompareAndStoreData));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_COMPARE_EXCHANGE_VALUE,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, CompareExchangeValue));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_FLOATING_POINT_STATUS,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, FloatingPointStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_PREDICATES,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, Predicates));
    // BranchRegisters[n] holds bn; GeneralRegisters[n - 1] holds rn.
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B0,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, BranchRegisters[0U]));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B1,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, BranchRegisters[1U]));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B2,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, BranchRegisters[2U]));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B3,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, BranchRegisters[3U]));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B4,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, BranchRegisters[4U]));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B5,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, BranchRegisters[5U]));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B6,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, BranchRegisters[6U]));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B7,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, BranchRegisters[7U]));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_GENERAL_REGISTERS_R1,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, GeneralRegisters[1U - 1U]));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_GENERAL_REGISTERS_R16,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, GeneralRegisters[16U - 1U]));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_FLOATING_REGISTERS,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, FloatingRegisters));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_COVERED_RSE_BACKING_STORE_POINTER,
                       __builtin_offsetof(Ia64ExternalPreemptionFrame, CoveredRseBackingStorePointer));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CODE_RANGE_ALIGNMENT_MASK,
                       alignof(Ia64ExternalPreemptionCodeRange) - 1U);
}
