// SPDX-License-Identifier: Apache-2.0

// TLB shootdown interrupt entry; APIC accesses use Amd64LocalApic.

.text
.p2align 4
.global AsteriaAmd64TlbShootdownInterruptEntry
.hidden AsteriaAmd64TlbShootdownInterruptEntry
.type AsteriaAmd64TlbShootdownInterruptEntry, @function
AsteriaAmd64TlbShootdownInterruptEntry:
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %r12
    andq $-16, %rsp
    cld
    call AsteriaAmd64HandleTlbShootdownInterrupt
    movq %r12, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    iretq
.size AsteriaAmd64TlbShootdownInterruptEntry, . - AsteriaAmd64TlbShootdownInterruptEntry
