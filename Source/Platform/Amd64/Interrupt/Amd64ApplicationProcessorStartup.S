// SPDX-License-Identifier: Apache-2.0

#include "Generated/Platform/Amd64/Amd64FrameScrubberAsmLayout.h"
// Application-processor startup code, copied below 1 MiB and entered by SIPI.
// Amd64StartupMailbox offsets come from the generated C++ layout; the mailbox
// is the page after the code page.

#include "Generated/Platform/Amd64/Amd64ApplicationProcessorStartupAsmLayout.h"

    .pushsection .rodata
    .balign 16
    .global AsteriaAmd64StartupCodeStart
    .global AsteriaAmd64StartupLongEntry
    .global AsteriaAmd64StartupCodeEnd
    .hidden AsteriaAmd64StartupCodeStart
    .hidden AsteriaAmd64StartupLongEntry
    .hidden AsteriaAmd64StartupCodeEnd
    .code16
AsteriaAmd64StartupCodeStart:
.Lasteria_startup_begin:
    cli
    cld
    movw %cs,%ax
    movw %ax,%ds
    movw %ax,%ss
    lgdtl 0x1000 + ASTERIA_AMD64_STARTUP_MAILBOX_GDT_LIMIT
    wbinvd
    movl $0x277,%ecx
    movl $0x00070406,%eax
    movl $0x00070401,%edx
    wrmsr
    movl %cr4,%eax
    orl $0x20,%eax
    andl $ASTERIA_DISABLED_TRANSLATION_FEATURES_CLEAR,%eax
    movl %eax,%cr4
    movl 0x1000 + ASTERIA_AMD64_STARTUP_MAILBOX_ROOT,%eax
    movl %eax,%cr3
    movl $0xc0000080,%ecx
    rdmsr
    orl $0x900,%eax
    wrmsr
    movl %cr0,%eax
    andl $0x9fffffff,%eax
    orl $0x8001000d,%eax
    movl %eax,%cr0
    ljmpl *0x1000 + ASTERIA_AMD64_STARTUP_MAILBOX_LONG_ENTRY
    .code64
AsteriaAmd64StartupLongEntry:
    movw $16,%ax
    movw %ax,%ss
    movw %ax,%ds
    movw %ax,%es
    xor %eax,%eax
    movw %ax,%fs
    movw %ax,%gs
    leaq .Lasteria_startup_begin(%rip),%rbx
    addq $4096,%rbx
    movq ASTERIA_AMD64_STARTUP_MAILBOX_STACK_TOP(%rbx),%rsp
    andq $-16,%rsp
    xor %ebp,%ebp
    movq ASTERIA_AMD64_STARTUP_MAILBOX_CONTEXT(%rbx),%rdi
    movq ASTERIA_AMD64_STARTUP_MAILBOX_PROCESSOR(%rbx),%rsi
    movq ASTERIA_AMD64_STARTUP_MAILBOX_ENTRY(%rbx),%rax
    call *%rax
1:  cli
    hlt
    jmp 1b
AsteriaAmd64StartupCodeEnd:
    .popsection
