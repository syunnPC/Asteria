// SPDX-License-Identifier: Apache-2.0

// IA-64 runtime interrupt vector table: the ar.k3/cr.iva install leaf, its
// fail-stop and address query, and the 32 KiB table with the alternate
// instruction/data TLB refill handlers, the data handler's user-fault escape,
// and the direct break, external interrupt and page-fault trampolines. Table
// geometry, context and ledger offsets, limits and the user-fault selector
// come from the generated C++ layout.

#include "Generated/Platform/Ia64/Ia64RuntimeInterruptVectorTableAsmLayout.h"
#include "Generated/Platform/Ia64/Ia64StopAsmLayout.h"
#include "Generated/Platform/Ia64/Ia64UserFaultEntryAsmLayout.h"

// Explicit groups: in.auto mode GAS ignores `; ` and placed the srlz.i in the group of the cr.iva
// write, where it serializes nothing.
.explicit
.text
.align 16
.global AsteriaIa64InstallRuntimeInterruptVectorTable
.hidden AsteriaIa64InstallRuntimeInterruptVectorTable
.type AsteriaIa64InstallRuntimeInterruptVectorTable, @function
AsteriaIa64InstallRuntimeInterruptVectorTable:
// The P64 arguments are the validated context and fixed table addresses.
// This leaf has no alloc, stack access, store, or GP dependency.
    mov r8 = psr
// CPL0 and PSR.vm=0 are caller preconditions because mov-from-PSR does
// not expose those high privileged fields.
    movl r14 = 0x000000180F064002
    ;;
    and r14 = r8, r14
    ;;
    cmp.ne p6, p7 = r14, r0
(p6) br.cond.dpnt.few 1f
// Ar.k3 is committed only after the final SAL handoff. cr.iva follows; either readback mismatch is
// terminal because native state changed.
    mov ar.k3 = r32
    mov cr.iva = r33
    ;;
    srlz.i
    ;;
    mov r14 = ar.k3
    mov r15 = cr.iva
    ;;
    cmp.ne p6, p7 = r14, r32
(p6) br.cond.dpnt.few 2f
    ;;
    cmp.ne p6, p7 = r15, r33
(p6) br.cond.dpnt.few 2f
    ;;
    mov r8 = r0
    br.ret.sptk.many b0
    ;;
1:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
2:
    mov r16 = 0
    br.sptk.many AsteriaIa64StopEntry
    ;;
.align 16
.size AsteriaIa64InstallRuntimeInterruptVectorTable, . - AsteriaIa64InstallRuntimeInterruptVectorTable
.auto
.align 16
.global AsteriaIa64FailStopRuntimeInterruptVectorTableInstall
.hidden AsteriaIa64FailStopRuntimeInterruptVectorTableInstall
.type AsteriaIa64FailStopRuntimeInterruptVectorTableInstall, @function
AsteriaIa64FailStopRuntimeInterruptVectorTableInstall:
3:
    mov r16 = 0
    br.sptk.many AsteriaIa64StopEntry
.align 16
.size AsteriaIa64FailStopRuntimeInterruptVectorTableInstall, . - AsteriaIa64FailStopRuntimeInterruptVectorTableInstall
// A direct IMM64 address avoids a GOT/short-data dependency in the
// Region-0 nucleus final link.
.align 16
.global AsteriaIa64GetRuntimeInterruptVectorTableAddress
.hidden AsteriaIa64GetRuntimeInterruptVectorTableAddress
.type AsteriaIa64GetRuntimeInterruptVectorTableAddress, @function
AsteriaIa64GetRuntimeInterruptVectorTableAddress:
    movl r8 = AsteriaIa64RuntimeInterruptVectorTableImage
    br.ret.sptk.many b0
.align 16
.size AsteriaIa64GetRuntimeInterruptVectorTableAddress, . - AsteriaIa64GetRuntimeInterruptVectorTableAddress
.macro AsteriaIa64RuntimeFailStopBundle
    .byte 0x11, 0x00, 0x00, 0x00
    .byte 0x01, 0x00, 0x00, 0x00
    .byte 0x00, 0x02, 0x00, 0x00
    .byte 0x00, 0x00, 0x00, 0x40
