# SPDX-License-Identifier: Apache-2.0
"""The firmware CPUID adapter preserves RBX and gates its privileged MSR read."""
from AsteriaVerify import Disassembly
from AsteriaVerify.Checks import Property, register


@register
class Amd64InventoryCpu(Property):
    isas = ('Amd64',)

    def extract(self, context, variant):
        facts = {}
        for symbol in ('AsteriaAmd64InventoryCpuId', 'AsteriaAmd64InventoryApicMode'):
            path = context.defining_object(variant, symbol)
            decoded = Disassembly.disassemble(context.tool('objdump'), str(path), symbol=symbol)
            facts[symbol] = tuple(Disassembly.normalize_x86(decoded.instructions))
            if symbol.endswith('ApicMode'):
                facts['ApicOffsets'] = tuple(i.address - decoded.start for i in decoded.instructions)
        return facts

    def verify(self, facts):
        cpu = facts['AsteriaAmd64InventoryCpuId']
        apic = facts['AsteriaAmd64InventoryApicMode']
        problems = []
        if not cpu or cpu[0] != 'push %rbx' or cpu[-2:] != ('pop %rbx', 'ret'):
            problems.append('CPUID must preserve the SysV callee-saved RBX')
        for instruction in ('mov %rsi,%r8', 'mov %edi,%eax', 'xor %ecx,%ecx', 'cpuid',
                            'mov %eax,(%r8)', 'mov %ebx,0x4(%r8)', 'mov %ecx,0x8(%r8)', 'mov %edx,0xc(%r8)'):
            if instruction not in cpu:
                problems.append('CPUID input/output missing: ' + instruction)
        if apic[:2] != ('mov %cs,%ax', 'test $0x3,%al') or len(apic) < 3 or not apic[2].startswith('jne '):
            problems.append('CPL must be checked before any privileged read')
        offsets = facts['ApicOffsets']
        if len(apic) >= 3 and apic[2] != 'jne <+%#x>' % offsets[-2]:
            problems.append('CPL guard must branch directly to the unavailable return')
        required = ('cpuid', 'and $0x220,%edx', 'cmp $0x220,%edx', 'mov $0x1b,%ecx', 'rdmsr')
        positions = [apic.index(line) if line in apic else -1 for line in required]
        if -1 in positions or positions != sorted(positions):
            problems.append('APIC/MSR features must guard IA32_APIC_BASE read')
        elif apic[positions[2] + 1] != 'jne <+%#x>' % offsets[-3]:
            problems.append('unsupported APIC/MSR features must branch away')
        if apic.count('push %rbx') != 1 or apic.count('pop %rbx') != 2 or apic[-2:] != ('mov $0x3,%eax', 'ret'):
            problems.append('both CPUID paths must preserve RBX and return unavailable when refused')
        if 'test $0x800,%eax' not in apic or 'test $0x400,%eax' not in apic:
            problems.append('APIC enable and x2APIC bits must both be decoded')
        return problems

    def mutants(self, facts):
        guarded = {'mov %cs,%ax', 'test $0x3,%al', 'and $0x220,%edx', 'cmp $0x220,%edx',
                   'mov $0x1b,%ecx', 'rdmsr', 'test $0x800,%eax', 'test $0x400,%eax',
                   'mov $0x3,%eax', 'push %rbx', 'pop %rbx'}
        for symbol in ('AsteriaAmd64InventoryCpuId', 'AsteriaAmd64InventoryApicMode'):
            instructions = facts[symbol]
            for index, instruction in enumerate(instructions):
                selected = (symbol.endswith('CpuId') and instruction != 'ret') or instruction in guarded
                if selected:
                    changed = instructions[:index] + ('nop',) + instructions[index + 1:]
                    yield '%s: remove %d %s' % (symbol, index, instruction), {**facts, symbol: changed}
        symbol = 'AsteriaAmd64InventoryApicMode'
        instructions = facts[symbol]
        for index in (2, instructions.index('cmp $0x220,%edx') + 1):
            changed = instructions[:index] + ('jne <+%#x>' % facts['ApicOffsets'][instructions.index('rdmsr')],) + instructions[index + 1:]
            yield 'guard branches into privileged read %d' % index, {**facts, symbol: changed}
