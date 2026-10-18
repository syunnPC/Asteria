// SPDX-License-Identifier: Apache-2.0

// IA-64 BSP kernel-runtime helpers: worker evidence and descriptors, the
// native-preemption sentinel and its landing, and the kernel idle loop.
// Control offsets and sentinel constants come from the generated C++ layout.
// The native-preemption helpers belong to the one-shot probe and follow the
// rule of Ia64BspKernelRuntime.cpp: a firmware built with
// ASTERIA_IA64_NATIVE_PREEMPTION_PROBE=0 does not contain them.

#include "Generated/Platform/Ia64/Ia64BspKernelRuntimeAsmLayout.h"

#include "Platform/Ia64/Diagnostics/Ia64StopPoll.inc"

.text
.align 16
.global AsteriaIa64CaptureBspKernelWorkerEvidence
.hidden AsteriaIa64CaptureBspKernelWorkerEvidence
.type AsteriaIa64CaptureBspKernelWorkerEvidence, @function
AsteriaIa64CaptureBspKernelWorkerEvidence:
// This leaf has no alloc instruction: r12 and the application-register
// samples describe the worker at the direct C++ capture boundary.
    mov r14 = ar.bsp
    mov r15 = ar.bspstore
    mov r16 = ar.rnat
    mov r17 = ar.rsc
    mov r18 = psr
    ;;
    st8 [r32] = r12, 8
    ;;
    st8 [r32] = r14, 8
    ;;
    st8 [r32] = r15, 8
    ;;
    st8 [r32] = r16, 8
    ;;
    st8 [r32] = r17, 8
    ;;
    st8 [r32] = r18, 8
    ;;
    mov r14 = 1
    mf
    ;;
    st1 [r32] = r14
    ;;
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64CaptureBspKernelWorkerEvidence, . - AsteriaIa64CaptureBspKernelWorkerEvidence
.align 16
.global AsteriaIa64InitializeBspKernelWorkerDescriptor
.hidden AsteriaIa64InitializeBspKernelWorkerDescriptor
.type AsteriaIa64InitializeBspKernelWorkerDescriptor, @function
AsteriaIa64InitializeBspKernelWorkerDescriptor:
// Materialize the raw bundle address, not a P64 function pointer. The
// descriptor lives in the runtime owner and inherits this image's gp.
    movl r14 = _ZN7Asteria8Platform4Ia649Scheduler20Ia64BspKernelRuntime11WorkerEntryEPv
    mov r15 = r1
    ;;
    st8 [r32] = r14, 8
    ;;
    st8 [r32] = r15
    ;;
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64InitializeBspKernelWorkerDescriptor, . - AsteriaIa64InitializeBspKernelWorkerDescriptor
#if !defined(ASTERIA_IA64_NATIVE_PREEMPTION_PROBE) || (ASTERIA_IA64_NATIVE_PREEMPTION_PROBE != 0)
.align 16
.global AsteriaIa64InitializeBspNativePreemptionDescriptor
.hidden AsteriaIa64InitializeBspNativePreemptionDescriptor
.type AsteriaIa64InitializeBspNativePreemptionDescriptor, @function
AsteriaIa64InitializeBspNativePreemptionDescriptor:
    movl r14 = AsteriaIa64BspNativePreemptionCoordinator
    mov r15 = r1
    ;;
    movl r16 = AsteriaIa64BspNativePreemptionSentinelLandingStart
    movl r17 = AsteriaIa64BspNativePreemptionSentinelLandingEnd
    ;;
    movl r18 = AsteriaIa64BspNativePreemptionIdleLanding
    ;;
    st8 [r32] = r14, 8
    ;;
    st8 [r32] = r15, 8
    ;;
    st8 [r32] = r16, 8
    ;;
    st8 [r32] = r17, 8
    ;;
    st8 [r32] = r18, 8
    ;;
    st8 [r32] = r15
    ;;
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending return bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64InitializeBspNativePreemptionDescriptor, . - AsteriaIa64InitializeBspNativePreemptionDescriptor
.align 16
.global AsteriaIa64CaptureBspNativePreemptionObservation
.hidden AsteriaIa64CaptureBspNativePreemptionObservation
.type AsteriaIa64CaptureBspNativePreemptionObservation, @function
AsteriaIa64CaptureBspNativePreemptionObservation:
    movl r14 = AsteriaIa64BspNativePreemptionSentinelLandingStart
    mov r15 = psr
    ;;
    st8 [r32] = r14, 8
    ;;
    st8 [r32] = r15, 8
    ;;
    st8 [r32] = r12, 8
    ;;
    mov r14 = ar.rsc
    mov r15 = ar.bsp
    ;;
    st8 [r32] = r14, 8
    ;;
    st8 [r32] = r15, 8
    ;;
    mov r14 = ar.bspstore
    mov r15 = cr.itm
    ;;
    st8 [r32] = r14, 8
    ;;
    st8 [r32] = r15
    ;;
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64CaptureBspNativePreemptionObservation, . - AsteriaIa64CaptureBspNativePreemptionObservation
.align 16
.global AsteriaIa64RunBspNativePreemptionSentinel
.hidden AsteriaIa64RunBspNativePreemptionSentinel
.type AsteriaIa64RunBspNativePreemptionSentinel, @function
AsteriaIa64RunBspNativePreemptionSentinel:
// This no-alloc sentinel admits only one exact BSP PL0 stack/RBS and the
// architected srlz.d sampling bundle. Any contradiction after arm is
// fail-stop.
.Lasteria_ia64_bsp_preemption_attempt:
    cmp.eq p6, p7 = r32, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    ;;
    ld8.acq r14 = [r32]
    add r15 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE, r32
    ;;
    ld8.acq r15 = [r15]
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_MODE_BSP_ONE_SHOT_PREEMPTION
    ;;
    cmp.ne p6, p7 = r14, r16
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_ARMED
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    cmp.ne p6, p7 = r15, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    ;;
    add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_PROCESSOR_INDEX, r32
    ;;
    ld8 r15 = [r14], 8
    ;;
    ld8 r16 = [r14]
    ;;
    cmp.ne p6, p7 = r15, r0
    mov r17 = ASTERIA_IA64_EXTERNAL_INTERRUPT_TIMER_VECTOR
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    ;;
    add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_SENTINEL_ENTRY_POINT, r32
    ;;
    ld8 r15 = [r14], 8
    ;;
    ld8 r16 = [r14]
    movl r17 = AsteriaIa64BspNativePreemptionSentinelLandingStart
    ;;
    cmp.ne p6, p7 = r15, r17
    movl r17 = AsteriaIa64BspNativePreemptionSentinelLandingEnd
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    cmp.ne p6, p7 = r16, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    ;;
    add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_WORKER_MEMORY_STACK_START, r32
    ;;
    ld8 r15 = [r14], 8
    ;;
    ld8 r16 = [r14]
    ;;
    cmp.ltu p6, p7 = r12, r15
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    cmp.geu p6, p7 = r12, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    ;;
    mov r14 = psr
    movl r15 = ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_PROCESSOR_STATUS_MASK
    ;;
    and r14 = r14, r15
    movl r15 = ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_PROCESSOR_STATUS_VALUE
    ;;
    cmp.ne p6, p7 = r14, r15
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    ;;
    mov r14 = ar.rsc
    ;;
    cmp.ne p6, p7 = r14, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    ;;
    mov r14 = ar.bspstore
    mov r15 = ar.bsp
    ;;
    add r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_WORKER_RSE_BACKING_STORE_START, r32
    ;;
    ld8 r17 = [r16], 8
    ;;
    ld8 r18 = [r16]
    ;;
    cmp.ltu p6, p7 = r14, r17
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    cmp.geu p6, p7 = r14, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    cmp.ltu p6, p7 = r15, r14
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    cmp.geu p6, p7 = r15, r18
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    sub r15 = r15, r14
    mov r16 = ASTERIA_IA64_EXTERNAL_PREEMPTION_COORDINATOR_RSE_TRANSITION_SIZE_BYTES
    ;;
    cmp.geu p6, p7 = r15, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    ;;
    mov r14 = cr.itm
    add r15 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_ARMED_DEADLINE_TICKS, r32
    ;;
    ld8 r15 = [r15]
    ;;
    cmp.ne p6, p7 = r14, r15
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    cmp.eq p6, p7 = r14, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    movl r15 = -1
    ;;
    cmp.eq p6, p7 = r14, r15
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_bsp_preemption_reject
    mf
    ;;
    ssm psr.i
    ;;
