// SPDX-License-Identifier: Apache-2.0

// Bank-0 legacy external-interrupt entry, the P64 sequence poll with its
// two-argument wrapper, and the poll code-range getter. External-interrupt
// constants and the ar.k3 context, Ia64ExternalInterruptEntryContext and
// Ia64LocalTimerInterruptEvidence offsets come from the generated C++ layout.

#include "Generated/Platform/Ia64/Ia64ExternalInterruptEntryAsmLayout.h"

.explicit
#include "Platform/Ia64/Diagnostics/Ia64StopPoll.inc"

.text
.align 16
.global AsteriaIa64ExternalInterruptEntry
.hidden AsteriaIa64ExternalInterruptEntry
.type AsteriaIa64ExternalInterruptEntry, @function
AsteriaIa64ExternalInterruptEntry:
    AsteriaIa64PollStop ASTERIA_IA64_STOP_EXTERNAL_VECTOR
// Hardware has selected bank zero. Save PR before any comparison and use
// only r16-r31 until rfi restores the interrupted bank-one leaf.
    mov r31 = pr
    mov r30 = psr
    mov r29 = cr.iip
    mov r28 = cr.ipsr
    mov r27 = cr.ifs
    mov r26 = cr.isr
    ;;
// Current state is little-endian translated bank zero with delivery and
// collection disabled. PSR.be can be inherited from DCR.be on collection,
// so reject it before packed evidence stores. The interrupted state is
// exact except for legal RI and interrupted UP/AC/MFL/MFH user state that
// rfi restores without this handler interpreting it.
    movl r16 = ASTERIA_IA64_EXTERNAL_INTERRUPT_CURRENT_PROCESSOR_STATUS_MASK
    ;;
    and r17 = r30, r16
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_CURRENT_PROCESSOR_STATUS_VALUE
    ;;
    cmp.ne p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    movl r16 = ASTERIA_IA64_EXTERNAL_INTERRUPT_INTERRUPTED_PROCESSOR_STATUS_MASK
    ;;
    and r17 = r28, r16
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_INTERRUPTED_PROCESSOR_STATUS_VALUE
    ;;
    cmp.ne p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    extr.u r17 = r28, 41, 2
    ;;
    mov r18 = 3
    ;;
    cmp.eq p6, p7 = r17, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
// ISR.ni identifies a nested interruption whose IVR cannot be consumed by
// this non-nesting front end. Reject it before the destructive IVR read.
    extr.u r17 = r26, 39, 1
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    shr.u r17 = r27, 63
    and r18 = 15, r29
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    cmp.ne p6, p7 = r18, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
// Offset 24 of the retained ar.k3 context is the release publication.
    mov r26 = ar.k3
    ;;
    cmp.eq p6, p7 = r26, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    and r17 = ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_ALIGNMENT_MASK, r26
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_REGION_ZERO_LIMIT - ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_SIZE
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    cmp.gtu p6, p7 = r26, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    add r17 = ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_ADDRESS, r26
    ;;
    ld8.acq r25 = [r17]
    ;;
    cmp.eq p6, p7 = r25, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    and r17 = ASTERIA_IA64_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_ALIGNMENT_MASK, r25
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_REGION_ZERO_LIMIT - ASTERIA_IA64_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_SIZE
    ;;
    cmp.gtu p6, p7 = r25, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    adds r21 = ASTERIA_IA64_EXTERNAL_INTERRUPT_ENTRY_CONTEXT_EVIDENCE_ADDRESS, r25
    ;;
    ld8 r24 = [r21], 8
    ;;
    ld8 r23 = [r21], 8
    ;;
    ld8 r22 = [r21], 8
    ;;
    ld8 r21 = [r21]
    ;;
    cmp.eq p6, p7 = r24, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    and r17 = ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_ALIGNMENT_MASK, r24
    movl r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_REGION_ZERO_LIMIT - ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_SIZE
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    cmp.gtu p6, p7 = r24, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    mov r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_MAXIMUM_PROCESSOR_COUNT
    ;;
    cmp.geu p6, p7 = r23, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    mov r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_TIMER_VECTOR
    ;;
    cmp.ne p6, p7 = r22, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
// The dispatch front end owns optional native-control validation. Legacy
// evidence deliberately ignores word four so publication and one-shot
// disarm cannot invalidate this fallback continuation.
// Acquire both ownership words before IVR. Equality proves that normal
// context has finished the previous record; maximum is terminal because
// the next publication must never wrap to zero.
    ld8.acq r17 = [r24]
    add r16 = ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_ACKNOWLEDGED_SEQUENCE, r24
    ;;
    ld8.acq r21 = [r16]
    ;;
    cmp.ne p6, p7 = r17, r21
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    movl r16 = -1
    ;;
    cmp.eq p6, p7 = r17, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_fail_stop
    ;;
    add r21 = 1, r17
    ;;
