// SPDX-License-Identifier: Apache-2.0

// Exact-width sparse port leaves: UC alias, data translation off, mf.a.
.text
.align 16
.global AsteriaIa64ReadPhysicalPort
.hidden AsteriaIa64ReadPhysicalPort
.type AsteriaIa64ReadPhysicalPort, @function
AsteriaIa64ReadPhysicalPort:
    mov r14 = psr
    mov r16 = r32
    mov r22 = r33
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
    cmp.eq p6, p7 = 1, r22
    cmp.eq p8, p9 = 2, r22
    cmp.eq p10, p11 = 4, r22
    ;;
(p6) ld1.acq r8 = [r18]
    ;;
(p8) ld2.acq r8 = [r18]
    ;;
(p10) ld4.acq r8 = [r18]
    ;;
    mf.a
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
.align 16
.size AsteriaIa64ReadPhysicalPort, . - AsteriaIa64ReadPhysicalPort
.text
.align 16
.global AsteriaIa64WritePhysicalPort
.hidden AsteriaIa64WritePhysicalPort
.type AsteriaIa64WritePhysicalPort, @function
AsteriaIa64WritePhysicalPort:
    mov r14 = psr
    mov r16 = r32
    mov r22 = r33
    mov r19 = r34
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
    cmp.eq p6, p7 = 1, r22
    cmp.eq p8, p9 = 2, r22
    cmp.eq p10, p11 = 4, r22
    ;;
(p6) st1 [r18] = r19
    ;;
    mf.a
    ;;
    mf
    ;;
(p8) st2 [r18] = r19
    ;;
    mf.a
    ;;
    mf
    ;;
(p10) st4 [r18] = r19
    ;;
    mf.a
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
.align 16
.size AsteriaIa64WritePhysicalPort, . - AsteriaIa64WritePhysicalPort
