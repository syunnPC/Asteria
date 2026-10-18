// SPDX-License-Identifier: Apache-2.0

// IA-64 user-fault entry, the escape of the alternate data-TLB refill handler
// for a PL3 access its ledger does not map, the user exception entry of the
// General Exception vector, and the PL3 interrupt path of the
// external-interrupt dispatcher: the claim of an interrupt taken at PL3 and
// the user interrupt entry. The entries expand one body: admission, the
// complete capture of the interrupted PL3 state onto the running thread's
// fault frame, the register-stack switch onto the thread's entry storage, the
// C++ bridge call and the complete restore, returning with rfi to the
// interrupted instruction slot. Frame, context and root offsets and the
// admission and claim constants come from the generated C++ layout.

#include "Generated/Platform/Ia64/Ia64UserFaultEntryAsmLayout.h"

// Post-incremented loads and stores below walk consecutive words.
#if (ASTERIA_IA64_USER_FAULT_CONTEXT_MEMORY_STACK_START != ASTERIA_IA64_USER_FAULT_CONTEXT_FRAME_ADDRESS + 8) \
    || (ASTERIA_IA64_USER_FAULT_CONTEXT_MEMORY_STACK_END != ASTERIA_IA64_USER_FAULT_CONTEXT_FRAME_ADDRESS + 16) \
    || (ASTERIA_IA64_USER_FAULT_CONTEXT_RSE_BACKING_STORE_START != ASTERIA_IA64_USER_FAULT_CONTEXT_FRAME_ADDRESS + 24) \
    || (ASTERIA_IA64_USER_FAULT_CONTEXT_RSE_BACKING_STORE_END != ASTERIA_IA64_USER_FAULT_CONTEXT_FRAME_ADDRESS + 32) \
    || (ASTERIA_IA64_USER_FAULT_CONTEXT_HANDLER_ADDRESS != ASTERIA_IA64_USER_FAULT_CONTEXT_FRAME_ADDRESS + 40) \
    || (ASTERIA_IA64_USER_FAULT_CONTEXT_PROCESSOR_INDEX != ASTERIA_IA64_USER_FAULT_CONTEXT_FRAME_ADDRESS + 48) \
    || (ASTERIA_IA64_USER_FAULT_CONTEXT_RESERVED != ASTERIA_IA64_USER_FAULT_CONTEXT_FRAME_ADDRESS + 56)
#error "the context loads assume eight consecutive words"
#endif
#if (ASTERIA_IA64_USER_FAULT_FRAME_RSE_NAT_COLLECTION != ASTERIA_IA64_USER_FAULT_FRAME_RSE_BACKING_STORE_STORE_POINTER + 8) \
    || (ASTERIA_IA64_USER_FAULT_FRAME_RSE_DIRTY_PARTITION_LOAD_CONFIGURATION != ASTERIA_IA64_USER_FAULT_FRAME_RSE_NAT_COLLECTION + 8)
#error "the RSE restore assumes consecutive BSPSTORE, RNAT and load-configuration words"
#endif
#if (ASTERIA_IA64_USER_FAULT_FRAME_EPILOG_COUNTER != ASTERIA_IA64_USER_FAULT_FRAME_LOOP_COUNTER + 8) \
    || (ASTERIA_IA64_USER_FAULT_FRAME_COMPARE_AND_STORE_DATA != ASTERIA_IA64_USER_FAULT_FRAME_LOOP_COUNTER + 16) \
    || (ASTERIA_IA64_USER_FAULT_FRAME_COMPARE_EXCHANGE_VALUE != ASTERIA_IA64_USER_FAULT_FRAME_LOOP_COUNTER + 24) \
    || (ASTERIA_IA64_USER_FAULT_FRAME_FLOATING_POINT_STATUS != ASTERIA_IA64_USER_FAULT_FRAME_LOOP_COUNTER + 32)
#error "the application-register restore assumes consecutive LC, EC, CSD, CCV and FPSR words"
#endif
#if (ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTED_PROCESSOR_STATUS != ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTED_INSTRUCTION_POINTER + 8) \
    || (ASTERIA_IA64_USER_FAULT_FRAME_RSE_CONFIGURATION != ASTERIA_IA64_USER_FAULT_FRAME_PREVIOUS_FUNCTION_STATE + 8)
#error "the final restore assumes consecutive IIP/IPSR and PFS/RSC words"
#endif
#if ASTERIA_IA64_USER_FAULT_FRAME_STATE_INACTIVE != 0
#error "the final release stores r0 as the Inactive frame state"
#endif
#if ASTERIA_IA64_USER_FAULT_PHYSICAL_STACKED_REGISTERS != 96
#error "the stacked-register scrub clears exactly r32-r127"
#endif

.explicit
#include "Platform/Ia64/Diagnostics/Ia64StopPoll.inc"

.text
.macro AsteriaIa64UserFaultStoreFrame offset, value
    adds r18 = \offset, r22
    ;;
    st8 [r18] = \value
    ;;
.endm
// The body of both entries. An expansion admits only its selector and its
// interrupted-PSR value under its mask, records its vector offset in the
// frame (the bridge chooses the handler by it) and has its own fail-stop.
.macro AsteriaIa64UserCaptureEntry selector, interrupted_mask, interrupted_value, vector_offset, suffix, vector_from_k4=0, vector_from_k5=0
// RSC is PL3-writable. Preserve it in privileged scratch, then freeze the RSE before any
// stacked-register or backing-store access (SDM rev. 1.1 Vol. 1 §3.1.8.2).
    mov r18 = ar.rsc
    ;;
    mov.m ar.k6 = r18
    mov r18 = ASTERIA_IA64_USER_FAULT_USER_RSE_CONFIGURATION
    ;;
    mov ar.rsc = r18
    ;;
    AsteriaIa64PollStop \vector_offset, \vector_from_k4, \vector_from_k5
// Bank zero with PSR.ic and PSR.i clear. r16 holds the selector of the
// caller (the refill handler or the interrupt claim) and PR the interrupted
// predicates; bank-zero r17-r31 are the caller's scratch. Every other
// register is interrupted PL3 state, so until it is captured only bank-zero
// r16-r31, p6 and p7 are written.
// p6/p7 are static predicates; the rotating ones stay untouched.
    mov r31 = pr
.ifc \suffix,exception
// The syscall entry already froze RSC and retained the original in ar.k6.
// Its redirect restores PR and r31 is still the original user PR.
.global AsteriaIa64UserExceptionEntryNormalized
.hidden AsteriaIa64UserExceptionEntryNormalized
AsteriaIa64UserExceptionEntryNormalized:
.endif
    mov r30 = psr
    mov r29 = cr.iip
    mov r28 = cr.ipsr
    mov r27 = cr.isr
    mov r26 = cr.ifa
    mov r25 = cr.ifs
    ;;
    cmp.ne p6, p7 = \selector, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// Current execution is the translated bank-zero interruption context with
