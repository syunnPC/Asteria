// SPDX-License-Identifier: Apache-2.0
// The guest test program's extended-state pattern: the C++ entry loads it into xmm0-xmm15 and MXCSR
// first and checks it last. The program is compiled general-registers-only, so only the kernel's
// context switches, while the spinner and the pager run their own patterns, can change it.

    .section .rodata
    .p2align 4
AsteriaGuestAmd64Vectors:
    .irp index,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .quad 0x4755455354000000 + \index, 0x5a5a5a5a5a5a5a5a ^ \index
    .endr
AsteriaGuestAmd64Mxcsr:
    .long 0x3f80

    .text
    .global AsteriaGuestAmd64LoadExtendedState
    .hidden AsteriaGuestAmd64LoadExtendedState
    .type AsteriaGuestAmd64LoadExtendedState,@function
AsteriaGuestAmd64LoadExtendedState:
    lea AsteriaGuestAmd64Vectors(%rip),%rax
    .irp index,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    movdqa 16*\index(%rax),%xmm\index
    .endr
    ldmxcsr AsteriaGuestAmd64Mxcsr(%rip)
    ret
    .size AsteriaGuestAmd64LoadExtendedState,.-AsteriaGuestAmd64LoadExtendedState

// Returns 1 in EAX when xmm0-xmm15 and MXCSR still hold the pattern.
    .global AsteriaGuestAmd64ExtendedStateIntact
    .hidden AsteriaGuestAmd64ExtendedStateIntact
    .type AsteriaGuestAmd64ExtendedStateIntact,@function
AsteriaGuestAmd64ExtendedStateIntact:
    lea AsteriaGuestAmd64Vectors(%rip),%rcx
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
    cmp AsteriaGuestAmd64Mxcsr(%rip),%edx
    je 2f
    xor %eax,%eax
2:
    ret
    .size AsteriaGuestAmd64ExtendedStateIntact,.-AsteriaGuestAmd64ExtendedStateIntact
