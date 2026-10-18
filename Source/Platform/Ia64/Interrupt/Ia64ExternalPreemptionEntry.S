// SPDX-License-Identifier: Apache-2.0

// Sole IVT external-interrupt dispatcher, native preemption capture, raw
// restore tail and the preemption code-range getter. Constants and the
// Ia64ExternalPreemptionControl, Ia64ExternalPreemptionFrame and external-
// interrupt entry-input offsets come from the generated C++ layouts.

#include "Generated/Platform/Ia64/Ia64ExternalInterruptEntryAsmLayout.h"
#include "Generated/Platform/Ia64/Ia64ExternalPreemptionEntryAsmLayout.h"

    .explicit
    #include "Platform/Ia64/Diagnostics/Ia64StopPoll.inc"

.text

    .align 16
    .global AsteriaIa64ExternalInterruptDispatchEntry
    .hidden AsteriaIa64ExternalInterruptDispatchEntry
    .type AsteriaIa64ExternalInterruptDispatchEntry, @function
AsteriaIa64ExternalInterruptDispatchEntry:
    AsteriaIa64PollStop ASTERIA_IA64_STOP_EXTERNAL_VECTOR
    // Hardware selected bank zero. Preserve the complete interruption tuple,
    // original PR, and original CCV before selecting any entry path. Static
    // r1-r15 and interrupted bank-one r16-r31 remain untouched.
    mov r31 = pr
    mov r30 = psr
    mov r29 = cr.iip
    mov r28 = cr.ipsr
    mov r27 = cr.ifs
    mov r26 = cr.isr
    mov r20 = ar.ccv
    ;;

    // Validate the IVA-current and interrupted native PL0 states without the
    // legacy IFS.v restriction. Native capture needs the hardware IFS image;
    // the legacy entry repeats these checks and retains its original rejection.
    movl r16 = ASTERIA_IA64_EXTERNAL_INTERRUPT_CURRENT_PROCESSOR_STATUS_MASK
    ;;
    and r17 = r30, r16
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_CURRENT_PROCESSOR_STATUS_VALUE
    ;;
    cmp.ne p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;

    // The current state is every path's. An interrupt taken at PL3 (IPSR.cpl,
    // bits 32-33) continues in the user interrupt claim, whose entry admits
    // the interrupted PL3 state; the checks below are the PL0 paths'.
    extr.u r17 = r28, 32, 2
    ;;
    cmp.eq p6, p7 = 3, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_user
    ;;
    movl r16 = ASTERIA_IA64_EXTERNAL_INTERRUPT_INTERRUPTED_PROCESSOR_STATUS_MASK
    ;;
    and r17 = r28, r16
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_INTERRUPTED_PROCESSOR_STATUS_VALUE
    ;;
    cmp.ne p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    extr.u r17 = r28, 41, 2
    ;;
    mov r18 = 3
    ;;
    cmp.eq p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    extr.u r17 = r26, 39, 1
    and r18 = 15, r29
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;

    // Offset 24 of ar.k3's retained runtime context publishes the immutable
    // external-entry context. Its fourth word release-publishes an optional
    // native control address; zero preserves the exact legacy entry path.
    mov r25 = ar.k3
    ;;
    cmp.eq p6, p7 = r25, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    and r17 = ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_ALIGNMENT_MASK, r25
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_REGION_ZERO_LIMIT - ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_SIZE
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    cmp.gtu p6, p7 = r25, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    add r17 = ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_ADDRESS, r25
    ;;
    ld8.acq r25 = [r17]
    ;;
    cmp.eq p6, p7 = r25, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    and r17 = ASTERIA_IA64_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_ALIGNMENT_MASK, r25
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_REGION_ZERO_LIMIT - ASTERIA_IA64_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_SIZE
    ;;
    cmp.gtu p6, p7 = r25, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    adds r21 = ASTERIA_IA64_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_EVIDENCE_ADDRESS, r25
    ;;
    ld8 r24 = [r21], 8
    ;;
    ld8 r23 = [r21], 8
    ;;
    ld8 r22 = [r21], 8
    ;;
    ld8.acq r21 = [r21]
    ;;
    cmp.eq p6, p7 = r24, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    mov r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_MAXIMUM_PROCESSOR_COUNT
    ;;
    cmp.geu p6, p7 = r23, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    mov r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_TIMER_VECTOR
    ;;
    cmp.ne p6, p7 = r22, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    cmp.eq p6, p7 = r21, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_legacy
    ;;

    // Native controls are 64-byte aligned and 192 bytes long in Region 0.
    and r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_ALIGNMENT_MASK, r21
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_REGION_ZERO_LIMIT - ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_SIZE
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    cmp.gtu p6, p7 = r21, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    ld8.acq r17 = [r21]
    ;;
    cmp.eq p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_legacy
    ;;
    mov r18 = ASTERIA_IA64_EXTERNAL_PREEMPTION_MODE_BSP_ONE_SHOT_PREEMPTION
    ;;
    cmp.ne p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_dispatch_fail_stop
    ;;
    br.sptk.few AsteriaIa64ExternalPreemptionNativeEntry
    ;;

