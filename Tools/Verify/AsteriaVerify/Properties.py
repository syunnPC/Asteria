# SPDX-License-Identifier: Apache-2.0
"""Reusable judgments over ELF records and decoded instructions.

The judgments here take plain facts (symbols, sections, relocations,
decoded instructions or Code) and return a list of problem descriptions; an
empty list means the property holds. The fact helpers beside them
(symbol_facts, relocations_in_extent, resolved_values, fail_stop) read
values the judgments share. Property classes in TargetChecks extract the
facts from artifacts, call these functions from `verify`, and build mutants
by editing the same facts, so one implementation serves both the real check
and its self-test.
"""
from __future__ import annotations

import dataclasses
import re
from collections import Counter
from typing import Callable, Iterable

from AsteriaVerify import Ia64, Layout, Relocations
from AsteriaVerify.ControlFlow import MARKERS, Code
from AsteriaVerify.Disassembly import Instruction, describe_address, group_indices
from AsteriaVerify.Elf import (
    BINDING_NAMES, EF_IA_64_ABI64, EM_IA_64, EM_X86_64, ET_REL, PF_R, PF_W, PF_X, PT_LOAD, SHF_ALLOC,
    SHF_EXECINSTR, SHF_WRITE, SHN_UNDEF, SHT_NOBITS, SHT_PROGBITS, SHT_REL, SHT_RELA, STB_GLOBAL, STB_LOCAL,
    STB_WEAK, STT_FUNC, STT_NOTYPE, STT_OBJECT, STT_SECTION, STV_DEFAULT, STV_HIDDEN, STV_INTERNAL, STV_PROTECTED,
    TYPE_NAMES, VISIBILITY_NAMES, ElfFile, Section, Segment, Symbol)

# Sections that would require C++ runtime or dynamic-linking support the
# kernel does not provide.
FORBIDDEN_SECTIONS = (
    '.eh_frame', '.eh_frame_hdr', '.gcc_except_table',
    '.init_array', '.fini_array', '.preinit_array', '.ctors', '.dtors',
    '.tdata', '.tbss',
    '.plt', '.got.plt', '.dynamic', '.interp', '.dynsym', '.dynstr', '.rela.dyn',
)
# IA-64 objects must not carry unwind tables or function descriptors of
# their own; a linked image keeps its entry descriptor in `.opd`.
IA64_FORBIDDEN_OBJECT_SECTION_PREFIXES = ('.IA_64.unwind', '.opd')

MACHINES = {'Ia64': EM_IA_64, 'Amd64': EM_X86_64}
MACHINE_FLAGS = {'Ia64': EF_IA_64_ABI64, 'Amd64': 0}


# ELF facts -------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Header:
    elf_class: int
    data: int
    type: int
    machine: int
    flags: int
    entry: int


def header_of(elf: ElfFile) -> Header:
    return Header(elf.data[4], elf.data[5], elf.type, elf.machine, elf.flags, elf.entry)


def header_problems(header: Header, isa: str, expected_type: int) -> list[str]:
    problems = []
    if header.elf_class != 2 or header.data != 1:
        problems.append('not an ELF64 little-endian file')
    if header.type != expected_type:
        problems.append('e_type is %d, expected %d' % (header.type, expected_type))
    if header.machine != MACHINES[isa]:
        problems.append('e_machine is %d, expected %d' % (header.machine, MACHINES[isa]))
    if header.flags != MACHINE_FLAGS[isa]:
        problems.append('e_flags is %#x, expected %#x' % (header.flags, MACHINE_FLAGS[isa]))
    return problems


def forbidden_section_problems(section_names: Iterable[str], isa: str, *, linked: bool) -> list[str]:
    problems = []
    for name in section_names:
        if name in FORBIDDEN_SECTIONS:
            problems.append('forbidden section %s' % name)
        elif isa == 'Ia64' and name.startswith(IA64_FORBIDDEN_OBJECT_SECTION_PREFIXES):
            if not (linked and name == '.opd'):
                problems.append('forbidden section %s' % name)
    return problems


def undefined_names(elf: ElfFile) -> list[str]:
    return sorted({symbol.name for symbol in elf.undefined_symbols()})


def undefined_problems(undefined: Iterable[str], allowed: Iterable[str], *, exact: bool = False) -> list[str]:
    """Undefined symbols must be a subset of (or, when exact, equal to) `allowed`."""
    undefined, allowed = set(undefined), set(allowed)
    problems = ['unexpected undefined symbol %s' % name for name in sorted(undefined - allowed)]
    if exact:
        problems.extend('expected undefined symbol %s is not referenced' % name for name in sorted(allowed - undefined))
    return problems


