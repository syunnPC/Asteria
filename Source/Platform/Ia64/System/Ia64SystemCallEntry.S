// SPDX-License-Identifier: Apache-2.0

// IA-64 break system-call entry: admission, frame capture, the C++ bridge call
// and the rfi restore. Frame and context offsets and the admission constants
// come from the generated C++ layout.

#include "Generated/Platform/Ia64/Ia64SystemCallEntryAsmLayout.h"

.explicit
#include "Platform/Ia64/Diagnostics/Ia64StopPoll.inc"

.text
.align 16
.global AsteriaIa64SystemCallEntry
.hidden AsteriaIa64SystemCallEntry
.type AsteriaIa64SystemCallEntry, @function
.macro AsteriaIa64SystemCallStoreFrame offset, value
    add r17 = \offset, r29
    ;;
    st8 [r17] = \value
    ;;
.endm
AsteriaIa64SystemCallEntry:
// Freeze the user RSE before even the stop poll can access kernel memory;
// retain the original configuration in ar.k6.
    mov r16 = ar.rsc
    ;;
    mov.m ar.k6 = r16
    mov r17 = ASTERIA_IA64_SYSTEM_CALL_USER_RSE_CONFIGURATION
    ;;
    mov ar.rsc = r17
    ;;
    AsteriaIa64PollStop ASTERIA_IA64_STOP_BREAK_VECTOR
// Save PR before the first validation changes predicates. Bank-zero
// r16-r31 are the only scratch registers used before the interrupted
// unbanked and stacked values have been captured NaT-safely.
    mov r31 = pr
    mov r23 = psr
    mov r22 = cr.iip
    mov r21 = cr.ipsr
    mov r20 = cr.isr
    mov r19 = cr.iim
    mov r18 = cr.ifs
    ;;
// Current execution must be translated bank-zero interruption context
// with collection, interruption delivery, and protection-key checking
// disabled. The complete interrupted PSR carries the PL3/bank-one/native
// contract that mov-from-PSR cannot observe.
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_CURRENT_PROCESSOR_STATUS_MASK
    ;;
    and r17 = r23, r16
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_CURRENT_PROCESSOR_STATUS_VALUE
    ;;
    cmp.ne p6, p7 = r17, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_INTERRUPTED_PROCESSOR_STATUS_MASK
    ;;
    and r17 = r21, r16
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_INTERRUPTED_PROCESSOR_STATUS_VALUE
    ;;
    cmp.ne p6, p7 = r17, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_IFS_VALID_MASK
    ;;
    and r17 = r18, r16
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    and r16 = ASTERIA_IA64_SYSTEM_CALL_BUNDLE_SIZE_BYTES - 1, r22
    extr.u r17 = r21, ASTERIA_IA64_SYSTEM_CALL_INSTRUCTION_SLOT_SHIFT, 2
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    mov r16 = 3
    ;;
    cmp.eq p6, p7 = r17, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
// Redirect a PL3-writable RSC violation before any stacked argument or backing-store access.
    mov.m r16 = ar.k6
    mov r17 = ASTERIA_IA64_SYSTEM_CALL_USER_RSE_CONFIGURATION
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.spnt.few .Lasteria_ia64_system_call_user_state_fault
    ;;
