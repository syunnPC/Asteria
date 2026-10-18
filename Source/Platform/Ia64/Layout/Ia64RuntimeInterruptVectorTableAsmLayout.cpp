// SPDX-License-Identifier: Apache-2.0

// Runtime IVT geometry, the context, binding and ledger layout its refill
// handlers read, and the user-fault selector of the data handler's escape, for
// Memory/Ia64RuntimeInterruptVectorTable.S (see Platform/AsmLayout.hpp).

#include "Kernel/Memory/AddressSpaceMappingRecord.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp"
#include "Platform/Ia64/Memory/Ia64ActiveRuntimeTranslationBindings.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationLedger.hpp"

namespace
{
using Asteria::Kernel::Memory::MaximumRuntimeAddressSpaceMappings;
using Asteria::Kernel::Scheduler::MaximumProcessorCount;
using Asteria::Platform::Ia64::Interrupt::Ia64UserExceptionSelector;
using Asteria::Platform::Ia64::Interrupt::Ia64UserFaultAlternateDataTlbSelector;
using Asteria::Platform::Ia64::Memory::Ia64ActiveRuntimeTranslationBindingsWireStorage;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeAlternateDataTlbOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeAlternateInstructionTlbOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeBreakOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeDataAccessRightsOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeExternalInterruptOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeFloatingPointFaultOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeFloatingPointTrapOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeGeneralExceptionOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeInstructionAccessRightsOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeInterruptVectorSlotSizeBytes;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableContext;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableSizeBytes;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeKeyPermissionOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimePageNotPresentOffset;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeTranslationInsertion;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerWireStorage;
} // namespace

extern "C" void AsteriaIa64RuntimeInterruptVectorTableAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_SIZE_BYTES,
                       Ia64RuntimeInterruptVectorTableSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES, Ia64RuntimeInterruptVectorSlotSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES,
                       Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_ALTERNATE_INSTRUCTION_TLB_OFFSET, Ia64RuntimeAlternateInstructionTlbOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_ALTERNATE_DATA_TLB_OFFSET, Ia64RuntimeAlternateDataTlbOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_BREAK_OFFSET, Ia64RuntimeBreakOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_EXTERNAL_INTERRUPT_OFFSET, Ia64RuntimeExternalInterruptOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_PAGE_NOT_PRESENT_OFFSET, Ia64RuntimePageNotPresentOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_KEY_PERMISSION_OFFSET, Ia64RuntimeKeyPermissionOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INSTRUCTION_ACCESS_RIGHTS_OFFSET, Ia64RuntimeInstructionAccessRightsOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_DATA_ACCESS_RIGHTS_OFFSET, Ia64RuntimeDataAccessRightsOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_GENERAL_EXCEPTION_OFFSET, Ia64RuntimeGeneralExceptionOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_FLOATING_POINT_FAULT_OFFSET, Ia64RuntimeFloatingPointFaultOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_FLOATING_POINT_TRAP_OFFSET, Ia64RuntimeFloatingPointTrapOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_TRANSLATION_INSERTION, Ia64RuntimeTranslationInsertion);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_PROCESSOR_INDEX,
                       __builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, ProcessorIndex));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_ALIGNMENT_MASK,
                       alignof(Ia64RuntimeInterruptVectorTableContext) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_ACTIVE_RUNTIME_TRANSLATION_BINDINGS_WIRE_STORAGE_ALIGNMENT_MASK,
                       alignof(Ia64ActiveRuntimeTranslationBindingsWireStorage) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_TRANSLATION_LEDGER_WIRE_STORAGE_ALIGNMENT_MASK,
                       alignof(Ia64RuntimeTranslationLedgerWireStorage) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_TRANSLATION_LEDGER_WIRE_STORAGE_RECORDS,
                       __builtin_offsetof(Ia64RuntimeTranslationLedgerWireStorage, Records));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_TRANSLATION_LEDGER_WIRE_STORAGE_PUBLICATION_WORD,
                       __builtin_offsetof(Ia64RuntimeTranslationLedgerWireStorage, PublicationWord));
    // The publication word is n + 1 for n published records.
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_TRANSLATION_LEDGER_MAXIMUM_PUBLICATION,
                       MaximumRuntimeAddressSpaceMappings + 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_MAXIMUM_PROCESSOR_COUNT, MaximumProcessorCount);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_USER_FAULT_SELECTOR, Ia64UserFaultAlternateDataTlbSelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_RUNTIME_USER_EXCEPTION_SELECTOR, Ia64UserExceptionSelector);
}
