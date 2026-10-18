// SPDX-License-Identifier: Apache-2.0
// Guest test helper: the IA-64 user system-call transport with a check of
// the global pointer. It calls AsteriaIa64UserSystemCallTransport, whose one
// break returns with r1 as the kernel left it and which never writes r1
// itself, then counts the call and accumulates the bits in which r1 differs
// from this image's __gp. The abi.gp-preserved test point reads both words.
// The context argument is passed through; the transport ignores it.

.explicit
.text
.align 16
.global AsteriaGuestIa64CheckedSystemCallTransport
.hidden AsteriaGuestIa64CheckedSystemCallTransport
.type AsteriaGuestIa64CheckedSystemCallTransport, @function
.proc AsteriaGuestIa64CheckedSystemCallTransport
AsteriaGuestIa64CheckedSystemCallTransport:
// Inputs r32-r34 (context, request, response), locals r35 (ar.pfs) and r36
// (b0), and the three arguments again as outputs r37-r39.
    alloc r35 = ar.pfs, 3, 2, 3, 0
    ;;
    mov r36 = b0
    mov r37 = r32
    mov r38 = r33
    mov r39 = r34
    ;;
    br.call.sptk.many b0 = AsteriaIa64UserSystemCallTransport
    ;;
    movl r14 = __gp
    movl r15 = AsteriaGuestIa64CheckedSystemCallCount
    movl r16 = AsteriaGuestIa64GlobalPointerDifference
    ;;
    xor r14 = r14, r1
    ld8 r17 = [r15]
    ld8 r18 = [r16]
    ;;
    adds r17 = 1, r17
    or r18 = r18, r14
    ;;
    st8 [r15] = r17
    st8 [r16] = r18
    mov b0 = r36
    mov ar.pfs = r35
    ;;
    br.ret.sptk.many b0
    ;;
.endp AsteriaGuestIa64CheckedSystemCallTransport
.size AsteriaGuestIa64CheckedSystemCallTransport, . - AsteriaGuestIa64CheckedSystemCallTransport

// The number of checked calls, and the OR of r1 ^ __gp over all of them.
.section .bss, "aw", @nobits
.align 8
.global AsteriaGuestIa64CheckedSystemCallCount
.hidden AsteriaGuestIa64CheckedSystemCallCount
.type AsteriaGuestIa64CheckedSystemCallCount, @object
.size AsteriaGuestIa64CheckedSystemCallCount, 8
AsteriaGuestIa64CheckedSystemCallCount:
    .skip 8
.global AsteriaGuestIa64GlobalPointerDifference
.hidden AsteriaGuestIa64GlobalPointerDifference
.type AsteriaGuestIa64GlobalPointerDifference, @object
.size AsteriaGuestIa64GlobalPointerDifference, 8
AsteriaGuestIa64GlobalPointerDifference:
    .skip 8
