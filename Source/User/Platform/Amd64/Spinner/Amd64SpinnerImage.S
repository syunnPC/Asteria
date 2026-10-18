// SPDX-License-Identifier: Apache-2.0

// Entry of the AMD64 boot spinner image: loads the spinner's extended-state pattern, calls the C++
// main with the DiagnosticChannel handle in RDI/RSI and the WaitSet handle in RDX/RCX, and idles if
// it returns. The C++ code is compiled general-registers-only, so only the kernel's context
// switches can change xmm0-xmm15 and MXCSR before AsteriaAmd64SpinnerStateIntact checks them.

    .section .rodata
    .p2align 4
AsteriaAmd64SpinnerVectors:
    .irp index,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .quad 0x5350494e4e455200 + \index, 0xa5a5a5a5a5a5a5a5 ^ \index
    .endr
AsteriaAmd64SpinnerMxcsr:
    .long 0x7f80

    .text
    .global AsteriaAmd64SpinnerImageEntry
    .hidden AsteriaAmd64SpinnerImageEntry
    .type AsteriaAmd64SpinnerImageEntry,@function
AsteriaAmd64SpinnerImageEntry:
    xor %ebp,%ebp
    lea AsteriaAmd64SpinnerVectors(%rip),%rax
    .irp index,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    movdqa 16*\index(%rax),%xmm\index
    .endr
    ldmxcsr AsteriaAmd64SpinnerMxcsr(%rip)
    call AsteriaAmd64SpinnerImageMain
    .global AsteriaAmd64SpinnerImageIdle
    .hidden AsteriaAmd64SpinnerImageIdle
    .type AsteriaAmd64SpinnerImageIdle,@function
AsteriaAmd64SpinnerImageIdle:
    pause
    jmp AsteriaAmd64SpinnerImageIdle
    .size AsteriaAmd64SpinnerImageIdle,.-AsteriaAmd64SpinnerImageIdle
    .size AsteriaAmd64SpinnerImageEntry,.-AsteriaAmd64SpinnerImageEntry

// Returns 1 in EAX when xmm0-xmm15 and MXCSR still hold the entry's pattern.
    .global AsteriaAmd64SpinnerStateIntact
    .hidden AsteriaAmd64SpinnerStateIntact
    .type AsteriaAmd64SpinnerStateIntact,@function
AsteriaAmd64SpinnerStateIntact:
    lea AsteriaAmd64SpinnerVectors(%rip),%rcx
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
    cmp AsteriaAmd64SpinnerMxcsr(%rip),%edx
    je 2f
    xor %eax,%eax
2:
    ret
    .size AsteriaAmd64SpinnerStateIntact,.-AsteriaAmd64SpinnerStateIntact
