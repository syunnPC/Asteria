// SPDX-License-Identifier: Apache-2.0

// Invalidation of the local TLB entries of one AMD64 page.

.text
.p2align 4
.global AsteriaAmd64InvalidatePage
.hidden AsteriaAmd64InvalidatePage
.type AsteriaAmd64InvalidatePage, @function
AsteriaAmd64InvalidatePage:
    invlpg (%rdi)
    ret
.size AsteriaAmd64InvalidatePage, . - AsteriaAmd64InvalidatePage
