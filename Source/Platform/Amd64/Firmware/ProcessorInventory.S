// SPDX-License-Identifier: Apache-2.0

.text
.p2align 4
.global AsteriaAmd64InventoryCpuId
.hidden AsteriaAmd64InventoryCpuId
.type AsteriaAmd64InventoryCpuId, @function
AsteriaAmd64InventoryCpuId:
    pushq %rbx
    movq %rsi, %r8
    movl %edi, %eax
    xorl %ecx, %ecx
    cpuid
    movl %eax, 0(%r8)
    movl %ebx, 4(%r8)
    movl %ecx, 8(%r8)
    movl %edx, 12(%r8)
    popq %rbx
    ret
.size AsteriaAmd64InventoryCpuId, . - AsteriaAmd64InventoryCpuId

.p2align 4
.global AsteriaAmd64InventoryApicMode
.hidden AsteriaAmd64InventoryApicMode
.type AsteriaAmd64InventoryApicMode, @function
AsteriaAmd64InventoryApicMode:
    movw %cs, %ax
    testb $3, %al
    jnz .Lunavailable
    pushq %rbx
    movl $1, %eax
    xorl %ecx, %ecx
    cpuid
    andl $0x220, %edx
    cmpl $0x220, %edx
    jne .Lunsupported
    movl $0x1b, %ecx
    rdmsr
    popq %rbx
    testl $0x800, %eax
    jz .Ldisabled
    testl $0x400, %eax
    jnz .Lx2apic
    movl $1, %eax
    ret
.Lx2apic:
    movl $2, %eax
    ret
.Ldisabled:
    xorl %eax, %eax
    ret
.Lunsupported:
    popq %rbx
.Lunavailable:
    movl $3, %eax
    ret
.size AsteriaAmd64InventoryApicMode, . - AsteriaAmd64InventoryApicMode