.endm
// One stop trampoline at each otherwise unused architected vector. Fixed
// bundle counts avoid GAS's deferred branch-fragment counter inside.rept.
.macro AsteriaIa64RuntimeUnusedSlot bytes
.explicit
{ .mib
    nop.m 0
    mov r16 = ip
    br.sptk.many AsteriaIa64StopFromIvt
}
.auto
    .rept (\bytes - 16) / 16
    AsteriaIa64RuntimeFailStopBundle
    .endr
.endm
// Each expansion is self-contained in one architected 0x400-byte slot.
// It uses only bank-0 GR16-GR31, direct local branches, and no stores,
// calls, stack, RSE, allocator, lock, callback, purge, or recovery path.
// The data expansion alone has one exit besides rfi: a PL3 access that is
// not an RSE reference and finds no ledger record branches, PR restored, to
// AsteriaIa64UserFaultEntry.
.macro AsteriaIa64RuntimeRefillHandler instruction
// The native installer and private admission path establish the immutable
// context, corridor, slot, and published-record invariants. The miss path
// retains only checks needed to use bank 0 safely and bound concurrent
// publication.
    mov r16 = pr
    mov r31 = psr
    mov r30 = cr.ipsr
    mov r29 = cr.isr
    movl r28 = 0x000000000000E000
    and r28 = r31, r28
    movl r27 = 0x0000001008020000
    xor r26 = r31, r30
    and r26 = r26, r27
    or r28 = r28, r26
    and r26 = r31, r27
    xor r26 = r26, r27
    or r28 = r28, r26
    movl r27 = 0x0000101000002000
    and r26 = r30, r27
    xor r26 = r26, r27
    or r28 = r28, r26
    movl r27 = 0x0000400400008000
    and r26 = r30, r27
    or r28 = r28, r26
.if \instruction
    movl r27 = 0x000000E000000000
    and r26 = r29, r27
.else
// A data miss may originate from the RSE. In that case ISR.ir is its
// architected companion and is safe to admit; isolated ir is not.
    movl r27 = 0x0000008000000000
    and r26 = r29, r27
    shl r27 = r29, 1
    andcm r27 = r29, r27
    movl r31 = 0x0000004000000000
    and r27 = r27, r31
    or r26 = r26, r27
.endif
    or r28 = r28, r26
    cmp.ne p6, p7 = r28, r0
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
// Ar.k3 names the per-CPU immutable context. Derive the acquire slot directly from its binding-wire
// base and exact processor index.
    mov r17 = ar.k3
    movl r27 = 0x2000000000000000
    cmp.eq p6, p7 = r17, r0
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    and r28 = ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_ALIGNMENT_MASK, r17
    cmp.ne p6, p7 = r28, r0
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    cmp.geu p6, p7 = r17, r27
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
AsteriaIa64RuntimeRefillStopPollStart\@:
    add r28 = ASTERIA_IA64_STOP_CONTEXT_STORAGE, r17
    ld8 r28 = [r28]
    ld8.acq r28 = [r28]
    cmp.ne p6, p7 = r28, r0
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
AsteriaIa64RuntimeRefillStopPollEnd\@:
    ld8 r20 = [r17]
    add r28 = ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_CONTEXT_PROCESSOR_INDEX, r17
    ld8 r21 = [r28]
    mov r28 = ASTERIA_MAXIMUM_PROCESSOR_COUNT
    cmp.geu p6, p7 = r21, r28
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    cmp.eq p6, p7 = r20, r0
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    and r28 = ASTERIA_IA64_ACTIVE_RUNTIME_TRANSLATION_BINDINGS_WIRE_STORAGE_ALIGNMENT_MASK, r20
    cmp.ne p6, p7 = r28, r0
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    cmp.geu p6, p7 = r20, r27
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    shl r21 = r21, 3
    add r21 = r20, r21
    ld8.acq r22 = [r21]
    mov r28 = 1
    cmp.leu p6, p7 = r22, r28
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    and r28 = ASTERIA_IA64_RUNTIME_TRANSLATION_LEDGER_WIRE_STORAGE_ALIGNMENT_MASK, r22
    cmp.ne p6, p7 = r28, r0
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    cmp.geu p6, p7 = r22, r27
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
// Publication 1 means an empty immutable prefix; 257 means all 256
// records. Any other value outside that closed interval is rejected.
    add r28 = ASTERIA_IA64_RUNTIME_TRANSLATION_LEDGER_WIRE_STORAGE_PUBLICATION_WORD, r22
    ld8.acq r23 = [r28]
    mov r27 = ASTERIA_IA64_RUNTIME_TRANSLATION_LEDGER_MAXIMUM_PUBLICATION
    cmp.eq p6, p7 = r23, r0
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    cmp.gtu p6, p7 = r23, r27
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    add r23 = -1, r23
    add r25 = ASTERIA_IA64_RUNTIME_TRANSLATION_LEDGER_WIRE_STORAGE_RECORDS + 16, r22
    mov r24 = cr.ifa
    dep r24 = r0, r24, 0, 12
