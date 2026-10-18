// SPDX-License-Identifier: Apache-2.0

// Entry of the IA-64 component image: the function descriptor the ELF entry names, which the root
// service copies into its own memory for the thread's entry, the startup that calls
// AsteriaIa64ComponentImageMain with the start block's address, its crash and its clock.

.explicit
.text
.align 16
.global AsteriaIa64ComponentImageEntry
.hidden AsteriaIa64ComponentImageEntry
.type AsteriaIa64ComponentImageEntry, @function
.proc AsteriaIa64ComponentImageEntry
AsteriaIa64ComponentImageEntry:
// The launch passes the start block's address in r4 and the descriptor's gp
// in r1; the memory stack pointer is already the stack top less 16.
    alloc r2 = ar.pfs, 0, 0, 1, 0
    mov r32 = r4
    ;;
    br.call.sptk.many b0 = AsteriaIa64ComponentImageMain
    ;;
.Lasteria_ia64_component_idle:
    hint @pause
    br.sptk.few .Lasteria_ia64_component_idle
    ;;
.endp AsteriaIa64ComponentImageEntry
.size AsteriaIa64ComponentImageEntry, . - AsteriaIa64ComponentImageEntry
.section .AsteriaUserEntryDescriptor, "a", @progbits
.align 16
.global AsteriaIa64ComponentImageEntryDescriptor
.hidden AsteriaIa64ComponentImageEntryDescriptor
.type AsteriaIa64ComponentImageEntryDescriptor, @object
AsteriaIa64ComponentImageEntryDescriptor:
    data8 AsteriaIa64ComponentImageEntry
    data8 __gp
.size AsteriaIa64ComponentImageEntryDescriptor, 16
.previous

// The crashing incarnation's crash, by the kind in in0: reading the PSR is privileged at PL3, a
// General Exception; or (ComponentCrashFloatingPoint) with zero-divide traps enabled in ar.fpsr
// (traps.zd, bit 2, clear), a reciprocal of zero, a Floating-point Fault. The kernel hands either
// to the component's supervisor. Returns only if neither trapped.
.text
.align 16
.global AsteriaIa64ComponentCrash
.hidden AsteriaIa64ComponentCrash
.type AsteriaIa64ComponentCrash, @function
.proc AsteriaIa64ComponentCrash
AsteriaIa64ComponentCrash:
    cmp.eq p6, p7 = 1, r32
    ;;
(p7) mov r8 = psr
(p6) mov r8 = ar.fpsr
    ;;
(p6) dep r8 = 0, r8, 2, 1
    ;;
(p6) mov ar.fpsr = r8
    ;;
(p6) frcpa.s0 f8, p8 = f1, f0
    ;;
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64ComponentCrash
.size AsteriaIa64ComponentCrash, . - AsteriaIa64ComponentCrash

// Returns the interval time counter, the component's clock.
.align 16
.global AsteriaIa64ComponentClock
.hidden AsteriaIa64ComponentClock
.type AsteriaIa64ComponentClock, @function
.proc AsteriaIa64ComponentClock
AsteriaIa64ComponentClock:
    mov r8 = ar.itc
    ;;
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64ComponentClock
.size AsteriaIa64ComponentClock, . - AsteriaIa64ComponentClock