// collection, delivery, protection keys and the floating-point disables
// off. The interrupted state is native PL3 bank one, translated, with
// collection on and external interrupts masked at a fault and enabled at an
// interrupt.
    movl r17 = ASTERIA_IA64_USER_FAULT_CURRENT_PROCESSOR_STATUS_MASK
    ;;
    and r18 = r30, r17
    movl r17 = ASTERIA_IA64_USER_FAULT_CURRENT_PROCESSOR_STATUS_VALUE
    ;;
    cmp.ne p6, p7 = r18, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    movl r17 = \interrupted_mask
    ;;
    and r18 = r28, r17
    movl r17 = \interrupted_value
    ;;
    cmp.ne p6, p7 = r18, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// The interruption cleared IFS.v; cover makes the IFS that rfi consumes.
    shr.u r18 = r25, 63
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// IIP names a bundle and IPSR.ri one of its three instruction slots.
    and r18 = 15, r29
    extr.u r17 = r28, 41, 2
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    cmp.eq p6, p7 = 3, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// Nested interruptions do not collect an independent restart tuple. RSE
// mandatory faults and incomplete frames are completed by cover below.
    movl r17 = ASTERIA_IA64_USER_FAULT_REJECTED_INTERRUPTION_STATUS_MASK
    ;;
    and r18 = r27, r17
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// PSR.pk is inherited on interruption (SDM Vol. 2, §5.5). Kernel accesses
// use their pinned translations; restore returns the original IPSR.pk.
    rsm psr.pk
    ;;
    srlz.d
    ;;
// Offset 40 of the immutable ar.k3 root names the running thread's selected
// user-fault context. Subtractive upper bounds admit the complete root and
// context before either is read.
    mov r17 = ar.k3
    ;;
    cmp.eq p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    and r18 = ASTERIA_IA64_USER_FAULT_ROOT_ALIGNMENT_MASK, r17
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    movl r18 = ASTERIA_IA64_USER_FAULT_FIRST_NON_KERNEL_REGION_ADDRESS - ASTERIA_IA64_USER_FAULT_ROOT_SIZE
    ;;
    cmp.gtu p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    adds r18 = ASTERIA_IA64_USER_FAULT_ROOT_PROCESSOR_INDEX, r17
    adds r24 = ASTERIA_IA64_USER_FAULT_ROOT_CONTEXT_ADDRESS, r17
    ;;
    ld8 r23 = [r18]
    ;;
    ld8.acq r24 = [r24]
    ;;
    cmp.eq p6, p7 = r24, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    and r18 = ASTERIA_IA64_USER_FAULT_CONTEXT_ALIGNMENT_MASK, r24
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    movl r18 = ASTERIA_IA64_USER_FAULT_FIRST_NON_KERNEL_REGION_ADDRESS - ASTERIA_IA64_USER_FAULT_CONTEXT_SIZE
    ;;
    cmp.gtu p6, p7 = r24, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// R22 frame, r21/r20 memory stack start/end, r19/r17 RSE backing store start/end, r16 handler, r24
// processor index, r18 Reserved.
    adds r18 = ASTERIA_IA64_USER_FAULT_CONTEXT_FRAME_ADDRESS, r24
    ;;
    ld8 r22 = [r18], 8
    ;;
    ld8 r21 = [r18], 8
    ;;
    ld8 r20 = [r18], 8
    ;;
    ld8 r19 = [r18], 8
    ;;
    ld8 r17 = [r18], 8
    ;;
    ld8 r16 = [r18], 8
    ;;
    ld8 r24 = [r18], 8
    ;;
    ld8 r18 = [r18]
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    cmp.ne p6, p7 = r24, r23
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    mov r18 = ASTERIA_IA64_USER_FAULT_MAXIMUM_PROCESSOR_COUNT
    ;;
    cmp.geu p6, p7 = r24, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// The frame is written while collection is off: an aligned Region-0 frame.
// Admission proved the frame and both storage ranges lie in the DTR-covered
// runtime-object corridor and are pairwise disjoint.
    cmp.eq p6, p7 = r22, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    and r18 = ASTERIA_IA64_USER_FAULT_FRAME_ALIGNMENT_MASK, r22
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    movl r18 = ASTERIA_IA64_USER_FAULT_FIRST_NON_KERNEL_REGION_ADDRESS - ASTERIA_IA64_USER_FAULT_FRAME_SIZE
    ;;
    cmp.gtu p6, p7 = r22, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// The memory stack and the RSE backing store each end on a page in
// Region 0, above a nonzero start, exactly their size away.
    mov r23 = ASTERIA_IA64_USER_FAULT_STORAGE_PAGE_SIZE_BYTES - 1
    cmp.eq p6, p7 = r21, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    and r18 = r23, r20
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    cmp.geu p6, p7 = r21, r20
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    sub r18 = r20, r21
    mov r24 = ASTERIA_IA64_USER_FAULT_MEMORY_STACK_SIZE_BYTES
    ;;
    cmp.ne p6, p7 = r18, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    movl r18 = ASTERIA_IA64_USER_FAULT_FIRST_NON_KERNEL_REGION_ADDRESS
    ;;
    cmp.gtu p6, p7 = r20, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    cmp.eq p6, p7 = r19, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    and r18 = r23, r17
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    cmp.geu p6, p7 = r19, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    sub r18 = r17, r19
    mov r24 = ASTERIA_IA64_USER_FAULT_RSE_BACKING_STORE_SIZE_BYTES
    ;;
    cmp.ne p6, p7 = r18, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    movl r18 = ASTERIA_IA64_USER_FAULT_FIRST_NON_KERNEL_REGION_ADDRESS
    ;;
    cmp.gtu p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// The handler is an aligned Region-0 object the bridge calls.
    cmp.eq p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    and r18 = 7, r16
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    movl r18 = ASTERIA_IA64_USER_FAULT_FIRST_NON_KERNEL_REGION_ADDRESS
    ;;
    cmp.geu p6, p7 = r16, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// Claim the frame: no capture-only publication, State Inactive, then
// release-publish Capturing before any captured word is written.
    adds r18 = ASTERIA_IA64_USER_FAULT_FRAME_PUBLISHED_TOKEN, r22
    ;;
    ld8.acq r18 = [r18]
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    adds r23 = ASTERIA_IA64_USER_FAULT_FRAME_STATE, r22
    ;;
    ld8.acq r18 = [r23]
    ;;
    cmp.ne p6, p7 = ASTERIA_IA64_USER_FAULT_FRAME_STATE_INACTIVE, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    mov r18 = ASTERIA_IA64_USER_FAULT_FRAME_STATE_CAPTURING
    ;;
    st8.rel [r23] = r18
    ;;
