// SPDX-License-Identifier: Apache-2.0

// IA-64 user-mode launch leaf: validates the plan, retains the supervisor
// continuation and enters PL3 through rfi. Plan and continuation offsets come
// from the generated C++ layout.

#include "Generated/Platform/Ia64/Ia64UserModeLaunchAsmLayout.h"

.explicit
.text
.align 16
.global AsteriaIa64LaunchUserMode
.hidden AsteriaIa64LaunchUserMode
.type AsteriaIa64LaunchUserMode, @function
AsteriaIa64LaunchUserMode:
// The sole P64 input is copied out of the stacked-register frame before
// cover. No processor or continuation state changes on either rejection.
    mov r14 = r32
    mov r15 = psr
    mov r16 = ar.rsc
    ;;
    add r17 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_CURRENT_PROCESSOR_STATUS_MASK, r14
    add r18 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_CURRENT_PROCESSOR_STATUS_VALUE, r14
    ;;
    ld8 r17 = [r17]
    ld8 r18 = [r18]
    ;;
    and r15 = r15, r17
    ;;
    cmp.ne p6, p7 = r15, r18
    ;;
    cmp.ne.or p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_processor_reject
    ;;
    add r15 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_SUPERVISOR_CONTINUATION_ADDRESS, r14
    ;;
    ld8 r15 = [r15]
    ;;
    cmp.eq p6, p7 = r15, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
    and r16 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_ALIGNMENT - 1, r15
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
// The retained record must still be fresh when the CPU reaches the leaf.
    add r16 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_STATE, r15
    add r17 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_OWNER, r15
    ;;
    ld8 r16 = [r16]
    ld8 r17 = [r17]
    ;;
    or r16 = r16, r17
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
// Check the live supervisor stack against the caller-retained range.
    add r16 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_MEMORY_STACK_VIRTUAL_START, r15
    add r17 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_MEMORY_STACK_SIZE_BYTES, r15
    ;;
    ld8 r16 = [r16]
    ld8 r17 = [r17]
    ;;
    add r18 = r16, r17
    and r19 = 15, r12
    ;;
    cmp.ltu p6, p7 = r12, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
    cmp.ne.or p6, p7 = r19, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
    cmp.geu p6, p7 = r12, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
// Both architected RSE pointers must still name the retained supervisor
// range. Equality with its exclusive end is valid for an empty tail.
    add r16 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_RSE_BACKING_STORE_VIRTUAL_START, r15
    add r17 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_RSE_BACKING_STORE_SIZE_BYTES, r15
    mov r19 = ar.bspstore
    mov r20 = ar.bsp
    ;;
    ld8 r16 = [r16]
    ld8 r17 = [r17]
    ;;
    add r18 = r16, r17
    and r21 = 7, r19
    and r22 = 7, r20
    ;;
    or r21 = r21, r22
    cmp.ltu p6, p7 = r19, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
    cmp.ne.or p6, p7 = r21, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
    cmp.gtu p6, p7 = r19, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
    cmp.ltu p6, p7 = r20, r19
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
    cmp.gtu p6, p7 = r20, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_user_launch_supervisor_reject
    ;;
