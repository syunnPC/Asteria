// SPDX-License-Identifier: Apache-2.0

// Terminal snapshot and bank-zero landing; register-only physical scanout.
#include "Generated/Platform/Ia64/Ia64StopAsmLayout.h"

.explicit
.text
.align 16
.global AsteriaIa64SendPowerInit
.hidden AsteriaIa64SendPowerInit
.type AsteriaIa64SendPowerInit, @function
AsteriaIa64SendPowerInit:
// PowerOff.md; IA-64 SDM v2 §5.8.4.1 / SAL 3.0 §5.1. Caller has parked
// every peer, verified PSR.mc=0, and supplied this CPU's physical PIB address.
    mov r16 = r32
    rsm psr.i
    ;;
    srlz.d
    ;;
    mov ar.rsc = r0
    ;;
    rsm psr.dt
    ;;
    srlz.d
    ;;
    movl r17 = 0x8000000000000000
    mov r19 = ASTERIA_IA64_POWER_INIT_MESSAGE
    ;;
    or r18 = r16, r17
    mf
    ;;
    st8 [r18] = r19
    ;;
    mf.a
    ;;
    br.sptk.many AsteriaIa64StopPark
    ;;
.align 16
.size AsteriaIa64SendPowerInit, . - AsteriaIa64SendPowerInit

.global AsteriaIa64StopPark
.hidden AsteriaIa64StopPark
.type AsteriaIa64StopPark, @function
AsteriaIa64StopPark:
    rsm psr.i
    ;;
    srlz.d
    ;;
.Lstop_park:
    hint @pause
    ;;
    br.cond.sptk.few .Lstop_park
    ;;
.align 16
.size AsteriaIa64StopPark, . - AsteriaIa64StopPark

.align 16
.global AsteriaIa64CaptureStopSnapshot
.hidden AsteriaIa64CaptureStopSnapshot
.type AsteriaIa64CaptureStopSnapshot, @function
AsteriaIa64CaptureStopSnapshot:
    mov r14 = r32
    mov r15 = b0
    mov r16 = psr
    mov r23 = psr
    ;;
    mov r18 = ar.rsc
    ;;
    dep r19 = 0, r18, 0, 2
    ;;
    mov ar.rsc = r19
    rsm psr.ic
    ;;
    srlz.d
    ;;
    st8 [r14] = r0, 8
    ;;
    st8 [r14] = r15, 8
    ;;
    st8 [r14] = r16, 8
    ;;
    mov r15 = cr.isr
    ;;
    st8 [r14] = r15, 8
    ;;
    mov r15 = cr.ifa
    ;;
    st8 [r14] = r15, 8
    ;;
    mov r15 = cr.iim
    ;;
    st8 [r14] = r15, 8
    ;;
    mov r15 = cr.iipa
    ;;
    st8 [r14] = r15, 8
    ;;
    mov r15 = cr.ifa
    ;;
    mov r16 = rr[r15]
    ;;
    st8 [r14] = r16, 8
    extr.u r17 = r16, 8, 24
    ;;
    st8 [r14] = r17, 8
    mov r15 = b0
    ;;
    st8 [r14] = r15
    ;;
    tbit.nz p6, p7 = r23, 13
    ;;
(p6) ssm psr.ic
    ;;
    srlz.d
    ;;
    mov ar.rsc = r18
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64CaptureStopSnapshot, . - AsteriaIa64CaptureStopSnapshot

.align 16
.global AsteriaIa64StopEntry
.hidden AsteriaIa64StopEntry
.type AsteriaIa64StopEntry, @function
AsteriaIa64StopEntry:
    mov r17 = psr
    ;;
    tbit.z p6, p7 = r17, 17
    ;;
(p6) br.cond.dpnt.few AsteriaIa64StopPark
    tbit.z p6, p7 = r17, 36
    ;;
(p6) br.cond.dpnt.few AsteriaIa64StopPark
// R16 is the exact architected vector offset, or zero at a cooperative leaf. ar.k3 and the stop
// owner are inside the existing locked data page. No unbanked/stacked register, stack or RSE is
// touched before capture.
    rsm psr.i | psr.ic
    ;;
    srlz.d
    ;;
    mov r17 = ar.k3
    ;;
    cmp.eq p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few AsteriaIa64StopPark
    adds r18 = ASTERIA_IA64_STOP_CONTEXT_STORAGE, r17
    adds r19 = ASTERIA_IA64_STOP_CONTEXT_CPU, r17
    ;;
    ld8 r30 = [r18]
    ld8 r31 = [r19]
    ;;
    cmp.eq p6, p7 = r30, r0
    ;;
