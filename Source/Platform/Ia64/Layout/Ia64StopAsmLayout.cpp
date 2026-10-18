// SPDX-License-Identifier: Apache-2.0

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"
#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp"

namespace
{
using Asteria::Platform::Ia64::Diagnostics::Ia64StopStorage;
using Asteria::Platform::Ia64::Diagnostics::Ia64StopsEvidence;
using Asteria::Platform::Ia64::Diagnostics::Ia64StopTerminalContext;
using Asteria::Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableContext;
}
extern "C" void AsteriaIa64StopAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_MAXIMUM_PROCESSORS, Asteria::Kernel::Scheduler::MaximumProcessorCount);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_FROZEN,
                       static_cast<Asteria::Foundation::UInt64>(Asteria::Kernel::Diagnostics::StopState::Frozen));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_STOPPING,
                       static_cast<Asteria::Foundation::UInt64>(Asteria::Kernel::Diagnostics::StopState::Stopping));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_BREAK_VECTOR, Asteria::Platform::Ia64::Memory::Ia64RuntimeBreakOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_EXTERNAL_VECTOR,
                       Asteria::Platform::Ia64::Memory::Ia64RuntimeExternalInterruptOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_PAGE_FAULT_VECTOR,
                       Asteria::Platform::Ia64::Memory::Ia64RuntimePageNotPresentOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_DATA_MISS_VECTOR,
                       Asteria::Platform::Ia64::Memory::Ia64RuntimeAlternateDataTlbOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_INSTRUCTION_MISS_VECTOR,
                       Asteria::Platform::Ia64::Memory::Ia64RuntimeAlternateInstructionTlbOffset);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_EXIT_SHIFT, Asteria::Platform::Ia64::Diagnostics::Ia64StopExitShift);
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_STOP_EXIT_REFILL_HANDLER,
        static_cast<Asteria::Foundation::UInt64>(Asteria::Platform::Ia64::Diagnostics::Ia64StopExit::RefillHandler));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_STOP_EXIT_USER_ENTRY,
        static_cast<Asteria::Foundation::UInt64>(Asteria::Platform::Ia64::Diagnostics::Ia64StopExit::UserEntry));
    ASTERIA_ASM_LAYOUT(
        ASTERIA_IA64_STOP_EXIT_USER_FAULT_BRIDGE,
        static_cast<Asteria::Foundation::UInt64>(Asteria::Platform::Ia64::Diagnostics::Ia64StopExit::UserFaultBridge));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_BRIDGE_STATUS_SHIFT,
                       Asteria::Platform::Ia64::Diagnostics::Ia64StopBridgeStatusShift);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_BRIDGE_STATUS_BITS,
                       Asteria::Platform::Ia64::Diagnostics::Ia64StopBridgeStatusBits);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_KERNEL_FPSR,
                       Asteria::Platform::Ia64::Scheduler::Ia64KernelDefaultFloatingPointStatus);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_CONTEXT_STORAGE,
                       __builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, StopStorageAddress));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_CONTEXT_CPU,
                       __builtin_offsetof(Ia64RuntimeInterruptVectorTableContext, ProcessorIndex));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_STORAGE_EVIDENCE, __builtin_offsetof(Ia64StopStorage, Evidence));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_STORAGE_RECORDS, __builtin_offsetof(Ia64StopStorage, Evidence) +
                                                              __builtin_offsetof(Ia64StopsEvidence, Processors));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_STORAGE_TERMINAL, __builtin_offsetof(Ia64StopStorage, Terminal));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_POWER_LATCH_SHIFT, Asteria::Platform::Ia64::Diagnostics::Ia64PowerLatchShift);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_POWER_CONFIRMED, __builtin_offsetof(Ia64StopStorage, PowerConfirmed));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_POWER_INIT_MESSAGE, Asteria::Platform::Ia64::Diagnostics::Ia64PowerInitMessage);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_TERMINAL_BYTES, sizeof(Ia64StopTerminalContext));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_RECORD_BYTES,
                       sizeof(Asteria::Platform::Ia64::Diagnostics::Ia64ProcessorStopRecord));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_SNAPSHOT,
                       __builtin_offsetof(Asteria::Platform::Ia64::Diagnostics::Ia64ProcessorStopRecord, Snapshot));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_STOP_STATE, __builtin_offsetof(Asteria::Kernel::Diagnostics::StopRecord, State));
}