.Lasteria_ia64_external_dispatch_legacy:
    mov pr = r31, -1
    ;;
    br.sptk.few AsteriaIa64ExternalInterruptEntry
    ;;

.Lasteria_ia64_external_dispatch_user:
    mov pr = r31, -1
    ;;
    br.sptk.few AsteriaIa64UserInterruptClaim
    ;;

.Lasteria_ia64_external_dispatch_fail_stop:
    hint @pause
    ;;
    br.sptk.many AsteriaIa64StopExternal
    ;;
    .align 16
    .size AsteriaIa64ExternalInterruptDispatchEntry, . - AsteriaIa64ExternalInterruptDispatchEntry

    .macro AsteriaIa64PreemptionStoreFrame offset, source
        add r18 = \offset, r19
        ;;
        st8 [r18] = \source
        ;;
    .endm

    .align 16
    .global AsteriaIa64ExternalPreemptionNativeEntry
    .hidden AsteriaIa64ExternalPreemptionNativeEntry
    .type AsteriaIa64ExternalPreemptionNativeEntry, @function
AsteriaIa64ExternalPreemptionNativeEntry:
    // Raw IVT-tail contract: r20 is the interrupted CCV; r21 is the acquired
    // control; r22/r23 are vector/CPU; r26-r31 retain ISR/IFS/IPSR/IIP/PSR/PR.
    // Static r1-r15 and interrupted bank-one r16-r31 are still untouched.
    ld8.acq r16 = [r21]
    ;;
    mov r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_MODE_BSP_ONE_SHOT_PREEMPTION
    ;;
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE, r21
    ;;
    ld8.acq r17 = [r16]
    ;;
    mov r18 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_ARMED
    ;;
    cmp.ne p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;

    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_PROCESSOR_INDEX, r21
    ;;
    ld8 r17 = [r16], 8
    ;;
    ld8 r18 = [r16]
    ;;
    cmp.ne p6, p7 = r17, r23
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    cmp.ne p6, p7 = r18, r22
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    mov r16 = ASTERIA_IA64_EXTERNAL_INTERRUPT_TIMER_VECTOR
    ;;
    cmp.ne p6, p7 = r18, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;

    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_SENTINEL_ENTRY_POINT, r21
    ;;
    ld8 r17 = [r16], 8
    ;;
    ld8 r18 = [r16]
    ;;
    cmp.ne p6, p7 = r29, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    adds r17 = 16, r17
    ;;
    cmp.ne p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    // The admitted sentinel extent is one aligned srlz.d sampling bundle.
    // Accept RI 0 to restart the idempotent serialization or RI 1 to finish
    // its no-op suffix; RI 2 and RI 3 are outside the finite restart proof.
    extr.u r16 = r28, 41, 2
    ;;
    mov r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_SENTINEL_LAST_RESTART_INSTRUCTION_SLOT + 1
    ;;
    cmp.geu p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;

    // External interrupts architecturally clear IFS.v. The later cover while
    // PSR.ic is zero must create the valid IFS image used by the rfi tail.
    shr.u r16 = r27, 63
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;

    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_WORKER_MEMORY_STACK_START, r21
    ;;
    ld8 r17 = [r16], 8
    ;;
    ld8 r18 = [r16]
    ;;
    cmp.ltu p6, p7 = r12, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    cmp.geu p6, p7 = r12, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;

    mov r16 = ar.rsc
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    mov r17 = ar.bspstore
    mov r18 = ar.bsp
    ;;
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_WORKER_RSE_BACKING_STORE_START, r21
    ;;
    ld8 r24 = [r16], 8
    ;;
    ld8 r25 = [r16]
    ;;
    cmp.ltu p6, p7 = r17, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    cmp.geu p6, p7 = r17, r25
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    cmp.ltu p6, p7 = r18, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    cmp.geu p6, p7 = r18, r25
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    sub r16 = r18, r17
    mov r24 = ASTERIA_IA64_EXTERNAL_PREEMPTION_COORDINATOR_RSE_TRANSITION_SIZE_BYTES
    ;;
    cmp.geu p6, p7 = r16, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;

    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_FRAME_ADDRESS, r21
    ;;
    ld8 r19 = [r16]
    ;;
    cmp.eq p6, p7 = r19, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    and r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_ALIGNMENT_MASK, r19
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_REGION_ZERO_LIMIT - ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_SIZE
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    cmp.gtu p6, p7 = r19, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    ld8.acq r17 = [r19]
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATE, r19
    ;;
    ld8 r17 = [r16]
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_NEXT_TOKEN, r19
    ;;
    ld8 r18 = [r16]
    ;;
    cmp.eq p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;

    // Claim exactly once without destroying the interrupted CCV.
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE, r21
    mov r24 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_ARMED
    ;;
    mov ar.ccv = r24
    mov r25 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_CAPTURING
    ;;
    cmpxchg8.acq r16 = [r17], r25, ar.ccv
    ;;
    mov ar.ccv = r20
    ;;
    cmp.ne p6, p7 = r16, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    mf
    ;;

    // Frame ownership becomes terminal immediately after the control claim.
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_TOKEN, r19
    ;;
    st8 [r16] = r18, 8
    ;;
    adds r17 = 1, r18
    ;;
    st8 [r16] = r17, 8
    ;;
    mov r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATE_CAPTURING
    ;;
    st8 [r16] = r17, 8
    ;;
    st8 [r16] = r0, 8
    ;;
    st8 [r16] = r23, 8
    ;;
    st8 [r16] = r22, 8
    ;;
    st8 [r16] = r0, 8
    ;;
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_ARMED_DEADLINE_TICKS, r21
    ;;
    ld8 r17 = [r16]
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_PROGRAMMED_DEADLINE_TICKS, r17
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_OBSERVED_TICKS, r0
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_MASKED_TIMER_VECTOR, r0
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_EOI_COUNT, r0

    // Capture every interruption register before cover changes cr.ifs.
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CURRENT_PROCESSOR_STATUS, r30
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTED_INSTRUCTION_POINTER, r29
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTED_PROCESSOR_STATUS, r28
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_HARDWARE_INTERRUPTION_FUNCTION_STATE, r27
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_STATUS, r26
    mov r16 = cr.ifa
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_FAULTING_ADDRESS, r16
    mov r16 = cr.itir
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_TRANSLATION_INFORMATION, r16
    mov r16 = cr.iipa
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_PREVIOUS_INSTRUCTION_POINTER, r16
    mov r16 = cr.iim
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_IMMEDIATE, r16
    mov r16 = cr.iha
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTION_HASH_ADDRESS, r16
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_INTERRUPTION_STATE
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_STAGE, r16

    // Save all application, predicate, and branch state before static spills.
    mov r16 = ar.pfs
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_PREVIOUS_FUNCTION_STATE, r16
    mov r16 = ar.rsc
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_CONFIGURATION, r16
    mov r16 = ar.bsp
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_BACKING_STORE_POINTER, r16
    mov r16 = ar.bspstore
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_BACKING_STORE_STORE_POINTER, r16
    mov r16 = ar.rnat
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_NAT_COLLECTION, r16
    mov r16 = ar.unat
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_ORIGINAL_NAT_COLLECTION, r16
    mov r16 = ar.lc
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_LOOP_COUNTER, r16
    mov r16 = ar.ec
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_EPILOG_COUNTER, r16
    mov r16 = ar.csd
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_COMPARE_AND_STORE_DATA, r16
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_COMPARE_EXCHANGE_VALUE, r20
    mov r16 = ar.fpsr
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_FLOATING_POINT_STATUS, r16
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_PREDICATES, r31
    mov r16 = b0
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B0, r16
    mov r16 = b1
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B1, r16
    mov r16 = b2
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B2, r16
    mov r16 = b3
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B3, r16
    mov r16 = b4
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B4, r16
    mov r16 = b5
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B5, r16
    mov r16 = b6
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B6, r16
    mov r16 = b7
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B7, r16

    // Static r1-r15 use their own software-spill UNAT collection.
    mov ar.unat = r0
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_GENERAL_REGISTERS_R1, r19
    ;;
    st8.spill [r16] = r1, 8
    ;;
    st8.spill [r16] = r2, 8
    ;;
    st8.spill [r16] = r3, 8
    ;;
    st8.spill [r16] = r4, 8
    ;;
    st8.spill [r16] = r5, 8
    ;;
    st8.spill [r16] = r6, 8
    ;;
    st8.spill [r16] = r7, 8
    ;;
    st8.spill [r16] = r8, 8
    ;;
    st8.spill [r16] = r9, 8
    ;;
    st8.spill [r16] = r10, 8
    ;;
    st8.spill [r16] = r11, 8
    ;;
    st8.spill [r16] = r12, 8
    ;;
    st8.spill [r16] = r13, 8
    ;;
    st8.spill [r16] = r14, 8
    ;;
    st8.spill [r16] = r15
    ;;
    mov r17 = ar.unat
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATIC_GENERAL_REGISTER_SPILL_NAT_COLLECTION, r17

    // r14/r15 become persistent frame carriers only after their exact spill.
    mov r15 = r19
    add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_GENERAL_REGISTERS_R16, r19
    ;;
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
    ;;
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BANKED_GENERAL_REGISTER_SPILL_NAT_COLLECTION, r15
    ;;
    st8 [r17] = r16
    ;;
    bsw.0
    ;;
    mov r19 = r15
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_INTEGER_STATE
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_STAGE, r16
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_APPLICATION_STATE
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_STAGE, r16

    // No floating register is used until the complete f2-f127 image is spilled.
    add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_FLOATING_REGISTERS, r19
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
    stf.spill [r14] = f127, 16
    ;;
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_FLOATING_STATE
    ;;
    AsteriaIa64PreemptionStoreFrame ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_STAGE, r16

    // Cover the interrupted CFM before rebinding the reusable per-CPU RBS.
    mov r14 = r21
    mov r15 = r19
    ;;
    cover
    ;;
    mov r16 = cr.ifs
    mov r18 = ar.bsp
    ;;
    shr.u r17 = r16, 63
    ;;
    cmp.eq p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_COVERED_INTERRUPTION_FUNCTION_STATE, r15
    ;;
    st8 [r17] = r16
    ;;
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_COVERED_RSE_BACKING_STORE_POINTER, r15
    ;;
    st8 [r17] = r18
    ;;

    // A valid covered IFS contains only v plus the 38-bit CFM. Reject every
    // reserved encoding and prove the architectural frame-size relationships.
    shr.u r17 = r16, 38
    movl r24 = 0x2000000
    ;;
    cmp.ne p6, p7 = r17, r24
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    extr.u r17 = r16, 0, 7
    extr.u r24 = r16, 7, 7
    ;;
    mov r25 = 96
    ;;
    cmp.gtu p6, p7 = r17, r25
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    cmp.gtu p6, p7 = r24, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    extr.u r24 = r16, 14, 4
    ;;
    shl r24 = r24, 3
    ;;
    cmp.gtu p6, p7 = r24, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    extr.u r25 = r16, 18, 7
    ;;
    cmp.eq p6, p7 = r24, r0
    ;;