AsteriaIa64RuntimeRefillScan\@:
    cmp.eq p6, p7 = r23, r0
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillMiss\@
// Count first, acquire, then the two payload words.
    ld8.acq r28 = [r25], -16
    ld8 r26 = [r25], 8
    ld8 r27 = [r25], 32
    cmp.ltu p6, p7 = r24, r26
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillNext\@
    sub r29 = r24, r26
    shr.u r29 = r29, 12
    cmp.geu p6, p7 = r29, r28
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillNext\@
    shl r29 = r29, 12
    movl r30 = 0x0003FFFFFFFFF000
    and r31 = r27, r30
    andcm r27 = r27, r30
    add r31 = r31, r29
    or r27 = r31, r27
    mov r30 = ASTERIA_IA64_RUNTIME_TRANSLATION_INSERTION
    mov cr.ifa = r24
    mov cr.itir = r30
.explicit
    ;;
.if \instruction
    itc.i r27
    ;;
    srlz.i
.else
    itc.d r27
    ;;
    srlz.d
.endif
    ;;
    mov pr = r16, -1
    ;;
    rfi
    ;;
.auto
AsteriaIa64RuntimeRefillNext\@:
    add r23 = -1, r23
    br.cond.sptk.few AsteriaIa64RuntimeRefillScan\@
// The ledger contains no mapping. A native PL3 access, including mandatory
// RSE traffic, is captured on its thread's storage. Kernel misses stop.
AsteriaIa64RuntimeRefillMiss\@:
    mov r30 = cr.ipsr
    extr.u r28 = r30, 32, 2
    cmp.ne p6, p7 = 3, r28
(p6) br.cond.dpnt.few AsteriaIa64RuntimeRefillFail\@
    mov pr = r16, -1
.explicit
{ .mib
.if \instruction
    mov.m ar.k4 = ASTERIA_IA64_USER_VECTOR_ALTERNATE_INSTRUCTION_TLB >> ASTERIA_IA64_USER_EXCEPTION_VECTOR_SHIFT
    mov r16 = ASTERIA_IA64_USER_EXCEPTION_SELECTOR
    br.sptk.many AsteriaIa64UserExceptionEntry
.else
    nop.m 0
    mov r16 = ASTERIA_IA64_RUNTIME_USER_FAULT_SELECTOR
    br.sptk.many AsteriaIa64UserFaultEntry
.endif
}
.auto
// The vector word names this exit (Ia64StopExit::RefillHandler), so the stop record tells a refill
// failure from the user entries' at 0x1000.
AsteriaIa64RuntimeRefillFail\@:
.explicit
{ .mib
    nop.m 0
.if \instruction
    mov r16 = ASTERIA_IA64_STOP_INSTRUCTION_MISS_VECTOR | (ASTERIA_IA64_STOP_EXIT_REFILL_HANDLER << ASTERIA_IA64_STOP_EXIT_SHIFT)
.else
    mov r16 = ASTERIA_IA64_STOP_DATA_MISS_VECTOR | (ASTERIA_IA64_STOP_EXIT_REFILL_HANDLER << ASTERIA_IA64_STOP_EXIT_SHIFT)
.endif
    br.sptk.many AsteriaIa64StopEntry
}
.auto
.endm
.macro AsteriaIa64RuntimeUserSlot name, offset, bytes
AsteriaIa64Runtime\name\()Start:
    .if (AsteriaIa64Runtime\name\()Start - AsteriaIa64RuntimeInterruptVectorTableImage) != \offset
    .error "user vector offset drifted"
    .endif
