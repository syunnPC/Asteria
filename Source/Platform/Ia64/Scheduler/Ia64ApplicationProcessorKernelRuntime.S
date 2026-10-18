// SPDX-License-Identifier: Apache-2.0

// IA-64 application-processor kernel-runtime helpers: the register evidence
// capture, the worker descriptor and the local-timer task-priority install.

.text
.align 16
.global AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence
.hidden AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence
.type AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence, @function
AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence:
// This leaf intentionally has no alloc. r12 and the five architectural
// register samples describe the direct caller boundary.
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
// Force GAS to emit the pending return bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence, . - AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence
.align 16
.global AsteriaIa64InitializeApplicationProcessorKernelWorkerDescriptor
.hidden AsteriaIa64InitializeApplicationProcessorKernelWorkerDescriptor
.type AsteriaIa64InitializeApplicationProcessorKernelWorkerDescriptor, @function
AsteriaIa64InitializeApplicationProcessorKernelWorkerDescriptor:
// movl materializes the raw bundle address. No C++ P64 function pointer is
// formed on target, so this path does not request an FPTR relocation.
    movl r14 = AsteriaIa64ApplicationProcessorKernelRuntimeWorkerEntry
    mov r15 = r1
    ;;
    st8 [r32] = r14, 8
    ;;
    st8 [r32] = r15
    ;;
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending return bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64InitializeApplicationProcessorKernelWorkerDescriptor, . - AsteriaIa64InitializeApplicationProcessorKernelWorkerDescriptor
.align 16
.global AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority
.hidden AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority
.type AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority, @function
AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority:
// SAL rendezvous scratch state is not trusted. Install the fixed
// processor-wide timer priority and prove the exact cr.tpr readback without a
// stack, RSE frame, memory access, IVR/IRR drain, or EOI.
    mov r14 = 0xd0
    ;;
    mov cr.tpr = r14
    ;;
    srlz.d
    ;;
    mov r8 = cr.tpr
    ;;
    xor r8 = r8, r14
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority, . - AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority
