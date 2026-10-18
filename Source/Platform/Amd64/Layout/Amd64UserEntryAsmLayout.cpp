// SPDX-License-Identifier: Apache-2.0

// Selectors, the system-call vector, and Amd64EntryContext and Amd64TrapFrame
// offsets for Interrupt/Amd64UserEntry.S (see Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"

namespace
{
namespace Interrupt = Asteria::Platform::Amd64::Interrupt;
using Interrupt::Amd64EntryContext;
using Interrupt::Amd64TrapFrame;
} // namespace

extern "C" void AsteriaAmd64UserEntryAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_KERNEL_CODE_SELECTOR, Interrupt::KernelCodeSelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_KERNEL_DATA_SELECTOR, Interrupt::KernelDataSelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_USER_DATA_SELECTOR, Interrupt::UserDataSelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_USER_CODE_SELECTOR, Interrupt::UserCodeSelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_TASK_STATE_SELECTOR, Interrupt::TaskStateSelector);
    ASTERIA_ASM_LAYOUT(ASTERIA_SYSTEM_CALL_VECTOR, Interrupt::SystemCallVector);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_ENTRY_CONTEXT_KERNEL_STACK_TOP,
                       __builtin_offsetof(Amd64EntryContext, KernelStackTop));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_ENTRY_CONTEXT_USER_STACK_SCRATCH,
                       __builtin_offsetof(Amd64EntryContext, UserStackScratch));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_TRAP_FRAME_VECTOR, __builtin_offsetof(Amd64TrapFrame, Vector));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_TRAP_FRAME_RIP, __builtin_offsetof(Amd64TrapFrame, Rip));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_TRAP_FRAME_CS, __builtin_offsetof(Amd64TrapFrame, Cs));
}