.explicit
{ .mib
    mov.m ar.k4 = \offset >> ASTERIA_IA64_USER_EXCEPTION_VECTOR_SHIFT
    mov r16 = ASTERIA_IA64_USER_EXCEPTION_SELECTOR
    br.sptk.many AsteriaIa64UserExceptionEntry
}
.auto
AsteriaIa64Runtime\name\()TrampolineEnd:
    .rept (\bytes - 16) / 16
    AsteriaIa64RuntimeFailStopBundle
    .endr
AsteriaIa64Runtime\name\()End:
.endm
.section .text.ia64_runtime_ivt, "ax", @progbits
.balign ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_SIZE_BYTES
.global AsteriaIa64RuntimeInterruptVectorTableImage
.type AsteriaIa64RuntimeInterruptVectorTableImage, @object
AsteriaIa64RuntimeInterruptVectorTableImage:
    AsteriaIa64RuntimeUnusedSlot ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot InstructionTlb, ASTERIA_IA64_USER_VECTOR_INSTRUCTION_TLB, ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot DataTlb, ASTERIA_IA64_USER_VECTOR_DATA_TLB, ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
AsteriaIa64RuntimeAlternateInstructionTlbStart:
    .if (AsteriaIa64RuntimeAlternateInstructionTlbStart - AsteriaIa64RuntimeInterruptVectorTableImage) != ASTERIA_IA64_RUNTIME_ALTERNATE_INSTRUCTION_TLB_OFFSET
    .error "alternate instruction TLB vector offset drifted"
    .endif
    AsteriaIa64RuntimeRefillHandler 1
AsteriaIa64RuntimeAlternateInstructionTlbEnd:
    .if ((AsteriaIa64RuntimeAlternateInstructionTlbEnd - AsteriaIa64RuntimeAlternateInstructionTlbStart) & 15) != 0
    .error "alternate instruction TLB handler is not bundle-sized"
    .endif
    .if (AsteriaIa64RuntimeAlternateInstructionTlbEnd - AsteriaIa64RuntimeAlternateInstructionTlbStart) > ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    .error "alternate instruction TLB handler exceeds its slot"
    .endif
// GAS defers the final explicit terminal bundle while evaluating.rept.
// Account for it here; Verify checks the linked handler and complete slot.
    .rept (ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES - (AsteriaIa64RuntimeAlternateInstructionTlbEnd - AsteriaIa64RuntimeAlternateInstructionTlbStart) - 16) / 16
    AsteriaIa64RuntimeFailStopBundle
    .endr
AsteriaIa64RuntimeAlternateDataTlbStart:
    .if (AsteriaIa64RuntimeAlternateDataTlbStart - AsteriaIa64RuntimeInterruptVectorTableImage) != ASTERIA_IA64_RUNTIME_ALTERNATE_DATA_TLB_OFFSET
    .error "alternate data TLB vector offset drifted"
    .endif
    AsteriaIa64RuntimeRefillHandler 0
