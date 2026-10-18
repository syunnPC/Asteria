// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp"
// Ia64SystemCallEntryFrame and Ia64SystemCallEntryContext offsets and the
// entry admission constants for System/Ia64SystemCallEntry.S (see
// Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Firmware/PalBootstrapCapabilityCapture.hpp"
#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Platform::Ia64::Firmware::Ia64PhysicalStackedRegisterCount;
using Asteria::Platform::Ia64::System::Ia64SystemCallBreakImmediate;
using Asteria::Platform::Ia64::System::Ia64SystemCallBundleSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallCurrentProcessorStatusMask;
using Asteria::Platform::Ia64::System::Ia64SystemCallCurrentProcessorStatusValue;
using Asteria::Platform::Ia64::System::Ia64SystemCallEntryContext;
using Asteria::Platform::Ia64::System::Ia64SystemCallEntryContextSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallEntryFrame;
using Asteria::Platform::Ia64::System::Ia64SystemCallEntryFrameState;
using Asteria::Platform::Ia64::System::Ia64SystemCallFirstNonKernelRegionAddress;
using Asteria::Platform::Ia64::System::Ia64SystemCallIfsValidMask;
using Asteria::Platform::Ia64::System::Ia64SystemCallInstructionSlotShift;
using Asteria::Platform::Ia64::System::Ia64SystemCallInterruptedProcessorStatusMask;
using Asteria::Platform::Ia64::System::Ia64SystemCallInterruptedProcessorStatusValue;
using Asteria::Platform::Ia64::System::Ia64SystemCallKernelRseConfiguration;
using Asteria::Platform::Ia64::System::Ia64SystemCallMaximumStackedRegisters;
using Asteria::Platform::Ia64::System::Ia64SystemCallMemoryStackSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallRefillContextPublicationOffsetBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallRefillContextSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallRequiredOutputRegisters;
using Asteria::Platform::Ia64::System::Ia64SystemCallRseBackingStoreSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallRseTransitionSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallStoragePageSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallUserRseConfiguration;
using Asteria::Platform::Ia64::System::MaximumIa64SystemCallProcessorCount;
} // namespace

