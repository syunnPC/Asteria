// SPDX-License-Identifier: Apache-2.0

// IA-64 application-processor pivot from the physical SAL context into the
// translated runtime, and the translated landing that enters the AP kernel.
// The registers carried across bsw.1 come from
// Ia64ApplicationProcessorRuntimePivotRegisters.hpp.

#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimePivotRegisters.hpp"

.text
.align 16
.global AsteriaIa64PivotApplicationProcessorRuntime
.hidden AsteriaIa64PivotApplicationProcessorRuntime
.type AsteriaIa64PivotApplicationProcessorRuntime, @function
.proc AsteriaIa64PivotApplicationProcessorRuntime
AsteriaIa64PivotApplicationProcessorRuntime:
// Move every stacked P64 input out of the inherited RSE frame before any
// validation or cover. Only unbanked r0-r15 survive bsw.1.
    mov ASTERIA_IA64_AP_PIVOT_REGISTER(ASTERIA_IA64_AP_PIVOT_CPU_REGISTER_NUMBER) = r32
    mov ASTERIA_IA64_AP_PIVOT_REGISTER(ASTERIA_IA64_AP_PIVOT_STACK_REGISTER_NUMBER) = r33
    mov ASTERIA_IA64_AP_PIVOT_REGISTER(ASTERIA_IA64_AP_PIVOT_BSPSTORE_REGISTER_NUMBER) = r34
    ;;
// mov-from-PSR exposes the six fields that must remain clear throughout
// the physical Region-0 entry profile. CPL and VM are caller preconditions.
    mov r8 = psr
    ;;
    movl r17 = 0x000000100802E000
    ;;
    and r8 = r8, r17
    ;;
    cmp.ne p6, p7 = r8, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_ap_pivot_rejected
    ;;
// Resolve the fixed translated landing while physical stack/RSE state is
// still intact. gp and the selected r10 are unbanked.
    movl ASTERIA_IA64_AP_PIVOT_REGISTER(ASTERIA_IA64_AP_PIVOT_LANDING_REGISTER_NUMBER) = AsteriaIa64ApplicationProcessorTranslatedLanding
    ;;
// This is the no-return commit sequence. No edge below can recover the
// caller's register stack or return to its physical memory stack.
    bsw.1
    ;;
    cover
    ;;
    flushrs
    ;;
    mov ar.rsc = r0
    ;;
    invala
    ;;
// A BSPSTORE write makes RNAT undefined. Install the clean translated
// context in architectural order and perform no memory reference after
// the physical-to-virtual BSPSTORE cutoff.
    mov ar.bspstore = ASTERIA_IA64_AP_PIVOT_REGISTER(ASTERIA_IA64_AP_PIVOT_BSPSTORE_REGISTER_NUMBER)
    ;;
    mov ar.rnat = r0
    mov ar.pfs = r0
    mov ar.unat = r0
    ;;
    mf
    ;;
    mov r12 = ASTERIA_IA64_AP_PIVOT_REGISTER(ASTERIA_IA64_AP_PIVOT_STACK_REGISTER_NUMBER)
    ;;
    movl r18 = 0x0000101008022018
    ;;
    mov cr.ifs = r0
    mov cr.ipsr = r18
    mov cr.iip = ASTERIA_IA64_AP_PIVOT_REGISTER(ASTERIA_IA64_AP_PIVOT_LANDING_REGISTER_NUMBER)
    ;;
    rfi
    ;;
.Lasteria_ia64_ap_pivot_rejected:
    mov r8 = 1
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64PivotApplicationProcessorRuntime
.size AsteriaIa64PivotApplicationProcessorRuntime, . - AsteriaIa64PivotApplicationProcessorRuntime
.align 16
.global AsteriaIa64ApplicationProcessorTranslatedLanding
.hidden AsteriaIa64ApplicationProcessorTranslatedLanding
.type AsteriaIa64ApplicationProcessorTranslatedLanding, @function
.proc AsteriaIa64ApplicationProcessorTranslatedLanding
AsteriaIa64ApplicationProcessorTranslatedLanding:
// Translation is active and the new empty RSE context is available here.
// Build the continuation's one-output P64 frame from the unbanked CpuId.
    alloc r15 = ar.pfs, 0, 0, 1, 0
    ;;
    mov r32 = ASTERIA_IA64_AP_PIVOT_REGISTER(ASTERIA_IA64_AP_PIVOT_CPU_REGISTER_NUMBER)
    ;;
    br.call.sptk.many b0 = AsteriaIa64RunApplicationProcessorTranslatedKernel
    ;;
.Lasteria_ia64_ap_translated_continuation_returned:
    hint @pause
    ;;
    br.cond.sptk .Lasteria_ia64_ap_translated_continuation_returned
    ;;
.endp AsteriaIa64ApplicationProcessorTranslatedLanding
.size AsteriaIa64ApplicationProcessorTranslatedLanding, . - AsteriaIa64ApplicationProcessorTranslatedLanding
