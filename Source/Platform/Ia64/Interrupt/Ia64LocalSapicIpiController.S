// SPDX-License-Identifier: Apache-2.0

// Processor Interrupt Block delivery store of the IA-64 Local SAPIC IPI
// controller.

.text
.align 16
.global AsteriaIa64WriteProcessorInterruptBlock
.hidden AsteriaIa64WriteProcessorInterruptBlock
.type AsteriaIa64WriteProcessorInterruptBlock, @function
AsteriaIa64WriteProcessorInterruptBlock:
// The full fence makes preceding coherent mailbox stores globally
// ordered before the uncached Processor Interrupt Block write.
    mf
    ;;
    st8 [r32] = r33
    ;;
// An accepting fence completes this MMIO operation before a following
// invocation can issue another delivery store.
    mf.a
    ;;
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64WriteProcessorInterruptBlock, . - AsteriaIa64WriteProcessorInterruptBlock
