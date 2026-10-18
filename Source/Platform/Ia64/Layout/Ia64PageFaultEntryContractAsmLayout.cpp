// SPDX-License-Identifier: Apache-2.0

// Page-fault entry constants and the ar.k3 context, Ia64PageFaultEntryContext,
// Ia64PageFaultEntryControl and Ia64PageFaultFrame layouts for
// Interrupt/Ia64PageFaultEntryContract.S (see Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp"

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultCaptureStage;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultDataAccessRightsVectorOffset;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultEntryContext;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultEntryControl;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultEntryControlState;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultFrame;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultInstructionAccessRightsVectorOffset;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultKeyPermissionVectorOffset;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultMaximumProcessorCount;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultPageNotPresentVectorOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableContext;
} // namespace

extern "C" void AsteriaIa64PageFaultEntryContractAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_MAXIMUM_PROCESSOR_COUNT, Ia64PageFaultMaximumProcessorCount);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_PAGE_NOT_PRESENT_VECTOR_OFFSET, Ia64PageFaultPageNotPresentVectorOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_KEY_PERMISSION_VECTOR_OFFSET, Ia64PageFaultKeyPermissionVectorOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_INSTRUCTION_ACCESS_RIGHTS_VECTOR_OFFSET,
                       Ia64PageFaultInstructionAccessRightsVectorOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_DATA_ACCESS_RIGHTS_VECTOR_OFFSET,
                       Ia64PageFaultDataAccessRightsVectorOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_STATE_CONFIGURED,
                       static_cast<UInt64>(Ia64PageFaultEntryControlState::Configured));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_STATE_CLAIMING,
                       static_cast<UInt64>(Ia64PageFaultEntryControlState::Claiming));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_STATE_CAPTURING,
                       static_cast<UInt64>(Ia64PageFaultEntryControlState::Capturing));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_CAPTURE_STAGE_INTERRUPTION_STATE,
                       static_cast<UInt64>(Ia64PageFaultCaptureStage::InterruptionState));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_SIZE,
                       sizeof(Ia64RuntimeInterruptVectorTableContext));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_ALIGNMENT_MASK,
                       alignof(Ia64RuntimeInterruptVectorTableContext) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_PROCESSOR_INDEX,
                       __builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, ProcessorIndex));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_PAGE_FAULT_ENTRY_CONTEXT_ADDRESS,
                       __builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, PageFaultEntryContextAddress));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTEXT_SIZE, sizeof(Ia64PageFaultEntryContext));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTEXT_ALIGNMENT_MASK, alignof(Ia64PageFaultEntryContext) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTEXT_PROCESSOR_INDEX,
                       __builtin_offsetof(Ia64PageFaultEntryContext, ProcessorIndex));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTEXT_PAGE_NOT_PRESENT_VECTOR_OFFSET,
                       __builtin_offsetof(Ia64PageFaultEntryContext, PageNotPresentVectorOffset));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_SIZE, sizeof(Ia64PageFaultEntryControl));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_ALIGNMENT_MASK, alignof(Ia64PageFaultEntryControl) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_PROCESSOR_INDEX,
                       __builtin_offsetof(Ia64PageFaultEntryControl, ProcessorIndex));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_ENTRY_CAPTURE_TOKEN,
                       __builtin_offsetof(Ia64PageFaultEntryControl, EntryCaptureToken));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_FRAME_SIZE, sizeof(Ia64PageFaultFrame));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_FRAME_ALIGNMENT_MASK, alignof(Ia64PageFaultFrame) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_FRAME_PUBLISHED_TOKEN,
                       __builtin_offsetof(Ia64PageFaultFrame, PublishedToken));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_FRAME_CAPTURE_TOKEN,
                       __builtin_offsetof(Ia64PageFaultFrame, CaptureToken));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_PAGE_FAULT_FRAME_CAPTURE_STAGE,
                       __builtin_offsetof(Ia64PageFaultFrame, CaptureStage));
}
