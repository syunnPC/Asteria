// SPDX-License-Identifier: Apache-2.0

// Physical SAL_SET_VECTORS bridge of SalBootRendezvousRegistration: checks
// the entry PSR, then calls SAL with the entry, GP and eight arguments of one
// SalFirmwareCallRequest and returns its four result words untouched.

.text
.align 16
.global AsteriaIa64InvokePhysicalSalSetVectors
.hidden AsteriaIa64InvokePhysicalSalSetVectors
.type AsteriaIa64InvokePhysicalSalSetVectors, @function
AsteriaIa64InvokePhysicalSalSetVectors:
    alloc r34 = ar.pfs, 2, 5, 8, 0
    mov r35 = b0
    mov r36 = b5
    mov r37 = r1
    mov r38 = psr
    ;;
// mov-from-PSR exposes only PSR{36:35,31:0}. Reject the readable fields
// that violate this physical SAL adapter: be, dt, rt, and it.
    movl r14 = 0x0000001008020002
    ;;
    and r14 = r38, r14
    ;;
    cmp.ne p6, p7 = r14, r0
    ;;
(p6) br.cond.dpnt.few 1f
    ;;
    mov r38 = r32
    ;;
    ld8 r14 = [r38], 8
    ;;
    ld8 r15 = [r38], 8
    ;;
    mov b5 = r14
    mov r1 = r15
    ld8 r39 = [r38], 8
    ;;
    ld8 r40 = [r38], 8
    ;;
    ld8 r41 = [r38], 8
    ;;
    ld8 r42 = [r38], 8
    ;;
    ld8 r43 = [r38], 8
    ;;
    ld8 r44 = [r38], 8
    ;;
    ld8 r45 = [r38], 8
    ;;
    ld8 r46 = [r38]
    ;;
    br.call.sptk.many b0 = b5
    ;;
    st8 [r33] = r0
    mov r1 = r37
    mov b5 = r36
    mov b0 = r35
    mov ar.pfs = r34
    ;;
    br.ret.sptk.many b0
    ;;
1:
    mov r8 = r0
    mov r9 = r0
    mov r10 = r0
    mov r11 = r0
    mov r14 = 1
    ;;
    st8 [r33] = r14
    mov r1 = r37
    mov b5 = r36
    mov b0 = r35
    mov ar.pfs = r34
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64InvokePhysicalSalSetVectors, . - AsteriaIa64InvokePhysicalSalSetVectors