// The interruption registers, before cover changes cr.ifs. The exception entry serves three
// vectors; their trampolines left the offset, shifted right, in ar.k4.
.if \vector_from_k4
    mov.m r23 = ar.k4
    ;;
    shl r23 = r23, ASTERIA_IA64_USER_EXCEPTION_VECTOR_SHIFT
.elseif \vector_from_k5
// The interrupt claim left the interrupt vector offset, or that offset with a device vector in its
// low byte, in ar.k5.
    mov.m r23 = ar.k5
.else
    mov r23 = \vector_offset
.endif
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_VECTOR_OFFSET, r23
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_PRE_COVER_INTERRUPTION_STATUS, r27
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_CURRENT_PROCESSOR_STATUS, r30
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTED_INSTRUCTION_POINTER, r29
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTED_PROCESSOR_STATUS, r28
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_HARDWARE_INTERRUPTION_FUNCTION_STATE, r25
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTION_STATUS, r27
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTION_FAULTING_ADDRESS, r26
    mov r23 = cr.itir
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTION_TRANSLATION_INFORMATION, r23
    mov r23 = cr.iipa
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTION_PREVIOUS_INSTRUCTION_POINTER, r23
    mov r23 = cr.iim
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTION_IMMEDIATE, r23
// Every application and branch register. cover changes none of them, but
// the bridge and the threads that run while this one is blocked may.
    mov r23 = ar.pfs
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_PREVIOUS_FUNCTION_STATE, r23
    mov.m r23 = ar.k6
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_RSE_CONFIGURATION, r23
    mov r23 = ar.bsp
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_RSE_BACKING_STORE_POINTER, r23
    mov r23 = ar.unat
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_ORIGINAL_NAT_COLLECTION, r23
    mov r23 = ar.lc
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_LOOP_COUNTER, r23
    mov r23 = ar.ec
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_EPILOG_COUNTER, r23
    mov r23 = ar.csd
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_COMPARE_AND_STORE_DATA, r23
    mov r23 = ar.ccv
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_COMPARE_EXCHANGE_VALUE, r23
    mov r23 = ar.fpsr
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_FLOATING_POINT_STATUS, r23
    mov r23 = b0
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_BRANCH_REGISTERS + 0, r23
    mov r23 = b1
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_BRANCH_REGISTERS + 8, r23
    mov r23 = b2
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_BRANCH_REGISTERS + 16, r23
    mov r23 = b3
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_BRANCH_REGISTERS + 24, r23
    mov r23 = b4
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_BRANCH_REGISTERS + 32, r23
    mov r23 = b5
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_BRANCH_REGISTERS + 40, r23
    mov r23 = b6
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_BRANCH_REGISTERS + 48, r23
    mov r23 = b7
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_BRANCH_REGISTERS + 56, r23
// Static r1-r15 with their NaT bits, through their own spill collection;
// the interrupted ar.unat is already captured.
    mov ar.unat = r0
    adds r18 = ASTERIA_IA64_USER_FAULT_FRAME_GENERAL_REGISTERS_R1, r22
    ;;
    st8.spill [r18] = r1, 8
    ;;
    st8.spill [r18] = r2, 8
    ;;
    st8.spill [r18] = r3, 8
    ;;
    st8.spill [r18] = r4, 8
    ;;
    st8.spill [r18] = r5, 8
    ;;
    st8.spill [r18] = r6, 8
    ;;
    st8.spill [r18] = r7, 8
    ;;
    st8.spill [r18] = r8, 8
    ;;
    st8.spill [r18] = r9, 8
    ;;
    st8.spill [r18] = r10, 8
    ;;
    st8.spill [r18] = r11, 8
    ;;
    st8.spill [r18] = r12, 8
    ;;
    st8.spill [r18] = r13, 8
    ;;
    st8.spill [r18] = r14, 8
    ;;
    st8.spill [r18] = r15
    ;;
    mov r23 = ar.unat
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_STATIC_GENERAL_REGISTER_SPILL_NAT_COLLECTION, r23
// Bank-one r16-r31 with their NaT bits, through a separate collection. The
// captured static r14/r15 now carry the spill pointer and the frame.
    adds r14 = ASTERIA_IA64_USER_FAULT_FRAME_GENERAL_REGISTERS_R16, r22
    mov r15 = r22
    mov ar.unat = r0
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
    st8.spill [r14] = r31
    ;;
    mov r16 = ar.unat
    adds r17 = ASTERIA_IA64_USER_FAULT_FRAME_BANKED_GENERAL_REGISTER_SPILL_NAT_COLLECTION, r15
    ;;
    st8 [r17] = r16
    ;;
    bsw.0
    ;;
    mov ar.unat = r0
    ;;
