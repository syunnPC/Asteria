// SPDX-License-Identifier: Apache-2.0

// Cooperative AMD64 kernel-context entry and switch.
// Offsets into Amd64KernelRegisterState come from the generated C++ layout.

#include "Generated/Platform/Amd64/Amd64KernelContextAsmLayout.h"

.text
.p2align 4
.global AsteriaAmd64StartKernelContext
.hidden AsteriaAmd64StartKernelContext
.type AsteriaAmd64StartKernelContext, @function
AsteriaAmd64StartKernelContext:
    movq %r13, %rdi
    call *%r12
    cli
.Lasteria_amd64_returned_context_halt:
    hlt
    jmp .Lasteria_amd64_returned_context_halt
.size AsteriaAmd64StartKernelContext, . - AsteriaAmd64StartKernelContext
.p2align 4
.global AsteriaAmd64SwitchKernelContext
.hidden AsteriaAmd64SwitchKernelContext
.type AsteriaAmd64SwitchKernelContext, @function
AsteriaAmd64SwitchKernelContext:
    movq %rbx, ASTERIA_AMD64_KERNEL_REGISTER_STATE_RBX(%rdi)
    movq %rbp, ASTERIA_AMD64_KERNEL_REGISTER_STATE_RBP(%rdi)
    movq %r12, ASTERIA_AMD64_KERNEL_REGISTER_STATE_R12(%rdi)
    movq %r13, ASTERIA_AMD64_KERNEL_REGISTER_STATE_R13(%rdi)
    movq %r14, ASTERIA_AMD64_KERNEL_REGISTER_STATE_R14(%rdi)
    movq %r15, ASTERIA_AMD64_KERNEL_REGISTER_STATE_R15(%rdi)
    movq %rsp, ASTERIA_AMD64_KERNEL_REGISTER_STATE_RSP(%rdi)
    leaq .Lasteria_amd64_resume_kernel_context(%rip), %rax
    movq %rax, ASTERIA_AMD64_KERNEL_REGISTER_STATE_RIP(%rdi)
    movq ASTERIA_AMD64_KERNEL_REGISTER_STATE_RBX(%rsi), %rbx
    movq ASTERIA_AMD64_KERNEL_REGISTER_STATE_RBP(%rsi), %rbp
    movq ASTERIA_AMD64_KERNEL_REGISTER_STATE_R12(%rsi), %r12
    movq ASTERIA_AMD64_KERNEL_REGISTER_STATE_R13(%rsi), %r13
    movq ASTERIA_AMD64_KERNEL_REGISTER_STATE_R14(%rsi), %r14
    movq ASTERIA_AMD64_KERNEL_REGISTER_STATE_R15(%rsi), %r15
    movq ASTERIA_AMD64_KERNEL_REGISTER_STATE_RSP(%rsi), %rsp
    jmp *ASTERIA_AMD64_KERNEL_REGISTER_STATE_RIP(%rsi)
.Lasteria_amd64_resume_kernel_context:
    ret
.size AsteriaAmd64SwitchKernelContext, . - AsteriaAmd64SwitchKernelContext
