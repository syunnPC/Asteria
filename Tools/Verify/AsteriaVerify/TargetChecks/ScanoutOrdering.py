# SPDX-License-Identifier: Apache-2.0
"""display composition and desktop session: ordered display publication.

Inspect the final broker, composition loop and ISA ordering adapter. Mutants
remove each edge of this chain, including the callback's binding on IA-64.
"""
from __future__ import annotations

import re

from AsteriaVerify.Checks import Property, register
from AsteriaVerify.Disassembly import disassemble
from AsteriaVerify.Elf import STT_FUNC, load


def ordered_fence(instructions, isa: str) -> bool:
    mnemonics = [instruction.mnemonic for instruction in instructions]
    if isa == 'Amd64':
        return 'sfence' in mnemonics
    return any(mnemonics[index:index + 2] == ['mf.a', 'mf']
               for index in range(len(mnemonics) - 1))


def publication_pass_facts(instructions, render_name: str) -> dict[str, bool]:
    renders = [i for i, instruction in enumerate(instructions) if render_name in instruction.text]
    replies = [i for i, instruction in enumerate(instructions)
               if any(name in instruction.text for name in ('L6Answer', 'L12RetryAnswers', 'SystemCallClient4Send'))]
    credits = [i for i, instruction in enumerate(instructions) if 'DisplaySession10SendCredit' in instruction.text]
    presented = [i for i, instruction in enumerate(instructions) if 'DisplaySession9Presented' in instruction.text]
    return {
        'query after composition': bool(renders and replies) and min(replies) > max(renders),
        'credit before composition': bool(renders and credits) and min(credits) < min(renders),
        'Presented after composition': bool(renders and presented) and min(presented) > max(renders),
    }


def ia64_callback_bound(elf, instructions, adapter: int) -> bool:
    # The GP-relative GOT slot contains one eight-byte descriptor pointer;
    # the descriptor itself contains the entry and GP (two eight-byte words).
    # Virtual calls can use the last GOT slot.
    gp = elf.symbol('__gp').value
    for instruction in instructions:
        match = re.fullmatch(r'addl r\d+=(-?\d+),r1', instruction.text)
        if match:
            address = gp + int(match[1])
            section = elf.section_containing(address)
            if section and address + 8 <= section.address + section.size:
                descriptor = int.from_bytes(elf.read_address(address, 8), 'little')
                descriptor_section = elf.section_containing(descriptor)
                if descriptor_section and descriptor + 16 <= descriptor_section.address + descriptor_section.size:
                    if int.from_bytes(elf.read_address(descriptor, 8), 'little') == adapter:
                        return True
    return False


@register
class ScanoutStoreOrdering(Property):
    variants = ('firmware',)

    def extract(self, context, variant):
        path = context.artifact(variant + ':display_elf')
        elf = load(path)
        selectors = {
            'adapter': '19OrderScanoutStorage',
            'broker': 'L6RenderERKNS0_8Graphics12PixelSurface',
            'composer': 'Composer6Render',
            'drain': 'L5Drain',
            'publication': 'L10FinishPass',
            'serve': '12ServeDisplay',
        }
        code = {}
        symbols = {}
        for key, selector in selectors.items():
            found = [symbol for symbol in elf.defined_symbols() if symbol.type == STT_FUNC
                     and selector in symbol.name and (key not in ('drain', 'publication') or 'DisplayInfo' in symbol.name)]
            if len(found) != 1:
                return {key: False}
            symbols[key] = found[0]
            code[key] = disassemble(context.tool('objdump'), str(path), symbol=found[0].name).instructions

        bound = any('19OrderScanoutStorage' in instruction.text for instruction in code['broker'])
        if context.isa == 'Ia64':
            # Function pointers are descriptors. The final code materializes a
            # descriptor relative to gp; its entry must be the ordering adapter.
            bound = bound or ia64_callback_bound(elf, code['broker'], symbols['adapter'].value)

        pieces = [index for index, instruction in enumerate(code['composer'])
                  if 'Composer15RenderRectangle' in instruction.text]
        callbacks = [index for index, instruction in enumerate(code['composer'])
                     if (instruction.mnemonic in ('call', 'jmp') and '*' in instruction.text)
                     or re.fullmatch(r'br.call[^ ]* b0=b\d+', instruction.text)]
        initial = [index for index, instruction in enumerate(code['serve'])
                   if symbols['broker'].name in instruction.text]
        waits = [index for index, instruction in enumerate(code['serve']) if 'SystemCallClient4Wait' in instruction.text]
        return {
            'ISA fence': ordered_fence(code['adapter'], context.isa),
            'callback binding': bound and any('Composer6Render' in instruction.text for instruction in code['broker']),
            'render before ordering': bool(pieces and callbacks) and min(callbacks) > max(pieces),
            # Watchdog Queries are not drawing fences; retained answers were
            # already fenced. Inspect the canonical publication pass instead
            # of conflating every send in the broker's dispatch loop.
            **publication_pass_facts(code['publication'], symbols['broker'].name),
            'drain publishes composition': any(symbols['publication'].name in instruction.text
                                              for instruction in code['drain']),
            'initial composition': bool(initial and waits) and min(initial) < min(waits),
        }

    def verify(self, facts):
        return ['%s has no ordered composition-store path' % name for name, ordered in facts.items() if not ordered]

    def mutants(self, facts):
        for name in facts:
            yield 'remove ' + name, {**facts, name: False}
