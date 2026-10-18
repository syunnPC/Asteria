// SPDX-License-Identifier: Apache-2.0

// Native capture-only page-fault entry of the runtime IVT page-fault
// trampolines. Page-fault constants and the ar.k3 context,
// Ia64PageFaultEntryContext, Ia64PageFaultEntryControl and Ia64PageFaultFrame
// offsets come from the generated C++ layout; 1 << 61 is the Region 0 limit.

#include "Generated/Platform/Ia64/Ia64PageFaultEntryContractAsmLayout.h"

    .explicit
    #include "Platform/Ia64/Diagnostics/Ia64StopPoll.inc"

.text

    .align 16
    .global AsteriaIa64PageFaultNativeEntry
    .hidden AsteriaIa64PageFaultNativeEntry
    .type AsteriaIa64PageFaultNativeEntry, @function
AsteriaIa64PageFaultNativeEntry:
    AsteriaIa64PollStop ASTERIA_IA64_STOP_PAGE_FAULT_VECTOR, 1
    // Raw IVT contract: r16 is the immutable selector 0 through 3. Hardware
    // selected bank zero, so interrupted bank-one r16-r31 and static r1-r15
    // remain untouched throughout this capture-only checkpoint.
    mov r24 = r16
    mov r31 = pr
    mov r30 = psr
    mov r29 = cr.iip
    mov r28 = cr.ipsr
    mov r27 = cr.ifs
    mov r26 = cr.isr
    mov r25 = cr.ifa
    ;;
    mov r23 = ar.ccv
    mov r17 = 4
    ;;
    cmp.geu p6, p7 = r24, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;

    // Hardware supplies native PL0 bank zero on IVA entry. mov-from-PSR does
    // not expose CPL/IS/BN, so validate only the observable IC/I fields here.
    // Translation and key state are inherited from PL3 and checked below.
    extr.u r16 = r30, 13, 2
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;

    // Admit only the native PL3, bank-one, fully translated and collected
    // subset implemented by the semantic contract.
    extr.u r16 = r28, 32, 2
    mov r17 = 3
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    extr.u r16 = r28, 34, 1
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    extr.u r16 = r28, 44, 1
    mov r17 = 1
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    movl r17 = 0x0000001008022000
    ;;
    and r16 = r28, r17
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    movl r17 = 0x0000001008028000
    ;;
    xor r16 = r30, r28
    ;;
    and r16 = r16, r17
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;

    // Key-permission faults require the interrupted protection-key enable.
    mov r17 = 1
    ;;
    cmp.ne p6, p7 = r24, r17
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_page_fault_key_validated
    ;;
    extr.u r16 = r28, 15, 1
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
.Lasteria_ia64_page_fault_key_validated:

    and r16 = 15, r29
    extr.u r17 = r28, 41, 2
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    mov r16 = 3
    ;;
    cmp.eq p6, p7 = r17, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;

    // ISR.ni and ISR.ir make cover unsafe. Neither RSE state nor user backing
    // store memory is touched by this checkpoint, but reject them before
    // reserving ownership so the later continuation cannot weaken the rule.
    movl r17 = 0x000000c000000000
    ;;
    and r16 = r26, r17
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;

    // Offset 32 of ar.k3 release-publishes the exact retained page-fault
    // context. Revalidate its four identities before selecting one by the raw
    // trampoline ordinal.
    mov r22 = ar.k3
    ;;
    cmp.eq p6, p7 = r22, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    and r16 = ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_ALIGNMENT_MASK, r22
    movl r17 = (1 << 61) - ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_SIZE
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    cmp.gtu p6, p7 = r22, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    add r16 = ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_PROCESSOR_INDEX, r22
    ;;
    ld8 r21 = [r16]
    ;;
    add r16 = ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_PAGE_FAULT_ENTRY_CONTEXT_ADDRESS, r22
    ;;
    ld8.acq r20 = [r16]
    ;;
    cmp.eq p6, p7 = r20, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    and r16 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTEXT_ALIGNMENT_MASK, r20
    movl r17 = (1 << 61) - ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTEXT_SIZE
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    cmp.gtu p6, p7 = r20, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    add r16 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTEXT_PROCESSOR_INDEX, r20
    ;;
    ld8 r19 = [r16]
    ;;
    cmp.ne p6, p7 = r19, r21
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    mov r16 = ASTERIA_IA64_PAGE_FAULT_MAXIMUM_PROCESSOR_COUNT
    ;;
    cmp.geu p6, p7 = r19, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;

    add r17 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTEXT_PAGE_NOT_PRESENT_VECTOR_OFFSET, r20
    ;;
    ld8 r16 = [r17], 8
    ;;
    movl r18 = ASTERIA_IA64_PAGE_FAULT_PAGE_NOT_PRESENT_VECTOR_OFFSET
    ;;
    cmp.ne p6, p7 = r16, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17], 8
    ;;
    movl r18 = ASTERIA_IA64_PAGE_FAULT_KEY_PERMISSION_VECTOR_OFFSET
    ;;
    cmp.ne p6, p7 = r16, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17], 8
    ;;
    movl r18 = ASTERIA_IA64_PAGE_FAULT_INSTRUCTION_ACCESS_RIGHTS_VECTOR_OFFSET
    ;;
    cmp.ne p6, p7 = r16, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17], 8
    ;;
    movl r18 = ASTERIA_IA64_PAGE_FAULT_DATA_ACCESS_RIGHTS_VECTOR_OFFSET
    ;;
    cmp.ne p6, p7 = r16, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17], 8
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17]
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;

    shladd r17 = r24, 3, r20
    ;;
    adds r17 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTEXT_PAGE_NOT_PRESENT_VECTOR_OFFSET, r17
    ;;
    ld8 r24 = [r17]
    ld8 r18 = [r20]
    ;;

    // The published control is immutable while Configured. Keep its address
    // in r21 and the exact frame in r20 through the reservation and stores.
    mov r21 = r18
    ;;
    cmp.eq p6, p7 = r21, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    and r16 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_ALIGNMENT_MASK, r21
    movl r17 = (1 << 61) - ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_SIZE
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    cmp.gtu p6, p7 = r21, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8.acq r16 = [r21]
    ;;
    mov r17 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_STATE_CONFIGURED
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    add r16 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_PROCESSOR_INDEX, r21
    ;;
    ld8 r17 = [r16], 8
    ;;
    cmp.ne p6, p7 = r17, r19
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r20 = [r16]
    ;;
    cmp.eq p6, p7 = r20, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    and r16 = ASTERIA_IA64_PAGE_FAULT_FRAME_ALIGNMENT_MASK, r20
    movl r17 = (1 << 61) - ASTERIA_IA64_PAGE_FAULT_FRAME_SIZE
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    cmp.gtu p6, p7 = r20, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;

    // The one-shot publisher admitted an exact pristine frame. Recheck the
    // atomic publication word and every ownership/generation header before
    // reserving the control; retained payload is immutable under that owner.
    adds r17 = ASTERIA_IA64_PAGE_FAULT_FRAME_PUBLISHED_TOKEN, r20
    ;;
    ld8.acq r16 = [r17], 8
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17], 8
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17], 8
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17], 8
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17]
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;

    // Configured fields already passed full publication validation. Recheck
    // the one-shot generation/correlation words used by this raw claim.
    add r17 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_ENTRY_CAPTURE_TOKEN, r21
    ;;
    ld8 r16 = [r17], 8
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17], 8
    mov r18 = 1
    ;;
    cmp.ne p6, p7 = r16, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17], 8
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    ld8 r16 = [r17]
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;

    // Reserve exactly once without losing the interrupted CCV. Claiming is
    // private initialization; only the release store to Capturing exposes the
    // correlation and retained interruption tuple.
    mov r18 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_STATE_CONFIGURED
    mov r19 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_STATE_CLAIMING
    ;;
    mov ar.ccv = r18
    ;;
    cmpxchg8.acq r16 = [r21], r19, ar.ccv
    ;;
    mov ar.ccv = r23
    ;;
    cmp.ne p6, p7 = r16, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_page_fault_fail_stop
    ;;
    mf
    ;;

    // Begin generation one and retain the complete interruption tuple that is
    // stable before cover. PublishedToken and every later capture field remain
    // zero; static, banked, floating, RSE, BSPSTORE, and RNAT state is untouched.
    add r17 = ASTERIA_IA64_PAGE_FAULT_FRAME_CAPTURE_TOKEN, r20
    mov r16 = 1
    mov r18 = 2
    ;;
    st8 [r17] = r16, 8
    ;;
    st8 [r17] = r18, 8
    ;;
    st8 [r17] = r16, 8
    ;;
    st8 [r17] = r16, 8
    ;;
    st8 [r17] = r26, 8
    ;;
    st8 [r17] = r24, 8
    ;;
    st8 [r17] = r30, 8
    ;;
    st8 [r17] = r29, 8
    ;;
    st8 [r17] = r28, 8
    ;;
    st8 [r17] = r27, 8
    ;;
    st8 [r17] = r0, 8
    ;;
    st8 [r17] = r26, 8
    ;;
    st8 [r17] = r25, 8
    ;;
    mov r16 = cr.itir
    ;;
    st8 [r17] = r16, 8
    ;;
    mov r16 = cr.iipa
    ;;
    st8 [r17] = r16, 8
    ;;
    mov r16 = cr.iim
    ;;
    st8 [r17] = r16
    ;;
    add r17 = ASTERIA_IA64_PAGE_FAULT_FRAME_CAPTURE_STAGE, r20
    mov r16 = ASTERIA_IA64_PAGE_FAULT_CAPTURE_STAGE_INTERRUPTION_STATE
    ;;
    st8 [r17] = r16
    ;;

    add r17 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_ENTRY_CAPTURE_TOKEN, r21
    mov r16 = 1
    mov r18 = 2
    ;;
    st8 [r17] = r16, 8
    ;;
    st8 [r17] = r18, 8
    ;;
    st8 [r17] = r16, 8
    ;;
    st8 [r17] = r24
    ;;
    mov r16 = ASTERIA_IA64_PAGE_FAULT_ENTRY_CONTROL_STATE_CAPTURING
    ;;
    st8.rel [r21] = r16
    ;;

    // Capture stops here without spilling the remaining image, rebasing the
    // user RBS, or publishing Retained/Coordinator.
.Lasteria_ia64_page_fault_capture_checkpoint:
    hint @pause
    ;;
    br.cond.sptk.few .Lasteria_ia64_page_fault_capture_checkpoint
    ;;

.Lasteria_ia64_page_fault_fail_stop:
    hint @pause
    ;;
    br.sptk.many AsteriaIa64StopPageFault
    ;;
    .align 16
    .size AsteriaIa64PageFaultNativeEntry, . - AsteriaIa64PageFaultNativeEntry
