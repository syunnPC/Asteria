// SPDX-License-Identifier: Apache-2.0

// Entry of the minimal AMD64 user image: calls the C++ main and then idles.

    .text
    .global AsteriaAmd64MinimalUserImageEntry
    .hidden AsteriaAmd64MinimalUserImageEntry
    .type AsteriaAmd64MinimalUserImageEntry,@function
AsteriaAmd64MinimalUserImageEntry:
    xor %ebp,%ebp
    call AsteriaAmd64MinimalUserImageMain
    .global AsteriaAmd64MinimalUserImageIdle
    .hidden AsteriaAmd64MinimalUserImageIdle
    .type AsteriaAmd64MinimalUserImageIdle,@function
AsteriaAmd64MinimalUserImageIdle:
    pause
    jmp AsteriaAmd64MinimalUserImageIdle
    .size AsteriaAmd64MinimalUserImageIdle,.-AsteriaAmd64MinimalUserImageIdle
    .size AsteriaAmd64MinimalUserImageEntry,.-AsteriaAmd64MinimalUserImageEntry