def relocation_section_problems(sections: Iterable[Section]) -> list[str]:
    """A statically linked image must not keep relocation sections."""
    return ['relocation section %s remains (%d bytes)' % (s.name, s.size)
            for s in sections if s.type in (SHT_REL, SHT_RELA) and s.size]


def load_segment_problems(segments: Iterable[Segment], *, allow_rwx: bool = False) -> list[str]:
    problems = []
    loads = [segment for segment in segments if segment.type == PT_LOAD]
    if not loads:
        problems.append('no PT_LOAD segment')
    for index, segment in enumerate(loads):
        label = 'PT_LOAD %d at %#x' % (index, segment.virtual_address)
        if segment.file_size > segment.memory_size:
            problems.append('%s: p_filesz %#x exceeds p_memsz %#x' % (label, segment.file_size, segment.memory_size))
        if segment.memory_size == 0:
            problems.append('%s: p_memsz is zero' % label)
        if segment.virtual_address + segment.memory_size > 1 << 64:
            problems.append('%s: extent wraps the address space' % label)
        if not allow_rwx and segment.flags & (PF_R | PF_W | PF_X) == (PF_R | PF_W | PF_X):
            problems.append('%s: segment is writable and executable' % label)
    return problems


@dataclasses.dataclass(frozen=True)
class SymbolFacts:
    """A symbol together with its section, as needed by the symbol contract."""
    symbol: Symbol
    section: Section | None
    relocatable: bool


def symbol_facts(elf: ElfFile, name: str) -> list[SymbolFacts]:
    return [SymbolFacts(symbol, elf.section_of(symbol), elf.type == ET_REL)
            for symbol in elf.symbols_named(name) if symbol.section_index != SHN_UNDEF]


# Accepted (binding, visibility) pairs of function symbols.
EXPORTED = frozenset({(STB_GLOBAL, STV_DEFAULT)})
HIDDEN = frozenset({(STB_GLOBAL, STV_HIDDEN)})
# Called across objects, bound inside the statically linked image.
CALLABLE = EXPORTED | HIDDEN
# Internal to the image: LOCAL, or GLOBAL with HIDDEN or INTERNAL visibility
# (an asm helper in its own object); never exported with DEFAULT visibility.
INTERNAL = frozenset({(STB_LOCAL, visibility) for visibility in VISIBILITY_NAMES}
                     | {(STB_GLOBAL, STV_HIDDEN), (STB_GLOBAL, STV_INTERNAL)})
LOCAL = frozenset({(STB_LOCAL, STV_DEFAULT)})
_LINKAGE_PROBES = ((STB_LOCAL, STV_DEFAULT), (STB_LOCAL, STV_HIDDEN), (STB_GLOBAL, STV_DEFAULT),
                   (STB_GLOBAL, STV_HIDDEN), (STB_GLOBAL, STV_PROTECTED), (STB_WEAK, STV_DEFAULT),
                   (STB_WEAK, STV_HIDDEN))


def _linkage(binding: int, visibility: int) -> str:
    return '%s %s' % (BINDING_NAMES.get(binding, binding), VISIBILITY_NAMES.get(visibility, visibility))