// Offset 16 of the immutable 48-byte refill context is the trusted
// publication slot installed by the IVT owner. Subtractive upper bounds
// admit the complete root and 64-byte entry context before either load.
    mov r17 = ar.k3
    ;;
    cmp.eq p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    and r16 = 15, r17
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_FIRST_NON_KERNEL_REGION_ADDRESS - ASTERIA_IA64_SYSTEM_CALL_REFILL_CONTEXT_SIZE_BYTES
    ;;
    cmp.gtu p6, p7 = r17, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    add r17 = ASTERIA_IA64_SYSTEM_CALL_REFILL_CONTEXT_PUBLICATION_OFFSET_BYTES, r17
    ;;
    ld8.acq r30 = [r17]
    ;;
    cmp.eq p6, p7 = r30, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    and r16 = 15, r30
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_FIRST_NON_KERNEL_REGION_ADDRESS - ASTERIA_IA64_SYSTEM_CALL_ENTRY_CONTEXT_SIZE_BYTES
    ;;
    cmp.gtu p6, p7 = r30, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    adds r24 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_CONTEXT_FRAME_ADDRESS, r30
    ;;
    ld8 r29 = [r24], 8
    ;;
    ld8 r28 = [r24], 8
    ;;
    ld8 r27 = [r24], 8
    ;;
    ld8 r26 = [r24], 8
    ;;
    ld8 r25 = [r24], 8
    ;;
    ld8 r17 = [r24], 8
    ;;
    ld8 r16 = [r24], 8
    ;;
    ld8 r24 = [r24]
    ;;
    cmp.ne p6, p7 = r24, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    mov r24 = ASTERIA_MAXIMUM_IA64_SYSTEM_CALL_PROCESSOR_COUNT
    ;;
    cmp.geu p6, p7 = r16, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    cmp.eq p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    and r16 = 7, r17
    movl r24 = ASTERIA_IA64_SYSTEM_CALL_FIRST_NON_KERNEL_REGION_ADDRESS
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    cmp.geu p6, p7 = r17, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
// The immutable context root is no longer needed. Retain the admitted
// bridge-context address in disposable bank-zero r30 until it is copied
// into the P64 argument window.
    mov r30 = r17
    ;;
    and r16 = 15, r29
    cmp.eq p6, p7 = r29, r0
    ;;
    cmp.ne.or p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    add r24 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_SIZE, r29
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_FIRST_NON_KERNEL_REGION_ADDRESS
    ;;
    cmp.leu p6, p7 = r24, r29
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    cmp.gtu p6, p7 = r24, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    cmp.eq p6, p7 = r28, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    mov r17 = ASTERIA_IA64_SYSTEM_CALL_STORAGE_PAGE_SIZE_BYTES - 1
    ;;
    and r16 = r17, r28
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    and r16 = r17, r27
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    cmp.leu p6, p7 = r27, r28
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    sub r16 = r27, r28
    mov r17 = ASTERIA_IA64_SYSTEM_CALL_MEMORY_STACK_SIZE_BYTES
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_FIRST_NON_KERNEL_REGION_ADDRESS
    ;;
    cmp.gtu p6, p7 = r27, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    cmp.eq p6, p7 = r26, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    mov r17 = ASTERIA_IA64_SYSTEM_CALL_STORAGE_PAGE_SIZE_BYTES - 1
    ;;
    and r16 = r17, r26
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    and r16 = r17, r25
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    cmp.leu p6, p7 = r25, r26
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    sub r16 = r25, r26
    mov r17 = ASTERIA_IA64_SYSTEM_CALL_RSE_BACKING_STORE_SIZE_BYTES
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_FIRST_NON_KERNEL_REGION_ADDRESS
    ;;
    cmp.gtu p6, p7 = r25, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
// Reject pairwise overlap between the dedicated storage ranges and the
// frame. Branching the disjoint cases avoids predicate conjunction state.
    cmp.geu p6, p7 = r28, r25
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_system_call_storage_disjoint
    ;;
    cmp.geu p6, p7 = r26, r27
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_system_call_storage_disjoint
    ;;
    br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
.Lasteria_ia64_system_call_storage_disjoint:
    cmp.geu p6, p7 = r29, r27
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_system_call_frame_stack_disjoint
    ;;
    cmp.geu p6, p7 = r28, r24
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_system_call_frame_stack_disjoint
    ;;
    br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
.Lasteria_ia64_system_call_frame_stack_disjoint:
    cmp.geu p6, p7 = r29, r25
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_system_call_frame_rse_disjoint
    ;;
    cmp.geu p6, p7 = r26, r24
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_system_call_frame_rse_disjoint
    ;;
    br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
.Lasteria_ia64_system_call_frame_rse_disjoint:
// Every other break immediate is not a system call. The ABI defines no
// response to one and user faults are not recoverable yet, so it is
// reported on the admitted kernel stack and RSE, never captured.
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_BREAK_IMMEDIATE
    ;;
    cmp.ne p6, p7 = r19, r16
    ;;
(p6) br.cond.dpnt.few AsteriaIa64SystemCallUnknownBreak
    ;;
