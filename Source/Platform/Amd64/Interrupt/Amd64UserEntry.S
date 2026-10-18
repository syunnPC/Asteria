// SPDX-License-Identifier: Apache-2.0

// Descriptor-table load, the 256 interrupt vector stubs and their address
// table, SYSCALL entry, and the shared trap-frame save and user return.
// Selectors, the system-call vector, and Amd64EntryContext and
// Amd64TrapFrame offsets come from the generated C++ layout.

#include "Generated/Platform/Amd64/Amd64UserEntryAsmLayout.h"

    .text
    .p2align 4
    .global AsteriaAmd64LoadDescriptors
    .hidden AsteriaAmd64LoadDescriptors
    .type AsteriaAmd64LoadDescriptors,@function
AsteriaAmd64LoadDescriptors:
    cli
    lgdt (%rdi)
    pushq $ASTERIA_KERNEL_CODE_SELECTOR
    leaq 1f(%rip),%rax
    pushq %rax
    lretq
1:  movw $ASTERIA_KERNEL_DATA_SELECTOR,%ax
    movw %ax,%ss
    movw %ax,%ds
    movw %ax,%es
    xor %eax,%eax
    movw %ax,%fs
    movw %ax,%gs
    movw $ASTERIA_TASK_STATE_SELECTOR,%ax
    ltr %ax
    lidt (%rsi)
    ret
    .size AsteriaAmd64LoadDescriptors,.-AsteriaAmd64LoadDescriptors

    .altmacro
    .macro ASTERIA_VECTOR number
    .p2align 4
    .type AsteriaAmd64Vector\number,@function
AsteriaAmd64Vector\number:
    .if (\number != 8) && (\number != 10) && (\number != 11) && (\number != 12) && (\number != 13) && (\number != 14) && (\number != 17) && (\number != 21) && (\number != 29) && (\number != 30)
    pushq $0
    .endif
    pushq $\number
    jmp AsteriaAmd64InterruptEntry
    .size AsteriaAmd64Vector\number,.-AsteriaAmd64Vector\number
    .endm
    .set asteria_vector,0
    .rept 256
    ASTERIA_VECTOR %asteria_vector
    .set asteria_vector,asteria_vector+1
    .endr
    .macro ASTERIA_VECTOR_ADDRESS number
    .quad AsteriaAmd64Vector\number
    .endm
    .pushsection .rodata
    .p2align 3
    .global AsteriaAmd64InterruptHandlers
    .hidden AsteriaAmd64InterruptHandlers
    .type AsteriaAmd64InterruptHandlers,@object
AsteriaAmd64InterruptHandlers:
    .set asteria_vector,0
    .rept 256
    ASTERIA_VECTOR_ADDRESS %asteria_vector
    .set asteria_vector,asteria_vector+1
    .endr
    .size AsteriaAmd64InterruptHandlers,.-AsteriaAmd64InterruptHandlers
    .popsection
    .noaltmacro

    .p2align 4
    .global AsteriaAmd64SystemCallEntry
    .hidden AsteriaAmd64SystemCallEntry
    .type AsteriaAmd64SystemCallEntry,@function
AsteriaAmd64SystemCallEntry:
    swapgs
    lfence
    movq %rsp,%gs:ASTERIA_AMD64_ENTRY_CONTEXT_USER_STACK_SCRATCH
    movq %gs:ASTERIA_AMD64_ENTRY_CONTEXT_KERNEL_STACK_TOP,%rsp
    pushq $ASTERIA_USER_DATA_SELECTOR
    pushq %gs:ASTERIA_AMD64_ENTRY_CONTEXT_USER_STACK_SCRATCH
    pushq %r11
    pushq $ASTERIA_USER_CODE_SELECTOR
    pushq %rcx
    pushq $0
    pushq $ASTERIA_SYSTEM_CALL_VECTOR
    jmp AsteriaAmd64SaveTrapRegisters
    .size AsteriaAmd64SystemCallEntry,.-AsteriaAmd64SystemCallEntry

    .type AsteriaAmd64InterruptEntry,@function
AsteriaAmd64InterruptEntry:
    testb $3,ASTERIA_AMD64_TRAP_FRAME_CS - ASTERIA_AMD64_TRAP_FRAME_VECTOR(%rsp)
    jz AsteriaAmd64SaveTrapRegisters
    swapgs
    lfence
AsteriaAmd64SaveTrapRegisters:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    movq %rsp,%rbx
    movq %rsp,%rdi
    andq $-16,%rsp
    call AsteriaAmd64DispatchTrap
    movq %rbx,%rsp
    jmp AsteriaAmd64PopTrapRegisters
    .size AsteriaAmd64InterruptEntry,.-AsteriaAmd64InterruptEntry

    .global AsteriaAmd64RestoreUserFrame
    .hidden AsteriaAmd64RestoreUserFrame
    .type AsteriaAmd64RestoreUserFrame,@function
AsteriaAmd64RestoreUserFrame:
    movq %rdi,%rsp
AsteriaAmd64PopTrapRegisters:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $ASTERIA_AMD64_TRAP_FRAME_RIP - ASTERIA_AMD64_TRAP_FRAME_VECTOR,%rsp
    testb $3,ASTERIA_AMD64_TRAP_FRAME_CS - ASTERIA_AMD64_TRAP_FRAME_RIP(%rsp)
    jz 2f
    swapgs
2:  iretq
    .size AsteriaAmd64RestoreUserFrame,.-AsteriaAmd64RestoreUserFrame