(p6) br.cond.dpnt.few AsteriaIa64StopPark
    mov r20 = ASTERIA_IA64_STOP_MAXIMUM_PROCESSORS
    ;;
    cmp.geu p6, p7 = r31, r20
    ;;
(p6) br.cond.dpnt.few AsteriaIa64StopPark
    shl r18 = r31, 7
    ld8.acq r20 = [r30]
    ;;
    tbit.nz p6, p7 = r20, ASTERIA_IA64_POWER_LATCH_SHIFT
    ;;
(p6) br.cond.sptk.few .Lpower_confirm
    ;;
    add r18 = r30, r18
    ;;
    adds r18 = ASTERIA_IA64_STOP_STORAGE_RECORDS, r18
    ;;
    adds r19 = ASTERIA_IA64_STOP_STATE, r18
    ;;
    ld8.acq r20 = [r19]
    ;;
    cmp.eq p6, p7 = ASTERIA_IA64_STOP_FROZEN, r20
    ;;
(p6) br.cond.dpnt.few AsteriaIa64StopPark
    cmp.eq p6, p7 = ASTERIA_IA64_STOP_STOPPING, r20
    ;;
(p6) br.cond.sptk.few .Lstop_captured
    adds r18 = ASTERIA_IA64_STOP_SNAPSHOT, r18
    ;;
    st8 [r18] = r16, 8
    mov r20 = cr.iip
    ;;
    st8 [r18] = r20, 8
    mov r20 = cr.ipsr
    ;;
    st8 [r18] = r20, 8
    ;;
    mov r20 = cr.isr
    ;;
    st8 [r18] = r20, 8
    ;;
    mov r20 = cr.ifa
    ;;
    st8 [r18] = r20, 8
    ;;
    mov r20 = cr.iim
    ;;
    st8 [r18] = r20, 8
    ;;
    mov r20 = cr.iipa
    ;;
    st8 [r18] = r20, 8
    ;;
    mov r20 = cr.ifa
    ;;
    mov r21 = rr[r20]
    ;;
    st8 [r18] = r21, 8
    extr.u r22 = r21, 8, 24
    ;;
    st8 [r18] = r22, 8
    mov r20 = b0
    ;;
    st8 [r18] = r20
    ;;
.Lstop_captured:
    ld8.acq r20 = [r30]
    ;;
    tbit.nz p6, p7 = r20, ASTERIA_IA64_POWER_LATCH_SHIFT
    ;;
(p6) br.cond.sptk.few .Lpower_confirm
    cmp.eq p6, p7 = r20, r0
    ;;
(p6) br.cond.sptk.few .Lstop_landing
// Cooperative confirmation needs no C++, RSE, GP or new stack.
    mov r20 = ASTERIA_IA64_STOP_FROZEN
    ;;
    st8.rel [r19] = r20
    ;;
    br.sptk.many AsteriaIa64StopPark
    ;;
// PowerOff.md / processor stop policy: each CPU increments once and parks in the locked corridor,
// without a fatal record, stack, GP or RSE access.
.Lpower_confirm:
    adds r18 = ASTERIA_IA64_POWER_CONFIRMED, r30
    ;;
    fetchadd8.rel r19 = [r18], 1
    ;;
    br.sptk.many AsteriaIa64StopPark
    ;;
.Lstop_landing:
// Abandon the interrupted register frame permanently. Enforced lazy RSE
// prevents eager activity against the faulted backing store. No flushrs
// or load from the interrupted stack is needed in this terminal path.
    mov ar.rsc = r0
    ;;
    cover
    ;;
    shl r20 = r31, 4
    shl r21 = r31, 3
    ;;
    add r20 = r20, r21
    ;;
    add r20 = r30, r20
    ;;
    adds r20 = ASTERIA_IA64_STOP_STORAGE_TERMINAL, r20
    ;;
    ld8 r12 = [r20], 8
    ;;
    ld8 r21 = [r20], 8
    ;;
    ld8 r1 = [r20]
    ;;
    mov ar.bspstore = r21
    ;;
    mov ar.rnat = r0
    ;;
    alloc r22 = ar.pfs, 0, 0, 2, 0
    ;;
    mov r32 = r30
    mov r33 = r31
    mov ar.unat = r0
    ;;
    movl r14 = ASTERIA_IA64_STOP_KERNEL_FPSR
    ;;
    mov ar.fpsr = r14
    ssm psr.ic
    bsw.1
    ;;
    srlz.i
    ;;
    br.call.sptk.many b0 = AsteriaIa64StopCaptured
    ;;
    br.sptk.many AsteriaIa64StopPark
    ;;
