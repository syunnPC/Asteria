// SPDX-License-Identifier: Apache-2.0

// Entry descriptor of a created IA-64 thread's kernel context: the raw entry
// bundle and this image's gp, materialized at run time like the pager's
// (Ia64PagerRuntime.S) so the descriptor lives in the slot.

.text
.align 16
.global AsteriaIa64InitializeUserThreadEntryDescriptor
.hidden AsteriaIa64InitializeUserThreadEntryDescriptor
.type AsteriaIa64InitializeUserThreadEntryDescriptor, @function
AsteriaIa64InitializeUserThreadEntryDescriptor:
    movl r14 = AsteriaIa64UserThreadEntry
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
.size AsteriaIa64InitializeUserThreadEntryDescriptor, . - AsteriaIa64InitializeUserThreadEntryDescriptor