// Publish Active only after all immutable context fields are accepted.
    add r17 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_RESERVED, r29
    ;;
    ld8 r16 = [r17]
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    ld8.acq r16 = [r29]
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    mov r16 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_STATE_ACTIVE
    ;;
    st8.rel [r29] = r16
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_CURRENT_PROCESSOR_STATUS, r23
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_INTERRUPTED_INSTRUCTION_POINTER, r22
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_INTERRUPTED_PROCESSOR_STATUS, r21
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_INTERRUPTION_STATUS, r20
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_BREAK_IMMEDIATE, r19
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_HARDWARE_INTERRUPTION_FUNCTION_STATE, r18
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_PREDICATES, r31
    mov r16 = ar.pfs
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_PREVIOUS_FUNCTION_STATE, r16
    mov.m r16 = ar.k6
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_USER_RSE_CONFIGURATION, r16
    mov r17 = ASTERIA_IA64_SYSTEM_CALL_USER_RSE_CONFIGURATION
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
// Save every application and branch register before any local P64 frame is
// allocated. cover does not alter these values, but the bridge may.
    mov r16 = ar.ec
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_EPILOG_COUNTER, r16
    mov r16 = ar.unat
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_ORIGINAL_USER_NAT_COLLECTION, r16
    mov ar.unat = r0
    ;;
    mov r16 = ar.fpsr
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_FLOATING_POINT_STATUS, r16
    mov r16 = ar.lc
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_LOOP_COUNTER, r16
    mov r16 = ar.ccv
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_COMPARE_EXCHANGE_VALUE, r16
    mov r16 = b0
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B0, r16
    mov r16 = b1
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B1, r16
    mov r16 = b2
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B2, r16
    mov r16 = b3
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B3, r16
    mov r16 = b4
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B4, r16
    mov r16 = b5
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B5, r16
    mov r16 = b6
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B6, r16
    mov r16 = b7
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B7, r16
// Sample CFM without accessing a stacked register: br.call saves it in
// PFS; the matching local br.ret restores the same frame, EC and rename
// bases (SDM rev. 1.1 Vol. 3, "br", Operation). No frame is allocated.
// PFS and b6 were captured above; restore both before either exit.
    br.call.sptk.many b6 = .Lasteria_ia64_system_call_sample_cfm
    ;;
.Lasteria_ia64_system_call_cfm_sampled:
    mov r18 = ar.pfs
    add r17 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_PREVIOUS_FUNCTION_STATE, r29
    ;;
    ld8 r16 = [r17]
    ;;
    mov ar.pfs = r16
    add r17 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B6, r29
    ;;
    ld8 r16 = [r17]
    ;;
    mov b6 = r16
    extr.u r16 = r18, 7, 7
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.spnt.few .Lasteria_ia64_system_call_cfm_fault
    ;;
    extr.u r16 = r18, 14, 24
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.spnt.few .Lasteria_ia64_system_call_cfm_fault
    ;;
    and r16 = 127, r18
    ;;
    mov r17 = ASTERIA_IA64_SYSTEM_CALL_MAXIMUM_STACKED_REGISTERS
    ;;
    cmp.gtu p6, p7 = r16, r17
    ;;
(p6) br.cond.spnt.few .Lasteria_ia64_system_call_cfm_fault
    ;;
    mov r17 = ASTERIA_IA64_SYSTEM_CALL_REQUIRED_OUTPUT_REGISTERS
    ;;
    cmp.ltu p6, p7 = r16, r17
    ;;
(p6) br.cond.spnt.few .Lasteria_ia64_system_call_cfm_fault
    ;;
// Capture the fixed SOL-zero output window before cover makes all stacked
// registers inaccessible. tnat observes each input without consuming it,
// and spill stores retain the corresponding values and NaT bits.
    mov r18 = r0
    ;;
    tnat.nz p6, p7 = r15
    ;;
(p6) add r18 = 1, r18
    tnat.nz p6, p7 = r32
    ;;
(p6) add r18 = 2, r18
    tnat.nz p6, p7 = r33
    ;;
(p6) add r18 = 4, r18
    tnat.nz p6, p7 = r34
    ;;
(p6) add r18 = 8, r18
    tnat.nz p6, p7 = r35
    ;;
(p6) add r18 = 16, r18
    tnat.nz p6, p7 = r36
    ;;