.align 16
.global AsteriaIa64BspNativePreemptionSentinelLandingStart
.hidden AsteriaIa64BspNativePreemptionSentinelLandingStart
AsteriaIa64BspNativePreemptionSentinelLandingStart:
    srlz.d
    ;;
.global AsteriaIa64BspNativePreemptionSentinelLandingEnd
.hidden AsteriaIa64BspNativePreemptionSentinelLandingEnd
AsteriaIa64BspNativePreemptionSentinelLandingEnd:
    rsm psr.i
    ;;
    srlz.d
    ;;
    add r14 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE, r32
    ;;
    ld8.acq r14 = [r14]
    ;;
    mov r15 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_COMPLETE
    ;;
    cmp.eq p6, p7 = r14, r15
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_bsp_preemption_success
    mov r15 = ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_ARMED
    ;;
    cmp.eq p6, p7 = r14, r15
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_bsp_preemption_attempt
    ;;
.Lasteria_ia64_bsp_preemption_reject:
    rsm psr.i
    ;;
    srlz.d
    ;;
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
.Lasteria_ia64_bsp_preemption_success:
    mov r8 = r0
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64RunBspNativePreemptionSentinel, . - AsteriaIa64RunBspNativePreemptionSentinel
#endif
.align 16
.global AsteriaIa64KernelIdle
.hidden AsteriaIa64KernelIdle
.type AsteriaIa64KernelIdle, @function
AsteriaIa64KernelIdle:
    rsm psr.i
    ;;
    srlz.d
    ;;
.Lasteria_ia64_kernel_idle:
    AsteriaIa64PollStop 0
    hint @pause
    ;;
    br.cond.sptk .Lasteria_ia64_kernel_idle
    ;;
// Force GAS to emit the pending bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64KernelIdle, . - AsteriaIa64KernelIdle
