// SPDX-License-Identifier: Apache-2.0

// Physical static-PAL call bridge of PalStaticCallAdapter: checks the entry
// PSR, then calls PAL with interrupts and collection off and the RSE in
// enforced-lazy mode, and restores the caller's PSR, RSE and P64 state.

.text
.align 16
.global AsteriaIa64InvokePhysicalPalStatic
.hidden AsteriaIa64InvokePhysicalPalStatic
.type AsteriaIa64InvokePhysicalPalStatic, @function
AsteriaIa64InvokePhysicalPalStatic:
    alloc r38 = ar.pfs, 6, 8, 0, 0
    mov r39 = b0
    mov r40 = b5
    mov r41 = r1
    mov r42 = psr
    mov r43 = ar.rsc
    ;;
// Reject visible PSR state that violates the physical static-PAL entry
// contract: be, dt, dfl, db, lp, tb, rt, mc, and it must all be zero.
    movl r45 = 0x000000180f060002
    ;;
    and r45 = r42, r45
    ;;
    cmp.ne p6, p7 = r45, r0
    ;;
(p6) br.cond.dpnt.few 3f
    ;;
// SAL hands off bank one; reselect it before populating banked PAL inputs.
    bsw.1
    ;;
    mov r28 = r33
    mov r29 = r34
    mov r30 = r35
    mov r31 = r36
    mov b5 = r32
    ;;
    rsm psr.i
    ;;
    rsm psr.ic
    ;;
    srlz.d
    ;;
    srlz.i
    ;;
    mov ar.rsc = r0
    ;;
1:
    mov r44 = ip
    ;;
    adds r44 = 2f-1b, r44
    ;;
    mov b0 = r44
    ;;
    br.cond.sptk.many b5
    ;;
2:
    st8 [r37] = r0
    ;;
    mov ar.rsc = r43
    ;;
    mov psr.l = r42
    ;;
    srlz.d
    ;;
    srlz.i
    ;;
    mov b0 = r39
    mov b5 = r40
    mov r1 = r41
    mov ar.pfs = r38
    ;;
    br.ret.sptk.many b0
    ;;
3:
    mov r8 = r0
    mov r9 = r0
    mov r10 = r0
    mov r11 = r0
    mov r45 = 1
    ;;
    st8 [r37] = r45
    ;;
    mov b0 = r39
    mov b5 = r40
    mov r1 = r41
    mov ar.pfs = r38
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64InvokePhysicalPalStatic, . - AsteriaIa64InvokePhysicalPalStatic
