// SPDX-License-Identifier: Apache-2.0
#include "Amd64SmpProtocol.h"
    .text
    .global AsteriaAmd64ComponentImageEntry
    .type AsteriaAmd64ComponentImageEntry,@function
AsteriaAmd64ComponentImageEntry:
    movq %rdi,%r12
    leaq AsteriaSmpControl(%rip),%r13
    movq $1,%r14
    movb %r12b,%cl
    shlq %cl,%r14
    lock orq %r14,ASTERIA_SMP_STARTED(%r13)
1:
    incq ASTERIA_SMP_PULSES(%r13,%r12,8)
    cmpq $1,ASTERIA_SMP_PHASE(%r13)
    jb 1b
    // Each worker faults on its own page, so resolving one does not mask
    // the other CPUs' faults in their shared AddressSpace.
    movq ASTERIA_SMP_WINDOW(%r13),%r15
    leaq 1(%r12),%rax
    shlq $12,%rax
    movq (%r15,%rax),%rdx
    movq (%r15),%rdx
    lock orq %r14,ASTERIA_SMP_READ(%r13)
2:
    incq ASTERIA_SMP_PULSES(%r13,%r12,8)
    cmpq $2,ASTERIA_SMP_PHASE(%r13)
    jb 2b
    // The supervisor publishes phase 2 only after MemoryUnmap completes.
    // This sibling read must enter its FaultChannel, never a stale TLB hit.
    movq (%r15),%rdx
    lock orq %r14,ASTERIA_SMP_AFTER_UNMAP(%r13)
3:
    incq ASTERIA_SMP_PULSES(%r13,%r12,8)
    cmpq $3,ASTERIA_SMP_PHASE(%r13)
    jb 3b
    movq $15,%rax // SystemCallNumber::ThreadExit (SystemCallAbi.hpp).
    movq $ASTERIA_SMP_EXIT,%rdi
    xorq %rsi,%rsi
    xorq %rdx,%rdx
    xorq %r10,%r10
    xorq %r8,%r8
    xorq %r9,%r9
    syscall
    ud2
    .size AsteriaAmd64ComponentImageEntry,.-AsteriaAmd64ComponentImageEntry
    .section .rodata
    .quad AsteriaSmpControl
    .bss
    .balign 4096
AsteriaSmpControl:
    .skip ASTERIA_SMP_PULSES+8*ASTERIA_SMP_WORKERS
    .section .note.GNU-stack,"",@progbits
