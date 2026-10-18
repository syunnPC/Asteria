// SPDX-License-Identifier: Apache-2.0

// IA-64 rfi from the physical boot context into translated Region-0
// execution, and the translated entry that calls the boot-owned kernel
// continuation.

.text
.align 16
.global AsteriaIa64ActivateInitialTranslation
.hidden AsteriaIa64ActivateInitialTranslation
.type AsteriaIa64ActivateInitialTranslation, @function
AsteriaIa64ActivateInitialTranslation:
// Reject only the visible required-clear PSR fields and a nonzero RSC.
// CPL0 and PSR.vm remain caller preconditions.
    mov r8 = psr
    mov r14 = ar.rsc
    ;;
    movl r15 = 0x000000100802E000
    ;;
    and r15 = r8, r15
    ;;
    cmp.ne p6, p7 = r15, r0
    ;;
    cmp.ne.or p6, p7 = r14, r0
    ;;
(p6) br.cond.dpnt.few 1f
    ;;
// Bank one is selected before any commit scratch use. r14, r15, and gp
// are static registers, so the target IPSR and IIP survive the boundary.
    bsw.1
    ;;
    movl r15 = 0x0000101008022018
    ;;
    movl r14 = AsteriaIa64InitialTranslatedExecution
    ;;
    mov cr.ifs = r0
    mov cr.ipsr = r15
    mov cr.iip = r14
    ;;
    rfi
    ;;
1:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64ActivateInitialTranslation, . - AsteriaIa64ActivateInitialTranslation
.align 16
.global AsteriaIa64InitialTranslatedExecution
.hidden AsteriaIa64InitialTranslatedExecution
.type AsteriaIa64InitialTranslatedExecution, @function
AsteriaIa64InitialTranslatedExecution:
    br.call.sptk.many b0 = AsteriaIa64RunInitialTranslatedKernel
    ;;
2:
    hint @pause
    ;;
    br.cond.sptk 2b
    ;;
.align 16
.size AsteriaIa64InitialTranslatedExecution, . - AsteriaIa64InitialTranslatedExecution