// Intel SDM Vol. 2 section 6.11.1: cover while the interrupted CFM is current and before BSPSTORE
// is switched. Capture every user CFM; the covered IFS header must be valid with its reserved bits
// clear. Invalid PL3-writable frame/rename fields become a thread Exception in the decoder. cover
// also resets the rename bases, so from here the rotating floating and predicate registers are
// addressed physically, as the restore addresses them.
    cover
    ;;
    mov r23 = cr.ifs
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_COVERED_INTERRUPTION_FUNCTION_STATE, r23
    shr.u r18 = r23, 38
    movl r24 = 0x2000000
    ;;
    cmp.ne p6, p7 = r18, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    mov r24 = ar.bsp
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_COVERED_RSE_BACKING_STORE_POINTER, r24
// The frame's PR is p1-p15 as sampled on entry and the rotating p16-p63 as
// read now, with CFM.rrb.pr zero. The restore writes PR with the same zero
// rename base, so the image is exact whether or not mov-to/from-PR renames.
    mov r24 = pr
    ;;
    dep r31 = r31, r24, 0, 16
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_PREDICATES, r31
// F2-f127 with CFM.rrb.fr zero: no floating register is used before.
    adds r14 = ASTERIA_IA64_USER_FAULT_FRAME_FLOATING_REGISTERS, r22
    ;;
    stf.spill [r14] = f2, 16
    ;;
    stf.spill [r14] = f3, 16
    ;;
    stf.spill [r14] = f4, 16
    ;;
    stf.spill [r14] = f5, 16
    ;;
    stf.spill [r14] = f6, 16
    ;;
    stf.spill [r14] = f7, 16
    ;;
    stf.spill [r14] = f8, 16
    ;;
    stf.spill [r14] = f9, 16
    ;;
    stf.spill [r14] = f10, 16
    ;;
    stf.spill [r14] = f11, 16
    ;;
    stf.spill [r14] = f12, 16
    ;;
    stf.spill [r14] = f13, 16
    ;;
    stf.spill [r14] = f14, 16
    ;;
    stf.spill [r14] = f15, 16
    ;;
    stf.spill [r14] = f16, 16
    ;;
    stf.spill [r14] = f17, 16
    ;;
    stf.spill [r14] = f18, 16
    ;;
    stf.spill [r14] = f19, 16
    ;;
    stf.spill [r14] = f20, 16
    ;;
    stf.spill [r14] = f21, 16
    ;;
    stf.spill [r14] = f22, 16
    ;;
    stf.spill [r14] = f23, 16
    ;;
    stf.spill [r14] = f24, 16
    ;;
    stf.spill [r14] = f25, 16
    ;;
    stf.spill [r14] = f26, 16
    ;;
    stf.spill [r14] = f27, 16
    ;;
    stf.spill [r14] = f28, 16
    ;;
    stf.spill [r14] = f29, 16
    ;;
    stf.spill [r14] = f30, 16
    ;;
    stf.spill [r14] = f31, 16
    ;;
    stf.spill [r14] = f32, 16
    ;;
    stf.spill [r14] = f33, 16
    ;;
    stf.spill [r14] = f34, 16
    ;;
    stf.spill [r14] = f35, 16
    ;;
    stf.spill [r14] = f36, 16
    ;;
    stf.spill [r14] = f37, 16
    ;;
    stf.spill [r14] = f38, 16
    ;;
    stf.spill [r14] = f39, 16
    ;;
    stf.spill [r14] = f40, 16
    ;;
    stf.spill [r14] = f41, 16
    ;;
    stf.spill [r14] = f42, 16
    ;;
    stf.spill [r14] = f43, 16
    ;;
    stf.spill [r14] = f44, 16
    ;;
    stf.spill [r14] = f45, 16
    ;;
    stf.spill [r14] = f46, 16
    ;;
    stf.spill [r14] = f47, 16
    ;;
    stf.spill [r14] = f48, 16
    ;;
    stf.spill [r14] = f49, 16
    ;;
    stf.spill [r14] = f50, 16
    ;;
    stf.spill [r14] = f51, 16
    ;;
    stf.spill [r14] = f52, 16
    ;;
    stf.spill [r14] = f53, 16
    ;;
    stf.spill [r14] = f54, 16
    ;;
    stf.spill [r14] = f55, 16
    ;;
    stf.spill [r14] = f56, 16
    ;;
    stf.spill [r14] = f57, 16
    ;;
    stf.spill [r14] = f58, 16
    ;;
    stf.spill [r14] = f59, 16
    ;;
    stf.spill [r14] = f60, 16
    ;;
    stf.spill [r14] = f61, 16
    ;;
    stf.spill [r14] = f62, 16
    ;;
    stf.spill [r14] = f63, 16
    ;;
    stf.spill [r14] = f64, 16
    ;;
    stf.spill [r14] = f65, 16
    ;;
    stf.spill [r14] = f66, 16
    ;;
    stf.spill [r14] = f67, 16
    ;;
    stf.spill [r14] = f68, 16
    ;;
    stf.spill [r14] = f69, 16
    ;;
    stf.spill [r14] = f70, 16
    ;;
    stf.spill [r14] = f71, 16
    ;;
    stf.spill [r14] = f72, 16
    ;;
    stf.spill [r14] = f73, 16
    ;;
    stf.spill [r14] = f74, 16
    ;;
    stf.spill [r14] = f75, 16
    ;;
    stf.spill [r14] = f76, 16
    ;;
    stf.spill [r14] = f77, 16
    ;;
    stf.spill [r14] = f78, 16
    ;;
    stf.spill [r14] = f79, 16
    ;;
    stf.spill [r14] = f80, 16
    ;;
    stf.spill [r14] = f81, 16
    ;;
    stf.spill [r14] = f82, 16
    ;;
    stf.spill [r14] = f83, 16
    ;;
    stf.spill [r14] = f84, 16
    ;;
    stf.spill [r14] = f85, 16
    ;;
    stf.spill [r14] = f86, 16
    ;;
    stf.spill [r14] = f87, 16
    ;;
    stf.spill [r14] = f88, 16
    ;;
    stf.spill [r14] = f89, 16
    ;;
    stf.spill [r14] = f90, 16
    ;;
    stf.spill [r14] = f91, 16
    ;;
    stf.spill [r14] = f92, 16
    ;;
    stf.spill [r14] = f93, 16
    ;;
    stf.spill [r14] = f94, 16
    ;;
    stf.spill [r14] = f95, 16
    ;;
    stf.spill [r14] = f96, 16
    ;;
    stf.spill [r14] = f97, 16
    ;;
    stf.spill [r14] = f98, 16
    ;;
    stf.spill [r14] = f99, 16
    ;;
    stf.spill [r14] = f100, 16
    ;;
    stf.spill [r14] = f101, 16
    ;;
    stf.spill [r14] = f102, 16
    ;;
    stf.spill [r14] = f103, 16
    ;;
    stf.spill [r14] = f104, 16
    ;;
    stf.spill [r14] = f105, 16
    ;;
    stf.spill [r14] = f106, 16
    ;;
    stf.spill [r14] = f107, 16
    ;;
    stf.spill [r14] = f108, 16
    ;;
    stf.spill [r14] = f109, 16
    ;;
    stf.spill [r14] = f110, 16
    ;;
    stf.spill [r14] = f111, 16
    ;;
    stf.spill [r14] = f112, 16
    ;;
    stf.spill [r14] = f113, 16
    ;;
    stf.spill [r14] = f114, 16
    ;;
    stf.spill [r14] = f115, 16
    ;;
    stf.spill [r14] = f116, 16
    ;;
    stf.spill [r14] = f117, 16
    ;;
    stf.spill [r14] = f118, 16
    ;;
    stf.spill [r14] = f119, 16
    ;;
    stf.spill [r14] = f120, 16
    ;;
    stf.spill [r14] = f121, 16
    ;;
    stf.spill [r14] = f122, 16
    ;;
    stf.spill [r14] = f123, 16
    ;;
    stf.spill [r14] = f124, 16
    ;;
    stf.spill [r14] = f125, 16
    ;;
    stf.spill [r14] = f126, 16
    ;;
    stf.spill [r14] = f127
    ;;
