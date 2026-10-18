// SPDX-License-Identifier: Apache-2.0

// Physical diagnostic UART character writer of WriteDiagnosticAscii: one
// polled transmit through the uncacheable UART alias with data translation
// off, usable before and after the firmware takeover. The caller names the
// transmit holding and line status registers by physical address
// (Ia64DebugUart, from the firmware's debug-port tables).

.text
.align 16
.global AsteriaIa64TryWriteDiagnosticCharacterPhysical
.hidden AsteriaIa64TryWriteDiagnosticCharacterPhysical
.type AsteriaIa64TryWriteDiagnosticCharacterPhysical, @function
AsteriaIa64TryWriteDiagnosticCharacterPhysical:
// Copy every stacked input before quiescing the RSE. Both register
// addresses get bit 63 here, so the native leaf cannot be passed a
// translated or write-back physical alias by mistake. IA-64 physical address
// bit 63 selects the uncacheable, sequential-access form required by mf.a.
    mov r14 = psr
    mov r16 = r32
    mov r17 = r33
    mov r22 = r34
    mov r23 = r35
    ;;
    movl r24 = 0x8000000000000000
    ;;
    or r15 = r22, r24
    or r18 = r23, r24
    rsm psr.i
    ;;
    srlz.d
    ;;
// Synchronize the RSE, preserve every ar.rsc field, and force only mode to
// lazy. For an entry state with PSR.dt clear, any eager RSE activity before
// this point belongs to the caller's entry regime. From the completed
// ar.rsc write through the UART transaction, the RSE cannot spill or fill.
    mov r20 = ar.rsc
    ;;
    dep r21 = 0, r20, 0, 2
    ;;
    mov ar.rsc = r21
    ;;
// The UART is deliberately absent from the runtime root. Uncacheable
// physical access with PSR.dt clear works both before and after takeover.
    rsm psr.dt
    ;;
    srlz.d
    ;;
1:
    ld1.acq r19 = [r18]
    ;;
    tbit.nz p6, p7 = r19, 5
    ;;
(p6) br.cond.sptk.few 2f
    hint @pause
    adds r17 = -1, r17
    ;;
    cmp.ne p6, p7 = r17, r0
    ;;
(p6) br.cond.sptk.few 1b
    mov r8 = r0
    ;;
    br.cond.sptk.few 3f
    ;;
2:
    mf
    ;;
    st1 [r15] = r16
    ;;
    mf.a
    mov r8 = 1
    ;;
3:
// Restore translation before eager RSE mode and interrupts. If translation
// was clear on entry, eager RSE activity can resume here only after the
// physical UART transaction has completed. Both predicates reflect entry
// PSR, and the complete entry ar.rsc value is restored exactly.
    tbit.nz p6, p7 = r14, 17
    tbit.nz p8, p9 = r14, 14
    ;;
(p6) ssm psr.dt
    ;;
    srlz.d
    ;;
    mov ar.rsc = r20
    ;;
(p8) ssm psr.i
    ;;
    srlz.d
    ;;
    br.ret.sptk.many b0
    ;;
// Force GAS to emit the pending return bundle before fixing the ELF extent.
.align 16
.size AsteriaIa64TryWriteDiagnosticCharacterPhysical, . - AsteriaIa64TryWriteDiagnosticCharacterPhysical