extern "C" void AsteriaIa64SystemCallEntryAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_SIZE, sizeof(Ia64SystemCallEntryFrame));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_CURRENT_PROCESSOR_STATUS,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, CurrentProcessorStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_INTERRUPTED_INSTRUCTION_POINTER,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, InterruptedInstructionPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_INTERRUPTED_PROCESSOR_STATUS,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, InterruptedProcessorStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_INTERRUPTION_STATUS,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, InterruptionStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_BREAK_IMMEDIATE,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, BreakImmediate));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_HARDWARE_INTERRUPTION_FUNCTION_STATE,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, HardwareInterruptionFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_CAPTURED_USER_INTERRUPTION_FUNCTION_STATE,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, CapturedUserInterruptionFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_PREVIOUS_FUNCTION_STATE,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, PreviousFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_USER_RSE_CONFIGURATION,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, UserRseConfiguration));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_USER_RSE_BACKING_STORE_STORE_POINTER,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, UserRseBackingStoreStorePointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_KERNEL_RSE_LOAD_CONFIGURATION,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, KernelRseLoadConfiguration));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_USER_RSE_NAT_COLLECTION,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, UserRseNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_ORIGINAL_USER_NAT_COLLECTION,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, OriginalUserNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_SPILL_NAT_COLLECTION,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, SpillNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_FLOATING_POINT_STATUS,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, FloatingPointStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_LOOP_COUNTER,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, LoopCounter));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_COMPARE_EXCHANGE_VALUE,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, CompareExchangeValue));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_EPILOG_COUNTER,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, EpilogCounter));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_PREDICATES,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, Predicates));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B0, __builtin_offsetof(Ia64SystemCallEntryFrame, B0));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B1, __builtin_offsetof(Ia64SystemCallEntryFrame, B1));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B2, __builtin_offsetof(Ia64SystemCallEntryFrame, B2));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B3, __builtin_offsetof(Ia64SystemCallEntryFrame, B3));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B4, __builtin_offsetof(Ia64SystemCallEntryFrame, B4));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B5, __builtin_offsetof(Ia64SystemCallEntryFrame, B5));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B6, __builtin_offsetof(Ia64SystemCallEntryFrame, B6));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B7, __builtin_offsetof(Ia64SystemCallEntryFrame, B7));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_SPILL_R1,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, SpillR1));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_REGISTERS_R15,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, Registers.R15));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_REGISTERS_R8,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, Registers.R8));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_RESERVED,
                       __builtin_offsetof(Ia64SystemCallEntryFrame, Reserved));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_STATE_ACTIVE,
                       static_cast<UInt64>(Ia64SystemCallEntryFrameState::Active));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_CONTEXT_FRAME_ADDRESS,
                       __builtin_offsetof(Ia64SystemCallEntryContext, FrameAddress));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_ENTRY_CONTEXT_SIZE_BYTES, Ia64SystemCallEntryContextSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_REFILL_CONTEXT_SIZE_BYTES, Ia64SystemCallRefillContextSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_REFILL_CONTEXT_PUBLICATION_OFFSET_BYTES,
                       Ia64SystemCallRefillContextPublicationOffsetBytes);

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_CURRENT_PROCESSOR_STATUS_MASK,
                       Ia64SystemCallCurrentProcessorStatusMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_CURRENT_PROCESSOR_STATUS_VALUE,
                       Ia64SystemCallCurrentProcessorStatusValue);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_INTERRUPTED_PROCESSOR_STATUS_MASK,
                       Ia64SystemCallInterruptedProcessorStatusMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_INTERRUPTED_PROCESSOR_STATUS_VALUE,
                       Ia64SystemCallInterruptedProcessorStatusValue);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_BREAK_IMMEDIATE, Ia64SystemCallBreakImmediate);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_IFS_VALID_MASK, Ia64SystemCallIfsValidMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_INSTRUCTION_SLOT_SHIFT, Ia64SystemCallInstructionSlotShift);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_BUNDLE_SIZE_BYTES, Ia64SystemCallBundleSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_MAXIMUM_STACKED_REGISTERS, Ia64SystemCallMaximumStackedRegisters);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_PHYSICAL_STACKED_REGISTERS, Ia64PhysicalStackedRegisterCount);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_REQUIRED_OUTPUT_REGISTERS, Ia64SystemCallRequiredOutputRegisters);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_KERNEL_RSE_CONFIGURATION, Ia64SystemCallKernelRseConfiguration);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_KERNEL_FLOATING_POINT_STATUS,
                       Asteria::Platform::Ia64::Scheduler::Ia64KernelDefaultFloatingPointStatus);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_USER_RSE_CONFIGURATION, Ia64SystemCallUserRseConfiguration);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_FAULT_VECTOR_SELECTOR,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::Break) >>
                           Asteria::Platform::Ia64::Interrupt::Ia64UserExceptionVectorShift);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_FAULT_ENTRY_SELECTOR,
                       Asteria::Platform::Ia64::Interrupt::Ia64UserExceptionSelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_STORAGE_PAGE_SIZE_BYTES, Ia64SystemCallStoragePageSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_MEMORY_STACK_SIZE_BYTES, Ia64SystemCallMemoryStackSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_RSE_TRANSITION_SIZE_BYTES, Ia64SystemCallRseTransitionSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_RSE_BACKING_STORE_SIZE_BYTES, Ia64SystemCallRseBackingStoreSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_MAXIMUM_IA64_SYSTEM_CALL_PROCESSOR_COUNT, MaximumIa64SystemCallProcessorCount);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SYSTEM_CALL_FIRST_NON_KERNEL_REGION_ADDRESS,
                       Ia64SystemCallFirstNonKernelRegionAddress);
}