.align 16
.size AsteriaIa64StopEntry, . - AsteriaIa64StopEntry

.align 16
.global AsteriaIa64FillStopPhysical
.hidden AsteriaIa64FillStopPhysical
.type AsteriaIa64FillStopPhysical, @function
AsteriaIa64FillStopPhysical:
    mov r14 = psr
    mov r15 = r32
    mov r16 = r33
    ;;
    mov r17 = r34
    mov r18 = r35
    mov r19 = r36
    ;;
    rsm psr.i
    ;;
    srlz.d
    ;;
    mov r21 = ar.rsc
    ;;
    dep r27 = 0, r21, 0, 2
    ;;
    mov ar.rsc = r27
    ;;
    movl r27 = 0x8000000000000000
    ;;
    or r15 = r15, r27
    ;;
    rsm psr.dt
    ;;
    srlz.d
    ;;
    mf
    ;;
.Lfill_row:
    mov r22 = r16
    mov r23 = r15
    ;;
.Lfill_pixel:
    st4 [r23] = r19, 4
    adds r22 = -1, r22
    ;;
    cmp.ne p6, p7 = r22, r0
    ;;
(p6) br.cond.sptk.few .Lfill_pixel
    add r15 = r15, r18
    adds r17 = -1, r17
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.sptk.few .Lfill_row
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
    mov ar.rsc = r21
    ;;
(p8) ssm psr.i
    ;;
    srlz.d
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64FillStopPhysical, . - AsteriaIa64FillStopPhysical

.align 16
.global AsteriaIa64DrawStopGlyphPhysical
.hidden AsteriaIa64DrawStopGlyphPhysical
.type AsteriaIa64DrawStopGlyphPhysical, @function
AsteriaIa64DrawStopGlyphPhysical:
    mov r14 = psr
    mov r15 = r32
    mov r16 = r33
    ;;
    mov r17 = r34
    mov r18 = r35
    mov r19 = r36
    ;;
    mov r20 = r37
    ;;
    rsm psr.i
    ;;
    srlz.d
    ;;
    mov r21 = ar.rsc
    ;;
    dep r27 = 0, r21, 0, 2
    ;;
    mov ar.rsc = r27
    ;;
    movl r27 = 0x8000000000000000
    ;;
    or r15 = r15, r27
    ;;
    rsm psr.dt
    ;;
    srlz.d
    ;;
    mf
    ;;
    mov r22 = 16
    ;;
.Lglyph_row:
    extr.u r23 = r17, 0, 8
    shr.u r17 = r17, 8
    mov r24 = 8
    mov r25 = r15
    ;;
.Lglyph_pixel:
    extr.u r26 = r23, 7, 1
    ;;
    cmp.ne p6, p7 = r26, r0
    ;;
(p6) mov r26 = r19
(p7) mov r26 = r20
    ;;
    st4 [r25] = r26, 4
    shl r23 = r23, 1
    adds r24 = -1, r24
    ;;
    cmp.ne p6, p7 = r24, r0
    ;;
(p6) br.cond.sptk.few .Lglyph_pixel
    add r15 = r15, r16
    adds r22 = -1, r22
    ;;
    cmp.eq p6, p7 = 8, r22
    ;;
(p6) mov r17 = r18
    cmp.ne p6, p7 = r22, r0
    ;;
(p6) br.cond.sptk.few .Lglyph_row
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
    mov ar.rsc = r21
    ;;
(p8) ssm psr.i
    ;;
    srlz.d
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64DrawStopGlyphPhysical, . - AsteriaIa64DrawStopGlyphPhysical