(p6) cmp.ne.unc p8, p9 = r25, r0
    ;;
(p8) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    cmp.ne p6, p7 = r24, r0
    ;;
(p6) cmp.geu.unc p8, p9 = r25, r24
    ;;
(p8) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    extr.u r24 = r16, 25, 7
    extr.u r25 = r16, 32, 6
    ;;
    mov r16 = 96
    ;;
    cmp.geu p6, p7 = r24, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    mov r16 = 48
    ;;
    cmp.geu p6, p7 = r25, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;

    // cover advances the pre-cover BSP by CFM.sof register slots while
    // skipping each RNAT slot. The separately retained post-cover BSP is the
    // restore convergence point; the original BSP remains exact evidence.
    add r24 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_BACKING_STORE_POINTER, r15
    ;;
    ld8 r25 = [r24]
    ;;
    extr.u r24 = r25, 3, 6
    ;;
    mov r16 = 63
    ;;
    cmp.eq p6, p7 = r24, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    add r24 = r24, r17
    mov r16 = 63
    ;;
    cmp.geu p6, p7 = r24, r16
    ;;
(p6) adds r17 = 1, r17
    mov r16 = 126
    ;;
    cmp.geu p6, p7 = r24, r16
    ;;
(p6) adds r17 = 1, r17
    ;;
    shladd r25 = r17, 3, r25
    ;;
    cmp.ne p6, p7 = r25, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    add r24 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_WORKER_RSE_BACKING_STORE_END, r14
    ;;
    ld8 r25 = [r24]
    ;;
    cmp.gtu p6, p7 = r18, r25
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    invala
    ;;
    mov ar.rsc = r0
    ;;
    // This post-cover pair is authoritative under the SDM 6.11.1 sequence.
    mov r16 = ar.bspstore
    mov r18 = ar.rnat
    ;;
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_BACKING_STORE_STORE_POINTER, r15
    ;;
    st8 [r17] = r16, 8
    ;;
    st8 [r17] = r18
    ;;
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_COORDINATOR_RSE_BACKING_STORE_START, r14
    ;;
    ld8 r17 = [r16]
    ;;
    mov ar.bspstore = r17
    ;;
    mov ar.rnat = r0
    ;;
    mov r18 = ar.bsp
    ;;
    cmp.ltu p6, p7 = r18, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    sub r18 = r18, r17
    ;;
    and r16 = 7, r18
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_COORDINATOR_RSE_TRANSITION_SIZE_BYTES
    ;;
    cmp.geu p6, p7 = r18, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    shl r18 = r18, 16
    ;;
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_DIRTY_PARTITION_LOAD_CONFIGURATION, r15
    ;;
    st8 [r16] = r18
    ;;
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_RSE_TRANSITION
    ;;
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_STAGE, r15
    ;;
    st8 [r17] = r16
    ;;
    mov r16 = 3
    ;;
    mov ar.rsc = r16
    ;;

    // Claim, validate, mask/read back, and EOI the fixed local timer once.
    mov r24 = cr.ivr
    ;;
    srlz.d
    ;;
    mov r16 = ASTERIA_IA64_EXTERNAL_INTERRUPT_SPURIOUS_VECTOR
    ;;
    cmp.eq p6, p7 = r24, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    mov r16 = ASTERIA_IA64_EXTERNAL_INTERRUPT_TIMER_VECTOR
    ;;
    cmp.ne p6, p7 = r24, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_quiesce_fail_stop
    ;;
    mov r25 = cr.itm
    mov r26 = ar.itc
    ;;
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_ARMED_DEADLINE_TICKS, r14
    ;;
    ld8 r17 = [r16]
    ;;
    cmp.ne p6, p7 = r25, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_quiesce_fail_stop
    ;;
    cmp.ltu p6, p7 = r26, r25
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_quiesce_fail_stop
    ;;
    movl r16 = -1
    ;;
    cmp.eq p6, p7 = r26, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_quiesce_fail_stop
    ;;
    movl r16 = ASTERIA_IA64_EXTERNAL_INTERRUPT_MASKED_TIMER_VECTOR
    ;;
    mov cr.itv = r16
    ;;
    srlz.d
    ;;
    mov r17 = cr.itv
    ;;
    cmp.ne p6, p7 = r17, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_eoi_fail_stop
    ;;
    mov cr.eoi = r0
    ;;
    srlz.d
    ;;
    add r18 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CLAIMED_VECTOR, r15
    ;;
    st8 [r18] = r24, 8
    ;;
    st8 [r18] = r25, 8
    ;;
    st8 [r18] = r26, 8
    ;;
    st8 [r18] = r17, 8
    ;;
    mov r16 = 1
    ;;
    st8 [r18] = r16
    ;;
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_TIMER_ACKNOWLEDGED
    ;;
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_STAGE, r15
    ;;
    st8 [r17] = r16
    ;;
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATE_RETAINED
    ;;
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATE, r15
    ;;
    st8 [r17] = r16
    ;;
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_TOKEN, r15
    ;;
    ld8 r18 = [r17]
    ;;
    mf
    ;;
    st8.rel [r15] = r18
    ;;

    // Publish control only after the retained frame token is acquire-visible.
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_CAPTURED_FRAME_TOKEN, r14
    ;;
    st8 [r17] = r18
    ;;
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_CAPTURE_COUNT, r14
    ;;
    ld8 r16 = [r17]
    ;;
    adds r16 = 1, r16
    ;;
    st8 [r17] = r16
    ;;
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_COORDINATOR
    ;;
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE, r14
    ;;
    st8.rel [r17] = r16
    ;;

    // Reuse the admitted per-CPU corridor for one returning C++ coordinator.
    // Only this assembly bridge may enter the raw restore core after the call
    // chain has unwound to its exact SP/BSP baseline.
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_COORDINATOR_MEMORY_STACK_END, r14
    ;;
    ld8 r12 = [r17]
    add r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_COORDINATOR_ENTRY_POINT, r14
    ;;
    adds r12 = -16, r12
    ld8 r6 = [r17], 8
    ;;
    ld8 r1 = [r17], 8
    ;;
    ld8 r7 = [r17]
    mov r4 = r15
    mov r5 = r14
    ;;
    ssm psr.ic
    bsw.1
    ;;
    srlz.d
    ;;
    alloc r16 = ar.pfs, 0, 0, 1, 0
    ;;
    mov r32 = r7
    mov b6 = r6
    ;;
    mov r6 = ar.bsp
    mov r7 = r12
    ;;
    br.call.sptk.many b0 = b6
    ;;
    mf
    ;;
    cmp.ne p6, p7 = r8, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    mov r14 = ar.bsp
    ;;
    cmp.ne p6, p7 = r12, r7
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    cmp.ne p6, p7 = r14, r6
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_fail_stop
    ;;
    add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_CAPTURED_FRAME_TOKEN, r5
    ;;
    ld8 r6 = [r14], 8
    ;;
    ld8 r7 = [r14]
    ;;
    alloc r16 = ar.pfs, 0, 0, 0, 0
    ;;
    rsm psr.ic
    ;;
    srlz.d
    ;;
    br.sptk.few .Lasteria_ia64_preemption_restore_committed
    ;;