// IVR is destructive. Every failure below an ordinary claimed vector
// performs exactly one EOI; architected vector 15 performs none.
    mov r20 = cr.ivr
    ;;
    srlz.d
    ;;
    mov r18 = ASTERIA_IA64_EXTERNAL_INTERRUPT_SPURIOUS_VECTOR
    ;;
    cmp.eq p6, p7 = r20, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_spurious
    ;;
    cmp.ne p6, p7 = r20, r22
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_quiesce_claimed_fail_stop
    ;;
    mov r19 = cr.itm
    mov r18 = ar.itc
    ;;
    cmp.eq p6, p7 = r19, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_quiesce_claimed_fail_stop
    ;;
    movl r17 = -1
    ;;
    cmp.eq p6, p7 = r19, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_quiesce_claimed_fail_stop
    ;;
    cmp.ltu p6, p7 = r18, r19
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_quiesce_claimed_fail_stop
    ;;
    add r16 = ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_PROGRAMMED_DEADLINE_TICKS, r24
    ;;
    st8 [r16] = r19
    ;;
    add r16 = ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_OBSERVED_INTERVAL_TIME_COUNTER, r24
    ;;
    st8 [r16] = r18
    ;;
    add r16 = ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_INTERRUPTED_INSTRUCTION_POINTER, r24
    ;;
    st8 [r16] = r29
    ;;
    add r16 = ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_INTERRUPTED_PROCESSOR_STATUS, r24
    ;;
    st8 [r16] = r28
    ;;
    add r16 = ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_INTERRUPTED_IFS_VALID, r24
    ;;
    st8 [r16] = r0
    ;;
    shl r17 = r22, ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_VECTOR_SHIFT
    ;;
    or r17 = r17, r23
    add r16 = ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_PROCESSOR_INDEX, r24
    ;;
    st8 [r16] = r17
    ;;
// Quiesce and read back the timer only after every legacy payload field
// is complete, then relinquish the one in-service vector exactly once.
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
(p6) br.cond.dpnt.few .Lasteria_ia64_external_claimed_fail_stop
    ;;
    mov cr.eoi = r0
    ;;
    srlz.d
    ;;
    st8.rel [r24] = r21
    ;;
    mov pr = r31, -1
    ;;
    rfi
    ;;
.Lasteria_ia64_external_spurious:
    mov pr = r31, -1
    ;;
    rfi
    ;;
// An unknown claimed source is fatal. The local timer is
// masked/read back before the claimed vector is
// acknowledged so no timer source remains live in the fail-stop state.
.Lasteria_ia64_external_quiesce_claimed_fail_stop:
    movl r17 = ASTERIA_IA64_EXTERNAL_INTERRUPT_MASKED_TIMER_VECTOR
    ;;
    mov cr.itv = r17
    ;;
    srlz.d
    ;;
    mov r18 = cr.itv
    ;;
    cmp.ne p6, p7 = r18, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_external_timer_mask_fail_stop
    ;;
.Lasteria_ia64_external_claimed_fail_stop:
    mov cr.eoi = r0
    ;;
    srlz.d
    ;;
.Lasteria_ia64_external_fail_stop:
    hint @pause
    ;;
    br.sptk.many AsteriaIa64StopExternal
    ;;
.Lasteria_ia64_external_timer_mask_fail_stop:
    mov cr.eoi = r0
    ;;
    srlz.d
    ;;
.Lasteria_ia64_external_timer_mask_terminal_fail_stop:
    hint @pause
    ;;
    br.sptk.many AsteriaIa64StopExternal
    ;;
.align 16
.size AsteriaIa64ExternalInterruptEntry, . - AsteriaIa64ExternalInterruptEntry
.align 16
.global AsteriaIa64PollForExternalInterrupt
.hidden AsteriaIa64PollForExternalInterrupt
.type AsteriaIa64PollForExternalInterrupt, @function
AsteriaIa64PollForExternalInterrupt:
// Preserve the Phase-D two-argument ABI as a stackless exact-sequence-one
// tail branch into the reusable three-argument leaf.
    mov r34 = r33
    mov r33 = 1
    ;;
    br.sptk.few AsteriaIa64PollForExternalInterruptSequence
    ;;