.align 16
.global AsteriaIa64StopFromIvt
.hidden AsteriaIa64StopFromIvt
.type AsteriaIa64StopFromIvt, @function
AsteriaIa64StopFromIvt:
    movl r17 = AsteriaIa64RuntimeInterruptVectorTableImage
    ;;
    sub r16 = r16, r17
    ;;
    mov r18 = 0x5000
    ;;
    cmp.ltu p6, p7 = r16, r18
    ;;
(p6) dep r16 = r0, r16, 0, 10
(p7) dep r16 = r0, r16, 0, 8
    ;;
    br.sptk.many AsteriaIa64StopEntry
    ;;
.align 16
.size AsteriaIa64StopFromIvt, . - AsteriaIa64StopFromIvt

.align 16
.global AsteriaIa64StopBreak
.hidden AsteriaIa64StopBreak
.type AsteriaIa64StopBreak, @function
AsteriaIa64StopBreak:
    mov r16 = ASTERIA_IA64_STOP_BREAK_VECTOR
    ;;
    br.sptk.many AsteriaIa64StopEntry
    ;;
.align 16
.size AsteriaIa64StopBreak, . - AsteriaIa64StopBreak

.align 16
.global AsteriaIa64StopPageFault
.hidden AsteriaIa64StopPageFault
.type AsteriaIa64StopPageFault, @function
AsteriaIa64StopPageFault:
    mov r16 = ar.k4
    ;;
    shl r16 = r16, 8
    ;;
    br.sptk.many AsteriaIa64StopEntry
    ;;
.align 16
.size AsteriaIa64StopPageFault, . - AsteriaIa64StopPageFault

.align 16
.global AsteriaIa64StopExternal
.hidden AsteriaIa64StopExternal
.type AsteriaIa64StopExternal, @function
AsteriaIa64StopExternal:
    mov r16 = ASTERIA_IA64_STOP_EXTERNAL_VECTOR
    ;;
    br.sptk.many AsteriaIa64StopEntry
    ;;
.align 16
.size AsteriaIa64StopExternal, . - AsteriaIa64StopExternal

.align 16
.global AsteriaIa64StopUserFault
.hidden AsteriaIa64StopUserFault
.type AsteriaIa64StopUserFault, @function
AsteriaIa64StopUserFault:
    mov r16 = ASTERIA_IA64_STOP_DATA_MISS_VECTOR
    ;;
    br.sptk.many AsteriaIa64StopUserExit
    ;;
.align 16
.size AsteriaIa64StopUserFault, . - AsteriaIa64StopUserFault

.align 16
.global AsteriaIa64StopException
.hidden AsteriaIa64StopException
.type AsteriaIa64StopException, @function
AsteriaIa64StopException:
    mov r16 = ar.k4
    ;;
    shl r16 = r16, 8
    ;;
    br.sptk.many AsteriaIa64StopUserExit
    ;;
.align 16
.size AsteriaIa64StopException, . - AsteriaIa64StopException

// The user-fault and user-exception entries' one fail-stop reaches here with
// the vector in r16. Collection is still off before their bridge call and on
// from that call until their restore, which has no check after it clears
// collection, so PSR.ic names the exit; after the bridge r8 holds its status,
// zero when a restore check failed instead (Ia64StopExit).
.align 16
.global AsteriaIa64StopUserExit
.hidden AsteriaIa64StopUserExit
.type AsteriaIa64StopUserExit, @function
AsteriaIa64StopUserExit:
    mov r17 = psr
    ;;
    tbit.nz p6, p7 = r17, 13
    ;;
(p7) mov r18 = ASTERIA_IA64_STOP_EXIT_USER_ENTRY << ASTERIA_IA64_STOP_EXIT_SHIFT
    ;;
(p6) mov r18 = ASTERIA_IA64_STOP_EXIT_USER_FAULT_BRIDGE << ASTERIA_IA64_STOP_EXIT_SHIFT
    ;;
(p6) dep r18 = r8, r18, ASTERIA_IA64_STOP_BRIDGE_STATUS_SHIFT, ASTERIA_IA64_STOP_BRIDGE_STATUS_BITS
    ;;
    or r16 = r16, r18
    ;;
    br.sptk.many AsteriaIa64StopEntry
    ;;
.align 16
.size AsteriaIa64StopUserExit, . - AsteriaIa64StopUserExit
