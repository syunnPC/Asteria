// SPDX-License-Identifier: Apache-2.0

// User-mode SYSCALL transport: loads the request registers, enters the kernel,
// and stores the response. SystemCallRequest and SystemCallResponse offsets
// come from the generated C++ layout.

#include "Generated/Platform/Amd64/Amd64SystemCallTransportAsmLayout.h"

    .text
    .global AsteriaAmd64UserSystemCallTransport
    .hidden AsteriaAmd64UserSystemCallTransport
    .type AsteriaAmd64UserSystemCallTransport,@function
AsteriaAmd64UserSystemCallTransport:
    pushq %rbx
    movq %rdx,%rbx
    movq %rsi,%r11
    movq ASTERIA_SYSTEM_CALL_REQUEST_NUMBER(%r11),%rax
    movq ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_0(%r11),%rdi
    movq ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_1(%r11),%rsi
    movq ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_2(%r11),%rdx
    movq ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_3(%r11),%r10
    movq ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_4(%r11),%r8
    movq ASTERIA_SYSTEM_CALL_REQUEST_ARGUMENTS_5(%r11),%r9
    syscall
    movq %rax,ASTERIA_SYSTEM_CALL_RESPONSE_ERROR(%rbx)
    movq %rdx,ASTERIA_SYSTEM_CALL_RESPONSE_VALUE0(%rbx)
    movq %r8,ASTERIA_SYSTEM_CALL_RESPONSE_VALUE1(%rbx)
    popq %rbx
    ret
    .size AsteriaAmd64UserSystemCallTransport,.-AsteriaAmd64UserSystemCallTransport
