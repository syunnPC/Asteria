// SPDX-License-Identifier: Apache-2.0
// Guest test helper for the abi.stacked-registers point: after one system
// call, the stacked registers a user frame grows into must hold nothing the
// kernel left there. It issues the call from a frame of exactly the six
// syscall outputs, after flushrs has moved every caller frame to the backing
// store, so that on return this frame is the only stacked state the user
// owns. It then grows the frame to all 96 stacked registers and returns the
// OR of the 90 new ones, r38-r127, or all-ones if any has its NaT bit set.
//
// UInt64 AsteriaGuestIa64ReadStackedRegistersAfterSystemCall(
//     UInt64 number, UInt64 argument0, UInt64 argument1);
// Arguments two to five are zero. The response is discarded.

.explicit
.text
.align 16
.global AsteriaGuestIa64ReadStackedRegistersAfterSystemCall
.hidden AsteriaGuestIa64ReadStackedRegistersAfterSystemCall
.type AsteriaGuestIa64ReadStackedRegistersAfterSystemCall, @function
.proc AsteriaGuestIa64ReadStackedRegistersAfterSystemCall
AsteriaGuestIa64ReadStackedRegistersAfterSystemCall:
// Copy the inputs out of the caller's frame before replacing it with the
// six-output syscall frame; r3 keeps the caller's frame marker. The kernel
// restores every static register but r8-r10 on return.
    mov r15 = r32
    mov r16 = r33
    mov r17 = r34
    ;;
    alloc r3 = ar.pfs, 0, 0, 6, 0
    ;;
    mov r32 = r16
    mov r33 = r17
    mov r34 = r0
    mov r35 = r0
    mov r36 = r0
    mov r37 = r0
    ;;
    flushrs
    ;;
// Ia64SystemCallBreakImmediate, the ABI's system-call break.
    { .mii
        nop.m 0
        nop.i 0
        break.i 0x100000
    }
    ;;
    alloc r2 = ar.pfs, 0, 96, 0, 0
    ;;
    or r38 = r38, r39
    or r40 = r40, r41
    or r42 = r42, r43
    or r44 = r44, r45
    or r46 = r46, r47
    or r48 = r48, r49
    or r50 = r50, r51
    or r52 = r52, r53
    or r54 = r54, r55
    or r56 = r56, r57
    or r58 = r58, r59
    or r60 = r60, r61
    or r62 = r62, r63
    or r64 = r64, r65
    or r66 = r66, r67
    or r68 = r68, r69
    or r70 = r70, r71
    or r72 = r72, r73
    or r74 = r74, r75
    or r76 = r76, r77
    or r78 = r78, r79
    or r80 = r80, r81
    or r82 = r82, r83
    or r84 = r84, r85
    or r86 = r86, r87
    or r88 = r88, r89
    or r90 = r90, r91
    or r92 = r92, r93
    or r94 = r94, r95
    or r96 = r96, r97
    or r98 = r98, r99
    or r100 = r100, r101
    or r102 = r102, r103
    or r104 = r104, r105
    or r106 = r106, r107
    or r108 = r108, r109
    or r110 = r110, r111
    or r112 = r112, r113
    or r114 = r114, r115
    or r116 = r116, r117
    or r118 = r118, r119
    or r120 = r120, r121
    or r122 = r122, r123
    or r124 = r124, r125
    or r126 = r126, r127
    ;;
    or r38 = r38, r40
    or r42 = r42, r44
    or r46 = r46, r48
    or r50 = r50, r52
    or r54 = r54, r56
    or r58 = r58, r60
    or r62 = r62, r64
    or r66 = r66, r68
    or r70 = r70, r72
    or r74 = r74, r76
    or r78 = r78, r80
    or r82 = r82, r84
    or r86 = r86, r88
    or r90 = r90, r92
    or r94 = r94, r96
    or r98 = r98, r100
    or r102 = r102, r104
    or r106 = r106, r108
    or r110 = r110, r112
    or r114 = r114, r116
    or r118 = r118, r120
    or r122 = r122, r124
    ;;
    or r38 = r38, r42
    or r46 = r46, r50
    or r54 = r54, r58
    or r62 = r62, r66
    or r70 = r70, r74
    or r78 = r78, r82
    or r86 = r86, r90
    or r94 = r94, r98
    or r102 = r102, r106
    or r110 = r110, r114
    or r118 = r118, r122
    ;;
    or r38 = r38, r46
    or r54 = r54, r62
    or r70 = r70, r78
    or r86 = r86, r94
    or r102 = r102, r110
    or r118 = r118, r126
    ;;
    or r38 = r38, r54
    or r70 = r70, r86
    or r102 = r102, r118
    ;;
    or r38 = r38, r70
    ;;
    or r38 = r38, r102
    ;;
    tnat.nz p6, p7 = r38
    ;;
(p6) mov r8 = -1
(p7) mov r8 = r38
    mov ar.pfs = r3
    ;;
    br.ret.sptk.many b0
    ;;
.endp AsteriaGuestIa64ReadStackedRegistersAfterSystemCall
.size AsteriaGuestIa64ReadStackedRegistersAfterSystemCall, . - AsteriaGuestIa64ReadStackedRegistersAfterSystemCall