// With the interrupted frame covered, enforced-lazy mode freezes its dirty
// partition while BSPSTORE and RNAT are captured and rebound to the thread's
// RSE backing store. Writing BSPSTORE preserves that partition and makes
// the new BSP distance the exact loadrs quantity of section 6.11.2.
    invala
    ;;
    mov ar.rsc = r0
    ;;
    mov r23 = ar.bspstore
    mov r24 = ar.rnat
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_RSE_BACKING_STORE_STORE_POINTER, r23
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_RSE_NAT_COLLECTION, r24
    mov ar.bspstore = r19
    ;;
    mov ar.rnat = r0
    ;;
    mov r23 = ar.bsp
    ;;
    cmp.ltu p6, p7 = r23, r19
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    sub r23 = r23, r19
    ;;
    and r24 = 7, r23
    ;;
    cmp.ne p6, p7 = r24, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    mov r24 = ASTERIA_IA64_USER_FAULT_RSE_TRANSITION_SIZE_BYTES
    ;;
    cmp.geu p6, p7 = r23, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    shl r23 = r23, 16
    ;;
    AsteriaIa64UserFaultStoreFrame ASTERIA_IA64_USER_FAULT_FRAME_RSE_DIRTY_PARTITION_LOAD_CONFIGURATION, r23
    mov r24 = ASTERIA_IA64_USER_FAULT_KERNEL_RSE_CONFIGURATION
    ;;
    mov ar.rsc = r24
    ;;
    adds r12 = -16, r20
    ;;
    mf
    ;;
// R4 carries the frame across the P64 bridge, out0/out1 pass the frame and the handler. Bank zero
// is disposable once collection is on, so a recoverable refill may use its normal heavyweight bank.
    alloc r24 = ar.pfs, 0, 0, 2, 0
    ;;
    mov r4 = r22
    ;;
    mov r32 = r4
    mov r33 = r16
    ;;
    ssm psr.ic
    bsw.1
    ;;
    srlz.d
    ;;
// The kernel computes under its own fpsr, all traps disabled, never the user's: the user's was
// captured with the frame and is restored on the way out.
    movl r1 = __gp
    movl r14 = ASTERIA_IA64_USER_FAULT_KERNEL_FLOATING_POINT_STATUS
    ;;
    mov ar.fpsr = r14
    ;;
    br.call.sptk.many b0 = AsteriaIa64UserFaultBridge
    ;;
    cmp.ne p6, p7 = r8, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
    mf
    ;;
    adds r14 = ASTERIA_IA64_USER_FAULT_FRAME_STATE, r4
    ;;
    ld8.acq r15 = [r14]
    ;;
    cmp.ne p6, p7 = ASTERIA_IA64_USER_FAULT_FRAME_STATE_RESUME_COMMITTED, r15
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// Select enforced-lazy mode with the exact dirty-partition load distance.
// Every physical stacked register outside that partition was last written
// by the kernel, so scrub the whole file before PL3 can allocate one: grow
// the frame to all of them (PAL_RSE_INFO proved there are exactly as many
// as one frame can hold; mandatory spills first write any still-resident
// part of the interrupted partition to the thread's backing store), clear
// each value and NaT bit and drop the frame. loadrs then reloads exactly the
// interrupted dirty partition and invalidates every other stacked register,
// which holds zero. BSPSTORE and RNAT are restored afterwards in the
// architected order, and BSP must converge on the post-cover value.
    adds r15 = ASTERIA_IA64_USER_FAULT_FRAME_RSE_BACKING_STORE_STORE_POINTER, r4
    ;;
    ld8 r17 = [r15], 8
    ;;
    ld8 r18 = [r15], 8
    ;;
    ld8 r16 = [r15]
    ;;
    mov ar.rsc = r16
    ;;
    alloc r22 = ar.pfs, 0, ASTERIA_IA64_USER_FAULT_PHYSICAL_STACKED_REGISTERS, 0, 0
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
    mov r16 = ar.bsp
    adds r15 = ASTERIA_IA64_USER_FAULT_FRAME_COVERED_RSE_BACKING_STORE_POINTER, r4
    ;;
    ld8 r17 = [r15]
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_\suffix\()_fail_stop
    ;;
