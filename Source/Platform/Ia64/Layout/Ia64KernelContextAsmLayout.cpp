// SPDX-License-Identifier: Apache-2.0

// Ia64KernelRegisterState offsets for Scheduler/Ia64KernelContext.S (see
// Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"

namespace
{
using Asteria::Platform::Ia64::Scheduler::Ia64KernelRegisterState;
} // namespace

extern "C" void AsteriaIa64KernelContextAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_R4, __builtin_offsetof(Ia64KernelRegisterState, R4));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_R5, __builtin_offsetof(Ia64KernelRegisterState, R5));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_R6, __builtin_offsetof(Ia64KernelRegisterState, R6));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_R7, __builtin_offsetof(Ia64KernelRegisterState, R7));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_GLOBAL_POINTER,
                       __builtin_offsetof(Ia64KernelRegisterState, GlobalPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_STACK_POINTER,
                       __builtin_offsetof(Ia64KernelRegisterState, StackPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_THREAD_POINTER,
                       __builtin_offsetof(Ia64KernelRegisterState, ThreadPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_INSTRUCTION_POINTER,
                       __builtin_offsetof(Ia64KernelRegisterState, InstructionPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_PREVIOUS_FUNCTION_STATE,
                       __builtin_offsetof(Ia64KernelRegisterState, PreviousFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_RSE_CONFIGURATION,
                       __builtin_offsetof(Ia64KernelRegisterState, RseConfiguration));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_RSE_BACKING_STORE_POINTER,
                       __builtin_offsetof(Ia64KernelRegisterState, RseBackingStorePointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_RSE_NAT_COLLECTION,
                       __builtin_offsetof(Ia64KernelRegisterState, RseNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_USER_NAT_COLLECTION,
                       __builtin_offsetof(Ia64KernelRegisterState, UserNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_PRESERVED_GENERAL_NAT_COLLECTION,
                       __builtin_offsetof(Ia64KernelRegisterState, PreservedGeneralNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_LOOP_COUNTER,
                       __builtin_offsetof(Ia64KernelRegisterState, LoopCounter));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_FLOATING_POINT_STATUS,
                       __builtin_offsetof(Ia64KernelRegisterState, FloatingPointStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_PREDICATES,
                       __builtin_offsetof(Ia64KernelRegisterState, Predicates));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_B1, __builtin_offsetof(Ia64KernelRegisterState, B1));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_B2, __builtin_offsetof(Ia64KernelRegisterState, B2));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_B3, __builtin_offsetof(Ia64KernelRegisterState, B3));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_B4, __builtin_offsetof(Ia64KernelRegisterState, B4));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_B5, __builtin_offsetof(Ia64KernelRegisterState, B5));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_KERNEL_REGISTER_STATE_FLOATING_REGISTERS,
                       __builtin_offsetof(Ia64KernelRegisterState, FloatingRegisters));
}