.Lasteria_ia64_preemption_quiesce_fail_stop:
    movl r16 = ASTERIA_IA64_EXTERNAL_INTERRUPT_MASKED_TIMER_VECTOR
    ;;
    mov cr.itv = r16
    ;;
    srlz.d
    ;;
    mov r17 = cr.itv
    ;;
    cmp.ne p6, p7 = r17, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_eoi_fail_stop
    ;;
.Lasteria_ia64_preemption_eoi_fail_stop:
    mov cr.eoi = r0
    ;;
    srlz.d
    ;;
.Lasteria_ia64_preemption_fail_stop:
    hint @pause
    ;;
    br.sptk.many AsteriaIa64StopExternal
    ;;
    .align 16
    .size AsteriaIa64ExternalPreemptionNativeEntry, . - AsteriaIa64ExternalPreemptionNativeEntry
        .align 16
        .global AsteriaIa64ExternalPreemptionRestoreTail
        .hidden AsteriaIa64ExternalPreemptionRestoreTail
        .type AsteriaIa64ExternalPreemptionRestoreTail, @function
AsteriaIa64ExternalPreemptionRestoreTail:
        // Raw, non-C++ contract: r4/r5 identify frame/control, r6/r7 identify
        // the committed frame/idle generations, and the current RSE frame is
        // empty. Only the returning entry bridge can satisfy this contract.
