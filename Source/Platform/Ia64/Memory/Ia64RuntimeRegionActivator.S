// SPDX-License-Identifier: Apache-2.0

// IA-64 runtime region activation: writes RR1-RR7 with PSR.ic cleared and
// reads every region register back. The expected RR0 value comes from the
// generated Ia64SharedBootstrapRegionRegister layout.

#include "Generated/Platform/Ia64/Ia64RuntimeRegionActivatorAsmLayout.h"

.text
.align 16
.global AsteriaIa64ActivateRuntimeRegions
.hidden AsteriaIa64ActivateRuntimeRegions
.type AsteriaIa64ActivateRuntimeRegions, @function
AsteriaIa64ActivateRuntimeRegions:
// The P64 ABI supplies the seven RR operands in r32 through r38. This leaf
// deliberately has no alloc instruction, memory reference, or stack use.
    mov r9 = psr
    ;;
// mov-from-PSR exposes IC, I, and PK, but not CPL or VM. Reject enabled
// interrupts or protection keys before mutation. IC is preserved across
// the bounded commit window below.
    movl r14 = 0xC000
    ;;
    and r14 = r9, r14
    ;;
    cmp.ne p6, p7 = r14, r0
    ;;
(p6) br.cond.dpnt.few 1f
    ;;
    mov r14 = rr[r0]
    ;;
    movl r15 = ASTERIA_IA64_SHARED_BOOTSTRAP_REGION_REGISTER
    ;;
    cmp.ne p6, p7 = r14, r15
    ;;
(p6) br.cond.dpnt.few 1f
    ;;
// Resolve every RR address before the first write. Region zero is never a
// destination, and the first RR1 write below is the native commit point.
    movl r14 = 0x2000000000000000
    movl r15 = 0x4000000000000000
    ;;
    movl r16 = 0x6000000000000000
    movl r17 = 0x8000000000000000
    ;;
    movl r18 = 0xA000000000000000
    movl r19 = 0xC000000000000000
    ;;
    movl r20 = 0xE000000000000000
    ;;
    rsm psr.ic
    ;;
    srlz.d
    ;;
    srlz.i
    ;;
    mov rr[r14] = r32
    ;;
    mov rr[r15] = r33
    ;;
    mov rr[r16] = r34
    ;;
    mov rr[r17] = r35
    ;;
    mov rr[r18] = r36
    ;;
    mov rr[r19] = r37
    ;;
    mov rr[r20] = r38
    ;;
    srlz.d
    ;;
    srlz.i
    ;;
// Read every committed word before deciding whether exact activation can
// be reported. No mismatch edge attempts to reconstruct prior RR state.
    mov r21 = rr[r14]
    ;;
    mov r22 = rr[r15]
    ;;
    mov r23 = rr[r16]
    ;;
    mov r24 = rr[r17]
    ;;
    mov r25 = rr[r18]
    ;;
    mov r26 = rr[r19]
    ;;
    mov r27 = rr[r20]
    ;;
    cmp.ne p6, p7 = r21, r32
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    cmp.ne p6, p7 = r22, r33
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    cmp.ne p6, p7 = r23, r34
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    cmp.ne p6, p7 = r24, r35
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    cmp.ne p6, p7 = r25, r36
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    cmp.ne p6, p7 = r26, r37
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    cmp.ne p6, p7 = r27, r38
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    mov r8 = r0
    br.cond.sptk.few 3f
    ;;
1:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
2:
    mov r8 = 2
    ;;
3:
    tbit.nz p8, p9 = r9, 13
    ;;
(p8) ssm psr.ic
    ;;
    srlz.d
    ;;
    srlz.i
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64ActivateRuntimeRegions, . - AsteriaIa64ActivateRuntimeRegions
