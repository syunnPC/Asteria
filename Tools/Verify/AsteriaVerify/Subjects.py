# SPDX-License-Identifier: Apache-2.0
"""The artifacts a property inspects, decoded once per run, and the property
bases that inspect them.

A `Subject` is one artifact that contains a checked symbol: the object of a
variant that defines it (located by symbol, never by source file, so a check
does not depend on which object holds the code) or a linked image
(`image_key`). `code` decodes a
symbol's extent into a ControlFlow.Code and `leaf` an assembly leaf up to the
next symbol; both are cached, since several properties read the same extent.

Property bases: `CodeProperty` (the decoded extent of one symbol per
subject), `SymbolContract` (the function-symbol contract of a set of
symbols), and the helper-group properties, which judge each hand-written
helper of a `HelperGroup` per variant (`HelperSymbols`, `HelperExtents`,
`HelperRelocations`).
"""
from __future__ import annotations

import dataclasses
import functools
import pathlib
from typing import Callable

from AsteriaVerify import Disassembly, Mutate, Properties
from AsteriaVerify.Checks import ItemProperty, Property
from AsteriaVerify.Context import ContextError
from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.Elf import STT_FILE, STT_SECTION, ElfFile, Symbol, load

VARIANTS = ('firmware', 'O0', 'O2')


def image_key(variant: str) -> str:
    """The linked kernel image of a variant."""
    return 'firmware:kernel_elf' if variant == 'firmware' else '%s:nucleus_elf' % variant


@dataclasses.dataclass(frozen=True)
class Subject:
    label: str
    variant: str
    path: pathlib.Path
    linked: bool


def artifacts(context, symbol: str, *, variants=VARIANTS, objects: bool = True, images: bool = False,
              image_filter: Callable[[ElfFile], bool] | None = None) -> list[Subject]:
    """The objects that define `symbol` and, with `images`, the linked images
    of the same variants (those `image_filter` accepts)."""
    subjects = []
    for variant in variants:
        if not context.has_variant(variant):
            continue
        if objects:
            subjects.append(Subject('%s object' % variant, variant, context.defining_object(variant, symbol), False))
        key = image_key(variant)
        if images and context.has_artifact(key) and (image_filter is None or image_filter(load(context.artifact(key)))):
            subjects.append(Subject(key, variant, context.artifact(key), True))
    return subjects


def defining(context, variant: str, symbol: str) -> pathlib.Path | None:
    """The object of `variant` that defines `symbol`, or None."""
    try:
        return context.defining_object(variant, symbol)
    except ContextError:
        return None


def code(context, path, symbol: str | None = None, *, start: str | None = None, end: str | None = None) -> Code:
    """The decoded extent of `symbol`, or of [start, end), with its relocations."""
    return _code(context.tool('objdump'), str(path), symbol, start, end)


@functools.lru_cache(maxsize=None)
def _code(objdump: str, path: str, symbol: str | None, start: str | None, end: str | None) -> Code:
    decoded = Disassembly.disassemble(objdump, path, symbol=symbol, start_symbol=start, end_symbol=end)
    elf = load(path)
    relocations = Properties.relocations_in_extent(elf, elf.symbol(symbol or start).section_index, decoded.start,
                                                   decoded.end)
    return Code(decoded.start, tuple(decoded.instructions), tuple(relocations), decoded.end, decoded.label)


# Assembly leaves ----------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Leaf:
    """A leaf's ELF extent and the code decoded after its start: up to the
    next symbol of its section, or to the section end when it is the last
    symbol, so that code an undersized `.size` leaves unlabelled is decoded
    too; otherwise exactly the extent. `relocations` are those of the
    extent and of the code."""
    name: str
    start: int
    size: int
    limit: int | None           # start of the next symbol of the section
    end: int                    # end of the decoded code
    instructions: tuple[Instruction, ...]
    relocations: tuple[Properties.RelocationFact, ...]

    @property
    def code(self) -> Code:
        """The leaf's instructions up to the last bundle that holds a
        meaningful one: alignment padding consists of nop-only bundles, so
        this is the code whether or not `.size` covers all of it."""
        return Code(self.start, _code_of(self.instructions), self.relocations, self.end, self.name)


def _code_of(instructions) -> tuple[Instruction, ...]:
    last = max((instruction.address & ~0xF for instruction in instructions if not instruction.is_nop), default=None)
    return tuple(instruction for instruction in instructions if last is not None and instruction.address & ~0xF <= last)


