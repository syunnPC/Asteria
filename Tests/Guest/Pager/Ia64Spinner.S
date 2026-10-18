// SPDX-License-Identifier: Apache-2.0
// Project-owned CPU-bound state probe: Intel SDM rev. 2.3 Vol. 1 §§3, 5, Vol. 3.
// The test pager copies the code segment to an RX page and runs it as a
// created thread, called from AsteriaIa64FaultInstruction. It loads a canary
// into every register it can write at PL3, counts for 50 user time slices
// of ITC time (2.5 s at QEMU's 400-MHz ITC) without a memory access or system
// call, so the local timer must end many of its runs (Ia64UserPreemption.hpp)
// while the host, sampling the evidence a few times a second, watches it
// count, then compares every canary and ends with break 0x1234 (state
// preserved) or break 0x4321 (state lost), which its supervisor terminates.
#include "Generated/Platform/Ia64/Ia64UserModeLaunchAsmLayout.h"

// The ITC ticks the count lasts: the ITC follows the host's clock, so a
// loaded host shortens the spinner's share of them but not their number.
#define ASTERIA_IA64_SPINNER_COUNT_TICKS (ASTERIA_IA64_USER_SCHEDULING_PERIOD_TICKS * 50)
// The loop-count register's canary; the count loop never uses ar.lc.
#define ASTERIA_IA64_SPINNER_LOOP_COUNT_CANARY 0x5a5a5a5a5a5a5a5a
.explicit
.text
.align 16
.global AsteriaIa64SpinnerEntry
.hidden AsteriaIa64SpinnerEntry
.type AsteriaIa64SpinnerEntry, @function
.proc AsteriaIa64SpinnerEntry
AsteriaIa64SpinnerEntry:
    alloc r2 = ar.pfs, 0, 96, 0, 0
    ;;
    movl r16 = ASTERIA_IA64_USER_MODE_INITIAL_FLOATING_POINT_STATUS ^ 0x400
    ;;
    mov ar.fpsr = r16
    ;;
    movl r16 = 0xc000000000000007
    ;;
    mov ar.pfs = r16
    ;;
    mov r16 = 13
    ;;
    mov ar.ec = r16
    ;;
    movl r16 = 0x0123456789abcdef
    ;;
    mov ar.unat = r16
    ;;
.irp number, 2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,\
    18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,\
    34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,\
    50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,\
    66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,\
    82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,\
    98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,\
    114,115,116,117,118,119,120,121,122,123,124,125,126,127
    mov r16 = \number
    ;;
    setf.sig f\number = r16
    ;;
.endr
    mov r16 = 4096
    ;;
    mov b0 = r16
    ;;
    mov r16 = 4112
    ;;
    mov b1 = r16
    ;;
    mov r16 = 4128
    ;;
    mov b2 = r16
    ;;
    mov r16 = 4144
    ;;
    mov b3 = r16
    ;;
    mov r16 = 4160
    ;;
    mov b4 = r16
    ;;
    mov r16 = 4176
    ;;
    mov b5 = r16
    ;;
    mov r16 = 4192
    ;;
    mov b6 = r16
    ;;
    mov r16 = 4208
    ;;
    mov b7 = r16
    ;;
    movl r16 = 0xaaaaaaaaaaaaaaaa
    ;;
    mov pr = r16, -1
    ;;
    movl r16 = ASTERIA_IA64_SPINNER_LOOP_COUNT_CANARY
    ;;
    mov ar.lc = r16
    ;;
    mov r1 = 1
    ;;
    mov r2 = 2
    ;;
    mov r3 = 3
    ;;
    mov r4 = 4
    ;;
    mov r5 = 5
    ;;
    mov r6 = 6
    ;;
    mov r7 = 7
    ;;
    mov r8 = 0
    ;;
    mov r9 = 0
    ;;
    mov r12 = 12
    ;;
    mov r13 = 13
    ;;
    mov r14 = 14
    ;;
    mov r15 = 15
    ;;
    mov r16 = 16
    ;;
    mov r17 = 17
    ;;
    mov r18 = 18
    ;;
    mov r19 = 19
    ;;
    mov r20 = 20
    ;;
    mov r21 = 21
    ;;
    mov r26 = 26
    ;;
    mov r27 = 27
    ;;
    mov r28 = 28
    ;;
    mov r29 = 29
    ;;
    mov r30 = 30
    ;;
    mov r31 = 31
    ;;
    mov r22 = ar.bsp
    ;;
    mov r23 = ar.bspstore
    ;;
    mov r24 = ar.rnat
    ;;
    mov r25 = ar.fpsr
    ;;
.irp number, 34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,\
    50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,\
    66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,\
    82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,\
    98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,\
    114,115,116,117,118,119,120,121,122,123,124,125,126,127
    mov r\number = \number
    ;;
