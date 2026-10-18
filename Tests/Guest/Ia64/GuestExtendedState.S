// SPDX-License-Identifier: Apache-2.0
// The IA-64 guest test program's extended-state pattern: the C++ entry loads the significands base
// + n into f32-f127 first and checks them last. The program is compiled integer-only, so only the
// kernel's context switches, while the pager's thread runs its own pattern, can change them; the
// boot domains launch with PSR.dfh clear and PSR.dfl set (f2-f31 stay disabled at PL3).
.explicit
.text
.align 16
.global AsteriaGuestIa64LoadExtendedState
.hidden AsteriaGuestIa64LoadExtendedState
.type AsteriaGuestIa64LoadExtendedState, @function
.proc AsteriaGuestIa64LoadExtendedState
AsteriaGuestIa64LoadExtendedState:
    movl r14 = 0x4755455354000000
    ;;
    .irp index, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    adds r15 = \index, r14
    ;;
    setf.sig f\index = r15
    ;;
    .endr
    br.ret.sptk.many b0
    ;;
.endp AsteriaGuestIa64LoadExtendedState
.size AsteriaGuestIa64LoadExtendedState, . - AsteriaGuestIa64LoadExtendedState

// Returns 1 in r8 when f32-f127 still hold the pattern.
.align 16
.global AsteriaGuestIa64ExtendedStateIntact
.hidden AsteriaGuestIa64ExtendedStateIntact
.type AsteriaGuestIa64ExtendedStateIntact, @function
.proc AsteriaGuestIa64ExtendedStateIntact
AsteriaGuestIa64ExtendedStateIntact:
    movl r14 = 0x4755455354000000
    mov r8 = 1
    ;;
    .irp index, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127
    getf.sig r15 = f\index
    adds r16 = \index, r14
    ;;
    cmp.ne p6, p0 = r15, r16
    ;;
(p6) mov r8 = 0
    ;;
    .endr
    br.ret.sptk.many b0
    ;;
.endp AsteriaGuestIa64ExtendedStateIntact
.size AsteriaGuestIa64ExtendedStateIntact, . - AsteriaGuestIa64ExtendedStateIntact
