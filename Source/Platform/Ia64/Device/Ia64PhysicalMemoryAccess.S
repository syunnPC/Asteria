// SPDX-License-Identifier: Apache-2.0

// Uncached physical read and write leaves of Ia64PhysicalMemoryAccess:
// interrupts masked, the RSE lazy and data translation off around one
// access through the bit-63 uncacheable alias, between mf and mf.a.

.text
.align 16
.global AsteriaIa64ReadUncachedPhysicalDoubleWord
.hidden AsteriaIa64ReadUncachedPhysicalDoubleWord
.type AsteriaIa64ReadUncachedPhysicalDoubleWord, @function
AsteriaIa64ReadUncachedPhysicalDoubleWord:
    mov r14 = psr
    mov r16 = r32
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
    movl r17 = 0x8000000000000000
    ;;
    or r18 = r16, r17
    ;;
    mf
    ;;
    ld4.acq r8 = [r18]
    ;;
    mf.a
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
.align 16
.size AsteriaIa64ReadUncachedPhysicalDoubleWord, . - AsteriaIa64ReadUncachedPhysicalDoubleWord
.text
.align 16
.global AsteriaIa64WriteUncachedPhysicalDoubleWord
.hidden AsteriaIa64WriteUncachedPhysicalDoubleWord
.type AsteriaIa64WriteUncachedPhysicalDoubleWord, @function
AsteriaIa64WriteUncachedPhysicalDoubleWord:
    mov r14 = psr
    mov r16 = r32
    mov r19 = r33
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
    movl r17 = 0x8000000000000000
    ;;
    or r18 = r16, r17
    ;;
    mf
    ;;
    st4 [r18] = r19
    ;;
    mf.a
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
.align 16
.size AsteriaIa64WriteUncachedPhysicalDoubleWord, . - AsteriaIa64WriteUncachedPhysicalDoubleWord
.text
.align 16
.global AsteriaIa64ReadUncachedPhysicalByte
.hidden AsteriaIa64ReadUncachedPhysicalByte
.type AsteriaIa64ReadUncachedPhysicalByte, @function
AsteriaIa64ReadUncachedPhysicalByte:
    mov r14 = psr
    mov r16 = r32
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
    movl r17 = 0x8000000000000000
    ;;
    or r18 = r16, r17
    ;;
    mf
    ;;
    ld1.acq r8 = [r18]
    ;;
    mf.a
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
.align 16
.size AsteriaIa64ReadUncachedPhysicalByte, . - AsteriaIa64ReadUncachedPhysicalByte
.text
.align 16
.global AsteriaIa64WriteUncachedPhysicalByte
.hidden AsteriaIa64WriteUncachedPhysicalByte
.type AsteriaIa64WriteUncachedPhysicalByte, @function
AsteriaIa64WriteUncachedPhysicalByte:
    mov r14 = psr
    mov r16 = r32
    mov r19 = r33
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
    movl r17 = 0x8000000000000000
    ;;
    or r18 = r16, r17
    ;;
    mf
    ;;
    st1 [r18] = r19
    ;;
    mf.a
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
.align 16
.size AsteriaIa64WriteUncachedPhysicalByte, . - AsteriaIa64WriteUncachedPhysicalByte
.text
.align 16
.global AsteriaIa64ReadUncachedPhysicalWord
.hidden AsteriaIa64ReadUncachedPhysicalWord
.type AsteriaIa64ReadUncachedPhysicalWord, @function
AsteriaIa64ReadUncachedPhysicalWord:
    mov r14 = psr
    mov r16 = r32
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
    movl r17 = 0x8000000000000000
    ;;
    or r18 = r16, r17
    ;;
    mf
    ;;
    ld2.acq r8 = [r18]
    ;;
    mf.a
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
.align 16
.size AsteriaIa64ReadUncachedPhysicalWord, . - AsteriaIa64ReadUncachedPhysicalWord
.text
.align 16
.global AsteriaIa64WriteUncachedPhysicalWord
.hidden AsteriaIa64WriteUncachedPhysicalWord
.type AsteriaIa64WriteUncachedPhysicalWord, @function
AsteriaIa64WriteUncachedPhysicalWord:
    mov r14 = psr
    mov r16 = r32
    mov r19 = r33
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
    movl r17 = 0x8000000000000000
    ;;
    or r18 = r16, r17
    ;;
    mf
    ;;
    st2 [r18] = r19
    ;;
    mf.a
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
.align 16
.size AsteriaIa64WriteUncachedPhysicalWord, . - AsteriaIa64WriteUncachedPhysicalWord
.text
.align 16
.global AsteriaIa64ReadUncachedPhysicalQuadWord
.hidden AsteriaIa64ReadUncachedPhysicalQuadWord
.type AsteriaIa64ReadUncachedPhysicalQuadWord, @function
AsteriaIa64ReadUncachedPhysicalQuadWord:
    mov r14 = psr
    mov r16 = r32
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
    movl r17 = 0x8000000000000000
    ;;
    or r18 = r16, r17
    ;;
    mf
    ;;
    ld8.acq r8 = [r18]
    ;;
    mf.a
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
.align 16
.size AsteriaIa64ReadUncachedPhysicalQuadWord, . - AsteriaIa64ReadUncachedPhysicalQuadWord
.text
.align 16
.global AsteriaIa64WriteUncachedPhysicalQuadWord
.hidden AsteriaIa64WriteUncachedPhysicalQuadWord
.type AsteriaIa64WriteUncachedPhysicalQuadWord, @function
AsteriaIa64WriteUncachedPhysicalQuadWord:
    mov r14 = psr
    mov r16 = r32
    mov r19 = r33
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
    movl r17 = 0x8000000000000000
    ;;
    or r18 = r16, r17
    ;;
    mf
    ;;
    st8 [r18] = r19
    ;;
    mf.a
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
.align 16
.size AsteriaIa64WriteUncachedPhysicalQuadWord, . - AsteriaIa64WriteUncachedPhysicalQuadWord