.endr
// r8 and r9 count the iterations; r10 and r11 are the loop's ITC sample and
// deadline. The loop's compare leaves p6 clear and p7 set, as the canary
// pattern has them, once the deadline passes.
    mov.m r11 = ar.itc
    ;;
    movl r10 = ASTERIA_IA64_SPINNER_COUNT_TICKS
    ;;
    add r11 = r11, r10
    ;;
    mov r32 = pr
    ;;
.global AsteriaIa64SpinnerCount
AsteriaIa64SpinnerCount:
    adds r8 = 1, r8
    adds r9 = 1, r9
    mov.m r10 = ar.itc
    ;;
    cmp.ltu p6, p7 = r10, r11
    ;;
(p6) br.cond.sptk.few AsteriaIa64SpinnerCount
    ;;
    mov r33 = pr
    ;;
    cmp.ne p6, p7 = 16, r16
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 1, r1
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 2, r2
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 3, r3
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 4, r4
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 5, r5
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 6, r6
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 7, r7
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 12, r12
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 13, r13
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 14, r14
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 15, r15
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 17, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 18, r18
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 19, r19
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 20, r20
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 21, r21
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 26, r26
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 27, r27
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 28, r28
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 29, r29
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 30, r30
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = 31, r31
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = r8, r9
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.eq p6, p7 = r0, r8
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    cmp.ne p6, p7 = r32, r33
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    movl r16 = 0xaaaaaaaaaaaaaaab
    ;;
    cmp.ne p6, p7 = r32, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
.irp number, 34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,\
    50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,\
    66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,\
    82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,\
    98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,\
    114,115,116,117,118,119,120,121,122,123,124,125,126,127
    cmp.ne p6, p7 = \number, r\number
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
.endr
.irp number, 2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,\
    18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,\
    34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,\
    50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,\
    66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,\
    82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,\
    98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,\
    114,115,116,117,118,119,120,121,122,123,124,125,126,127
    getf.exp r16 = f\number
    ;;
    movl r17 = 0x1003e
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    getf.sig r16 = f\number
    ;;
    cmp.ne p6, p7 = \number, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
.endr
    mov r16 = b0
    ;;
    mov r17 = 4096
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = b1
    ;;
    mov r17 = 4112
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = b2
    ;;
    mov r17 = 4128
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = b3
    ;;
    mov r17 = 4144
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = b4
    ;;
    mov r17 = 4160
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = b5
    ;;
    mov r17 = 4176
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = b6
    ;;
    mov r17 = 4192
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = b7
    ;;
    mov r17 = 4208
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = ar.bsp
    ;;
    cmp.ne p6, p7 = r16, r22
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = ar.bspstore
    ;;
    cmp.ne p6, p7 = r16, r23
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = ar.rnat
    ;;
    cmp.ne p6, p7 = r16, r24
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = ar.fpsr
    ;;
    cmp.ne p6, p7 = r16, r25
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = ar.rsc
    ;;
    movl r17 = ASTERIA_IA64_USER_MODE_INITIAL_RSE_CONFIGURATION
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = ar.pfs
    ;;
    movl r17 = 0xc000000000000007
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = ar.ec
    ;;
    movl r17 = 13
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = ar.lc
    ;;
    movl r17 = ASTERIA_IA64_SPINNER_LOOP_COUNT_CANARY
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
    mov r16 = ar.unat
    ;;
    movl r17 = 0x0123456789abcdef
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_spinner_lost
    ;;
.align 16
.global AsteriaIa64SpinnerSuccess
AsteriaIa64SpinnerSuccess:
{ .mii
    nop.m 0
    break.i 0x1234
    nop.i 0
}
    ;;
.Lasteria_ia64_spinner_lost:
{ .mii
    nop.m 0
    break.i 0x4321
    nop.i 0
}
    ;;
.endp AsteriaIa64SpinnerEntry
.align 16
.size AsteriaIa64SpinnerEntry, . - AsteriaIa64SpinnerEntry
AsteriaIa64SpinnerEnd:
// The contract the test pager reads from the image's second segment: the
// code bytes, the success break's bundle offset, the ITC ticks the count
// lasts, and the success break's immediate.
.section .rodata.SpinnerContract,"a",@progbits
.align 16
.quad AsteriaIa64SpinnerEnd - AsteriaIa64SpinnerEntry
.quad AsteriaIa64SpinnerSuccess - AsteriaIa64SpinnerEntry
.quad ASTERIA_IA64_SPINNER_COUNT_TICKS
.quad 0x1234
.section .AsteriaUserEntryDescriptor,"a",@progbits
.align 16
.global AsteriaIa64ComponentImageEntryDescriptor
.hidden AsteriaIa64ComponentImageEntryDescriptor
.type AsteriaIa64ComponentImageEntryDescriptor, @object
AsteriaIa64ComponentImageEntryDescriptor:
.quad AsteriaIa64SpinnerEntry
.quad __gp
.size AsteriaIa64ComponentImageEntryDescriptor, 16
.bss
.align 16
.skip 16
