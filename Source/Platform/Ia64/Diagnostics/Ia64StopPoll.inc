// SPDX-License-Identifier: Apache-2.0

#include "Generated/Platform/Ia64/Ia64StopAsmLayout.h"

// Only use at boundaries where banked r24-r26 are scratch. The normal
// path restores every predicate. The taken path is terminal, never a call.
.macro AsteriaIa64PollStop vector=0, vector_from_k4=0, vector_from_k5=0
AsteriaIa64StopPollStart\@:
    mov r24 = pr
    mov r25 = ar.k3
    ;;
    cmp.eq p6, p7 = r25, r0
    ;;
(p6) br.cond.sptk.few .Lasteria_stop_poll_done\@
    adds r25 = ASTERIA_IA64_STOP_CONTEXT_STORAGE, r25
    ;;
    ld8 r25 = [r25]
    ;;
    cmp.eq p6, p7 = r25, r0
    ;;
(p6) br.cond.sptk.few .Lasteria_stop_poll_done\@
    ld8.acq r26 = [r25]
    ;;
    cmp.eq p6, p7 = r26, r0
    ;;
(p6) br.cond.sptk.few .Lasteria_stop_poll_done\@
.if \vector_from_k4
    mov r16 = ar.k4
    ;;
    shl r16 = r16, 8
.elseif \vector_from_k5
    mov r16 = ar.k5
    ;;
    dep r16 = r0, r16, 0, 8
.else
    mov r16 = \vector
.endif
    ;;
    br.sptk.many AsteriaIa64StopEntry
    ;;
.Lasteria_stop_poll_done\@:
    mov pr = r24, -1
    ;;
AsteriaIa64StopPollEnd\@:
.endm