// F2-f127 with CFM.rrb.fr zero, as they were spilled.
    adds r14 = ASTERIA_IA64_USER_FAULT_FRAME_FLOATING_REGISTERS, r4
    ;;
    ldf.fill f2 = [r14], 16
    ;;
    ldf.fill f3 = [r14], 16
    ;;
    ldf.fill f4 = [r14], 16
    ;;
    ldf.fill f5 = [r14], 16
    ;;
    ldf.fill f6 = [r14], 16
    ;;
    ldf.fill f7 = [r14], 16
    ;;
    ldf.fill f8 = [r14], 16
    ;;
    ldf.fill f9 = [r14], 16
    ;;
    ldf.fill f10 = [r14], 16
    ;;
    ldf.fill f11 = [r14], 16
    ;;
    ldf.fill f12 = [r14], 16
    ;;
    ldf.fill f13 = [r14], 16
    ;;
    ldf.fill f14 = [r14], 16
    ;;
    ldf.fill f15 = [r14], 16
    ;;
    ldf.fill f16 = [r14], 16
    ;;
    ldf.fill f17 = [r14], 16
    ;;
    ldf.fill f18 = [r14], 16
    ;;
    ldf.fill f19 = [r14], 16
    ;;
    ldf.fill f20 = [r14], 16
    ;;
    ldf.fill f21 = [r14], 16
    ;;
    ldf.fill f22 = [r14], 16
    ;;
    ldf.fill f23 = [r14], 16
    ;;
    ldf.fill f24 = [r14], 16
    ;;
    ldf.fill f25 = [r14], 16
    ;;
    ldf.fill f26 = [r14], 16
    ;;
    ldf.fill f27 = [r14], 16
    ;;
    ldf.fill f28 = [r14], 16
    ;;
    ldf.fill f29 = [r14], 16
    ;;
    ldf.fill f30 = [r14], 16
    ;;
    ldf.fill f31 = [r14], 16
    ;;
    ldf.fill f32 = [r14], 16
    ;;
    ldf.fill f33 = [r14], 16
    ;;
    ldf.fill f34 = [r14], 16
    ;;
    ldf.fill f35 = [r14], 16
    ;;
    ldf.fill f36 = [r14], 16
    ;;
    ldf.fill f37 = [r14], 16
    ;;
    ldf.fill f38 = [r14], 16
    ;;
    ldf.fill f39 = [r14], 16
    ;;
    ldf.fill f40 = [r14], 16
    ;;
    ldf.fill f41 = [r14], 16
    ;;
    ldf.fill f42 = [r14], 16
    ;;
    ldf.fill f43 = [r14], 16
    ;;
    ldf.fill f44 = [r14], 16
    ;;
    ldf.fill f45 = [r14], 16
    ;;
    ldf.fill f46 = [r14], 16
    ;;
    ldf.fill f47 = [r14], 16
    ;;
    ldf.fill f48 = [r14], 16
    ;;
    ldf.fill f49 = [r14], 16
    ;;
    ldf.fill f50 = [r14], 16
    ;;
    ldf.fill f51 = [r14], 16
    ;;
    ldf.fill f52 = [r14], 16
    ;;
    ldf.fill f53 = [r14], 16
    ;;
    ldf.fill f54 = [r14], 16
    ;;
    ldf.fill f55 = [r14], 16
    ;;
    ldf.fill f56 = [r14], 16
    ;;
    ldf.fill f57 = [r14], 16
    ;;
    ldf.fill f58 = [r14], 16
    ;;
    ldf.fill f59 = [r14], 16
    ;;
    ldf.fill f60 = [r14], 16
    ;;
    ldf.fill f61 = [r14], 16
    ;;
    ldf.fill f62 = [r14], 16
    ;;
    ldf.fill f63 = [r14], 16
    ;;
    ldf.fill f64 = [r14], 16
    ;;
    ldf.fill f65 = [r14], 16
    ;;
    ldf.fill f66 = [r14], 16
    ;;
    ldf.fill f67 = [r14], 16
    ;;
    ldf.fill f68 = [r14], 16
    ;;
    ldf.fill f69 = [r14], 16
    ;;
    ldf.fill f70 = [r14], 16
    ;;
    ldf.fill f71 = [r14], 16
    ;;
    ldf.fill f72 = [r14], 16
    ;;
    ldf.fill f73 = [r14], 16
    ;;
    ldf.fill f74 = [r14], 16
    ;;
    ldf.fill f75 = [r14], 16
    ;;
    ldf.fill f76 = [r14], 16
    ;;
    ldf.fill f77 = [r14], 16
    ;;
    ldf.fill f78 = [r14], 16
    ;;
    ldf.fill f79 = [r14], 16
    ;;
    ldf.fill f80 = [r14], 16
    ;;
    ldf.fill f81 = [r14], 16
    ;;
    ldf.fill f82 = [r14], 16
    ;;
    ldf.fill f83 = [r14], 16
    ;;
    ldf.fill f84 = [r14], 16
    ;;
    ldf.fill f85 = [r14], 16
    ;;
    ldf.fill f86 = [r14], 16
    ;;
    ldf.fill f87 = [r14], 16
    ;;
    ldf.fill f88 = [r14], 16
    ;;
    ldf.fill f89 = [r14], 16
    ;;
    ldf.fill f90 = [r14], 16
    ;;
    ldf.fill f91 = [r14], 16
    ;;
    ldf.fill f92 = [r14], 16
    ;;
    ldf.fill f93 = [r14], 16
    ;;
    ldf.fill f94 = [r14], 16
    ;;
    ldf.fill f95 = [r14], 16
    ;;
    ldf.fill f96 = [r14], 16
    ;;
    ldf.fill f97 = [r14], 16
    ;;
    ldf.fill f98 = [r14], 16
    ;;
    ldf.fill f99 = [r14], 16
    ;;
    ldf.fill f100 = [r14], 16
    ;;
    ldf.fill f101 = [r14], 16
    ;;
    ldf.fill f102 = [r14], 16
    ;;
    ldf.fill f103 = [r14], 16
    ;;
    ldf.fill f104 = [r14], 16
    ;;
    ldf.fill f105 = [r14], 16
    ;;
    ldf.fill f106 = [r14], 16
    ;;
    ldf.fill f107 = [r14], 16
    ;;
    ldf.fill f108 = [r14], 16
    ;;
    ldf.fill f109 = [r14], 16
    ;;
    ldf.fill f110 = [r14], 16
    ;;
    ldf.fill f111 = [r14], 16
    ;;
    ldf.fill f112 = [r14], 16
    ;;
    ldf.fill f113 = [r14], 16
    ;;
    ldf.fill f114 = [r14], 16
    ;;
    ldf.fill f115 = [r14], 16
    ;;
    ldf.fill f116 = [r14], 16
    ;;
    ldf.fill f117 = [r14], 16
    ;;
    ldf.fill f118 = [r14], 16
    ;;
    ldf.fill f119 = [r14], 16
    ;;
    ldf.fill f120 = [r14], 16
    ;;
    ldf.fill f121 = [r14], 16
    ;;
    ldf.fill f122 = [r14], 16
    ;;
    ldf.fill f123 = [r14], 16
    ;;
    ldf.fill f124 = [r14], 16
    ;;
    ldf.fill f125 = [r14], 16
    ;;
    ldf.fill f126 = [r14], 16
    ;;
    ldf.fill f127 = [r14]
    ;;
    adds r14 = ASTERIA_IA64_USER_FAULT_FRAME_LOOP_COUNTER, r4
    ;;
    ld8 r15 = [r14], 8
    ;;
    ld8 r16 = [r14], 8
    ;;
    ld8 r17 = [r14], 8
    ;;
    ld8 r18 = [r14], 8
    ;;
    ld8 r19 = [r14]
    ;;
    mov ar.lc = r15
    mov ar.ec = r16
    ;;
    mov ar.csd = r17
    ;;
    mov ar.ccv = r18
    mov ar.fpsr = r19
    ;;
    adds r14 = ASTERIA_IA64_USER_FAULT_FRAME_BRANCH_REGISTERS, r4
    ;;
    ld8 r15 = [r14], 8
    ;;
    ld8 r16 = [r14], 8
    ;;
    ld8 r17 = [r14], 8
    ;;
    ld8 r18 = [r14], 8
    ;;
    ld8 r19 = [r14], 8
    ;;
    ld8 r20 = [r14], 8
    ;;
    ld8 r21 = [r14], 8
    ;;
    ld8 r22 = [r14]
    ;;
    mov b0 = r15
    mov b1 = r16
    ;;
    mov b2 = r17
    mov b3 = r18
    ;;
    mov b4 = r19
    mov b5 = r20
    ;;
    mov b6 = r21
    mov b7 = r22
    ;;
