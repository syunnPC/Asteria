// SPDX-License-Identifier: Apache-2.0

// Entry descriptor of the IA-64 boot pager's kernel context: the raw entry
// bundle and this image's gp, materialized at run time like the BSP worker's
// (Ia64BspKernelRuntime.S) so the descriptor lives in the runtime owner.

.text
.align 16
.global AsteriaIa64InitializePagerEntryDescriptor
.hidden AsteriaIa64InitializePagerEntryDescriptor
.type AsteriaIa64InitializePagerEntryDescriptor, @function
AsteriaIa64InitializePagerEntryDescriptor:
    movl r14 = AsteriaIa64PagerRuntimeEntry
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
.size AsteriaIa64InitializePagerEntryDescriptor, . - AsteriaIa64InitializePagerEntryDescriptor