(p6) add r18 = 32, r18
    tnat.nz p6, p7 = r37
    ;;
(p6) add r18 = 64, r18
    ;;
    add r17 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_SPILL_R1, r29
    ;;
    st8.spill [r17] = r1, 8
    ;;
    st8.spill [r17] = r2, 8
    ;;
    st8.spill [r17] = r3, 8
    ;;
    st8.spill [r17] = r4, 8
    ;;
    st8.spill [r17] = r5, 8
    ;;
    st8.spill [r17] = r6, 8
    ;;
    st8.spill [r17] = r7, 8
    ;;
    st8.spill [r17] = r11, 8
    ;;
    st8.spill [r17] = r12, 8
    ;;
    st8.spill [r17] = r13, 8
    ;;
    st8.spill [r17] = r14, 8
    ;;
    st8.spill [r17] = r15, 8
    ;;
    add r17 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_REGISTERS_R15, r29
    ;;
    st8.spill [r17] = r15, 8
    ;;
    st8.spill [r17] = r32, 8
    ;;
    st8.spill [r17] = r33, 8
    ;;
    st8.spill [r17] = r34, 8
    ;;
    st8.spill [r17] = r35, 8
    ;;
    st8.spill [r17] = r36, 8
    ;;
    st8.spill [r17] = r37, 8
    ;;
    st8 [r17] = r18
    ;;
    mov r16 = ar.unat
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_SPILL_NAT_COLLECTION, r16
// Save bank one before it becomes the P64 execution bank. Its scratch
// prefix is retained kernel memory and remains disjoint from the downward
// growing P64 stack. A separate UNAT image avoids address-index aliasing
// with the frame's software-spill collection.
    mov ar.unat = r0
    mov r14 = r28
    ;;
    bsw.1
    ;;
    st8.spill [r14] = r16, 8
    ;;
    st8.spill [r14] = r17, 8
    ;;
    st8.spill [r14] = r18, 8
    ;;
    st8.spill [r14] = r19, 8
    ;;
    st8.spill [r14] = r20, 8
    ;;
    st8.spill [r14] = r21, 8
    ;;
    st8.spill [r14] = r22, 8
    ;;
    st8.spill [r14] = r23, 8
    ;;
    st8.spill [r14] = r24, 8
    ;;
    st8.spill [r14] = r25, 8
    ;;
    st8.spill [r14] = r26, 8
    ;;
    st8.spill [r14] = r27, 8
    ;;
    st8.spill [r14] = r28, 8
    ;;
    st8.spill [r14] = r29, 8
    ;;
    st8.spill [r14] = r30, 8
    ;;
    st8.spill [r14] = r31, 8
    ;;
    mov r15 = ar.unat
    ;;
    st8 [r14] = r15
    ;;
    bsw.0
    ;;
    mov ar.unat = r0
    ;;
// Intel SDM Vol. 2 section 6.11.1 requires cover while the interrupted CFM
// is current and before BSPSTORE is switched. The direct cover result is
// the authoritative IFS that rfi must later consume.
    cover
    ;;
    mov r18 = cr.ifs
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_CAPTURED_USER_INTERRUPTION_FUNCTION_STATE, r18
    movl r16 = ASTERIA_IA64_SYSTEM_CALL_IFS_VALID_MASK
    ;;
    and r17 = r18, r16
    ;;
    cmp.eq p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    extr.u r16 = r18, 38, 25
    extr.u r17 = r18, 14, 24
    ;;
    or r16 = r16, r17
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    extr.u r16 = r18, 0, 7
    extr.u r17 = r18, 7, 7
    ;;
    mov r19 = ASTERIA_IA64_SYSTEM_CALL_MAXIMUM_STACKED_REGISTERS
    ;;
    cmp.gtu p6, p7 = r16, r19
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    sub r16 = r16, r17
    ;;
    mov r19 = ASTERIA_IA64_SYSTEM_CALL_REQUIRED_OUTPUT_REGISTERS
    ;;
    cmp.ltu p6, p7 = r16, r19
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
// With the interrupted frame covered, enforced-lazy mode freezes its dirty
// partition while BSPSTORE and RNAT are captured and rebound. Writing
// BSPSTORE preserves that dirty partition and makes the new BSP distance
// the exact loadrs quantity required by section 6.11.2.
    invala
    ;;
    mov ar.rsc = r0
    ;;
    mov r16 = ar.bspstore
    mov r19 = ar.rnat
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_USER_RSE_BACKING_STORE_STORE_POINTER, r16
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_USER_RSE_NAT_COLLECTION, r19
    mov ar.bspstore = r26
    ;;
    mov ar.rnat = r0
    ;;
    mov r18 = ar.bsp
    ;;
    cmp.ltu p6, p7 = r18, r26
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    sub r18 = r18, r26
    ;;
    and r24 = 7, r18
    ;;
    cmp.ne p6, p7 = r24, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    mov r24 = ASTERIA_IA64_SYSTEM_CALL_RSE_TRANSITION_SIZE_BYTES
    ;;
    cmp.geu p6, p7 = r18, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    shl r18 = r18, 16
    ;;
    AsteriaIa64SystemCallStoreFrame ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_KERNEL_RSE_LOAD_CONFIGURATION, r18
    mov r22 = ASTERIA_IA64_SYSTEM_CALL_KERNEL_RSE_CONFIGURATION
    ;;
    mov ar.rsc = r22
    ;;
    add r12 = -16, r27
    ;;
    mf
    ;;
// R4 and r5 carry the frame and low-level bank-save pointers across the P64 bridge. Caller
// out0/out1 carry only the admitted frame and trusted bridge context. Bank zero remains disposable
// while collection is enabled so a recoverable nested interruption may use its normal heavyweight
// bank.
    alloc r22 = ar.pfs, 0, 0, 2, 0
    ;;
    mov r4 = r29
    mov r5 = r28
    ;;
    mov r32 = r4
    mov r33 = r30
    ;;
    ssm psr.ic
    bsw.1
    ;;
    srlz.d
    ;;
// The kernel computes under its own fpsr, all traps disabled, never the user's: the user's was
// captured with the frame and is restored on the way out.
    movl r1 = __gp
    movl r14 = ASTERIA_IA64_SYSTEM_CALL_KERNEL_FLOATING_POINT_STATUS
    ;;
    mov ar.fpsr = r14
    ;;
    br.call.sptk.many b0 = AsteriaIa64SystemCallBridge
    ;;
    cmp.ne p6, p7 = r8, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_system_call_fail_stop
    ;;
    mf
    ;;
// Select enforced-lazy mode with the exact dirty-partition load distance.
// Every physical stacked register outside that partition was last written
// by the kernel, so scrub the whole file before PL3 can allocate one: grow
// the P64 caller frame to all of them (PAL_RSE_INFO proved there are
// exactly as many as one frame can hold; mandatory spills first write any
// still-resident part of the interrupted partition to the kernel backing
// store), clear each value and NaT bit, and drop the frame. loadrs then
// reloads exactly the interrupted dirty partition and invalidates every
// other stacked register, which holds zero. Collection is enabled
// throughout; BSPSTORE and RNAT are restored afterwards in the architected
// order. alloc and loadrs are first in their instruction groups.
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_USER_RSE_BACKING_STORE_STORE_POINTER, r4
    ;;
    ld8 r17 = [r15], 8
    ;;
    ld8 r16 = [r15], 8
    ;;
    ld8 r18 = [r15]
    ;;
    mov ar.rsc = r16
    ;;
