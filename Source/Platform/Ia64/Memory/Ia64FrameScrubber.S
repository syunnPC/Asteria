// SPDX-License-Identifier: Apache-2.0

// The physical-mode leaves of Ia64FrameScrubber: external interrupts masked,
// the RSE lazy and data translation off around a run of cacheable physical
// stores, or one cacheable physical load (bit 63 clear); only bank-1 scratch
// registers, no stack.

// Explicit groups, as the runtime IVT: every stop is the one written here.
.explicit
.text
.align 16
.global AsteriaIa64ZeroPhysicalRun
.hidden AsteriaIa64ZeroPhysicalRun
.type AsteriaIa64ZeroPhysicalRun, @function
AsteriaIa64ZeroPhysicalRun:
// r32 is the page-aligned physical start, r33 the nonzero byte count, a
// multiple of the page size. r14 keeps the caller's PSR, r20 its RSC.
    mov r14 = psr
    mov r16 = r32
    ;;
    add r19 = r32, r33
    rsm psr.i
    ;;
    srlz.d
    ;;
    mov r20 = ar.rsc
    ;;
    dep r21 = 0, r20, 0, 2
    ;;
    mov ar.rsc = r21
    ;;
    rsm psr.dt
    ;;
    srlz.d
    ;;
1:
    st8 [r16] = r0, 8
    ;;
    cmp.ltu p6, p7 = r16, r19
    ;;
(p6) br.cond.sptk.few 1b
    ;;
    mf
    ;;
    tbit.nz p6, p7 = r14, 17
    tbit.nz p8, p9 = r14, 14
    ;;
(p6) ssm psr.dt
    ;;
    srlz.d
    ;;
    mov ar.rsc = r20
    ;;
(p8) ssm psr.i
    ;;
    srlz.d
    ;;
    br.ret.sptk.many b0
    ;;
.size AsteriaIa64ZeroPhysicalRun, . - AsteriaIa64ZeroPhysicalRun

.align 16
.global AsteriaIa64ReadPhysicalQuad
.hidden AsteriaIa64ReadPhysicalQuad
.type AsteriaIa64ReadPhysicalQuad, @function
AsteriaIa64ReadPhysicalQuad:
// r32 is an 8-byte aligned cacheable physical address (bit 63 clear); r8
// returns the quad there. r14 keeps the caller's PSR, r20 its RSC.
    mov r14 = psr
    ;;
    rsm psr.i
    ;;
    srlz.d
    ;;
    mov r20 = ar.rsc
    ;;
    dep r21 = 0, r20, 0, 2
    ;;
    mov ar.rsc = r21
    ;;
    rsm psr.dt
    ;;
    srlz.d
    ;;
    ld8 r8 = [r32]
    ;;
    tbit.nz p6, p7 = r14, 17
    tbit.nz p8, p9 = r14, 14
    ;;
(p6) ssm psr.dt
    ;;
    srlz.d
    ;;
    mov ar.rsc = r20
    ;;
(p8) ssm psr.i
    ;;
    srlz.d
    ;;
    br.ret.sptk.many b0
    ;;
.size AsteriaIa64ReadPhysicalQuad, . - AsteriaIa64ReadPhysicalQuad
