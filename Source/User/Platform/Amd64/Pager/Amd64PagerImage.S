// SPDX-License-Identifier: Apache-2.0

// Entry of the AMD64 boot pager image: calls the C++ main with the address of
// its start block (RDI) and idles if the pager ever stops serving.

    .text
    .global AsteriaAmd64PagerImageEntry
    .hidden AsteriaAmd64PagerImageEntry
    .type AsteriaAmd64PagerImageEntry,@function
AsteriaAmd64PagerImageEntry:
    xor %ebp,%ebp
    call AsteriaAmd64PagerImageMain
    .global AsteriaAmd64PagerImageIdle
    .hidden AsteriaAmd64PagerImageIdle
    .type AsteriaAmd64PagerImageIdle,@function
AsteriaAmd64PagerImageIdle:
    pause
    jmp AsteriaAmd64PagerImageIdle
    .size AsteriaAmd64PagerImageIdle,.-AsteriaAmd64PagerImageIdle
    .size AsteriaAmd64PagerImageEntry,.-AsteriaAmd64PagerImageEntry

// The pager's self-test thread: its entry loads the thread's own pattern into xmm0-xmm15 and MXCSR,
// unlike the spinner's and the guest's, and calls the C++ main with the control block's address the
// kernel passed in RDI. The C++ code is compiled general-registers-only, so only the kernel's
// context switches can change the pattern before AsteriaAmd64PagerThreadStateIntact checks it.

    .section .rodata
    .p2align 4
AsteriaAmd64PagerThreadVectors:
    .irp index,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .quad 0x5448524541440000 + \index, 0x3c3c3c3c3c3c3c3c ^ \index
    .endr
AsteriaAmd64PagerThreadMxcsr:
    .long 0x5f80

    .text
    .global AsteriaAmd64PagerThreadEntry
    .hidden AsteriaAmd64PagerThreadEntry
    .type AsteriaAmd64PagerThreadEntry,@function
AsteriaAmd64PagerThreadEntry:
    xor %ebp,%ebp
    lea AsteriaAmd64PagerThreadVectors(%rip),%rax
    .irp index,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    movdqa 16*\index(%rax),%xmm\index
    .endr
    ldmxcsr AsteriaAmd64PagerThreadMxcsr(%rip)
    call AsteriaAmd64PagerThreadMain
1:
    pause
    jmp 1b
    .size AsteriaAmd64PagerThreadEntry,.-AsteriaAmd64PagerThreadEntry

// Returns 1 in EAX when xmm0-xmm15 and MXCSR still hold the thread's pattern.
    .global AsteriaAmd64PagerThreadStateIntact
    .hidden AsteriaAmd64PagerThreadStateIntact
    .type AsteriaAmd64PagerThreadStateIntact,@function
AsteriaAmd64PagerThreadStateIntact:
    lea AsteriaAmd64PagerThreadVectors(%rip),%rcx
    mov $1,%eax
    .irp index,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    pcmpeqb 16*\index(%rcx),%xmm\index
    pmovmskb %xmm\index,%edx
    cmp $0xffff,%edx
    je 1f
    xor %eax,%eax
1:
    .endr
    sub $8,%rsp
    stmxcsr (%rsp)
    mov (%rsp),%edx
    add $8,%rsp
    cmp AsteriaAmd64PagerThreadMxcsr(%rip),%edx
    je 2f
    xor %eax,%eax
2:
    ret
    .size AsteriaAmd64PagerThreadStateIntact,.-AsteriaAmd64PagerThreadStateIntact

// The pager's second self-test thread: it reads page zero, which no root maps and the pager does
// not back, so the pager terminates it; it has no other path.
    .global AsteriaAmd64PagerFaultingThreadEntry
    .hidden AsteriaAmd64PagerFaultingThreadEntry
    .type AsteriaAmd64PagerFaultingThreadEntry,@function
AsteriaAmd64PagerFaultingThreadEntry:
    mov 0x8,%rax
1:
    jmp 1b
    .size AsteriaAmd64PagerFaultingThreadEntry,.-AsteriaAmd64PagerFaultingThreadEntry

// The third self-test thread executes an invalid instruction, an exception the kernel hands to the
// pager like a fault, which terminates it.
    .global AsteriaAmd64PagerExceptionThreadEntry
    .hidden AsteriaAmd64PagerExceptionThreadEntry
    .type AsteriaAmd64PagerExceptionThreadEntry,@function
AsteriaAmd64PagerExceptionThreadEntry:
    ud2
1:
    jmp 1b
    .size AsteriaAmd64PagerExceptionThreadEntry,.-AsteriaAmd64PagerExceptionThreadEntry

// The fourth self-test thread counts in the word its argument (RDI) names and never stops; the
// pager kills it once it counted. The aligned 64-bit increment is atomic and publishes to the
// pager's acquire load.
    .global AsteriaAmd64PagerSpinningThreadEntry
    .hidden AsteriaAmd64PagerSpinningThreadEntry
    .type AsteriaAmd64PagerSpinningThreadEntry,@function
AsteriaAmd64PagerSpinningThreadEntry:
    lock incq (%rdi)
    pause
    jmp AsteriaAmd64PagerSpinningThreadEntry
    .size AsteriaAmd64PagerSpinningThreadEntry,.-AsteriaAmd64PagerSpinningThreadEntry

// The fifth self-test thread waits on a WaitSet: it calls the C++ main with the control block's
// address in RDI and exits from there.
    .global AsteriaAmd64PagerWaitingThreadEntry
    .hidden AsteriaAmd64PagerWaitingThreadEntry
    .type AsteriaAmd64PagerWaitingThreadEntry,@function
AsteriaAmd64PagerWaitingThreadEntry:
    xor %ebp,%ebp
    call AsteriaAmd64PagerWaitingThreadMain
1:
    pause
    jmp 1b
    .size AsteriaAmd64PagerWaitingThreadEntry,.-AsteriaAmd64PagerWaitingThreadEntry

// Returns the time-stamp counter, the thread's clock.
    .global AsteriaAmd64PagerThreadClock
    .hidden AsteriaAmd64PagerThreadClock
    .type AsteriaAmd64PagerThreadClock,@function
AsteriaAmd64PagerThreadClock:
    rdtsc
    shl $32,%rdx
    or %rdx,%rax
    ret
    .size AsteriaAmd64PagerThreadClock,.-AsteriaAmd64PagerThreadClock
