// SPDX-License-Identifier: Apache-2.0
#if !defined(ASTERIA_KERNEL_AP_ENTRY)
// Test firmware entry only. Preserve UEFI arguments and force EXTD before
// production boot checks. APs independently observe their own enabled mode.
.text
.p2align 4
.global AsteriaAmd64X2ApicFirmwareEntry
.type AsteriaAmd64X2ApicFirmwareEntry, @function
AsteriaAmd64X2ApicFirmwareEntry:
    pushq %rcx
    pushq %rdx
    movl $0x1b, %ecx
    rdmsr
    orl $0x400, %eax
    wrmsr
    popq %rdx
    popq %rcx
    jmp EfiMain
.size AsteriaAmd64X2ApicFirmwareEntry, . - AsteriaAmd64X2ApicFirmwareEntry
.section .note.GNU-stack,"",@progbits

#else
// INIT/SIPI enters through the ordinary trampoline. The link-time wrapper
// switches every AP before entering the runtime, preserving its SysV arguments.
.text
.p2align 4
.globl __wrap_AsteriaAmd64ApplicationProcessorEntry
.type __wrap_AsteriaAmd64ApplicationProcessorEntry,@function
__wrap_AsteriaAmd64ApplicationProcessorEntry:
    mov $0x1b, %ecx
    rdmsr
    or $0x400, %eax
    wrmsr
    jmp __real_AsteriaAmd64ApplicationProcessorEntry
.size __wrap_AsteriaAmd64ApplicationProcessorEntry, .-__wrap_AsteriaAmd64ApplicationProcessorEntry

#endif