def leaf(context, variant: str, name: str) -> Leaf:
    """The leaf `name` of the object of `variant` that defines it."""
    return _leaf(context.tool('objdump'), str(context.defining_object(variant, name)), name)


def _following(elf: ElfFile, symbol: Symbol) -> tuple[Symbol | None, int | None]:
    """The first uniquely named symbol of the section after `symbol`, and the
    start of the first symbol of any name after it."""
    later = [candidate for candidate in elf.symbols
             if candidate.section_index == symbol.section_index and candidate.name
             and candidate.type not in (STT_SECTION, STT_FILE) and candidate.value > symbol.value]
    if not later:
        return None, None
    first = min(candidate.value for candidate in later)
    unique = sorted((candidate for candidate in later
                     if candidate.value == first and len(elf.symbols_named(candidate.name)) == 1),
                    key=lambda candidate: candidate.name)
    return (unique[0] if unique else None), first


@functools.lru_cache(maxsize=None)
def _leaf(objdump: str, path: str, name: str) -> Leaf:
    elf = load(path)
    definitions = elf.definitions(name)
    if len(definitions) != 1:
        raise ContextError('%s defines %s %d times' % (path, name, len(definitions)))
    symbol = definitions[0]
    section = elf.section_of(symbol)
    end = symbol.value + symbol.size
    following, limit = _following(elf, symbol)
    if following is not None and following.value >= end:
        decoded = Disassembly.disassemble(objdump, path, start_symbol=name, end_symbol=following.name)
    else:
        decoded = Disassembly.disassemble(objdump, path, symbol=name)
    instructions, decoded_end = tuple(decoded.instructions), decoded.end
    if following is None or following.value < end:
        section_end = section.address + section.size if section is not None else end
        if limit is None and section_end >= end:
            limit = section_end
            if section_end > end:
                instructions += tuple(Disassembly.raw_instructions(objdump, path, section.name, end, section_end))
                decoded_end = section_end
    code = _code_of(instructions)
    code_end = (code[-1].address & ~0xF) + 16 if code else symbol.value
    relocations = Properties.relocations_in_extent(elf, symbol.section_index, symbol.value, max(code_end, end))
    return Leaf(name, symbol.value, symbol.size, limit, decoded_end, instructions, tuple(relocations))


# Property bases -----------------------------------------------------------------

class CodeProperty(Property):
    """Facts: the decoded extent of SYMBOL in the object of each variant that
    defines it and, with IMAGES, in each linked image."""
    isas = ('Ia64',)
    SYMBOL = ''
    IMAGES = False

    def subjects(self, context):
        return artifacts(context, self.SYMBOL, images=self.IMAGES)

    def describe(self, subject):
        return subject.label

    def extract(self, context, subject):
        return code(context, subject.path, self.SYMBOL)


class SymbolContract(Property):
    """The function-symbol contract CONTRACT of every symbol of SYMBOLS, each
    in the object that defines it (with SAME_SECTION, all in the object that
    defines the first, in one section) and, with IMAGES, in the linked
    images."""
    isas = ('Ia64',)
    SYMBOLS: tuple[str, ...] = ()
    CONTRACT = Properties.FunctionContract(Properties.HIDDEN, alignment=16, size_multiple=16)
    SAME_SECTION = False
    IMAGES = False

    def subjects(self, context):
        return artifacts(context, self.SYMBOLS[0], images=self.IMAGES)

    def describe(self, subject):
        return subject.label

    def extract(self, context, subject):
        facts = []
        for name in self.SYMBOLS:
            path = subject.path if subject.linked or self.SAME_SECTION else defining(context, subject.variant, name)
            facts.append((name, tuple(Properties.symbol_facts(load(path), name)) if path is not None else ()))
        return tuple(facts)

    def verify(self, facts):
        problems = [problem for name, entries in facts for problem in self.CONTRACT.problems(entries, name)]
        sections = {entry.symbol.section_index for _, entries in facts for entry in entries}
        if self.SAME_SECTION and len(sections) > 1:
            problems.append('the symbols lie in %d sections, expected one' % len(sections))
        return problems

    def mutants(self, facts):
        for index, (name, entries) in enumerate(facts):
            for description, changed in self.CONTRACT.mutants(entries):
                yield '%s: %s' % (name, description), facts[:index] + ((name, changed),) + facts[index + 1:]
            if self.SAME_SECTION and len(facts) > 1:
                entry = entries[0]
                moved = dataclasses.replace(entry, symbol=dataclasses.replace(
                    entry.symbol, section_index=entry.symbol.section_index + 1), section=dataclasses.replace(
                    entry.section, index=entry.symbol.section_index + 1))
                yield '%s: in another section' % name, facts[:index] + ((name, (moved,) + entries[1:]),) + facts[
                    index + 1:]


