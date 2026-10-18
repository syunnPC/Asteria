// SPDX-License-Identifier: Apache-2.0

// CR3 load with read-back and CR4 read for runtime address-space activation.

.text
.p2align 4
.global AsteriaAmd64LoadCr3AndReadBack
.hidden AsteriaAmd64LoadCr3AndReadBack
.type AsteriaAmd64LoadCr3AndReadBack, @function
AsteriaAmd64LoadCr3AndReadBack:
    movq %rdi, %cr3
    movq %cr3, %rax
    ret
.size AsteriaAmd64LoadCr3AndReadBack, . - AsteriaAmd64LoadCr3AndReadBack
.p2align 4
.global AsteriaAmd64ReadCr4
.hidden AsteriaAmd64ReadCr4
.type AsteriaAmd64ReadCr4, @function
AsteriaAmd64ReadCr4:
    movq %cr4, %rax
    ret
.size AsteriaAmd64ReadCr4, . - AsteriaAmd64ReadCr4