// Capture the complete synchronous P64 continuation. Offsets are pinned
// by the public register-state assertions in Ia64KernelContext.hpp.
    mov r16 = ar.rsc
    mov r17 = ar.bsp
    mov r18 = ar.pfs
    mov r19 = ar.unat
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_GLOBAL_POINTER, r15
    ;;
    st8 [r20] = r1
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_STACK_POINTER, r15
    ;;
    st8 [r20] = r12
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_THREAD_POINTER, r15
    ;;
    st8 [r20] = r13
    mov r21 = b0
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_INSTRUCTION_POINTER, r15
    ;;
    st8 [r20] = r21
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_PREVIOUS_FUNCTION_STATE, r15
    ;;
    st8 [r20] = r18
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_RSE_CONFIGURATION, r15
    ;;
    st8 [r20] = r16
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_RSE_BACKING_STORE_POINTER, r15
    ;;
    st8 [r20] = r17
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_USER_NAT_COLLECTION, r15
    ;;
    st8 [r20] = r19
    ;;
    mov r16 = ar.lc
    mov r17 = ar.fpsr
    mov r18 = pr
    mov r19 = b1
    mov r21 = b2
    mov r22 = b3
    mov r23 = b4
    mov r24 = b5
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_LOOP_COUNTER, r15
    ;;
    st8 [r20] = r16
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_FLOATING_POINT_STATUS, r15
    ;;
    st8 [r20] = r17
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_PREDICATES, r15
    ;;
    st8 [r20] = r18
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_B1, r15
    ;;
    st8 [r20] = r19
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_B2, r15
    ;;
    st8 [r20] = r21
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_B3, r15
    ;;
    st8 [r20] = r22
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_B4, r15
    ;;
    st8 [r20] = r23
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_B5, r15
    ;;
    st8 [r20] = r24
    ;;
// The zero-sized cover frame becomes the fresh user CFM. The old BSP and
// PFS retained above continue to name the suspended supervisor call.
    cover
    ;;
    flushrs
    ;;
    mov ar.rsc = r0
    ;;
    mov r16 = ar.rnat
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_RSE_NAT_COLLECTION, r15
    ;;
    st8 [r20] = r16
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_R4, r15
    ;;
    st8.spill [r20] = r4, 8
    ;;
    st8.spill [r20] = r5, 8
    ;;
    st8.spill [r20] = r6, 8
    ;;
    st8.spill [r20] = r7, 8
    ;;
    mov r16 = ar.unat
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_PRESERVED_GENERAL_NAT_COLLECTION, r15
    ;;
    st8 [r20] = r16
    ;;
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_FLOATING_REGISTERS, r15
    ;;
    stf.spill [r20] = f2, 16
    ;;
    stf.spill [r20] = f3, 16
    ;;
    stf.spill [r20] = f4, 16
    ;;
    stf.spill [r20] = f5, 16
    ;;
    stf.spill [r20] = f16, 16
    ;;
    stf.spill [r20] = f17, 16
    ;;
    stf.spill [r20] = f18, 16
    ;;
    stf.spill [r20] = f19, 16
    ;;
    stf.spill [r20] = f20, 16
    ;;
    stf.spill [r20] = f21, 16
    ;;
    stf.spill [r20] = f22, 16
    ;;
    stf.spill [r20] = f23, 16
    ;;
    stf.spill [r20] = f24, 16
    ;;
    stf.spill [r20] = f25, 16
    ;;
    stf.spill [r20] = f26, 16
    ;;
    stf.spill [r20] = f27, 16
    ;;
    stf.spill [r20] = f28, 16
    ;;
    stf.spill [r20] = f29, 16
    ;;
    stf.spill [r20] = f30, 16
    ;;
    stf.spill [r20] = f31, 16
    ;;
// Complete every potentially faulting load before publishing or detaching
// supervisor state. Bank-one r23/r24 and r21/r22 carry the clean initial
// arguments for r4/r5 and r6/r7, and r31 carries the initial GP across the
// no-memory cut. The remaining static registers retain commit operands.
    add r23 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_INITIAL_ARGUMENTS_REGISTER4, r14
    add r24 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_INITIAL_ARGUMENTS_REGISTER5, r14
    add r21 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_INITIAL_ARGUMENTS_REGISTER6, r14
    add r22 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_INITIAL_ARGUMENTS_REGISTER7, r14
    add r25 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_USER_RSE_BACKING_STORE_POINTER, r14
    add r26 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_USER_PROCESSOR_STATUS, r14
    add r27 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_USER_RSE_CONFIGURATION, r14
    add r28 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_ENTRY_BUNDLE_ADDRESS, r14
    add r29 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_USER_STACK_POINTER, r14
    add r31 = ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_INITIAL_GLOBAL_POINTER, r14
    ;;
    ld8 r23 = [r23]
    ld8 r24 = [r24]
    ld8 r21 = [r21]
    ld8 r22 = [r22]
    ld8 r25 = [r25]
    ld8 r26 = [r26]
    ld8 r27 = [r27]
    ld8 r28 = [r28]
    ld8 r29 = [r29]
    ld8 r31 = [r31]
    movl r30 = ASTERIA_IA64_USER_MODE_INITIAL_FLOATING_POINT_STATUS
    ;;