// The rest runs with collection off. Every access below is to the frame,
// which admission placed in the DTR-covered corridor, so none can miss, and
// bank-zero r16-r31 hold the final controls undisturbed: the State address,
// IIP, IPSR, covered IFS, PR, PFS, RSC and the interrupted UNAT.
    rsm psr.ic
    ;;
    srlz.d
    ;;
    bsw.0
    ;;
    adds r16 = ASTERIA_IA64_USER_FAULT_FRAME_STATE, r4
    adds r24 = ASTERIA_IA64_USER_FAULT_FRAME_INTERRUPTED_INSTRUCTION_POINTER, r4
    ;;
    ld8 r17 = [r24], 8
    ;;
    ld8 r18 = [r24]
    ;;
    adds r24 = ASTERIA_IA64_USER_FAULT_FRAME_COVERED_INTERRUPTION_FUNCTION_STATE, r4
    ;;
    ld8 r19 = [r24]
    ;;
    adds r24 = ASTERIA_IA64_USER_FAULT_FRAME_PREDICATES, r4
    ;;
    ld8 r20 = [r24]
    ;;
    adds r24 = ASTERIA_IA64_USER_FAULT_FRAME_PREVIOUS_FUNCTION_STATE, r4
    ;;
    ld8 r21 = [r24], 8
    ;;
    ld8 r22 = [r24]
    ;;
    adds r24 = ASTERIA_IA64_USER_FAULT_FRAME_ORIGINAL_NAT_COLLECTION, r4
    ;;
    ld8 r23 = [r24]
    ;;
    bsw.1
    ;;
// Bank one, then the static registers, each from its own collection; the
// spill pointer r15 is filled last.
    adds r14 = ASTERIA_IA64_USER_FAULT_FRAME_BANKED_GENERAL_REGISTER_SPILL_NAT_COLLECTION, r4
    ;;
    ld8 r15 = [r14]
    ;;
    mov ar.unat = r15
    adds r14 = ASTERIA_IA64_USER_FAULT_FRAME_GENERAL_REGISTERS_R16, r4
    ;;
    ld8.fill r16 = [r14], 8
    ;;
    ld8.fill r17 = [r14], 8
    ;;
    ld8.fill r18 = [r14], 8
    ;;
    ld8.fill r19 = [r14], 8
    ;;
    ld8.fill r20 = [r14], 8
    ;;
    ld8.fill r21 = [r14], 8
    ;;
    ld8.fill r22 = [r14], 8
    ;;
    ld8.fill r23 = [r14], 8
    ;;
    ld8.fill r24 = [r14], 8
    ;;
    ld8.fill r25 = [r14], 8
    ;;
    ld8.fill r26 = [r14], 8
    ;;
    ld8.fill r27 = [r14], 8
    ;;
    ld8.fill r28 = [r14], 8
    ;;
    ld8.fill r29 = [r14], 8
    ;;
    ld8.fill r30 = [r14], 8
    ;;
    ld8.fill r31 = [r14]
    ;;
    adds r14 = ASTERIA_IA64_USER_FAULT_FRAME_STATIC_GENERAL_REGISTER_SPILL_NAT_COLLECTION, r4
    ;;
    ld8 r15 = [r14]
    ;;
    mov ar.unat = r15
    ;;
    adds r15 = ASTERIA_IA64_USER_FAULT_FRAME_GENERAL_REGISTERS_R1, r4
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
    ld8.fill r7 = [r15], 8
    ;;
    ld8.fill r8 = [r15], 8
    ;;
    ld8.fill r9 = [r15], 8
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
// The last frame access releases it; nothing after it can fault, and IIP
// and IPSR are the captured ones: the faulting instruction runs again, or
// the interrupted one runs.
    bsw.0
    ;;
    st8.rel [r16] = r0
    ;;
    mov ar.pfs = r21
    ;;
    mov cr.ifs = r19
    ;;
    mov ar.rsc = r22
    ;;
    mov cr.iip = r17
    mov cr.ipsr = r18
    ;;
    mov ar.unat = r23
    ;;
    mov pr = r20, -1
    ;;
    rfi
    ;;
.Lasteria_ia64_user_\suffix\()_fail_stop:
    hint @pause
    ;;
.if \vector_from_k4
    br.sptk.many AsteriaIa64StopException
.elseif \vector_from_k5
    br.sptk.many AsteriaIa64StopExternal
.else
    br.sptk.many AsteriaIa64StopUserFault
.endif
    ;;
.endm

.align 16
.global AsteriaIa64UserFaultEntry
.hidden AsteriaIa64UserFaultEntry
.type AsteriaIa64UserFaultEntry, @function
AsteriaIa64UserFaultEntry:
    AsteriaIa64UserCaptureEntry ASTERIA_IA64_USER_FAULT_SELECTOR, \
        ASTERIA_IA64_USER_FAULT_INTERRUPTED_PROCESSOR_STATUS_MASK, \
        ASTERIA_IA64_USER_FAULT_INTERRUPTED_PROCESSOR_STATUS_VALUE, \
        ASTERIA_IA64_USER_FAULT_VECTOR_OFFSET, fault
.align 16
.size AsteriaIa64UserFaultEntry, . - AsteriaIa64UserFaultEntry

.align 16
.global AsteriaIa64UserExceptionEntry
.hidden AsteriaIa64UserExceptionEntry
.type AsteriaIa64UserExceptionEntry, @function
AsteriaIa64UserExceptionEntry:
    AsteriaIa64UserCaptureEntry ASTERIA_IA64_USER_EXCEPTION_SELECTOR, \
        ASTERIA_IA64_USER_FAULT_INTERRUPTED_PROCESSOR_STATUS_MASK, \
        ASTERIA_IA64_USER_FAULT_INTERRUPTED_PROCESSOR_STATUS_VALUE, \
        ASTERIA_IA64_USER_EXCEPTION_VECTOR_OFFSET, exception, 1
.align 16
.size AsteriaIa64UserExceptionEntry, . - AsteriaIa64UserExceptionEntry

.align 16
.global AsteriaIa64UserInterruptEntry
.hidden AsteriaIa64UserInterruptEntry
.type AsteriaIa64UserInterruptEntry, @function
AsteriaIa64UserInterruptEntry:
    AsteriaIa64UserCaptureEntry ASTERIA_IA64_USER_INTERRUPT_SELECTOR, \
        ASTERIA_IA64_USER_INTERRUPT_INTERRUPTED_PROCESSOR_STATUS_MASK, \
        ASTERIA_IA64_USER_INTERRUPT_INTERRUPTED_PROCESSOR_STATUS_VALUE, \
        ASTERIA_IA64_USER_INTERRUPT_VECTOR_OFFSET, interrupt, 0, 1
.align 16
.size AsteriaIa64UserInterruptEntry, . - AsteriaIa64UserInterruptEntry
.purgem AsteriaIa64UserCaptureEntry
.purgem AsteriaIa64UserFaultStoreFrame

