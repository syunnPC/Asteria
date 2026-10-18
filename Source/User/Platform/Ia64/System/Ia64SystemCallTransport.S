// SPDX-License-Identifier: Apache-2.0

// IA-64 user system-call transport: loads a SystemCallRequest into the
// six-output syscall frame, enters the kernel with one break, and stores the
// SystemCallResponse. The break immediate comes from the generated C++ layout.

#include "Generated/Platform/Ia64/Ia64SystemCallTransportAsmLayout.h"

.explicit
.text
.align 16
.global AsteriaIa64UserSystemCallTransport
.hidden AsteriaIa64UserSystemCallTransport
.type AsteriaIa64UserSystemCallTransport, @function
.proc AsteriaIa64UserSystemCallTransport
AsteriaIa64UserSystemCallTransport:
// Preserve the request and response pointers before replacing the caller's
// stacked-register view with the six-output syscall frame.
    mov r14 = r33
    mov r16 = r34
    ;;
    alloc r2 = ar.pfs, 0, 0, 6, 0
    ;;
    ld8 r15 = [r14], 8
    ;;
    ld8 r32 = [r14], 8
    ;;
    ld8 r33 = [r14], 8
    ;;
    ld8 r34 = [r14], 8
    ;;
    ld8 r35 = [r14], 8
    ;;
    ld8 r36 = [r14], 8
    ;;
    ld8 r37 = [r14]
    ;;
    { .mii
        nop.m 0
        nop.i 0
        break.i ASTERIA_IA64_SYSTEM_CALL_BREAK_IMMEDIATE
    }
    ;;
    st8 [r16] = r8, 8
    ;;
    st8 [r16] = r9, 8
    ;;
    st8 [r16] = r10
    mov ar.pfs = r2
    ;;
    br.ret.sptk.many b0
    ;;
.endp AsteriaIa64UserSystemCallTransport
.size AsteriaIa64UserSystemCallTransport, . - AsteriaIa64UserSystemCallTransport