AsteriaIa64RuntimeAlternateDataTlbEnd:
    .if ((AsteriaIa64RuntimeAlternateDataTlbEnd - AsteriaIa64RuntimeAlternateDataTlbStart) & 15) != 0
    .error "alternate data TLB handler is not bundle-sized"
    .endif
    .if (AsteriaIa64RuntimeAlternateDataTlbEnd - AsteriaIa64RuntimeAlternateDataTlbStart) > ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    .error "alternate data TLB handler exceeds its slot"
    .endif
    .rept (ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES - (AsteriaIa64RuntimeAlternateDataTlbEnd - AsteriaIa64RuntimeAlternateDataTlbStart) - 16) / 16
    AsteriaIa64RuntimeFailStopBundle
    .endr
AsteriaIa64RuntimeAlternateDataTlbSlotEnd:
    .if (AsteriaIa64RuntimeAlternateDataTlbSlotEnd - AsteriaIa64RuntimeInterruptVectorTableImage) != (ASTERIA_IA64_RUNTIME_ALTERNATE_DATA_TLB_OFFSET + ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES)
    .error "alternate data TLB slot end drifted"
    .endif
    AsteriaIa64RuntimeUnusedSlot ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot InstructionKeyMiss, ASTERIA_IA64_USER_VECTOR_INSTRUCTION_KEY_MISS, ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot DataKeyMiss, ASTERIA_IA64_USER_VECTOR_DATA_KEY_MISS, ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot DirtyBit, ASTERIA_IA64_USER_VECTOR_DIRTY_BIT, ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot InstructionAccessBit, ASTERIA_IA64_USER_VECTOR_INSTRUCTION_ACCESS_BIT, ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot DataAccessBit, ASTERIA_IA64_USER_VECTOR_DATA_ACCESS_BIT, ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
AsteriaIa64RuntimeBreakStart:
    .if (AsteriaIa64RuntimeBreakStart - AsteriaIa64RuntimeInterruptVectorTableImage) != ASTERIA_IA64_RUNTIME_BREAK_OFFSET
    .error "break vector offset drifted"
    .endif
.explicit
{ .mib
    nop.m 0
    nop.i 0
    br.sptk.many AsteriaIa64UserBreakEntry
}
.auto
AsteriaIa64RuntimeBreakEnd:
    .if ((AsteriaIa64RuntimeBreakEnd - AsteriaIa64RuntimeBreakStart) & 15) != 0
    .error "break trampoline is not bundle-sized"
    .endif
    .if (AsteriaIa64RuntimeBreakEnd - AsteriaIa64RuntimeBreakStart) > ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    .error "break trampoline exceeds its slot"
    .endif
// The explicit MIB trampoline above is exactly one bundle. An external
// branch relocation is relaxed after.rept expressions, so keep the
// remaining architected 63 bundles explicit and verify the slot end.
    .rept (ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES - 16) / 16
    AsteriaIa64RuntimeFailStopBundle
    .endr
AsteriaIa64RuntimeBreakSlotEnd:
    .if (AsteriaIa64RuntimeBreakSlotEnd - AsteriaIa64RuntimeInterruptVectorTableImage) != (ASTERIA_IA64_RUNTIME_BREAK_OFFSET + ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES)
    .error "break slot end drifted"
    .endif
AsteriaIa64RuntimeExternalInterruptStart:
    .if (AsteriaIa64RuntimeExternalInterruptStart - AsteriaIa64RuntimeInterruptVectorTableImage) != ASTERIA_IA64_RUNTIME_EXTERNAL_INTERRUPT_OFFSET
    .error "external interrupt vector offset drifted"
    .endif
.explicit
{ .mib
    nop.m 0
    nop.i 0
    br.sptk.many AsteriaIa64ExternalInterruptDispatchEntry
}
.auto
AsteriaIa64RuntimeExternalInterruptEnd:
    .if ((AsteriaIa64RuntimeExternalInterruptEnd - AsteriaIa64RuntimeExternalInterruptStart) & 15) != 0
    .error "external interrupt trampoline is not bundle-sized"
    .endif
    .if (AsteriaIa64RuntimeExternalInterruptEnd - AsteriaIa64RuntimeExternalInterruptStart) > ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    .error "external interrupt trampoline exceeds its slot"
    .endif
    .rept (ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES - 16) / 16
    AsteriaIa64RuntimeFailStopBundle
    .endr
