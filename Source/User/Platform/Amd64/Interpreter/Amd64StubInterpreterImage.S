// SPDX-License-Identifier: Apache-2.0

// Entry of the AMD64 stub interpreter image: like every component image's, the root service starts
// its thread here with the start block's address in RDI.

    .text
    .global AsteriaAmd64ComponentImageEntry
    .hidden AsteriaAmd64ComponentImageEntry
    .type AsteriaAmd64ComponentImageEntry,@function
AsteriaAmd64ComponentImageEntry:
    xor %ebp,%ebp
    call AsteriaAmd64StubInterpreterImageMain
1:
    pause
    jmp 1b
    .size AsteriaAmd64ComponentImageEntry,.-AsteriaAmd64ComponentImageEntry
