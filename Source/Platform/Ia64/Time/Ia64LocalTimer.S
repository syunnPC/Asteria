// SPDX-License-Identifier: Apache-2.0

// IA-64 interval time counter read and the local-timer (cr.itm/cr.itv)
// initialize, program and cancel helpers with serialized readback.

.text
.align 16
.global AsteriaIa64ReadIntervalTimeCounter
.hidden AsteriaIa64ReadIntervalTimeCounter
.type AsteriaIa64ReadIntervalTimeCounter, @function
AsteriaIa64ReadIntervalTimeCounter:
    mov r8 = ar.itc
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64ReadIntervalTimeCounter, . - AsteriaIa64ReadIntervalTimeCounter
.align 16
.global AsteriaIa64InitializeLocalTimer
.hidden AsteriaIa64InitializeLocalTimer
.type AsteriaIa64InitializeLocalTimer, @function
AsteriaIa64InitializeLocalTimer:
    mov cr.itv = r32
    ;;
    srlz.d
    ;;
    mov cr.itm = r0
    ;;
    srlz.d
    ;;
    mov r14 = cr.itv
    mov r15 = cr.itm
    ;;
    cmp.ne p6, p7 = r14, r32
    ;;
(p6) br.cond.dpnt.few 1f
    ;;
    cmp.ne p6, p7 = r15, r0
    ;;
(p6) br.cond.dpnt.few 1f
    ;;
    mov r8 = r0
    br.ret.sptk.many b0
    ;;
1:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64InitializeLocalTimer, . - AsteriaIa64InitializeLocalTimer
.align 16
.global AsteriaIa64ProgramLocalTimer
.hidden AsteriaIa64ProgramLocalTimer
.type AsteriaIa64ProgramLocalTimer, @function
AsteriaIa64ProgramLocalTimer:
    mov cr.itm = r32
    ;;
    srlz.d
    ;;
    mov cr.itv = r33
    ;;
    srlz.d
    ;;
    mov r14 = cr.itm
    mov r15 = cr.itv
    ;;
    cmp.ne p6, p7 = r14, r32
    ;;
(p6) br.cond.dpnt.few 5f
    ;;
    cmp.ne p6, p7 = r15, r33
    ;;
(p6) br.cond.dpnt.few 5f
    ;;
    mov r16 = ar.itc
    ;;
    cmp.geu p6, p7 = r16, r32
    ;;
(p6) br.cond.dpnt.few 4f
    ;;
    mov r8 = r0
    br.ret.sptk.many b0
    ;;
4:
    mov cr.itv = r36
    ;;
    srlz.d
    ;;
    mov cr.itm = r34
    ;;
    srlz.d
    ;;
    mov cr.itv = r35
    ;;
    srlz.d
    ;;
    mov r14 = cr.itm
    mov r15 = cr.itv
    ;;
    cmp.ne p6, p7 = r14, r34
    ;;
(p6) br.cond.dpnt.few 5f
    ;;
    cmp.ne p6, p7 = r15, r35
    ;;
(p6) br.cond.dpnt.few 5f
    ;;
    mov r8 = 2
    br.ret.sptk.many b0
    ;;
5:
// A program attempt may have exposed either the requested vector or an
// incompletely restored prior vector. Every readback-failure edge must
// therefore converge on this serialized, independently verified mask.
    mov cr.itv = r36
    ;;
    srlz.d
    ;;
    mov r14 = cr.itv
    ;;
    cmp.ne p6, p7 = r14, r36
    ;;
(p6) br.cond.dpnt.few 6f
    ;;
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
6:
// Status 3 is terminal: the forced mask write itself could not be proven.
    mov r8 = 3
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64ProgramLocalTimer, . - AsteriaIa64ProgramLocalTimer
.align 16
.global AsteriaIa64CancelLocalTimer
.hidden AsteriaIa64CancelLocalTimer
.type AsteriaIa64CancelLocalTimer, @function
AsteriaIa64CancelLocalTimer:
    mov cr.itv = r32
    ;;
    srlz.d
    ;;
    mov r14 = cr.itv
    ;;
    cmp.ne p6, p7 = r14, r32
    ;;
(p6) br.cond.dpnt.few 3f
    ;;
    mov r8 = r0
    br.ret.sptk.many b0
    ;;
3:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64CancelLocalTimer, . - AsteriaIa64CancelLocalTimer

// Intel Itanium SDM Vol. 2 rev. 1.1 §3.3.4.2. Caller canceled ITM first. No
// armed deadline may span this write.
.align 16
.global AsteriaIa64WriteOnlineIntervalCounter
.hidden AsteriaIa64WriteOnlineIntervalCounter
.type AsteriaIa64WriteOnlineIntervalCounter, @function
AsteriaIa64WriteOnlineIntervalCounter:
    mov ar.itc = r32
    ;;
    srlz.d
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64WriteOnlineIntervalCounter, . - AsteriaIa64WriteOnlineIntervalCounter