.align 16
.size AsteriaIa64PollForExternalInterrupt, . - AsteriaIa64PollForExternalInterrupt
.align 16
.global AsteriaIa64PollForExternalInterruptSequence
.hidden AsteriaIa64PollForExternalInterruptSequence
.type AsteriaIa64PollForExternalInterruptSequence, @function
AsteriaIa64PollForExternalInterruptSequence:
// This P64 leaf deliberately has no alloc, stack, RSE, GP, or call use.
// mov-from-PSR cannot validate CPL, VM, ISA, or bank one; those remain
// trusted caller preconditions and are verified by the interrupt IPSR.
    mov r14 = psr
    ;;
    movl r15 = ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_PROCESSOR_STATUS_MASK
    ;;
    and r14 = r14, r15
    movl r15 = ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_PROCESSOR_STATUS_VALUE
    ;;
    cmp.ne p6, p7 = r14, r15
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_invalid_state
    ;;
    cmp.eq p6, p7 = r32, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_invalid_evidence
    ;;
    and r14 = ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_ALIGNMENT_MASK, r32
    movl r15 = ASTERIA_IA64_EXTERNAL_INTERRUPT_REGION_ZERO_LIMIT - ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_SIZE
    ;;
    cmp.ne p6, p7 = r14, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_invalid_evidence
    ;;
    cmp.gtu p6, p7 = r32, r15
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_invalid_evidence
    ;;
    cmp.eq p6, p7 = r33, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_invalid_evidence
    ;;
    cmp.eq p6, p7 = r34, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_invalid_evidence
    ;;
    movl r15 = -1
    ;;
    cmp.eq p6, p7 = r34, r15
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_invalid_evidence
    ;;
    add r16 = -1, r33
    add r15 = ASTERIA_IA64_LOCAL_TIMER_INTERRUPT_EVIDENCE_ACKNOWLEDGED_SEQUENCE, r32
    ;;
    ld8.acq r14 = [r32]
    ld8.acq r17 = [r15]
    ;;
    cmp.ne p6, p7 = r17, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_invalid_evidence
    ;;
    cmp.eq p6, p7 = r14, r33
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_completed
    ;;
    cmp.ne p6, p7 = r14, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_invalid_evidence
    ;;
    mov r14 = ar.itc
    ;;
    cmp.geu p6, p7 = r14, r34
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_timeout_disabled
    ;;
    ssm psr.i
    ;;
    srlz.d
    ;;
.Lasteria_ia64_poll_loop:
    AsteriaIa64PollStop 0
    ld8.acq r14 = [r32]
    ;;
    cmp.eq p6, p7 = r14, r33
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_disable
    ;;
    cmp.ne p6, p7 = r14, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_disable
    ;;
    mov r15 = ar.itc
    ;;
    cmp.geu p6, p7 = r15, r34
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_disable
    ;;
    hint @pause
    ;;
    br.cond.sptk .Lasteria_ia64_poll_loop
    ;;
// Disable first, then acquire-recheck. This closes the timeout boundary
// against an interrupt that completed immediately before rsm became live.
.Lasteria_ia64_poll_disable:
    rsm psr.i
    ;;
    srlz.d
    ;;
    ld8.acq r14 = [r32]
    ;;
    cmp.eq p6, p7 = r14, r33
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_completed
    ;;
    cmp.ne p6, p7 = r14, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_invalid_evidence
    ;;
    mov r8 = ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_STATUS_TIMED_OUT
    br.ret.sptk.many b0
    ;;
.Lasteria_ia64_poll_invalid_evidence:
    mov r8 = ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_STATUS_INVALID_EVIDENCE
    br.ret.sptk.many b0
    ;;
.Lasteria_ia64_poll_invalid_state:
    mov r8 = ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_STATUS_INCOMPATIBLE_PROCESSOR_STATE
    br.ret.sptk.many b0
    ;;
.Lasteria_ia64_poll_timeout_disabled:
    mov r8 = ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_STATUS_TIMED_OUT
    br.ret.sptk.many b0
    ;;
.Lasteria_ia64_poll_completed:
    mov r8 = r0
    br.ret.sptk.many b0
    ;;
.align 16
.Lasteria_ia64_poll_code_end:
.size AsteriaIa64PollForExternalInterruptSequence, . - AsteriaIa64PollForExternalInterruptSequence
.align 16
.global AsteriaIa64GetExternalInterruptPollCodeRangeWords
.hidden AsteriaIa64GetExternalInterruptPollCodeRangeWords
.type AsteriaIa64GetExternalInterruptPollCodeRangeWords, @function
AsteriaIa64GetExternalInterruptPollCodeRangeWords:
    cmp.eq p6, p7 = r32, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_range_invalid
    ;;
    and r14 = ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_CODE_RANGE_ALIGNMENT_MASK, r32
    ;;
    cmp.ne p6, p7 = r14, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_poll_range_invalid
    ;;
    movl r14 = AsteriaIa64PollForExternalInterruptSequence
    movl r15 = .Lasteria_ia64_poll_code_end
    ;;
    st8 [r32] = r14, 8
    ;;
    st8 [r32] = r15
    ;;
    mov r8 = r0
    br.ret.sptk.many b0
    ;;
.Lasteria_ia64_poll_range_invalid:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64GetExternalInterruptPollCodeRangeWords, . - AsteriaIa64GetExternalInterruptPollCodeRangeWords
