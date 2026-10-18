// SPDX-License-Identifier: Apache-2.0

// Serialized TSC read. APIC timer adapters use Amd64LocalApic.

.text
.p2align 4
.global AsteriaAmd64ReadTimeStampCounter
.hidden AsteriaAmd64ReadTimeStampCounter
.type AsteriaAmd64ReadTimeStampCounter, @function
AsteriaAmd64ReadTimeStampCounter:
    pushq %rbx
    xorl %eax, %eax
    xorl %ecx, %ecx
    cpuid
    rdtsc
    shlq $32, %rdx
    orq %rdx, %rax
    popq %rbx
    ret
.size AsteriaAmd64ReadTimeStampCounter, . - AsteriaAmd64ReadTimeStampCounter