#if ASTERIA_IA64_SYSTEM_CALL_PHYSICAL_STACKED_REGISTERS != 96
#error "the stacked-register scrub clears exactly r32-r127"
#endif
    alloc r22 = ar.pfs, 0, ASTERIA_IA64_SYSTEM_CALL_PHYSICAL_STACKED_REGISTERS, 0, 0
    ;;
    mov r32 = r0
    mov r33 = r0
    mov r34 = r0
    mov r35 = r0
    mov r36 = r0
    mov r37 = r0
    mov r38 = r0
    mov r39 = r0
    ;;
    mov r40 = r0
    mov r41 = r0
    mov r42 = r0
    mov r43 = r0
    mov r44 = r0
    mov r45 = r0
    mov r46 = r0
    mov r47 = r0
    ;;
    mov r48 = r0
    mov r49 = r0
    mov r50 = r0
    mov r51 = r0
    mov r52 = r0
    mov r53 = r0
    mov r54 = r0
    mov r55 = r0
    ;;
    mov r56 = r0
    mov r57 = r0
    mov r58 = r0
    mov r59 = r0
    mov r60 = r0
    mov r61 = r0
    mov r62 = r0
    mov r63 = r0
    ;;
    mov r64 = r0
    mov r65 = r0
    mov r66 = r0
    mov r67 = r0
    mov r68 = r0
    mov r69 = r0
    mov r70 = r0
    mov r71 = r0
    ;;
    mov r72 = r0
    mov r73 = r0
    mov r74 = r0
    mov r75 = r0
    mov r76 = r0
    mov r77 = r0
    mov r78 = r0
    mov r79 = r0
    ;;
    mov r80 = r0
    mov r81 = r0
    mov r82 = r0
    mov r83 = r0
    mov r84 = r0
    mov r85 = r0
    mov r86 = r0
    mov r87 = r0
    ;;
    mov r88 = r0
    mov r89 = r0
    mov r90 = r0
    mov r91 = r0
    mov r92 = r0
    mov r93 = r0
    mov r94 = r0
    mov r95 = r0
    ;;
    mov r96 = r0
    mov r97 = r0
    mov r98 = r0
    mov r99 = r0
    mov r100 = r0
    mov r101 = r0
    mov r102 = r0
    mov r103 = r0
    ;;
    mov r104 = r0
    mov r105 = r0
    mov r106 = r0
    mov r107 = r0
    mov r108 = r0
    mov r109 = r0
    mov r110 = r0
    mov r111 = r0
    ;;
    mov r112 = r0
    mov r113 = r0
    mov r114 = r0
    mov r115 = r0
    mov r116 = r0
    mov r117 = r0
    mov r118 = r0
    mov r119 = r0
    ;;
    mov r120 = r0
    mov r121 = r0
    mov r122 = r0
    mov r123 = r0
    mov r124 = r0
    mov r125 = r0
    mov r126 = r0
    mov r127 = r0
    ;;
    alloc r22 = ar.pfs, 0, 0, 0, 0
    ;;
    loadrs
    ;;
    invala
    ;;
    mov ar.bspstore = r17
    ;;
    mov ar.rnat = r18
    ;;
// Keep PFS and RSC outside bank zero until every faultable access is done.
// Architectural IVA entry clears PSR.dfl and PSR.dfh; the entry validation
// checks that state, and neither bit is set again before rfi. Low floating
// registers are therefore accessible as well as syscall-clobbered, unbanked,
// and untouched by the bank-zero-only VHPT refill path. These bit transfers
// are non-arithmetic and the plain-loaded metadata cannot carry NaT.
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_PREVIOUS_FUNCTION_STATE, r4
    ;;
    ld8 r6 = [r15], 8
    ;;
    ld8 r7 = [r15]
    ;;
    setf.sig f10 = r6
    setf.sig f11 = r7
    ;;
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_FLOATING_POINT_STATUS, r4
    ;;
    ld8 r16 = [r15], 8
    ;;
    ld8 r17 = [r15], 8
    ;;
    ld8 r18 = [r15], 8
    ;;
    ld8 r19 = [r15]
    ;;
    mov ar.fpsr = r16
    mov ar.lc = r17
    mov ar.ccv = r18
    mov ar.ec = r19
    ;;
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_B0, r4
    ;;
    ld8 r16 = [r15], 8
    ;;
    ld8 r17 = [r15], 8
    ;;
    ld8 r18 = [r15], 8
    ;;
    ld8 r19 = [r15], 8
    ;;
    ld8 r22 = [r15], 8
    ;;
    ld8 r23 = [r15], 8
    ;;
    ld8 r24 = [r15], 8
    ;;
    ld8 r25 = [r15]
    ;;
    mov b0 = r16
    mov b1 = r17
    mov b2 = r18
    mov b3 = r19
    mov b4 = r22
    mov b5 = r23
    mov b6 = r24
    mov b7 = r25
    ;;
