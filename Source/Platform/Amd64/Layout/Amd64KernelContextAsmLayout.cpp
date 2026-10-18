// SPDX-License-Identifier: Apache-2.0

// Amd64KernelRegisterState offsets for Scheduler/Amd64KernelContext.S (see
// Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Amd64/Scheduler/Amd64KernelContext.hpp"

namespace
{
using Asteria::Platform::Amd64::Scheduler::Amd64KernelRegisterState;
} // namespace

extern "C" void AsteriaAmd64KernelContextAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_KERNEL_REGISTER_STATE_RBX, __builtin_offsetof(Amd64KernelRegisterState, Rbx));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_KERNEL_REGISTER_STATE_RBP, __builtin_offsetof(Amd64KernelRegisterState, Rbp));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_KERNEL_REGISTER_STATE_R12, __builtin_offsetof(Amd64KernelRegisterState, R12));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_KERNEL_REGISTER_STATE_R13, __builtin_offsetof(Amd64KernelRegisterState, R13));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_KERNEL_REGISTER_STATE_R14, __builtin_offsetof(Amd64KernelRegisterState, R14));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_KERNEL_REGISTER_STATE_R15, __builtin_offsetof(Amd64KernelRegisterState, R15));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_KERNEL_REGISTER_STATE_RSP, __builtin_offsetof(Amd64KernelRegisterState, Rsp));
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_KERNEL_REGISTER_STATE_RIP, __builtin_offsetof(Amd64KernelRegisterState, Rip));
}
