// SPDX-License-Identifier: Apache-2.0

// User-fault entry constants and the ar.k3 root, Ia64UserFaultEntryContext and
// Ia64PageFaultFrame layouts for Interrupt/Ia64UserFaultEntry.S (see
// Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Firmware/PalBootstrapCapabilityCapture.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"
#include "Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp"
#include "Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp"
#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Platform::Ia64::Firmware::Ia64PhysicalStackedRegisterCount;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultAlternateDataTlbVectorOffset;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultFrame;
using Asteria::Platform::Ia64::Interrupt::Ia64PageFaultFrameState;
using Asteria::Platform::Ia64::Interrupt::Ia64UserExceptionSelector;
using Asteria::Platform::Ia64::Interrupt::Ia64UserFaultAlternateDataTlbSelector;
using Asteria::Platform::Ia64::Interrupt::Ia64UserGeneralExceptionVectorOffset;
using Asteria::Platform::Ia64::Interrupt::Ia64UserFaultCurrentProcessorStatusMask;
using Asteria::Platform::Ia64::Interrupt::Ia64UserFaultCurrentProcessorStatusValue;
using Asteria::Platform::Ia64::Interrupt::Ia64UserFaultEntryContext;
using Asteria::Platform::Ia64::Interrupt::Ia64UserFaultEntryContextSizeBytes;
using Asteria::Platform::Ia64::Interrupt::Ia64UserFaultInterruptedProcessorStatusMask;
using Asteria::Platform::Ia64::Interrupt::Ia64UserFaultInterruptedProcessorStatusValue;
using Asteria::Platform::Ia64::Interrupt::Ia64UserFaultRejectedInterruptionStatusMask;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptMaskedTimerVector;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptSpuriousVector;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
using Asteria::Platform::Ia64::Interrupt::Ia64UserInterruptInterruptedProcessorStatusMask;
using Asteria::Platform::Ia64::Interrupt::Ia64UserInterruptInterruptedProcessorStatusValue;
using Asteria::Platform::Ia64::Interrupt::Ia64UserInterruptDeviceVectorCount;
using Asteria::Platform::Ia64::Interrupt::Ia64UserInterruptFirstDeviceVector;
using Asteria::Platform::Ia64::Interrupt::Ia64UserInterruptSelector;
using Asteria::Platform::Ia64::Interrupt::Ia64UserInterruptVectorOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeExternalInterruptOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableContext;
using Asteria::Platform::Ia64::System::Ia64SystemCallFirstNonKernelRegionAddress;
using Asteria::Platform::Ia64::System::Ia64SystemCallKernelRseConfiguration;
using Asteria::Platform::Ia64::System::Ia64SystemCallMemoryStackSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallRseBackingStoreSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallRseTransitionSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallStoragePageSizeBytes;
using Asteria::Platform::Ia64::System::Ia64SystemCallUserRseConfiguration;
using Asteria::Platform::Ia64::System::MaximumIa64SystemCallProcessorCount;

static_assert(Ia64UserInterruptVectorOffset == Ia64RuntimeExternalInterruptOffset);

// GeneralRegisters maps r1-r15 and then the interrupted bank-one r16-r31.
constexpr UInt64 BankedGeneralRegisterIndex{15U};
} // namespace