@dataclasses.dataclass(frozen=True)
class FunctionContract:
    """The function-symbol contract: exactly one definition; FUNC with an
    accepted (binding, visibility) pair; non-zero size; defined in a PROGBITS
    section whose flags are exactly ALLOC|EXECINSTR and whose alignment is a
    power of two of at least `alignment` (named `section` when given); value
    and size multiples of `alignment` and `size_multiple`; the extent inside
    the section. `mutants` breaks each clause in turn."""
    linkage: frozenset[tuple[int, int]]
    alignment: int = 1
    size_multiple: int = 1
    section: str | None = None

    def problems(self, facts: Iterable[SymbolFacts], name: str) -> list[str]:
        facts = list(facts)
        if len(facts) != 1:
            return ['%s has %d definitions, expected exactly one' % (name, len(facts))]
        fact = facts[0]
        symbol, section = fact.symbol, fact.section
        problems = []
        if symbol.type != STT_FUNC:
            problems.append('%s is %s, not FUNC' % (name, TYPE_NAMES.get(symbol.type, symbol.type)))
        if (symbol.binding, symbol.visibility) not in self.linkage:
            problems.append('%s is %s, expected %s' % (name, _linkage(symbol.binding, symbol.visibility), ' or '.join(
                sorted(_linkage(*pair) for pair in self.linkage))))
        if symbol.size == 0:
            problems.append('%s has size zero' % name)
        if section is None:
            return problems + ['%s is not defined in a section' % name]
        if section.type != SHT_PROGBITS:
            problems.append('%s lies in %s of type %d, not PROGBITS' % (name, section.name, section.type))
        if self.section is not None and section.name != self.section:
            problems.append('%s lies in %s, not in %s' % (name, section.name, self.section))
        if section.flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR) != SHF_ALLOC | SHF_EXECINSTR:
            problems.append('%s lies in %s whose flags are %#x, not exactly ALLOC|EXECINSTR'
                            % (name, section.name, section.flags))
        if symbol.value % self.alignment or symbol.size % self.size_multiple:
            problems.append('%s value %#x or size %#x is not a multiple of %d/%d'
                            % (name, symbol.value, symbol.size, self.alignment, self.size_multiple))
        if section.alignment < self.alignment or section.alignment & (section.alignment - 1):
            problems.append('%s section %s is aligned to %d, not a power of two of at least %d'
                            % (name, section.name, section.alignment, self.alignment))
        base = 0 if fact.relocatable else section.address
        if not (base <= symbol.value and symbol.value + symbol.size <= base + section.size):
            problems.append('%s extent [%#x, %#x) lies outside %s'
                            % (name, symbol.value, symbol.value + symbol.size, section.name))
        return problems

    def mutants(self, facts: tuple[SymbolFacts, ...]) -> Iterable[tuple[str, tuple[SymbolFacts, ...]]]:
        entry, rest = facts[0], facts[1:]
        symbol, section = entry.symbol, entry.section

        def with_symbol(**changes):
            return (dataclasses.replace(entry, symbol=dataclasses.replace(symbol, **changes)),) + rest

        def with_section(**changes):
            return (dataclasses.replace(entry, section=dataclasses.replace(section, **changes)),) + rest

        yield 'zero size', with_symbol(size=0)
        yield 'data type', with_symbol(type=STT_OBJECT)
        yield 'untyped symbol', with_symbol(type=STT_NOTYPE)
        for binding, visibility in _LINKAGE_PROBES:
            if (binding, visibility) not in self.linkage:
                yield _linkage(binding, visibility), with_symbol(binding=binding, visibility=visibility)
        if self.alignment > 1:
            yield 'value off alignment', with_symbol(value=symbol.value + self.alignment // 2)
            yield 'section aligned below the contract', with_section(alignment=1)
        if self.size_multiple > 1:
            yield 'partial size', with_symbol(size=symbol.size + self.size_multiple // 2)
        base = 0 if entry.relocatable else section.address
        yield 'extent beyond its section', with_symbol(
            value=base + (section.size + self.alignment - 1) // self.alignment * self.alignment)
        yield 'writable section', with_section(flags=section.flags | SHF_WRITE)
        yield 'non-executable section', with_section(flags=section.flags & ~SHF_EXECINSTR)
        yield 'unallocated section', with_section(flags=section.flags & ~SHF_ALLOC)
        yield 'NOBITS section', with_section(type=SHT_NOBITS)
        yield 'section alignment not a power of two', with_section(alignment=48)
        if self.section is not None:
            yield 'another section', with_section(name=self.section + '.unlikely')
        yield 'no section', (dataclasses.replace(entry, symbol=dataclasses.replace(symbol, section_index=0),
                                                 section=None),) + rest
        yield 'second definition', facts + facts[:1]
        yield 'missing', ()


def unique_code_definition_problems(facts: list[SymbolFacts], name: str) -> list[str]:
    """Exactly one FUNC definition in an executable section (any binding)."""
    code = [fact for fact in facts
            if fact.symbol.type == STT_FUNC and fact.section is not None
            and fact.section.flags & SHF_EXECINSTR]
    if len(facts) != 1 or len(code) != 1:
        return ['%s: %d definitions, %d of them FUNC in executable sections; expected exactly one'
                % (name, len(facts), len(code))]
    return []


# Relocations -------------------------------------------------------------------

@dataclasses.dataclass(frozen=True, order=True)
class RelocationFact:
    """A relocation inside a symbol extent, independent of object layout."""
    offset: int         # from the start of the extent
    type: str           # without the R_IA64_/R_X86_64_ prefix
    target: str         # symbol name, or `symbol+0xN` for section-relative targets
    addend: int         # addend of a named-symbol relocation; 0 when folded into target
    # (section index, value) the relocation resolves to (not compared)
    resolved: tuple[int, int] | None = dataclasses.field(default=None, compare=False, repr=False)


def relocations_in_extent(elf: ElfFile, section_index: int, start: int, end: int) -> list[RelocationFact]:
    """Relocations whose r_offset lies in [start, end) of a section."""
    facts = []
    for relocation in elf.relocations_in(section_index, start, end):
        kind = Relocations.name(elf.machine, relocation.type)
        symbol = relocation.symbol
        if symbol is None:
            target, addend, resolved = '', relocation.addend, (0, relocation.addend)
        elif symbol.type == STT_SECTION:
            value = (0 if elf.type == ET_REL else elf.sections[symbol.section_index].address) + relocation.addend
            target, addend = describe_address(elf, symbol.section_index, value), 0
            resolved = (symbol.section_index, value)
        else:
            target, addend = symbol.name, relocation.addend
            resolved = (symbol.section_index, symbol.value + relocation.addend)
        facts.append(RelocationFact(relocation.offset - start, kind, target, addend, resolved))
    return sorted(facts)


def multiset_problems(actual: Iterable, expected: Iterable, what: str) -> list[str]:
    """Each item `actual` holds beyond `expected`, and each it lacks (with multiplicity)."""
    actual, expected = Counter(actual), Counter(expected)
    return (['unexpected %s %s' % (what, item) for item in sorted((actual - expected).elements())]
            + ['missing %s %s' % (what, item) for item in sorted((expected - actual).elements())])


# Decoded instructions ----------------------------------------------------------

def extent_problems(instructions: list[Instruction], start: int, size: int) -> list[str]:
    """IA-64 bundles tile [start, start + size): one bundle per 16 bytes."""
    if size % 16:
        return ['extent size %#x is not a whole number of bundles' % size]
    bundles = sorted({instruction.address & ~0xF for instruction in instructions})
    expected = list(range(start, start + size, 16))
    if bundles != expected:
        return ['decoded bundles %s do not tile [%#x, %#x)' % ([hex(b) for b in bundles[:4]], start, start + size)]
    templates = [instruction for instruction in instructions if instruction.slot == 0]
    if len(templates) != size // 16:
        return ['decoded %d bundle starts for %d bundles' % (len(templates), size // 16)]
    return []


def leaf_extent_problems(name: str, leaf) -> list[str]:
    """P-EXTENT of an assembly leaf decoded up to the next symbol (see
    Subjects.Leaf): the decoded bundles tile [start, end); the ELF extent is
    whole bundles that end with the bundle of an unconditional branch or
    return, stays clear of the next symbol, and is followed by nothing but
    nop padding before that symbol."""
    problems = []
    end = leaf.start + leaf.size
    problems += ['%s: decoded code %s' % (name, problem) for problem in
                 extent_problems(list(leaf.instructions), leaf.start, leaf.end - leaf.start)]
    extent = [instruction for instruction in leaf.instructions if instruction.address < end]
    problems += ['%s: ELF extent: %s' % (name, problem) for problem in extent_problems(extent, leaf.start, leaf.size)]
    if leaf.limit is not None and leaf.limit < end:
        problems.append('%s: its extent [%#x, %#x) runs into the next symbol at %#x'
                        % (name, leaf.start, end, leaf.limit))
    meaningful = [instruction for instruction in extent if not instruction.is_nop]
    if not meaningful:
        problems.append('%s: its extent holds no instruction' % name)
    else:
        last = meaningful[-1]
        operation = Ia64.parse(last)
        if not operation.is_branch or operation.is_call or operation.is_conditional_branch:
            problems.append('%s: execution falls off the end of its extent after %s at +%#x'
                            % (name, last.rendered(), last.address - leaf.start))
        elif last.address & ~0xF != end - 16:
            problems.append('%s: its extent ends %#x bytes after its last instruction bundle'
                            % (name, end - (last.address & ~0xF) - 16))
    if leaf.limit is not None:
        unlabelled = [instruction for instruction in leaf.instructions
                      if end <= instruction.address < leaf.limit and not instruction.is_nop]
        if unlabelled:
            problems.append('%s: its ELF extent [%#x, %#x) misses %d instructions of its code before the next '
                            'symbol at %#x (%s); a `.size` evaluated before GAS emitted the pending bundles '
                            '(no `.align 16` before `.size`) truncates it'
                            % (name, leaf.start, end, len(unlabelled), leaf.limit,
                               '; '.join(instruction.rendered() for instruction in unlabelled)))
    return problems


_LINK = re.compile(r'^br\S*\s+(b\d)=')


def branch_references(instructions: Iterable[Instruction], callee: str) -> list[int]:
    """Indices of instructions that branch to `callee`+0: direct calls and
    branches, and any instruction carrying a branch relocation against it.

    An object names the callee through a relocation (`<reloc:PCREL21B C>`) or,
    for a function of the same section that the assembler resolved, through
    the resolved target; a linked image through the resolved target.
    """
    references = []
    for index, instruction in enumerate(instructions):
        operation = Ia64.parse(instruction)
        target = operation.target
        if target is None or target.symbol != callee or target.offset != 0:
            continue
        if operation.is_branch or (target.kind == 'reloc' and target.relocation in Relocations.IA64_BRANCH):
            references.append(index)
    return references


def call_form(instructions: list[Instruction], callee: str) -> str | None:
    """'call' or 'tail branch' for the single branch reference to `callee`."""
    references = branch_references(instructions, callee)
    if len(references) != 1:
        return None
    operation = Ia64.parse(instructions[references[0]])
    if operation.is_call:
        return 'call'
    return 'tail branch' if operation.is_branch and not operation.is_return else None


def call_problems(instructions: list[Instruction], callee: str, label: str | None = None, *,
                  allow_tail_branch: bool = False) -> list[str]:
    """Exactly one direct, unpredicated `br.call b0=` to `callee` (or, with
    `allow_tail_branch`, one unpredicated direct tail branch to it). In an
    object, a call through a relocation carries a PCREL21B relocation in
    slot 2 of the call bundle."""
    label = label or callee
    references = branch_references(instructions, callee)
    if len(references) != 1:
        return ['%d direct branch references to %s, expected exactly one' % (len(references), label)]
    instruction = instructions[references[0]]
    operation = Ia64.parse(instruction)
    problems = []
    if instruction.predicate is not None:
        problems.append('the branch to %s is predicated (%s)' % (label, instruction.rendered()))
    if operation.is_call:
        link = _LINK.match(instruction.text)
        if not link or link.group(1) != 'b0':
            problems.append('the call to %s links through %s, not b0' % (label, link.group(1) if link else '?'))
    elif not (allow_tail_branch and operation.is_branch and not operation.is_return):
        problems.append('the reference to %s is not a br.call (%s)' % (label, instruction.rendered()))
    target = operation.target
    if target.kind == 'reloc':
        if target.relocation != 'PCREL21B':
            problems.append('the call to %s uses a %s relocation, not PCREL21B' % (label, target.relocation))
        if instruction.slot != 2:
            problems.append('the relocation of the call to %s is in slot %d, not slot 2 of the call bundle'
                            % (label, instruction.slot))
    return problems


def order_problems(instructions: list[Instruction], callees, labels) -> list[str]:
    """Each callee has one branch reference and they appear in address order
    (a proxy for program order where the code is compiler-generated)."""
    sites = []
    for callee, label in zip(callees, labels):
        references = branch_references(instructions, callee)
        if len(references) != 1:
            return ['cannot order the calls: %d references to %s' % (len(references), label)]
        sites.append(references[0])
    return ['the call to %s does not precede the call to %s' % (first_label, second_label)
            for (first, first_label), (second, second_label) in zip(zip(sites, labels), zip(sites[1:], labels[1:]))
            if not first < second]


def serialization_problems(instructions: list[Instruction], *,
                           is_write: Callable[[Ia64.Operation], bool] = Ia64.is_serialization_write,
                           is_dependent: Callable[[Ia64.Operation, Ia64.Operation], bool] | None = None,
                           ) -> list[str]:
    """P-SERIALIZE: each serialization-requiring write is followed by a srlz
    in a later instruction group, and no dependent instruction runs before
    that srlz has completed: none between the write and the srlz, and none in
    the srlz's own instruction group.

    By default an instruction depends on a write when it reads or writes the
    written resource, accesses memory, branches, or is itself a
    serialization-requiring write; a branch in the write's own instruction
    group is exempt. A caller's `is_dependent` has no exemption.
    """
    groups = group_indices(instructions)
    operations = [Ia64.parse(instruction) for instruction in instructions]

    def default_dependent(write: Ia64.Operation, later: Ia64.Operation) -> bool:
        resources = write.writes - {'tlb'}
        return (bool(resources & (later.reads | later.writes)) or later.accesses_memory
                or later.is_branch or is_write(later) or 'tlb' in write.writes and later.memory is not None)

    dependent = is_dependent or default_dependent
    problems = []
    for index, operation in enumerate(operations):
        if not is_write(operation) or operation.is_nop:
            continue
        problem, serializer = None, None
        for later in range(index + 1, len(operations)):
            candidate = operations[later]
            if candidate.is_nop:
                continue
            if serializer is not None and groups[later] > groups[serializer]:
                break
            if Ia64.is_serialize(candidate):
                if serializer is None and groups[later] > groups[index]:
                    serializer = later
                continue
            if not dependent(operation, candidate):
                continue
            if serializer is not None:
                problem = 'is used by %s (#%d) in the group of its srlz (#%d)' % (candidate.text, later, serializer)
            elif groups[later] > groups[index]:
                problem = 'is used by %s (#%d) before a srlz in a later group' % (candidate.text, later)
            elif not candidate.is_branch or is_dependent is not None:
                problem = 'and dependent %s (#%d) share an instruction group' % (candidate.text, later)
            else:
                continue
            break
        if problem is None and serializer is None:
            problem = 'is never serialized'
        if problem is not None:
            problems.append('%s (#%d) %s' % (operation.text, index, problem))
    return problems


def leaf_problems(instructions: list[Instruction], *, allow: Callable[[Ia64.Operation], bool] = lambda op: False,
                  relocations: Iterable[RelocationFact] = ()) -> list[str]:
    """P-LEAF: no alloc, call, gp/sp use, RSE control, memory access or
    relocation, except operations `allow` accepts."""
    problems = []
    for index, instruction in enumerate(instructions):
        operation = Ia64.parse(instruction)
        if operation.is_nop or allow(operation):
            continue
        reasons = []
        if operation.base == 'alloc':
            reasons.append('allocates a register frame')
        if operation.is_call:
            reasons.append('calls')
        if {Ia64.GLOBAL_POINTER, Ia64.STACK_POINTER} & (operation.reads | operation.writes):
            reasons.append('uses gp or sp')
        if Ia64.touches_rse(operation):
            reasons.append('controls the register stack')
        if operation.memory is not None:
            reasons.append('accesses memory')
        if reasons:
            problems.append('%s (#%d) %s' % (operation.text, index, ' and '.join(reasons)))
    problems.extend('relocation %s' % (fact,) for fact in relocations)
    return problems


def register_domain_problems(instructions: list[Instruction], allowed: frozenset[str],
                             *, allow_other_classes: bool = False) -> list[str]:
    """Every general register operand is in `allowed`; unless
    `allow_other_classes`, no branch, floating-point or application-stacked
    registers appear either."""
    problems = []
    for index, instruction in enumerate(instructions):
        operation = Ia64.parse(instruction)
        if operation.is_nop:
            continue
        outside = sorted(operation.general_registers - allowed)
        if outside:
            problems.append('%s (#%d) uses %s' % (operation.text, index, ', '.join(outside)))
        if not allow_other_classes:
            others = sorted(r for r in operation.reads | operation.writes if r[0] in 'bf' and r[1:].isdigit())
            if others:
                problems.append('%s (#%d) uses %s' % (operation.text, index, ', '.join(others)))
    return problems


# Code-level judgments ----------------------------------------------------------

FIRST_STACKED_REGISTER = 32
# The physical stacked-register count the PAL capture admits on every CPU.
PHYSICAL_STACKED_HEADER = 'Source/Platform/Ia64/Firmware/PalBootstrapCapabilityCapture.hpp'
PHYSICAL_STACKED_CONSTANT = 'Ia64PhysicalStackedRegisterCount'


def physical_stacked_register_count(root) -> int | None:
    """Ia64PhysicalStackedRegisterCount as the capture header states it."""
    return Layout.constants(Layout.read('%s/%s' % (root, PHYSICAL_STACKED_HEADER))).get(PHYSICAL_STACKED_CONSTANT)
# Instructions that may not run inside a stacked-register scrub: control
# transfers and every operation that moves the register frame or the RSE.
SCRUB_FORBIDDEN_BASES = frozenset({'br', 'brl', 'chk', 'cover', 'loadrs', 'flushrs', 'bsw', 'rfi', 'alloc', 'epc',
                                   'clrrrb'})


def _stacked(registers: Iterable[str]) -> list[str]:
    return sorted((register for register in registers
                   if register[1:].isdigit() and int(register[1:]) >= FIRST_STACKED_REGISTER),
                  key=lambda register: int(register[1:]))


def stacked_register_scrub(code: Code, after, before, count: int) -> tuple[list[str], tuple | None]:
    """The scrub of the physical stacked-register file that a PL3 entry runs
    between the steps `after` and `before`, and its problems. The scrub is
    exactly one unpredicated `alloc R=ar.pfs,count,count,0` (a frame of
    `count` locals), then exactly one unpredicated `mov rK=r0` for each of
    r32..r(32+count-1) and no other write of a stacked register, then one
    unpredicated `alloc R=ar.pfs,0,0,0` that drops the frame. No control
    transfer, frame, bank or RSE operation runs inside it, and nothing after
    the drop writes a stacked register before `before`. With `count` the
    physical stacked-register count, every physical stacked register then
    holds zero with a clear NaT bit. Returns (problems, (grow, drop))."""
    window = code.window(after=after, before=before)
    grows = [step for step in window if step.operation.base == 'alloc'
             and Ia64.alloc_frame(step.operation) == (count, count, 0)]
    if len(grows) != 1:
        return ['%d scrub frames (alloc R=ar.pfs,%d,%d,0) between %s and %s, expected one'
                % (len(grows), count, count, after.describe(), before.describe())], None
    grow = grows[0]
    drop = next((step for step in code.window(after=grow, before=before) if step.operation.base == 'alloc'), None)
    if drop is None or Ia64.alloc_frame(drop.operation) != (0, 0, 0):
        return ['the scrub frame at %s is not dropped by alloc R=ar.pfs,0,0,0 before %s'
                % (grow.describe(), before.describe())], None
    problems = ['%s is predicated' % step.describe() for step in (grow, drop) if step.instruction.predicate is not None]
    cleared = Counter()
    for step in code.window(after=grow, before=drop):
        operation = step.operation
        if operation.base in SCRUB_FORBIDDEN_BASES or operation.is_branch:
            problems.append('%s runs inside the stacked-register scrub' % step.describe())
        stacked = _stacked(operation.general_writes)
        if not stacked:
            continue
        if len(stacked) == 1 and step.text == 'mov %s=r0' % stacked[0] and step.instruction.predicate is None:
            cleared[stacked[0]] += 1
        else:
            problems.append('%s writes %s other than by one unpredicated mov from r0'
                            % (step.describe(), ', '.join(stacked)))
    problems += multiset_problems(cleared, ['r%d' % number for number in range(
        FIRST_STACKED_REGISTER, FIRST_STACKED_REGISTER + count)], 'stacked-register clear')
    problems += ['%s writes the stacked register(s) %s after the scrub' % (step.describe(), ', '.join(stacked))
                 for step in code.window(after=drop, before=before)
                 for stacked in [_stacked(step.operation.general_writes)] if stacked]
    return problems, (grow, drop)


def stacked_register_scrub_mutants(code: Code, grow, drop, count: int):
    """The defects stacked_register_scrub exists for, as (description, code)."""
    clears = code.all(lambda step: bool(_stacked(step.operation.general_writes)), after=grow, before=drop)
    last = clears[-1]
    yield 'one register left uncleared', code.nop(clears[len(clears) // 2])
    yield 'a register cleared from r1', code.replace(last, text=last.text.replace('=r0', '=r1'))
    yield 'a predicated clear', code.replace(clears[0], predicate='p06')
    yield 'a frame one register short', code.replace(grow, text=grow.text.replace(
        ',%d,%d,0' % (count, count), ',%d,%d,0' % (count - 1, count - 1))).nop(last)
    yield 'the scrub frame kept', code.nop(drop)
    yield 'a stacked write after the scrub', code.insert(drop, 'mov r32=r8', after=True)
    yield 'a call inside the scrub', code.insert(clears[0], 'br.call.sptk.many b0=b6')
    yield 'no scrub', code.nop(grow)

def resolved_values(elf: ElfFile, section_index: int, instructions: Iterable[Instruction]) -> tuple[int | None, ...]:
    """Per instruction: the `movl` immediate or direct call target encoded in
    the artifact's bytes (None for other instructions), where the rendered
    text does not carry the resolved value (linked images, and the
    --emit-relocs firmware image whose relocations Disassembly folds in)."""
    section = elf.sections[section_index]
    base = 0 if elf.type == ET_REL else section.address
    values = []
    for instruction in instructions:
        operation, bundle = Ia64.parse(instruction), instruction.address & ~0xF
        if operation.base == 'movl':
            values.append(Ia64.movl_immediate(elf.read(section, bundle - base, 16)))
        elif operation.is_call and operation.target is not None and operation.base == 'br':
            values.append(Ia64.branch_target(elf.read(section, bundle - base, 16), instruction.slot, bundle))
        else:
            values.append(None)
    return tuple(values)


def fail_stop(code: Code, at) -> bool:
    """`hint @pause` followed by an unconditional branch back to it."""
    from AsteriaVerify.Ia64StopViews import terminal
    step, following = code.step(at), code.next(at)
    return (step is not None and step.operation.base == 'hint' and following is not None
            and following.operation.is_branch and not following.operation.is_predicated
            and not following.operation.is_call and (code.target(following) is step
                or terminal(following)))


def psr_i_window_problems(code: Code, *, strict_exit: bool, window_allows: Callable[[Ia64.Operation], bool],
                          back_edges: int, disables: int | None = None) -> list[str]:
    """P-PSR-I-WINDOW: one serialized window of enabled external interrupts.

    - exactly one `ssm 0x4000`; `disables` `rsm 0x4000` (at least one); no
      other ssm/rsm/sum/rum and no `mov psr.l=`;
    - every psr.i write is followed by `srlz.d`, alone in the next
      instruction group;
    - between the ssm and the first following rsm (the window) only
      instructions `window_allows` accepts; every branch in the window goes
      to the rsm, except `back_edges` unpredicated branches back to the first
      instruction after the ssm's srlz.d; no branch from outside enters the
      window;
    - on every control-flow path, a return (or leaving the extent) never
      happens with interrupts enabled by the ssm; with `strict_exit`, every
      return happens after an rsm, whatever the entry state was.
    """
    problems, enables, masks = [], [], []
    for step in code.steps:
        operation = step.operation
        if operation.base in ('ssm', 'rsm') and operation.immediates == (Ia64.PSR_I,):
            (enables if operation.base == 'ssm' else masks).append(step)
        elif operation.base in ('ssm', 'rsm', 'sum', 'rum') or {'psr.l', 'psr.um'} & operation.writes:
            problems.append('%s (#%d) writes PSR bits other than a psr.i window' % (operation.text, step.position))
    if len(enables) != 1:
        problems.append('%d `ssm 0x4000`, expected exactly one' % len(enables))
    if not masks or disables is not None and len(masks) != disables:
        problems.append('%d `rsm 0x4000`, expected %s' % (len(masks), disables if disables is not None
                                                          else 'at least one'))
    for step in enables + masks:
        serialize = code.next(step)
        if serialize is None or serialize.mnemonic != 'srlz.d':
            problems.append('%s (#%d) is not followed by srlz.d' % (step.operation.text, step.position))
        elif serialize.group <= step.group or not code.ends_group(serialize):
            problems.append('%s (#%d) is not followed by srlz.d alone in the next instruction group'
                            % (step.operation.text, step.position))
    if len(enables) == 1:
        enable = enables[0]
        closing = [step for step in masks if step.position > enable.position]
        if not closing:
            problems.append('the psr.i window is never closed by a later rsm')
        else:
            window = code.window(after=enable, before=closing[0])
            serialize = code.next(enable)
            head = code.next(serialize) if serialize is not None else None
            loops = 0
            for step in window:
                if not window_allows(step.operation):
                    problems.append('%s (#%d) runs with interrupts enabled' % (step.operation.text, step.position))
                if step.operation.is_branch:
                    target = code.target(step)
                    if target is closing[0]:
                        continue
                    if target is not None and target is head and target.position <= step.position \
                            and not step.operation.is_predicated:
                        loops += 1
                        continue
                    problems.append('%s (#%d) leaves the psr.i window other than through its rsm'
                                    % (step.operation.text, step.position))
            if loops != back_edges:
                problems.append('%d loop branches inside the psr.i window, expected %d' % (loops, back_edges))
            inside = {step.position for step in window}
            problems += ['%s (#%d) enters the psr.i window without its ssm' % (step.operation.text, step.position)
                         for step in code.steps if step.position not in inside and step.operation.is_branch
                         and (target := code.target(step)) is not None and target.position in inside]
    return problems + _psr_i_path_problems(code, {step.position for step in enables},
                                           {step.position for step in masks}, strict_exit)


def _psr_i_path_problems(code: Code, enables: set[int], masks: set[int], strict_exit: bool) -> list[str]:
    if not code.steps:
        return ['empty extent']
    problems: dict[str, None] = {}
    stack, seen = [(code.steps[0].position, 'entry')], set()
    while stack:
        position, state = stack.pop()
        state = 'enabled' if position in enables else 'masked' if position in masks else state
        if (position, state) in seen:
            continue
        seen.add((position, state))
        stopped, successors = fail_stop(code, position), code.graph.successors[position]
        if stopped or MARKERS & set(successors):
            where = '%s (#%d)' % (code.operations[position].text, position)
            if state == 'enabled':
                problems['%s is reached with interrupts enabled' % where] = None
            elif strict_exit and state != 'masked' and not stopped:
                problems['%s is reached without re-masking interrupts' % where] = None
        if not stopped:
            stack.extend((successor, state) for successor in successors if successor not in MARKERS)
    return list(problems)
