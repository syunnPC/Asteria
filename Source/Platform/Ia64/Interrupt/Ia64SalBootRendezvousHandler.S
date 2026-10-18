// SPDX-License-Identifier: Apache-2.0

// Raw SAL OS_BOOT_RENDEZ entry of the IA-64 boot rendezvous handler and its
// cr.lid, entry-address and gp getters. Dispatcher results, takeover states
// and takeover-record constants come from the generated C++ layout.

#include "Generated/Platform/Ia64/Ia64SalBootRendezvousHandlerAsmLayout.h"

.text
.align 16
.global AsteriaIa64ReadCurrentCrLid
.hidden AsteriaIa64ReadCurrentCrLid
.type AsteriaIa64ReadCurrentCrLid, @function
.proc AsteriaIa64ReadCurrentCrLid
AsteriaIa64ReadCurrentCrLid:
    mov r8 = cr.lid
    ;;
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64ReadCurrentCrLid
.size AsteriaIa64ReadCurrentCrLid, . - AsteriaIa64ReadCurrentCrLid
.align 16
.global AsteriaIa64GetSalBootRendezvousEntryAddress
.hidden AsteriaIa64GetSalBootRendezvousEntryAddress
.type AsteriaIa64GetSalBootRendezvousEntryAddress, @function
.proc AsteriaIa64GetSalBootRendezvousEntryAddress
AsteriaIa64GetSalBootRendezvousEntryAddress:
// Return a raw bundle address. Taking a C++ function pointer here would
// instead materialize a P64 descriptor and an R_IA64_FPTR64LSB datum.
    movl r8 = AsteriaIa64SalBootRendezvousHandlerEntry
    ;;
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64GetSalBootRendezvousEntryAddress
.size AsteriaIa64GetSalBootRendezvousEntryAddress, . - AsteriaIa64GetSalBootRendezvousEntryAddress
.align 16
.global AsteriaIa64GetSalBootRendezvousGlobalPointerAddress
.hidden AsteriaIa64GetSalBootRendezvousGlobalPointerAddress
.type AsteriaIa64GetSalBootRendezvousGlobalPointerAddress, @function
.proc AsteriaIa64GetSalBootRendezvousGlobalPointerAddress
AsteriaIa64GetSalBootRendezvousGlobalPointerAddress:
    mov r8 = r1
    ;;
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64GetSalBootRendezvousGlobalPointerAddress
.size AsteriaIa64GetSalBootRendezvousGlobalPointerAddress, . - AsteriaIa64GetSalBootRendezvousGlobalPointerAddress
.align 16
.global AsteriaIa64SalBootRendezvousHandlerEntry
.hidden AsteriaIa64SalBootRendezvousHandlerEntry
.type AsteriaIa64SalBootRendezvousHandlerEntry, @function
.proc AsteriaIa64SalBootRendezvousHandlerEntry
AsteriaIa64SalBootRendezvousHandlerEntry:
// Do not add GAS unwind annotations here. This firmware image has no
// unwinder, and their SEGREL relocations are not part of its PE closure.
    alloc r32 = ar.pfs, 0, 3, 2, 0
    mov r33 = b0
    mov r34 = r1
    ;;
// Validate without changing inherited state. ar.rsc.loadrs, PL, and
// reserved fields are permitted to be nonzero; only mode and BE must
// describe enforced-lazy, little-endian operation. RSC.pl does not govern
// the inherited physical RSE accesses while PSR.rt is clear. The
// dispatcher uses this flag only before a first takeover Claim, so
// ordinary SAL returns remain unchanged.
    mov r14 = psr
    mov r15 = ar.rsc
    ;;
    and r15 = 19, r15
    ;;
    movl r16 = 0x0000401B0F064002
    ;;
    and r14 = r14, r16
    ;;
    or r14 = r14, r15
    ;;
    cmp.eq p6, p7 = r14, r0
    ;;
(p6) mov r36 = ASTERIA_IA64_SAL_BOOT_RENDEZVOUS_TAKEOVER_STATE_VALID
(p7) mov r36 = r0
    ;;
