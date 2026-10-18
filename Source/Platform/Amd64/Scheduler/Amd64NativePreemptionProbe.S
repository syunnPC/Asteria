// SPDX-License-Identifier: Apache-2.0

// Preemptible worker loop of the native-preemption diagnostic: it loads
// sentinel register values, spins with interrupts enabled until resumed, and
// stores the registers it observes afterwards. The sentinels are the C++
// constants of Amd64NativePreemptionProbe.hpp, from the generated layout.

#include "Generated/Platform/Amd64/Amd64NativePreemptionProbeAsmLayout.h"

    .text
    .p2align 4
    .global AsteriaAmd64PreemptibleLoop
    .hidden AsteriaAmd64PreemptibleLoop
    .type AsteriaAmd64PreemptibleLoop,@function
AsteriaAmd64PreemptibleLoop:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_RAX,%eax
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_RBX,%ebx
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_RCX,%ecx
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_RDX,%edx
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_RBP,%ebp
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_R8,%r8d
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_R9,%r9d
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_R10,%r10d
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_R11,%r11d
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_R12,%r12d
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_R13,%r13d
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_R14,%r14d
    mov $ASTERIA_AMD64_PREEMPTION_SENTINEL_R15,%r15d
    sti
    .global AsteriaAmd64PreemptibleLoopStart
    .hidden AsteriaAmd64PreemptibleLoopStart
AsteriaAmd64PreemptibleLoopStart:
    pause
    cmpq $1,(%rdi)
    jne AsteriaAmd64PreemptibleLoopStart
    .global AsteriaAmd64PreemptibleLoopEnd
    .hidden AsteriaAmd64PreemptibleLoopEnd
AsteriaAmd64PreemptibleLoopEnd:
    cli
    movq %rax,0(%rsi)
    movq %rbx,8(%rsi)
    movq %rcx,16(%rsi)
    movq %rdx,24(%rsi)
    movq %rbp,32(%rsi)
    movq %r8,40(%rsi)
    movq %r9,48(%rsi)
    movq %r10,56(%rsi)
    movq %r11,64(%rsi)
    movq %r12,72(%rsi)
    movq %r13,80(%rsi)
    movq %r14,88(%rsi)
    movq %r15,96(%rsi)
    movq %rdi,104(%rsi)
    movq %rsi,112(%rsi)
    movq %rsp,120(%rsi)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
    .size AsteriaAmd64PreemptibleLoop,.-AsteriaAmd64PreemptibleLoop
