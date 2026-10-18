# SPDX-License-Identifier: Apache-2.0
"""The portable split-ring transport must emit full device-memory barriers.

Inspect optimized native code, where the platform adapter is inlined. Each
queue publication, availability publication, MMIO notification and completion
consumption must retain a hardware fence. Removing one is a mutation failure.
"""
from __future__ import annotations

from AsteriaVerify.Checks import Property, register
from AsteriaVerify.Disassembly import disassemble
from AsteriaVerify.Elf import STT_FUNC, load


@register
class VirtioDeviceMemoryOrdering(Property):
    variants = ('O2',)

    def extract(self, context, variant):
        path = context.object(variant, 'Source/Platform/Device/VirtioPciTransport.cpp')
        elf = load(path)
        facts = {}
        for method in ('SetupQueue', 'Submit', 'Notify', 'TryPoll'):
            matches = [symbol for symbol in elf.defined_symbols()
                       if symbol.type == STT_FUNC and ('18VirtioPciTransport' + str(len(method)) + method)
                       in symbol.name]
            if len(matches) != 1:
                facts[method] = False
                continue
            instructions = disassemble(context.tool('objdump'), str(path), symbol=matches[0].name).instructions
            facts[method] = any(instruction.mnemonic in ('mf', 'mfence', 'lock') for instruction in instructions)
        return facts

    def verify(self, facts):
        return ['%s has no full DMA/MMIO barrier' % method for method, fenced in facts.items() if not fenced]

    def mutants(self, facts):
        for method in facts:
            yield 'remove ' + method + ' barrier', dict(facts, **{method: False})