// SAL invokes a no-argument raw entry after installing the registered GP.
// Read the only processor identity needed by the state machine.
    mov r35 = cr.lid
    ;;
    br.call.sptk.many b0 = AsteriaIa64DispatchSalBootRendezvous
    ;;
// Every disposition first discharges SAL's inherited ITR(0). A zero page
// operand is the dispatcher's malformed-state sentinel and cannot be used
// by ptr.i.
    cmp.eq p6, p7 = r9, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    ptr.i r8, r9
    ;;
    srlz.i
    ;;
    cmp.eq p6, p7 = r10, r0
    ;;
(p6) br.cond.sptk.few .Lasteria_ia64_rendezvous_return_to_sal
    ;;
    mov r14 = ASTERIA_IA64_SAL_BOOT_RENDEZVOUS_DISPOSITION_TAKE_OVER
    ;;
    cmp.eq p6, p7 = r10, r14
    ;;
(p7) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
// The claim record is read while the inherited physical stack and RSE
// still exist. Only non-banked GRs carry its values across bsw.1.
    cmp.eq p6, p7 = r11, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    tbit.nz p6, p7 = r11, 63
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    and r16 = ASTERIA_IA64_APPLICATION_PROCESSOR_TAKEOVER_RECORD_ALIGNMENT_MASK, r11
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    ld8 r14 = [r11], 8
    ;;
    ld8 r15 = [r11], 8
    ;;
    ld8 r8 = [r11]
    ;;
    tbit.nz p6, p7 = r14, 63
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    and r16 = 15, r14
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    tbit.nz p6, p7 = r15, 63
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    movl r17 = 0x7FFF
    ;;
    and r16 = r15, r17
    ;;
    cmp.ne p6, p7 = r16, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    add r16 = ASTERIA_APPLICATION_PROCESSOR_INITIAL_STACK_SCRATCH_SIZE_BYTES, r14
    ;;
    cmp.ne p6, p7 = r16, r15
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    cmp.eq p6, p7 = r8, r0
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    mov r16 = ASTERIA_MAXIMUM_PROCESSOR_COUNT
    ;;
    cmp.geu p6, p7 = r8, r16
    ;;
(p6) br.cond.dpnt.few .Lasteria_ia64_rendezvous_fail_stop
    ;;
    mov r1 = r34
    ;;
    bsw.1
    ;;
// SAL may enter with interruption collection enabled. The owned AP IVT
// contract requires it excluded before the inherited frame is abandoned.
    rsm psr.ic
    ;;
    srlz.d
    ;;
    srlz.i
    ;;
    cover
    ;;
    flushrs
    ;;
    mov ar.rsc = r0
    ;;
    invala
    ;;
    mov ar.bspstore = r15
    ;;
    mov ar.rnat = r0
    mov ar.pfs = r0
    ;;
    mf
    ;;
    mov r12 = r14
    ;;
// Create the new continuation's sole stacked output and pass CpuId as an
// ordinary scalar. The old SAL BR0 and PFS are never restored after cover.
    alloc r14 = ar.pfs, 0, 0, 1, 0
    ;;
    mov r32 = r8
    ;;
    br.call.sptk.many b0 = AsteriaIa64RunApplicationProcessorOwnedExecutionContext
    ;;
    br.cond.sptk .Lasteria_ia64_rendezvous_fail_stop
    ;;
.Lasteria_ia64_rendezvous_return_to_sal:
// Restore the complete explicit bridge frame only on SAL return.
    mov r1 = r34
    mov b0 = r33
    mov ar.pfs = r32
    ;;
    br.ret.sptk.many b0
    ;;
.Lasteria_ia64_rendezvous_fail_stop:
// Unknown actions and every malformed takeover state park this AP without
// touching the abandoned or SAL return contexts.
    hint @pause
    ;;
    br.cond.sptk .Lasteria_ia64_rendezvous_fail_stop
    ;;
.endp AsteriaIa64SalBootRendezvousHandlerEntry
.size AsteriaIa64SalBootRendezvousHandlerEntry, . - AsteriaIa64SalBootRendezvousHandlerEntry