// Carry the user's original UNAT outside bank zero as well. The spill UNAT
// remains available for the final fill sequence; no user or control value
// is installed in bank zero while a later access can still refill.
    add r14 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_ORIGINAL_USER_NAT_COLLECTION, r4
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_SPILL_NAT_COLLECTION, r4
    ;;
    ld8 r6 = [r14]
    ld8 r7 = [r15]
    ;;
    setf.sig f12 = r6
    ;;
// Restore bank one from the dedicated low-stack prefix before returning to
// bank zero. Its software-spill NaT collection is stored at offset 128.
    add r15 = 128, r5
    ;;
    ld8 r6 = [r15]
    ;;
    mov ar.unat = r6
    mov r15 = r5
    ;;
    ld8.fill r16 = [r15], 8
    ;;
    ld8.fill r17 = [r15], 8
    ;;
    ld8.fill r18 = [r15], 8
    ;;
    ld8.fill r19 = [r15], 8
    ;;
    ld8.fill r20 = [r15], 8
    ;;
    ld8.fill r21 = [r15], 8
    ;;
    ld8.fill r22 = [r15], 8
    ;;
    ld8.fill r23 = [r15], 8
    ;;
    ld8.fill r24 = [r15], 8
    ;;
    ld8.fill r25 = [r15], 8
    ;;
    ld8.fill r26 = [r15], 8
    ;;
    ld8.fill r27 = [r15], 8
    ;;
    ld8.fill r28 = [r15], 8
    ;;
    ld8.fill r29 = [r15], 8
    ;;
    ld8.fill r30 = [r15], 8
    ;;
    ld8.fill r31 = [r15]
    ;;
// Complete every frame access while bank one is active. All carrier inputs
// come from plain ld8: f6/f7/f8/f9 carry IIP/IPSR/IFS/PR and
// f10/f11/f12/f13 carry PFS/RSC/original UNAT/response r10. NaT-bearing
// user static registers are reconstructed only with ld8.fill into their
// final unbanked registers, except user r7, which uses r10 temporarily
// while r7 retains the frame-state address.
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_SPILL_NAT_COLLECTION, r4
    ;;
    ld8 r6 = [r15]
    ;;
    mov ar.unat = r6
    ;;
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_REGISTERS_R8, r4
    ;;
    ld8 r8 = [r15], 8
    ;;
    ld8 r9 = [r15], 8
    ;;
    ld8 r10 = [r15]
    ;;
    setf.sig f13 = r10
    ;;
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_INTERRUPTED_INSTRUCTION_POINTER, r4
    ;;
    ld8 r2 = [r15], 8
    ;;
    ld8 r3 = [r15]
    ;;
    setf.sig f6 = r2
    setf.sig f7 = r3
    ;;
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_CAPTURED_USER_INTERRUPTION_FUNCTION_STATE, r4
    ;;
    ld8 r6 = [r15]
    ;;
    setf.sig f8 = r6
    ;;
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_PREDICATES, r4
    ;;
    ld8 r5 = [r15]
    ;;
    setf.sig f9 = r5
    ;;
    add r15 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_SPILL_R1, r4
    mov r7 = r4
    ;;
    ld8.fill r1 = [r15], 8
    ;;
    ld8.fill r2 = [r15], 8
    ;;
    ld8.fill r3 = [r15], 8
    ;;
    ld8.fill r4 = [r15], 8
    ;;
    ld8.fill r5 = [r15], 8
    ;;
    ld8.fill r6 = [r15], 8
    ;;
    ld8.fill r10 = [r15], 8
    ;;
    ld8.fill r11 = [r15], 8
    ;;
    ld8.fill r12 = [r15], 8
    ;;
    ld8.fill r13 = [r15], 8
    ;;
    ld8.fill r14 = [r15], 8
    ;;
    ld8.fill r15 = [r15]
    ;;
    st8.rel [r7] = r0
    ;;
// No instruction after the release store can fault or refill. While IC is
// still enabled, section 3.3.7 permits final state only in bank-zero
// r16-r23: r24-r31 are firmware scratch even when external interrupts are
// disabled. Copy user r7's NaT-bearing temporary before recovering response
// r10, then leave bank zero immediately and make the final IC-off switch.
    bsw.0
    ;;
    mov r16 = r2
    mov r17 = r3
    mov r18 = r4
    mov r19 = r5
    mov r21 = r10
    ;;
    getf.sig r10 = f13
    getf.sig r20 = f12
    getf.sig r22 = f10
    getf.sig r23 = f11
    ;;
    bsw.1
    ;;
    rsm psr.ic
    bsw.0
    ;;
    srlz.d
    ;;