// Publish only after every retained byte and user operand is resident.
    mf
    ;;
    mov r16 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_STATE_RETAINED
    add r20 = ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_STATE, r15
    ;;
    st8 [r20] = r16
    ;;
    mf
    ;;
// CR.IIP/IPSR/IFS writes and rfi require both interruption delivery and
// interruption collection disabled. No memory is touched after this cut.
    rsm 0x6000
    ;;
    srlz.d
    ;;
// BSPSTORE makes RNAT undefined. Install the fresh collection afterward,
// sanitize user-visible application state, and select enforced-lazy PL3
// RSE operation before rfi.
    invala
    ;;
// PL3 owns no stacked register yet, and every physical one was last written
// by the supervisor. Scrub the complete file, value and NaT bit, before the
// user can allocate a register: flushrs left no dirty register and
// PAL_RSE_INFO proved one frame spans every physical register, so neither
// alloc moves memory. The second alloc restores the zero-sized cover frame,
// which rfi keeps as the fresh user CFM because cr.ifs is zero.
#if ASTERIA_IA64_USER_MODE_PHYSICAL_STACKED_REGISTERS != 96
#error "the stacked-register scrub clears exactly r32-r127"
#endif
    alloc r16 = ar.pfs, 0, ASTERIA_IA64_USER_MODE_PHYSICAL_STACKED_REGISTERS, 0, 0
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
    alloc r16 = ar.pfs, 0, 0, 0, 0
    ;;
    mov ar.bspstore = r25
    ;;
    mov ar.rnat = r0
    mov ar.unat = r0
    mov ar.pfs = r0
    mov ar.lc = r0
    mov ar.ccv = r0
    mov ar.ec = r0
    mov ar.fpsr = r30
    ;;
    mov ar.rsc = r27
    ;;
    mov cr.iip = r28
    mov cr.ipsr = r26
    mov cr.ifs = r0
    mov r12 = r29
    ;;
// Canonicalize the static integer, predicate, and branch state consumed by
// this fixed probe while retaining only the P64 global pointer and the r4-r7
// launch arguments. The carriers are cleared in the next instruction group,
// after their consumers. PSR.bn remains one, so bank zero stays untouched for
// heavy entry. The runtime refill context intentionally remains in ar.k3.
    mov r1 = r31
    mov r2 = r0
    mov r3 = r0
    mov r4 = r23
    mov r5 = r24
    mov r6 = r21
    mov r7 = r22
    mov r8 = r0
    mov r9 = r0
    mov r10 = r0
    mov r11 = r0
    mov r13 = r0
    mov r14 = r0
    mov r15 = r0
    mov r16 = r0
    mov r17 = r0
    mov r18 = r0
    mov r19 = r0
    mov r20 = r0
    ;;
    mov r21 = r0
    mov r22 = r0
    mov r23 = r0
    mov r24 = r0
    mov r25 = r0
    mov r26 = r0
    mov r27 = r0
    mov r28 = r0
    mov r29 = r0
    mov r30 = r0
    mov r31 = r0
    mov b0 = r0
    mov b1 = r0
    mov b2 = r0
    mov b3 = r0
    mov b4 = r0
    mov b5 = r0
    mov b6 = r0
    mov b7 = r0
    mov pr = r0, -1
    mov ar.unat = r0
    ;;
    rfi
    ;;
.Lasteria_ia64_user_launch_processor_reject:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
.Lasteria_ia64_user_launch_supervisor_reject:
    mov r8 = 2
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64LaunchUserMode, . - AsteriaIa64LaunchUserMode
.explicit
