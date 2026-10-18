// SPDX-License-Identifier: Apache-2.0

// Serialized x2APIC interrupt-command write for TLB shootdown notification.

.text
.p2align 4
.global AsteriaAmd64WriteX2ApicInterruptCommand
.hidden AsteriaAmd64WriteX2ApicInterruptCommand
.type AsteriaAmd64WriteX2ApicInterruptCommand, @function
AsteriaAmd64WriteX2ApicInterruptCommand:
    movq %rdi, %rax
    movq %rdi, %rdx
    shrq $32, %rdx
    movl $0x830, %ecx
    mfence
    lfence
    wrmsr
    ret
.size AsteriaAmd64WriteX2ApicInterruptCommand, . - AsteriaAmd64WriteX2ApicInterruptCommand
