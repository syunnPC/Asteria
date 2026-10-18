// SPDX-License-Identifier: Apache-2.0

// Entry of the IA-64 component image: the function descriptor the ELF entry names, which the root
// service copies into its own memory for the thread's entry, the startup that calls
// AsteriaIa64DirectoryImageMain with the start block's address.

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
    br.call.sptk.many b0 = AsteriaIa64DirectoryImageMain
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

