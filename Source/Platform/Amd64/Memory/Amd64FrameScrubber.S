// SPDX-License-Identifier: Apache-2.0

// Intel SDM Vol. 3A 253668-093US sections 5.10.4.1 and
// 5.10.5: repoint, invalidate, use; restore and invalidate before return. No call and no interrupt
// window.
#include "Generated/Platform/Amd64/Amd64FrameScrubberAsmLayout.h"
.text
.p2align 4
.global AsteriaAmd64ZeroScrubChunk
.hidden AsteriaAmd64ZeroScrubChunk
.type AsteriaAmd64ZeroScrubChunk, @function
AsteriaAmd64ZeroScrubChunk:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cli
    cld
    movq %rdx, %r12
    movq %rcx, %r13
    movq %rdi, %r14
    movq %rsi, %r15
    movq %r8, %rbx
    movq %gs:16, %r9
    cmpq $ASTERIA_SCRUB_PROCESSORS, %r9
    jae .Lscrub_fault
    testq %r15, %r15
    jz .Lscrub_fault
    cmpq $ASTERIA_SCRUB_CHUNK_PAGES, %r15
    ja .Lscrub_fault
    shlq $12, %r9
    leaq ASTERIA_SCRUB_WINDOW(%r9), %r10
    movq (%r12), %rax
    andq $-97, %rax
    cmpq %r13, %rax
    jne .Lscrub_fault
    movabsq $0x000ffffffffFF000, %r11
.Lscrub_page:
    movq %r13, %rax
    movq %r11, %rcx
    notq %rcx
    andq %rcx, %rax
    orq %r14, %rax
    movq %rax, (%r12)
    invlpg (%r10)
    // Keep the existing parked-DMA evidence without a call through the window.
    movq %rbx, %rbp
    movl $ASTERIA_SCRUB_WATCHES, %r9d
.Lscrub_watch:
    cmpq %r14, (%rbp)
    jne .Lscrub_watch_next
    cmpq $0, 8(%rbp)
    jne .Lscrub_watch_next
    xorl %ecx, %ecx
.Lscrub_sample:
    movq (%r10,%rcx,8), %rax
    movq %rax, 16(%rbp,%rcx,8)
    incq %rcx
    cmpq $ASTERIA_SCRUB_SAMPLE_WORDS, %rcx
    jb .Lscrub_sample
    movq $1, 8(%rbp)
.Lscrub_watch_next:
    addq $ASTERIA_SCRUB_SAMPLE_BYTES, %rbp
    decq %r9
    jnz .Lscrub_watch
    movq %r10, %rdi
    movl $512, %ecx
    xorl %eax, %eax
    rep stosq
    addq $4096, %r14
    decq %r15
    jnz .Lscrub_page
    movq %r13, (%r12)
    invlpg (%r10)
    movq (%r12), %rax
    andq $-97, %rax
    cmpq %r13, %rax
    jne .Lscrub_fault
    xorl %eax, %eax
    jmp .Lscrub_return
.Lscrub_fault:
    movl $1, %eax
.Lscrub_return:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
.size AsteriaAmd64ZeroScrubChunk, . - AsteriaAmd64ZeroScrubChunk