extern "C" void AsteriaIa64UserFaultEntryAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_INSTRUCTION_TLB,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::InstructionTlb));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_DATA_TLB,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::DataTlb));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_USER_VECTOR_ALTERNATE_INSTRUCTION_TLB,
        static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::AlternateInstructionTlb));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_ALTERNATE_DATA_TLB,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::AlternateDataTlb));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_USER_VECTOR_INSTRUCTION_KEY_MISS,
        static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::InstructionKeyMiss));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_DATA_KEY_MISS,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::DataKeyMiss));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_DIRTY_BIT,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::DirtyBit));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_USER_VECTOR_INSTRUCTION_ACCESS_BIT,
        static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::InstructionAccessBit));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_DATA_ACCESS_BIT,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::DataAccessBit));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_BREAK,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::Break));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_PAGE_NOT_PRESENT,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::PageNotPresent));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_KEY_PERMISSION,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::KeyPermission));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_USER_VECTOR_INSTRUCTION_ACCESS_RIGHTS,
        static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::InstructionAccessRights));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_DATA_ACCESS_RIGHTS,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::DataAccessRights));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_GENERAL_EXCEPTION,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::GeneralException));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_USER_VECTOR_DISABLED_FLOATING_POINT_REGISTER,
        static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::DisabledFloatingPointRegister));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_NAT_CONSUMPTION,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::NatConsumption));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_SPECULATION,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::Speculation));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_DEBUG,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::Debug));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_USER_VECTOR_UNALIGNED_REFERENCE,
        static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::UnalignedReference));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_USER_VECTOR_UNSUPPORTED_DATA_REFERENCE,
        static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::UnsupportedDataReference));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_USER_VECTOR_FLOATING_POINT_FAULT,
        static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::FloatingPointFault));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_FLOATING_POINT_TRAP,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::FloatingPointTrap));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_USER_VECTOR_LOWER_PRIVILEGE_TRANSFER,
        static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::LowerPrivilegeTransfer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_TAKEN_BRANCH,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::TakenBranch));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_VECTOR_SINGLE_STEP,
                       static_cast<UInt64>(Asteria::Platform::Ia64::Interrupt::Ia64UserFaultVector::SingleStep));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_BREAK_SYSTEM_CALL_IMMEDIATE,
                       Asteria::Platform::Ia64::System::Ia64SystemCallBreakImmediate);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_SELECTOR, Ia64UserFaultAlternateDataTlbSelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_VECTOR_OFFSET, Ia64PageFaultAlternateDataTlbVectorOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CURRENT_PROCESSOR_STATUS_MASK, Ia64UserFaultCurrentProcessorStatusMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CURRENT_PROCESSOR_STATUS_VALUE,
                       Ia64UserFaultCurrentProcessorStatusValue);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_INTERRUPTED_PROCESSOR_STATUS_MASK,
                       Ia64UserFaultInterruptedProcessorStatusMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_INTERRUPTED_PROCESSOR_STATUS_VALUE,
                       Ia64UserFaultInterruptedProcessorStatusValue);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_RSE_CONFIGURATION_MASK,
                       Asteria::Platform::Ia64::Interrupt::Ia64UserFaultRseConfigurationMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_REJECTED_INTERRUPTION_STATUS_MASK,
                       Ia64UserFaultRejectedInterruptionStatusMask);

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXCEPTION_SELECTOR, Ia64UserExceptionSelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXCEPTION_VECTOR_OFFSET, Ia64UserGeneralExceptionVectorOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_EXCEPTION_VECTOR_SHIFT,
                       Asteria::Platform::Ia64::Interrupt::Ia64UserExceptionVectorShift);

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_INTERRUPT_SELECTOR, Ia64UserInterruptSelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_INTERRUPT_VECTOR_OFFSET, Ia64UserInterruptVectorOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_INTERRUPT_FIRST_DEVICE_VECTOR, Ia64UserInterruptFirstDeviceVector);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_INTERRUPT_DEVICE_VECTOR_COUNT, Ia64UserInterruptDeviceVectorCount);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_INTERRUPT_INTERRUPTED_PROCESSOR_STATUS_MASK,
                       Ia64UserInterruptInterruptedProcessorStatusMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_INTERRUPT_INTERRUPTED_PROCESSOR_STATUS_VALUE,
                       Ia64UserInterruptInterruptedProcessorStatusValue);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_INTERRUPT_TIMER_VECTOR, UInt64{Ia64ExternalInterruptTimerVector});
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_INTERRUPT_SPURIOUS_VECTOR, UInt64{Ia64ExternalInterruptSpuriousVector});
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_INTERRUPT_MASKED_TIMER_VECTOR, Ia64ExternalInterruptMaskedTimerVector);

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FIRST_NON_KERNEL_REGION_ADDRESS,
                       Ia64SystemCallFirstNonKernelRegionAddress);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_MAXIMUM_PROCESSOR_COUNT, MaximumIa64SystemCallProcessorCount);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_STORAGE_PAGE_SIZE_BYTES, Ia64SystemCallStoragePageSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_MEMORY_STACK_SIZE_BYTES, Ia64SystemCallMemoryStackSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_RSE_BACKING_STORE_SIZE_BYTES, Ia64SystemCallRseBackingStoreSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_RSE_TRANSITION_SIZE_BYTES, Ia64SystemCallRseTransitionSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_KERNEL_RSE_CONFIGURATION, Ia64SystemCallKernelRseConfiguration);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_USER_RSE_CONFIGURATION, Ia64SystemCallUserRseConfiguration);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_PHYSICAL_STACKED_REGISTERS, Ia64PhysicalStackedRegisterCount);

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_ROOT_SIZE, sizeof(Ia64RuntimeInterruptVectorTableContext));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_ROOT_ALIGNMENT_MASK,
                       alignof(Ia64RuntimeInterruptVectorTableContext) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_ROOT_PROCESSOR_INDEX,
                       __builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, ProcessorIndex));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_ROOT_CONTEXT_ADDRESS,
                       __builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, UserFaultEntryContextAddress));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CONTEXT_SIZE, Ia64UserFaultEntryContextSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CONTEXT_ALIGNMENT_MASK, alignof(Ia64UserFaultEntryContext) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CONTEXT_FRAME_ADDRESS,
                       __builtin_offsetof(Ia64UserFaultEntryContext, FrameAddress));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CONTEXT_MEMORY_STACK_START,
                       __builtin_offsetof(Ia64UserFaultEntryContext, MemoryStackStart));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CONTEXT_MEMORY_STACK_END,
                       __builtin_offsetof(Ia64UserFaultEntryContext, MemoryStackEnd));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CONTEXT_RSE_BACKING_STORE_START,
                       __builtin_offsetof(Ia64UserFaultEntryContext, RseBackingStoreStart));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CONTEXT_RSE_BACKING_STORE_END,
                       __builtin_offsetof(Ia64UserFaultEntryContext, RseBackingStoreEnd));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CONTEXT_HANDLER_ADDRESS,
                       __builtin_offsetof(Ia64UserFaultEntryContext, HandlerAddress));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CONTEXT_PROCESSOR_INDEX,
                       __builtin_offsetof(Ia64UserFaultEntryContext, ProcessorIndex));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_CONTEXT_RESERVED,
                       __builtin_offsetof(Ia64UserFaultEntryContext, Reserved));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_SIZE, sizeof(Ia64PageFaultFrame));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_ALIGNMENT_MASK, alignof(Ia64PageFaultFrame) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_STATE_INACTIVE,
                       static_cast<UInt64>(Ia64PageFaultFrameState::Inactive));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_STATE_CAPTURING,
                       static_cast<UInt64>(Ia64PageFaultFrameState::Capturing));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_STATE_RESUME_COMMITTED,
                       static_cast<UInt64>(Ia64PageFaultFrameState::ResumeCommitted));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_PUBLISHED_TOKEN,
                       __builtin_offsetof(Ia64PageFaultFrame, PublishedToken));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_STATE, __builtin_offsetof(Ia64PageFaultFrame, State));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_PRE_COVER_INTERRUPTION_STATUS,
                       __builtin_offsetof(Ia64PageFaultFrame, PreCoverInterruptionStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_VECTOR_OFFSET,
                       __builtin_offsetof(Ia64PageFaultFrame, VectorOffset));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_CURRENT_PROCESSOR_STATUS,
                       __builtin_offsetof(Ia64PageFaultFrame, CurrentProcessorStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTED_INSTRUCTION_POINTER,
                       __builtin_offsetof(Ia64PageFaultFrame, InterruptedInstructionPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTED_PROCESSOR_STATUS,
                       __builtin_offsetof(Ia64PageFaultFrame, InterruptedProcessorStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_HARDWARE_INTERRUPTION_FUNCTION_STATE,
                       __builtin_offsetof(Ia64PageFaultFrame, HardwareInterruptionFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_COVERED_INTERRUPTION_FUNCTION_STATE,
                       __builtin_offsetof(Ia64PageFaultFrame, CoveredInterruptionFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTION_STATUS,
                       __builtin_offsetof(Ia64PageFaultFrame, InterruptionStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTION_FAULTING_ADDRESS,
                       __builtin_offsetof(Ia64PageFaultFrame, InterruptionFaultingAddress));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTION_TRANSLATION_INFORMATION,
                       __builtin_offsetof(Ia64PageFaultFrame, InterruptionTranslationInformation));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTION_PREVIOUS_INSTRUCTION_POINTER,
                       __builtin_offsetof(Ia64PageFaultFrame, InterruptionPreviousInstructionPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTION_IMMEDIATE,
                       __builtin_offsetof(Ia64PageFaultFrame, InterruptionImmediate));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_PREVIOUS_FUNCTION_STATE,
                       __builtin_offsetof(Ia64PageFaultFrame, PreviousFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_RSE_CONFIGURATION,
                       __builtin_offsetof(Ia64PageFaultFrame, RseConfiguration));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_RSE_BACKING_STORE_POINTER,
                       __builtin_offsetof(Ia64PageFaultFrame, RseBackingStorePointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_RSE_BACKING_STORE_STORE_POINTER,
                       __builtin_offsetof(Ia64PageFaultFrame, RseBackingStoreStorePointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_RSE_NAT_COLLECTION,
                       __builtin_offsetof(Ia64PageFaultFrame, RseNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_RSE_DIRTY_PARTITION_LOAD_CONFIGURATION,
                       __builtin_offsetof(Ia64PageFaultFrame, RseDirtyPartitionLoadConfiguration));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_ORIGINAL_NAT_COLLECTION,
                       __builtin_offsetof(Ia64PageFaultFrame, OriginalNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_STATIC_GENERAL_REGISTER_SPILL_NAT_COLLECTION,
                       __builtin_offsetof(Ia64PageFaultFrame, StaticGeneralRegisterSpillNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_BANKED_GENERAL_REGISTER_SPILL_NAT_COLLECTION,
                       __builtin_offsetof(Ia64PageFaultFrame, BankedGeneralRegisterSpillNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_LOOP_COUNTER, __builtin_offsetof(Ia64PageFaultFrame, LoopCounter));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_EPILOG_COUNTER,
                       __builtin_offsetof(Ia64PageFaultFrame, EpilogCounter));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_COMPARE_AND_STORE_DATA,
                       __builtin_offsetof(Ia64PageFaultFrame, CompareAndStoreData));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_COMPARE_EXCHANGE_VALUE,
                       __builtin_offsetof(Ia64PageFaultFrame, CompareExchangeValue));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_FLOATING_POINT_STATUS,
                       __builtin_offsetof(Ia64PageFaultFrame, FloatingPointStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_PREDICATES, __builtin_offsetof(Ia64PageFaultFrame, Predicates));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_BRANCH_REGISTERS,
                       __builtin_offsetof(Ia64PageFaultFrame, BranchRegisters));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_GENERAL_REGISTERS_R1,
                       __builtin_offsetof(Ia64PageFaultFrame, GeneralRegisters));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_GENERAL_REGISTERS_R16,
                       __builtin_offsetof(Ia64PageFaultFrame, GeneralRegisters) +
                           (BankedGeneralRegisterIndex * sizeof(UInt64)));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_FLOATING_REGISTERS,
                       __builtin_offsetof(Ia64PageFaultFrame, FloatingRegisters));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_FRAME_COVERED_RSE_BACKING_STORE_POINTER,
                       __builtin_offsetof(Ia64PageFaultFrame, CoveredRseBackingStorePointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_FAULT_KERNEL_FLOATING_POINT_STATUS,
                       Asteria::Platform::Ia64::Scheduler::Ia64KernelDefaultFloatingPointStatus);
}