# Helper groups -------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class HelperGroup:
    """Hand-written assembly helpers written together (one asm block). With
    `option`, a configuration variable, the firmware objects contain the
    group only when the tree was built with that option `1`; the
    compile-check variants always do."""
    label: str
    helpers: tuple[str, ...]
    option: str | None = None

    def subset(self, *helpers: str) -> 'HelperGroup':
        return HelperGroup(self.label, helpers, self.option)

    def built(self, context, variant: str) -> bool:
        return self.option is None or variant != 'firmware' or context.configuration.get(self.option) == '1'


class HelperProperty(ItemProperty):
    """A property over the helpers of each (variant, helper group); by
    default an item is the helper's code (Leaf.code)."""
    isas = ('Ia64',)
    GROUPS: tuple[HelperGroup, ...] = ()

    def subjects(self, context):
        return [(variant, group) for variant in self.variants if context.has_variant(variant) for group in self.GROUPS
                if group.built(context, variant)]

    def describe(self, subject):
        return '%s %s' % (subject[0], subject[1].label)

    def extract(self, context, subject):
        return {name: self.item(context, subject[0], name) for name in subject[1].helpers}

    def item(self, context, variant: str, name: str):
        return leaf(context, variant, name).code


class HelperSymbols(HelperProperty):
    """P-HELPER-ELF for every helper of the groups: exactly one FUNC, internal
    to the image (LOCAL, or GLOBAL with HIDDEN visibility once the asm sits
    in its own object; never exported with DEFAULT visibility), with a
    non-zero whole-bundle extent that starts on a bundle boundary inside a
    `.text` PROGBITS section whose flags are exactly ALLOC|EXECINSTR and whose
    alignment is a power of two of at least 16. The exact size is in the
    golden."""
    CONTRACT = Properties.FunctionContract(Properties.INTERNAL, alignment=16, size_multiple=16, section='.text')

    def item(self, context, variant, name):
        path = defining(context, variant, name)
        return tuple(Properties.symbol_facts(load(path), name)) if path is not None else ()

    def verify(self, facts):
        return [problem for name, entries in facts.items() for problem in self.CONTRACT.problems(entries, name)]

    def item_mutants(self, entries):
        return self.CONTRACT.mutants(entries)


class HelperExtents(HelperProperty):
    """P-EXTENT for every helper of the groups: the ELF extent is exactly the
    helper's code. Its bundles tile [st_value, st_value + st_size), its last
    bundle ends with an unconditional branch or return, it does not reach the
    next symbol, and only nop padding lies between it and the next symbol, so
    every check that reads the extent sees the whole helper."""

    def item(self, context, variant, name):
        return leaf(context, variant, name)

    def verify(self, facts):
        return [problem for name, extent in facts.items() for problem in Properties.leaf_extent_problems(name, extent)]

    def item_mutants(self, extent):
        return Mutate.leaf_extent_mutants(extent)


class HelperRelocations(HelperProperty):
    """P-RELOC for every helper of the groups: the relocations whose r_offset
    lies in the helper's code, read from the ELF RELA sections and written as
    (offset from the helper, type, resolved target, addend), are exactly the
    expected ones; a section-relative target is resolved to the symbol it
    addresses. Helpers without an entry in EXPECTED have none."""
    EXPECTED: dict[str, tuple[tuple[int, str, str, int], ...]] = {}

    def extract(self, context, subject):
        variant, group = subject
        return {name: (tuple(sorted(leaf(context, variant, name).relocations)),
                       tuple(sorted(Properties.RelocationFact(*item) for item in self.EXPECTED.get(name, ()))))
                for name in group.helpers}

    def item_problems(self, item):
        return Properties.multiset_problems(*item, 'relocation')

    def mutants(self, facts):
        for name, (actual, expected) in facts.items():
            others = tuple(other for other in facts if other != name)
            for description, changed in Mutate.relocation_mutants(actual, others):
                yield '%s: %s' % (name, description), {**facts, name: (changed, expected)}
