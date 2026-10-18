# SPDX-License-Identifier: Apache-2.0
"""Builders of deliberately broken facts for the properties' self-tests.

Instruction edits are methods of ControlFlow.Code, and the symbol-contract
mutants belong to Properties.FunctionContract; the builders here edit other
records: tuples of dataclasses, relocation facts and leaf extents.
"""
from __future__ import annotations

import dataclasses
from typing import Iterable

from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.Properties import RelocationFact


def replace_at(items, index: int, **changes) -> tuple:
    """`items` with the dataclass at `index` changed."""
    items = tuple(items)
    return items[:index] + (dataclasses.replace(items[index], **changes),) + items[index + 1:]


def relocation_mutants(actual: tuple[RelocationFact, ...], other_symbols: Iterable[str] = ()):
    """An extra call and data reference, and each relocation retargeted (to
    another of `other_symbols`), made section-relative, moved to another
    slot, given an addend, turned into a function-descriptor relocation or
    dropped. Each mutant is sorted like the extracted facts."""
    other_symbols = tuple(other_symbols)
    callee = other_symbols[0] if other_symbols else 'AsteriaIa64UnexpectedCallee'
    yield 'gains a PCREL21B call', tuple(sorted(actual + (RelocationFact(0, 'PCREL21B', callee, 0),)))
    yield 'gains an LTOFF22X data reference', tuple(sorted(actual + (
        RelocationFact(0x10, 'LTOFF22X', 'AsteriaIa64UnexpectedDatum', 0),)))
    for index, fact in enumerate(actual):
        label = '%s at +%#x' % (fact.type, fact.offset)
        other = next((symbol for symbol in other_symbols if symbol != fact.target), 'AsteriaIa64UnexpectedTarget')
        for description, changes in (('retargeted', dict(target=other)), ('against .data', dict(target='.data')),
                                     ('moved to another slot', dict(offset=fact.offset + 0x10)),
                                     ('with an addend', dict(addend=fact.addend + 8)),
                                     ('as a function-descriptor relocation', dict(type='FPTR64I'))):
            yield '%s %s' % (label, description), tuple(sorted(replace_at(actual, index, **changes)))
        yield label + ' missing', actual[:index] + actual[index + 1:]


def leaf_extent_mutants(leaf):
    """Broken extents of a Subjects.Leaf, for Properties.leaf_extent_problems."""
    yield 'oversized by a bundle', dataclasses.replace(leaf, size=leaf.size + 16)
    yield 'undersized by a bundle', dataclasses.replace(leaf, size=leaf.size - 16)
    yield 'tail bundle beyond the decoded code', dataclasses.replace(
        leaf, instructions=leaf.instructions + (Instruction(leaf.end, 'mov r8=r0', 'MIB', 0, True),))
    yield 'first bundle missing', dataclasses.replace(leaf, instructions=tuple(
        instruction for instruction in leaf.instructions if instruction.address & ~0xF != leaf.start))
    extent = [index for index, instruction in enumerate(leaf.instructions)
              if instruction.address < leaf.start + leaf.size and not instruction.is_nop]
    if extent:
        yield 'falls through its last bundle', dataclasses.replace(leaf, instructions=replace_at(
            leaf.instructions, extent[-1], text='nop.b 0x0', predicate=None, relocation=None))
