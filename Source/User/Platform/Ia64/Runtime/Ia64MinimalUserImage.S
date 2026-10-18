// SPDX-License-Identifier: Apache-2.0

// Entry of the minimal IA-64 user image: the P64 entry descriptor the ELF
// entry names, the startup that calls AsteriaIa64MinimalUserImageMain, and
// the idle loop it falls into when Main returns.

.explicit
.text
.align 16
.global AsteriaIa64MinimalUserImageEntry
.hidden AsteriaIa64MinimalUserImageEntry
.type AsteriaIa64MinimalUserImageEntry, @function
.proc AsteriaIa64MinimalUserImageEntry
AsteriaIa64MinimalUserImageEntry:
// The launch boundary consumes the descriptor below and installs its
// second word in r1. Startup must preserve that loader-provided GP.
    alloc r2 = ar.pfs, 0, 0, 4, 0
    ;;
// The launch boundary supplies two capabilities in static registers: the
// bootstrap Endpoint in r4/r5 and the DiagnosticChannel in r6/r7. Copy them
// into the P64 output frame for the C++ entry.
    mov r32 = r4
    mov r33 = r5
    mov r34 = r6
    mov r35 = r7
    ;;
    br.call.sptk.many b0 = AsteriaIa64MinimalUserImageMain
    ;;
.global AsteriaIa64MinimalUserImageIdle
.hidden AsteriaIa64MinimalUserImageIdle
.type AsteriaIa64MinimalUserImageIdle, @function
AsteriaIa64MinimalUserImageIdle:
    hint @pause
    br.sptk.few AsteriaIa64MinimalUserImageIdle
    ;;
.endp AsteriaIa64MinimalUserImageEntry
.size AsteriaIa64MinimalUserImageIdle, . - AsteriaIa64MinimalUserImageIdle
.size AsteriaIa64MinimalUserImageEntry, . - AsteriaIa64MinimalUserImageEntry
.section .AsteriaUserEntryDescriptor, "a", @progbits
.align 16
.global AsteriaIa64MinimalUserImageEntryDescriptor
.hidden AsteriaIa64MinimalUserImageEntryDescriptor
.type AsteriaIa64MinimalUserImageEntryDescriptor, @object
AsteriaIa64MinimalUserImageEntryDescriptor:
    data8 AsteriaIa64MinimalUserImageEntry
    data8 __gp
.size AsteriaIa64MinimalUserImageEntryDescriptor, 16
.previous
