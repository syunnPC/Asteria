// SPDX-License-Identifier: Apache-2.0

// IA-64 cr.iva install leaves for the prepared initial interrupt vector
// table on the BSP and on an application processor. The AP status values
// come from the generated InitialInterruptVectorTableApInstallStatus layout.

#include "Generated/Platform/Ia64/InitialInterruptVectorTableInstallerAsmLayout.h"

.text
.align 16
.global AsteriaIa64InstallInitialInterruptVectorTable
.hidden AsteriaIa64InstallInitialInterruptVectorTable
.type AsteriaIa64InstallInitialInterruptVectorTable, @function
AsteriaIa64InstallInitialInterruptVectorTable:
    mov r8 = psr
    ;;
// Reject every architecturally visible state incompatible with a physical
// mode, interruption-disabled cr.iva update. CPL0 and the inherited SAL
// register bank remain caller preconditions because mov-from-PSR does not
// expose every inherited PSR field.
    movl r14 = 0x0000401B0F064002
    ;;
    and r14 = r8, r14
    ;;
    cmp.ne p6, p7 = r14, r0
    ;;
(p6) br.cond.dpnt.few 1f
    ;;
// The resource write, serializer, and dependent readback occupy three
// distinct instruction groups as required by the IA-64 serialization
// contract. srlz.i also provides the data serialization needed by the
// subsequent read of the same control register.
    mov cr.iva = r32
    ;;
    srlz.i
    ;;
    mov r14 = cr.iva
    ;;
    cmp.ne p6, p7 = r14, r32
    ;;
(p6) br.cond.dpnt.few 2f
    ;;
    mov r8 = r0
    br.ret.sptk.many b0
    ;;
1:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
// cr.iva has already changed on this edge, so returning an ordinary error
// would expose partially committed processor state.
2:
    hint @pause
    ;;
    br.cond.sptk 2b
    ;;
.align 16
.size AsteriaIa64InstallInitialInterruptVectorTable, . - AsteriaIa64InstallInitialInterruptVectorTable
.text
.align 16
.global AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable
.hidden AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable
.type AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable, @function
AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable:
    mov r8 = psr
;;
// As on the BSP path, CPL0 and the inherited SAL register bank remain
// caller preconditions because mov-from-PSR cannot expose every field.
    movl r14 = 0x0000401B0F064002
;;
    and r14 = r8, r14
;;
    cmp.ne p6, p7 = r14, r0
;;
(p6) br.cond.dpnt.few 1f
;;
    mov cr.iva = r32
;;
    srlz.i
;;
    mov r14 = cr.iva
;;
    cmp.ne p6, p7 = r14, r32
;;
(p6) br.cond.dpnt.few 2f
;;
    mov r8 = r0
    br.ret.sptk.many b0
;;
1:
    mov r8 = ASTERIA_INITIAL_INTERRUPT_VECTOR_TABLE_AP_INSTALL_STATUS_UNSUPPORTED_PROCESSOR_STATE
    br.ret.sptk.many b0
;;
// Unlike the BSP adapter, this edge returns only to the fixed noreturn AP
// continuation, which publishes failure and parks instead of returning to
// SAL with committed processor state.
2:
    mov r8 = ASTERIA_INITIAL_INTERRUPT_VECTOR_TABLE_AP_INSTALL_STATUS_COMMITTED_READBACK_MISMATCH
    br.ret.sptk.many b0
;;
.align 16
.size AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable, . - AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable

// Resolve the table through a relocatable IMM64, without a GOT data address.
.text
.align 16
.global AsteriaIa64GetBootstrapInterruptVectorTableAddress
.hidden AsteriaIa64GetBootstrapInterruptVectorTableAddress
.type AsteriaIa64GetBootstrapInterruptVectorTableAddress, @function
AsteriaIa64GetBootstrapInterruptVectorTableAddress:
    movl r8 = AsteriaIa64BootstrapInterruptVectorTableImage
    ;;
    br.ret.sptk.many b0
    ;;
.align 16
.size AsteriaIa64GetBootstrapInterruptVectorTableAddress, . - AsteriaIa64GetBootstrapInterruptVectorTableAddress

// Every bundle is an independent stackless self loop. Hardware interruption
// entry preserves PSR.it: physical before activation, then pinned by the
// image ITR. Runtime admission precedes translation on the BSP and every AP.
.section .text.ia64_bootstrap_ivt, "ax", @progbits
.balign ASTERIA_INITIAL_INTERRUPT_VECTOR_TABLE_SIZE_BYTES
.global AsteriaIa64BootstrapInterruptVectorTableImage
.hidden AsteriaIa64BootstrapInterruptVectorTableImage
.type AsteriaIa64BootstrapInterruptVectorTableImage, @object
AsteriaIa64BootstrapInterruptVectorTableImage:
.rept ASTERIA_INITIAL_INTERRUPT_VECTOR_TABLE_SIZE_BYTES / ASTERIA_INITIAL_INTERRUPT_VECTOR_BUNDLE_SIZE_BYTES
    .byte 0x11, 0, 0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0x40
.endr
.size AsteriaIa64BootstrapInterruptVectorTableImage, . - AsteriaIa64BootstrapInterruptVectorTableImage