// The external-interrupt dispatcher's continuation for an interrupt taken at PL3. Bank zero with
// PSR.ic and PSR.i clear; the dispatcher restored PR to the interrupted predicates. Until the claim
// leaves, it writes only bank-zero r16-r31, p6, p7 and ar.k5 and accesses no memory. Every path
// that read IVR, except the spurious one, writes EOI exactly once. A device vector (0x40-0x4F,
// Documentation/Architecture/IommuDomain.md ) is relinquished at once, since the I/O SAPIC's Remote
// IRR holds its line until the kernel acknowledges it there, and enters the capture with its vector
// in the low byte of the vector offset ar.k5 carries; any other source that is not the local timer
// stops after masking and reading back the timer.
.align 16
.global AsteriaIa64UserInterruptClaim
.hidden AsteriaIa64UserInterruptClaim
.type AsteriaIa64UserInterruptClaim, @function
AsteriaIa64UserInterruptClaim:
    mov r31 = pr
    ;;
    mov r20 = cr.ivr
    ;;
    srlz.d
    ;;
    mov r18 = ASTERIA_IA64_USER_INTERRUPT_SPURIOUS_VECTOR
    ;;
    cmp.eq p6, p7 = r20, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_interrupt_claim_spurious
    ;;
    mov r18 = ASTERIA_IA64_USER_INTERRUPT_TIMER_VECTOR
    ;;
    cmp.ne p6, p7 = r20, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_interrupt_claim_device
    ;;
    mov r19 = cr.itm
    mov r18 = ar.itc
    ;;
    cmp.eq p6, p7 = r19, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_interrupt_claim_quiesce_fail_stop
    ;;
    movl r17 = -1
    ;;
    cmp.eq p6, p7 = r19, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_interrupt_claim_quiesce_fail_stop
    ;;
// A timer interrupt that pended while delivery was masked, whose deadline
// the kernel has since replaced by a later one, is not due: acknowledge it
// and resume, leaving the armed deadline in cr.itm and cr.itv unmasked.
    cmp.ltu p6, p7 = r18, r19
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_interrupt_claim_not_due
    ;;
// A due tick: mask and read back the timer, then relinquish the vector
// before the capture, so the bridge runs with no source in service.
    movl r16 = ASTERIA_IA64_USER_INTERRUPT_MASKED_TIMER_VECTOR
    ;;
    mov cr.itv = r16
    ;;
    srlz.d
    ;;
    mov r17 = cr.itv
    ;;
    cmp.ne p6, p7 = r17, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_interrupt_claim_eoi_fail_stop
    ;;
    mov cr.eoi = r0
    ;;
    srlz.d
    ;;
    mov r18 = ASTERIA_IA64_USER_INTERRUPT_VECTOR_OFFSET
    ;;
    mov ar.k5 = r18
    ;;
    mov pr = r31, -1
    mov r16 = ASTERIA_IA64_USER_INTERRUPT_SELECTOR
    ;;
    br.sptk.few AsteriaIa64UserInterruptEntry
    ;;
// A device vector: relinquished, then captured with the vector in the low
// byte of the interrupt vector offset; the timer is left as it is.
.Lasteria_ia64_user_interrupt_claim_device:
    mov r18 = ASTERIA_IA64_USER_INTERRUPT_FIRST_DEVICE_VECTOR
    ;;
    cmp.ltu p6, p7 = r20, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_interrupt_claim_quiesce_fail_stop
    ;;
    adds r18 = ASTERIA_IA64_USER_INTERRUPT_DEVICE_VECTOR_COUNT, r18
    ;;
    cmp.geu p6, p7 = r20, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_interrupt_claim_quiesce_fail_stop
    ;;
    mov cr.eoi = r0
    ;;
    srlz.d
    ;;
    mov r18 = ASTERIA_IA64_USER_INTERRUPT_VECTOR_OFFSET
    ;;
    or r18 = r18, r20
    ;;
    mov ar.k5 = r18
    ;;
    mov pr = r31, -1
    mov r16 = ASTERIA_IA64_USER_INTERRUPT_SELECTOR
    ;;
    br.sptk.few AsteriaIa64UserInterruptEntry
    ;;
.Lasteria_ia64_user_interrupt_claim_not_due:
    mov cr.eoi = r0
    ;;
    srlz.d
    ;;
    mov pr = r31, -1
    ;;
    rfi
    ;;
.Lasteria_ia64_user_interrupt_claim_spurious:
    mov pr = r31, -1
    ;;
    rfi
    ;;
// Another source, or a timer vector without a deadline, is fatal; the timer
// is masked and read back before the claimed vector is acknowledged, so no
// timer source remains live in the fail-stop state.
.Lasteria_ia64_user_interrupt_claim_quiesce_fail_stop:
    movl r17 = ASTERIA_IA64_USER_INTERRUPT_MASKED_TIMER_VECTOR
    ;;
    mov cr.itv = r17
    ;;
    srlz.d
    ;;
    mov r18 = cr.itv
    ;;
    cmp.ne p6, p7 = r18, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_interrupt_claim_timer_mask_fail_stop
    ;;
.Lasteria_ia64_user_interrupt_claim_eoi_fail_stop:
    mov cr.eoi = r0
    ;;
    srlz.d
    ;;
.Lasteria_ia64_user_interrupt_claim_fail_stop:
    hint @pause
    ;;
    br.sptk.many AsteriaIa64StopExternal
    ;;
.Lasteria_ia64_user_interrupt_claim_timer_mask_fail_stop:
    mov cr.eoi = r0
    ;;
    srlz.d
    ;;
.Lasteria_ia64_user_interrupt_claim_timer_mask_terminal_fail_stop:
    hint @pause
    ;;
    br.sptk.many AsteriaIa64StopExternal
    ;;
.align 16
.size AsteriaIa64UserInterruptClaim, . - AsteriaIa64UserInterruptClaim

.align 16
.global AsteriaIa64UserBreakEntry
.hidden AsteriaIa64UserBreakEntry
.type AsteriaIa64UserBreakEntry, @function
AsteriaIa64UserBreakEntry:
    mov r31 = pr
    mov r17 = cr.iim
    movl r18 = ASTERIA_IA64_USER_BREAK_SYSTEM_CALL_IMMEDIATE
    ;;
    cmp.eq p6, p7 = r17, r18
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_user_break_system_call
    ;;
    mov pr = r31, -1
    ;;
{ .mib
    mov.m ar.k4 = ASTERIA_IA64_USER_VECTOR_BREAK >> ASTERIA_IA64_USER_EXCEPTION_VECTOR_SHIFT
    mov r16 = ASTERIA_IA64_USER_EXCEPTION_SELECTOR
    br.sptk.many AsteriaIa64UserExceptionEntry
}
    ;;
.Lasteria_ia64_user_break_system_call:
    mov pr = r31, -1
    ;;
    br.sptk.many AsteriaIa64SystemCallEntry
    ;;
.align 16
.size AsteriaIa64UserBreakEntry, . - AsteriaIa64UserBreakEntry
