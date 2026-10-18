// SPDX-License-Identifier: Apache-2.0

#include "Generated/Platform/Amd64/Amd64FrameScrubberAsmLayout.h"
// Activation of the initial address space (processor-state checks, switch to
// the prepared root, stack, GDT and IDT) and the fail-stop interrupt handler.
// The runtime entry reports the activation's stage; the handler writes its
// fault line to the diagnostic sinks the boot selected (Firmware/Diagnostic.cpp):
// the debug port where AsteriaAmd64DebugPortPresent is set, and the 16550 at
// AsteriaAmd64DiagnosticSerialPort where it is nonzero.

.pushsection .rodata
.global AsteriaAmd64ActivationReadOnlyStart
.hidden AsteriaAmd64ActivationReadOnlyStart
AsteriaAmd64ActivationReadOnlyStart:
.Lasteria_amd64_activation_fault:
.ascii "ASTERIA:AMD64-INITIAL-ADDRESS-SPACE:FAULT\r\n"
.Lasteria_amd64_activation_fault_end:
.global AsteriaAmd64ActivationReadOnlyEnd
.hidden AsteriaAmd64ActivationReadOnlyEnd
AsteriaAmd64ActivationReadOnlyEnd:
.popsection
.pushsection .text
.p2align 4
.global AsteriaAmd64ActivationTextStart
.hidden AsteriaAmd64ActivationTextStart
AsteriaAmd64ActivationTextStart:
.global AsteriaAmd64ActivateInitialAddressSpace
.hidden AsteriaAmd64ActivateInitialAddressSpace
.type AsteriaAmd64ActivateInitialAddressSpace, @function
AsteriaAmd64ActivateInitialAddressSpace:
    pushq %r12
    pushq %r13
    movq %rdx, %r12
    movq %rcx, %r13
    movq %rdi, %r10
    movq %rsi, %r11
    testq %r10, %r10
    jz .Lasteria_amd64_bad_parameters
    testq $0xfff, %r10
    jnz .Lasteria_amd64_bad_parameters
    testq %r11, %r11
    jz .Lasteria_amd64_bad_parameters
    testq $0xf, %r11
    jnz .Lasteria_amd64_bad_parameters
    testq %rdx, %rdx
    jz .Lasteria_amd64_bad_parameters
    testq %rcx, %rcx
    jz .Lasteria_amd64_bad_parameters
    movw %cs, %ax
    movl %r9d, %edi
    shrl $16, %edi
    cmpw %di, %ax
    jne .Lasteria_amd64_bad_selectors
    movw %ss, %ax
    cmpw %r9w, %ax
    jne .Lasteria_amd64_bad_selectors
    movq %cr3, %rax
    cmpq %r8, %rax
    jne .Lasteria_amd64_bad_control_state
    movq %cr0, %rax
    movq %rax, %rdi
    movl %eax, %r9d
    andl $0x80010001, %r9d
    cmpl $0x80010001, %r9d
    jne .Lasteria_amd64_bad_control_state
    testl $0x60000000, %eax
    jnz .Lasteria_amd64_bad_control_state
    movq %cr4, %rax
    movq %rax, %rsi
    testl $0x20, %eax
    jz .Lasteria_amd64_bad_control_state
    testl $0x19821080, %eax
    jnz .Lasteria_amd64_bad_control_state
    movq %rax, %r9
    shrq $32, %r9
    testl %r9d, %r9d
    jnz .Lasteria_amd64_bad_control_state
    movl $0xc0000080, %ecx
    rdmsr
    movl %eax, %r9d
    andl $0x00000d00, %r9d
    cmpl $0x00000d00, %r9d
    jne .Lasteria_amd64_bad_control_state
    movl $0x277, %ecx
    rdmsr
    andl $0xff, %eax
    cmpl $0x06, %eax
    jne .Lasteria_amd64_bad_pat
    cli
    lidt (%r13)
    cld
    xorl %ebp, %ebp
    movq %rdi, %rax
    orl $0x40000000, %eax
    andl $0xdfffffff, %eax
    movq %rax, %cr0
    wbinvd
    movq %rsi, %rax
    andq $ASTERIA_DISABLED_TRANSLATION_FEATURES_CLEAR, %rax
    movq %rax, %cr4
    movq %r8, %cr3
    movl $0x277, %ecx
    movl $0x00070406, %eax
    movl $0x00070401, %edx
    wrmsr
    movabsq $0x4153544552494141, %r9
    cmpq $0, -8(%r11)
    jne AsteriaAmd64FailStopInterrupt
    movq %r9, -8(%r11)
    mfence
    cmpq %r9, -8(%r11)
    jne AsteriaAmd64FailStopInterrupt
    movq $0, -8(%r11)
    mfence
    cmpq $0, -8(%r11)
    jne AsteriaAmd64FailStopInterrupt
    lgdt (%r12)
    movq %r11, %rsp
    movq %r10, %cr3
    wbinvd
    movq %rdi, %cr0
    movq %cr3, %rax
    cmpq %r10, %rax
    jne AsteriaAmd64FailStopInterrupt
    movq %cr0, %rax
    cmpq %rdi, %rax
    jne AsteriaAmd64FailStopInterrupt
    movq %cr4, %rax
    cmpq %rsi, %rax
    jne AsteriaAmd64FailStopInterrupt
    movl $0xc0000080, %ecx
    rdmsr
    movl %eax, %r9d
    andl $0x00000d00, %r9d
    cmpl $0x00000d00, %r9d
    jne AsteriaAmd64FailStopInterrupt
    movl $0x277, %ecx
    rdmsr
    cmpl $0x00070406, %eax
    jne AsteriaAmd64FailStopInterrupt
    cmpl $0x00070401, %edx
    jne AsteriaAmd64FailStopInterrupt
    pushfq
    popq %rax
    testl $0x600, %eax
    jnz AsteriaAmd64FailStopInterrupt
    cmpq %r11, %rsp
    jne AsteriaAmd64FailStopInterrupt
    movabsq $0x4153544552494141, %r9
    pushq %r9
    popq %rax
    cmpq %r9, %rax
    jne AsteriaAmd64FailStopInterrupt
    movabsq $AsteriaAmd64InitialRuntimeEntry, %rax
    call *%rax