// With IC clear, bank-zero r24-r31 are no longer asynchronously disposable.
// Materialize the four remaining non-NaT controls only inside this final,
// nonfaulting restore window.
    getf.sig r24 = f6
    getf.sig r25 = f7
    getf.sig r26 = f8
    getf.sig r27 = f9
    ;;
    mov ar.pfs = r22
    ;;
    mov cr.ifs = r26
    ;;
    mov ar.rsc = r23
    ;;
    mov cr.iip = r24
    mov cr.ipsr = r25
    ;;
    mov pr = r27, -1
    ;;
    mov r2 = r16
    mov r3 = r17
    mov r4 = r18
    mov r5 = r19
    ;;
    mov r7 = r21
    mov ar.unat = r20
    ;;
    rfi
    ;;
.Lasteria_ia64_system_call_sample_cfm:
    br.ret.sptk.many b6
    ;;
.Lasteria_ia64_system_call_cfm_fault:
// Only UNAT and the frame state changed before the CFM admission. Restore
// them so the complete entry captures the original thread image.
    add r17 = ASTERIA_IA64_SYSTEM_CALL_ENTRY_FRAME_ORIGINAL_USER_NAT_COLLECTION, r29
    ;;
    ld8 r16 = [r17]
    ;;
    mov ar.unat = r16
    st8.rel [r29] = r0
    ;;
.Lasteria_ia64_system_call_user_state_fault:
    mov pr = r31, -1
    ;;
    mov.m ar.k4 = ASTERIA_IA64_SYSTEM_CALL_FAULT_VECTOR_SELECTOR
    mov r16 = ASTERIA_IA64_SYSTEM_CALL_FAULT_ENTRY_SELECTOR
    ;;
    br.sptk.many AsteriaIa64UserExceptionEntryNormalized
    ;;
.Lasteria_ia64_system_call_fail_stop:
    hint @pause
    ;;
    br.sptk.many AsteriaIa64StopBreak
    ;;
.align 16
.size AsteriaIa64SystemCallEntry, . - AsteriaIa64SystemCallEntry
.purgem AsteriaIa64SystemCallStoreFrame

// A break with an immediate other than the system call's, from the admission
// path of AsteriaIa64SystemCallEntry: PSR.ic and PSR.i are off, bank zero is
// selected, and the per-CPU entry context is admitted. r19 holds cr.iim, r22
// cr.iip, r26 the kernel RSE start and r27 the kernel memory-stack end. The
// interrupted state is abandoned, not captured: the stacked registers are
// covered onto the kernel RSE as the system call does, and the diagnostic
// reporter runs on the kernel stack with collection on, bank one and the
// kernel GP. It prints the immediate and the bundle and stops this CPU.
// Local: only the entry branches here, with a displacement resolved by the
// assembler.
.align 16
.type AsteriaIa64SystemCallUnknownBreak, @function
AsteriaIa64SystemCallUnknownBreak:
    cover
    ;;
    invala
    ;;
    mov ar.rsc = r0
    ;;
    mov ar.bspstore = r26
    ;;
    mov ar.rnat = r0
    mov r16 = ASTERIA_IA64_SYSTEM_CALL_KERNEL_RSE_CONFIGURATION
    ;;
    mov ar.rsc = r16
    add r12 = -16, r27
    ;;
    alloc r16 = ar.pfs, 0, 0, 2, 0
    ;;
    mov r32 = r19
    mov r33 = r22
    ;;
    ssm psr.ic
    bsw.1
    ;;
    srlz.d
    ;;
    movl r1 = __gp
    ;;
    br.call.sptk.many b0 = AsteriaIa64FailStopUnknownSystemCallBreak
    ;;
.Lasteria_ia64_system_call_unknown_break_stop:
    hint @pause
    ;;
    br.cond.sptk.few .Lasteria_ia64_system_call_unknown_break_stop
    ;;
.align 16
.size AsteriaIa64SystemCallUnknownBreak, . - AsteriaIa64SystemCallUnknownBreak