.Lasteria_ia64_preemption_restore_committed:
        cmp.eq p6, p7 = r4, r0
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        cmp.eq p6, p7 = r5, r0
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        cmp.eq p6, p7 = r6, r0
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        cmp.eq p6, p7 = r7, r0
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;

        ld8.acq r14 = [r5]
        ;;
        cmp.ne p6, p7 = r14, r0
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE, r5
        ;;
        ld8.acq r15 = [r14]
        ;;
        mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_RESUME_COMMITTED
        ;;
        cmp.ne p6, p7 = r15, r16
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_FRAME_ADDRESS, r5
        ;;
        ld8 r15 = [r14]
        ;;
        cmp.ne p6, p7 = r15, r4
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_CAPTURED_FRAME_TOKEN, r5
        ;;
        ld8 r15 = [r14], 8
        ;;
        ld8 r16 = [r14]
        ;;
        cmp.ne p6, p7 = r15, r6
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        cmp.ne p6, p7 = r16, r7
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;

        // External entry's hardware IFS is invalid; cover's retained CFM is
        // the sole valid image written back to cr.ifs by this restore.
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_HARDWARE_INTERRUPTION_FUNCTION_STATE, r4
        ;;
        ld8 r15 = [r14], 8
        ;;
        ld8 r16 = [r14]
        ;;
        shr.u r15 = r15, 63
        shr.u r16 = r16, 63
        ;;
        cmp.ne p6, p7 = r15, r0
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        cmp.eq p6, p7 = r16, r0
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        ld8.acq r14 = [r4]
        ;;
        cmp.ne p6, p7 = r14, r6
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_TOKEN, r4
        ;;
        ld8 r15 = [r14], 8
        ;;
        cmp.ne p6, p7 = r15, r6
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        adds r14 = 8, r14
        ;;
        ld8 r15 = [r14], 8
        ;;
        ld8 r16 = [r14]
        ;;
        mov r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATE_RESUME_COMMITTED
        ;;
        cmp.ne p6, p7 = r15, r17
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        mov r17 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CAPTURE_STAGE_TIMER_ACKNOWLEDGED
        ;;
        cmp.ne p6, p7 = r16, r17
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;

        // Discard the returned coordinator frame, recover the covered dirty
        // partition, then restore the exact original BSPSTORE/RNAT pair.
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_RSE_BACKING_STORE_STORE_POINTER, r4
        ;;
        ld8 r15 = [r14], 8
        ;;
        ld8 r16 = [r14], 8
        ;;
        ld8 r17 = [r14]
        ;;
        alloc r18 = ar.pfs, 0, 0, 0, 0
        ;;
        mov ar.rsc = r17
        ;;
        loadrs
        ;;
        invala
        ;;
        mov ar.bspstore = r15
        ;;
        mov ar.rnat = r16
        ;;
        mov r14 = ar.bsp
        ;;
        add r15 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_COVERED_RSE_BACKING_STORE_POINTER, r4
        ;;
        ld8 r16 = [r15]
        ;;
        cmp.ne p6, p7 = r14, r16
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;

        // Restore the complete floating image before any FP register is reused.
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_FLOATING_REGISTERS, r4
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
        ldf.fill f127 = [r14], 16
        ;;

        // Restore application and branch registers that are not final rfi controls.
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_LOOP_COUNTER, r4
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

        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_BRANCH_REGISTERS_B0, r4
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

        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATIC_GENERAL_REGISTER_SPILL_NAT_COLLECTION, r4
        ;;
        ld8 r8 = [r14], 8
        ;;
        ld8 r9 = [r14]
        ;;

        // Bank zero is not part of the interrupted BN=1 image. Load every final
        // carrier there while all faultable frame accesses remain DTR-covered.
        bsw.0
        ;;
        mov r16 = r4
        mov r17 = r5
        mov r18 = r6
        ;;
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_INTERRUPTED_INSTRUCTION_POINTER, r4
        ;;
        ld8 r19 = [r14], 8
        ;;
        ld8 r20 = [r14], 8
        ;;
        adds r14 = 8, r14
        ;;
        ld8 r21 = [r14]
        ;;
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_PREVIOUS_FUNCTION_STATE, r4
        ;;
        ld8 r22 = [r14], 8
        ;;
        ld8 r23 = [r14]
        ;;
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_ORIGINAL_NAT_COLLECTION, r4
        ;;
        ld8 r24 = [r14], 8
        ;;
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_PREDICATES, r4
        ;;
        ld8 r25 = [r14]
        ;;

        // Preload the only control value that must be read-modify-written
        // before bank-one fills consume r16-r31. Once PublishedToken is
        // release-cleared, no later instruction may access memory.
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_RESTORE_COUNT, r17
        ;;
        ld8 r26 = [r14]
        ;;
        adds r26 = 1, r26
        ;;

        // This finite proof may return only to RI 0 or RI 1 of the admitted
        // srlz.d sampling bundle. The retained frame remains byte-exact; only
        // the outgoing IPSR carrier is normalized after these checks, so RI 0
        // re-executes serialization or RI 1 finishes the no-op suffix while
        // masked before the immediately following idempotent rsm psr.i.
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_SENTINEL_ENTRY_POINT, r17
        ;;
        ld8 r14 = [r14]
        ;;
        cmp.ne p6, p7 = r19, r14
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        extr.u r14 = r20, 41, 2
        ;;
        mov r15 = ASTERIA_IA64_EXTERNAL_PREEMPTION_SENTINEL_LAST_RESTART_INSTRUCTION_SLOT + 1
        ;;
        cmp.geu p6, p7 = r14, r15
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        extr.u r14 = r20, 14, 1
        ;;
        cmp.eq p6, p7 = r14, r0
        ;;
    (p6) br.cond.dpnt.few .Lasteria_ia64_preemption_restore_fail_stop
        ;;
        bsw.1
        ;;

        // Fill the interrupted bank-one and static images from their independent
        // software-spill NaT collections.
        mov ar.unat = r9
        ;;
        add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_GENERAL_REGISTERS_R16, r4
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

        mov ar.unat = r8
        ;;
        add r15 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_GENERAL_REGISTERS_R1, r4
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

        // All frame reads are complete, and the filled static and bank-one
        // registers now hold the complete interrupted image: from here to
        // rfi only bank-zero r16-r31 are written. Prepare inactive frame
        // metadata and complete the already-disarmed control while
        // PublishedToken still conservatively owns the frame. Its final
        // release-clear is the last memory access before the nonfaulting rfi
        // window.
        bsw.0
        ;;
        add r27 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_CAPTURE_STAGE, r16
        ;;
        st8 [r27] = r0
        ;;
        add r27 = ASTERIA_IA64_EXTERNAL_PREEMPTION_FRAME_STATE, r16
        ;;
        st8 [r27] = r0
        ;;
        add r27 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_ARMED_DEADLINE_TICKS, r17
        ;;
        st8 [r27] = r0
        ;;
        add r27 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_RESTORE_COUNT, r17
        ;;
        st8 [r27] = r26
        ;;
        mov r28 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_COMPLETE
        ;;
        add r27 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE, r17
        ;;
        st8.rel [r27] = r28
        ;;

        // Recover the remaining application and interruption controls while
        // PublishedToken still conservatively owns the frame.
        mov ar.pfs = r22
        ;;
        mov ar.unat = r24
        ;;
        mov cr.ifs = r21
        ;;
        mov ar.rsc = r23
        ;;

        // This is the final memory access. Nothing after the ownership release
        // may fault, refill, call, allocate, change RSE state, or return.
        st8.rel [r16] = r0
        ;;

        // Preserve the captured IPSR in the frame, but clear only the outgoing
        // I bit. The exact RI 0/RI 1 sentinel landing either re-executes the
        // serialization or completes its suffix while masked, immediately
        // before its idempotent rsm.i.
        dep r20 = r0, r20, 14, 1
        ;;
        mov cr.iip = r19
        mov cr.ipsr = r20
        ;;
        mov pr = r25, -1
        ;;
        rfi
        ;;

    .Lasteria_ia64_preemption_restore_fail_stop:
    hint @pause
    ;;
    br.sptk.many AsteriaIa64StopExternal
    ;;
        .align 16
        .size AsteriaIa64ExternalPreemptionRestoreTail, . - AsteriaIa64ExternalPreemptionRestoreTail
    .align 16
.Lasteria_ia64_external_preemption_code_end:

    .align 16
    .global AsteriaIa64GetExternalPreemptionCodeRangeWords
    .hidden AsteriaIa64GetExternalPreemptionCodeRangeWords
    .type AsteriaIa64GetExternalPreemptionCodeRangeWords, @function
AsteriaIa64GetExternalPreemptionCodeRangeWords:
    cmp.eq p6, p7 = r32, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_range_invalid
    ;;
    and r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CODE_RANGE_ALIGNMENT_MASK, r32
    ;;
    cmp.ne p6, p7 = r14, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_preemption_range_invalid
    ;;
    movl r14 = AsteriaIa64ExternalInterruptDispatchEntry
    movl r15 = .Lasteria_ia64_external_preemption_code_end
    ;;
    st8 [r32] = r14, 8
    ;;
    st8 [r32] = r15
    ;;
    mov r8 = r0
    br.ret.sptk.many b0
    ;;
.Lasteria_ia64_preemption_range_invalid:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
    .align 16
    .size AsteriaIa64GetExternalPreemptionCodeRangeWords, . - AsteriaIa64GetExternalPreemptionCodeRangeWords
    .purgem AsteriaIa64PreemptionStoreFrame