.Lasteria_amd64_activation_halt:
    hlt
    jmp .Lasteria_amd64_activation_halt
.Lasteria_amd64_bad_parameters:
    movl $1, %eax
    jmp .Lasteria_amd64_return
.Lasteria_amd64_bad_selectors:
    movl $2, %eax
    jmp .Lasteria_amd64_return
.Lasteria_amd64_bad_control_state:
    movl $3, %eax
    jmp .Lasteria_amd64_return
.Lasteria_amd64_bad_pat:
    movl $4, %eax
.Lasteria_amd64_return:
    popq %r13
    popq %r12
    ret
.size AsteriaAmd64ActivateInitialAddressSpace, . - AsteriaAmd64ActivateInitialAddressSpace
.p2align 4
.global AsteriaAmd64FailStopInterrupt
.hidden AsteriaAmd64FailStopInterrupt
.type AsteriaAmd64FailStopInterrupt, @function
AsteriaAmd64FailStopInterrupt:
    cli
    cld
    movabsq $AsteriaAmd64DebugPortPresent, %rax
    cmpb $0, (%rax)
    je .Lasteria_amd64_fault_serial
    movabsq $.Lasteria_amd64_activation_fault, %rsi
    movl $(.Lasteria_amd64_activation_fault_end - .Lasteria_amd64_activation_fault), %ecx
    movw $0x402, %dx
.Lasteria_amd64_activation_fault_loop:
    lodsb
    outb %al, %dx
    loop .Lasteria_amd64_activation_fault_loop
.Lasteria_amd64_fault_serial:
    movabsq $AsteriaAmd64DiagnosticSerialPort, %rax
    movzwl (%rax), %edi
    testl %edi, %edi
    jz .Lasteria_amd64_fault_halt
    movabsq $.Lasteria_amd64_activation_fault, %rsi
    movl $(.Lasteria_amd64_activation_fault_end - .Lasteria_amd64_activation_fault), %r8d
.Lasteria_amd64_fault_serial_byte:
    // Wait, bounded, for the transmitter holding register (LSR bit 5).
    leal 5(%rdi), %edx
    movl $20000, %ecx
.Lasteria_amd64_fault_serial_poll:
    inb %dx, %al
    testb $0x20, %al
    jnz .Lasteria_amd64_fault_serial_ready
    loop .Lasteria_amd64_fault_serial_poll
    jmp .Lasteria_amd64_fault_halt
.Lasteria_amd64_fault_serial_ready:
    movl %edi, %edx
    lodsb
    outb %al, %dx
    decl %r8d
    jnz .Lasteria_amd64_fault_serial_byte
.Lasteria_amd64_fault_halt:
    hlt
    jmp .Lasteria_amd64_fault_halt
.size AsteriaAmd64FailStopInterrupt, . - AsteriaAmd64FailStopInterrupt
.global AsteriaAmd64ActivationTextEnd
.hidden AsteriaAmd64ActivationTextEnd
AsteriaAmd64ActivationTextEnd:
.popsection