AsteriaIa64RuntimeExternalInterruptSlotEnd:
    .if (AsteriaIa64RuntimeExternalInterruptSlotEnd - AsteriaIa64RuntimeInterruptVectorTableImage) != (ASTERIA_IA64_RUNTIME_EXTERNAL_INTERRUPT_OFFSET + ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES)
    .error "external interrupt slot end drifted"
    .endif
    .rept (ASTERIA_IA64_RUNTIME_PAGE_NOT_PRESENT_OFFSET - (AsteriaIa64RuntimeExternalInterruptSlotEnd - AsteriaIa64RuntimeInterruptVectorTableImage)) / ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUnusedSlot ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    .endr
// Every supported native PL3 fault/trap uses the complete thread entry.
    AsteriaIa64RuntimeUserSlot PageNotPresent, ASTERIA_IA64_USER_VECTOR_PAGE_NOT_PRESENT, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot KeyPermission, ASTERIA_IA64_USER_VECTOR_KEY_PERMISSION, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot InstructionAccessRights, ASTERIA_IA64_USER_VECTOR_INSTRUCTION_ACCESS_RIGHTS, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot DataAccessRights, ASTERIA_IA64_USER_VECTOR_DATA_ACCESS_RIGHTS, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot GeneralException, ASTERIA_IA64_USER_VECTOR_GENERAL_EXCEPTION, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot DisabledFloatingPointRegister, ASTERIA_IA64_USER_VECTOR_DISABLED_FLOATING_POINT_REGISTER, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot NatConsumption, ASTERIA_IA64_USER_VECTOR_NAT_CONSUMPTION, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot Speculation, ASTERIA_IA64_USER_VECTOR_SPECULATION, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUnusedSlot ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot Debug, ASTERIA_IA64_USER_VECTOR_DEBUG, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot UnalignedReference, ASTERIA_IA64_USER_VECTOR_UNALIGNED_REFERENCE, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot UnsupportedDataReference, ASTERIA_IA64_USER_VECTOR_UNSUPPORTED_DATA_REFERENCE, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot FloatingPointFault, ASTERIA_IA64_USER_VECTOR_FLOATING_POINT_FAULT, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot FloatingPointTrap, ASTERIA_IA64_USER_VECTOR_FLOATING_POINT_TRAP, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot LowerPrivilegeTransfer, ASTERIA_IA64_USER_VECTOR_LOWER_PRIVILEGE_TRANSFER, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot TakenBranch, ASTERIA_IA64_USER_VECTOR_TAKEN_BRANCH, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUserSlot SingleStep, ASTERIA_IA64_USER_VECTOR_SINGLE_STEP, ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    .rept (ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_SIZE_BYTES - (AsteriaIa64RuntimeSingleStepEnd - AsteriaIa64RuntimeInterruptVectorTableImage)) / ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    AsteriaIa64RuntimeUnusedSlot ASTERIA_IA64_RUNTIME_HEAVYWEIGHT_INTERRUPT_VECTOR_SLOT_SIZE_BYTES
    .endr
AsteriaIa64RuntimeInterruptVectorTableImageEnd:
    .if (AsteriaIa64RuntimeInterruptVectorTableImageEnd - AsteriaIa64RuntimeInterruptVectorTableImage) != ASTERIA_IA64_RUNTIME_INTERRUPT_VECTOR_TABLE_SIZE_BYTES
    .error "runtime interrupt vector table size drifted"
    .endif
.size AsteriaIa64RuntimeInterruptVectorTableImage, AsteriaIa64RuntimeInterruptVectorTableImageEnd - AsteriaIa64RuntimeInterruptVectorTableImage
.explicit
.text
