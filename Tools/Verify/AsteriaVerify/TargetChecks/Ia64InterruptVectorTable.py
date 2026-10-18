# SPDX-License-Identifier: Apache-2.0
"""IA-64 interrupt vector tables: the runtime IVT, its refill handlers and
direct trampolines, the IVT install leaves, and the IVT transport of the
linked kernel images.

`AsteriaIa64RuntimeInterruptVectorTableImage` is the 32 KiB section
`.text.ia64_runtime_ivt` of Source/Platform/Ia64/Memory/
Ia64RuntimeInterruptVectorTable.S that becomes cr.iva after the final SAL
handoff. Every bundle of it is the fail-stop MIB self-loop except

- the alternate instruction-TLB (+0x0c00) and data-TLB (+0x1000) refill
  handlers, two expansions of one GAS macro assembled in `.auto` mode; the
  data handler alone ends its ledger miss path, for a PL3 access that is not
  an RSE reference, in one MIB bundle that passes the selector 4 in r16 to
  the user-fault entry, and
- nine one-bundle trampolines that branch directly to the native entries:
  break (+0x2c00) to the system-call entry, external interrupt (+0x3000) to
  the external-interrupt dispatcher, the four heavyweight page-fault
  vectors (+0x5000..+0x5300), which pass the selector 0..3 in r16 to the
  page-fault entry, and the General Exception (+0x5400, domain creation ABI), Floating-point Fault (+0x5c00) and Floating-point Trap (+0x5d00)
  vectors, which pass the selector 6 to the user exception
  entry and leave their offset >> 8 in ar.k4.

Goldens under Tests/Golden/Ia64/Memory pin the two refill handler ranges and
the IVT install leaves exactly. The properties here state the rules that
must survive a golden update, and check the linked images
(`NucleusCore.elf` O0/O2 and the firmware `AsteriaBoot.elf`), which the
goldens do not cover.
"""
from __future__ import annotations

import collections
import dataclasses
import re

from AsteriaVerify import Ia64StopViews, Ia64, Mutate, Properties, Subjects
from AsteriaVerify.Checks import Property, register
from AsteriaVerify.ControlFlow import Code, writes_between
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.Elf import (
    ET_REL, SHF_ALLOC, SHF_EXECINSTR, SHF_WRITE, SHN_ABS, SHN_UNDEF, SHT_NOBITS, SHT_PROGBITS, STB_GLOBAL, STB_LOCAL,
    STT_FUNC, STT_OBJECT, STV_DEFAULT, STV_HIDDEN, Section, Symbol, load)
from AsteriaVerify.Properties import RelocationFact

SECTION = '.text.ia64_runtime_ivt'
TABLE = 'AsteriaIa64RuntimeInterruptVectorTableImage'
TABLE_SIZE = 0x8000                 # Cr.iva ignores its low 15 bits
BUNDLE = 16
VECTOR_SLOT = 0x400
# `{.mib; nop.m 0; nop.i 0; br.few. }`: a position-independent self-loop.
FAIL_STOP_BUNDLE = bytes.fromhex('11000000010000000002000000000040')
# A PCREL21B branch reaches +-16 MiB: imm21 counts bundles.
PCREL21B_RANGE = (-0x1000000, 0xfffff0)

# Marker symbols at architected offsets from the table start (Itanium SDM
# Vol. 2, 5.7 "IVA-based interruption vectors").
FIXED_MARKERS = {
    'AsteriaIa64RuntimeAlternateInstructionTlbStart': 0x0c00,
    'AsteriaIa64RuntimeAlternateDataTlbStart': 0x1000,
    'AsteriaIa64RuntimeAlternateDataTlbSlotEnd': 0x1400,
    'AsteriaIa64RuntimeBreakStart': 0x2c00,
    'AsteriaIa64RuntimeBreakEnd': 0x2c10,
    'AsteriaIa64RuntimeBreakSlotEnd': 0x3000,
    'AsteriaIa64RuntimeExternalInterruptStart': 0x3000,
    'AsteriaIa64RuntimeExternalInterruptEnd': 0x3010,
    'AsteriaIa64RuntimeExternalInterruptSlotEnd': 0x3400,
    'AsteriaIa64RuntimePageNotPresentStart': 0x5000,
    'AsteriaIa64RuntimePageNotPresentTrampolineEnd': 0x5010,
    'AsteriaIa64RuntimePageNotPresentEnd': 0x5100,
    'AsteriaIa64RuntimeKeyPermissionStart': 0x5100,
    'AsteriaIa64RuntimeKeyPermissionTrampolineEnd': 0x5110,
    'AsteriaIa64RuntimeKeyPermissionEnd': 0x5200,
    'AsteriaIa64RuntimeInstructionAccessRightsStart': 0x5200,
    'AsteriaIa64RuntimeInstructionAccessRightsTrampolineEnd': 0x5210,
    'AsteriaIa64RuntimeInstructionAccessRightsEnd': 0x5300,
    'AsteriaIa64RuntimeDataAccessRightsStart': 0x5300,
    'AsteriaIa64RuntimeDataAccessRightsTrampolineEnd': 0x5310,
    'AsteriaIa64RuntimeDataAccessRightsEnd': 0x5400,
    'AsteriaIa64RuntimeGeneralExceptionStart': 0x5400,
    'AsteriaIa64RuntimeGeneralExceptionTrampolineEnd': 0x5410,
    'AsteriaIa64RuntimeGeneralExceptionEnd': 0x5500,
    'AsteriaIa64RuntimeFloatingPointFaultStart': 0x5c00,
    'AsteriaIa64RuntimeFloatingPointTrapStart': 0x5d00,
    'AsteriaIa64RuntimeFloatingPointTrapEnd': 0x5e00,
    'AsteriaIa64RuntimeInterruptVectorTableImageEnd': 0x8000,
}


@dataclasses.dataclass(frozen=True)
class RefillVector:
    label: str
    kind: str           # The itc/srlz completer: `i` or `d`
    start: str
    end: str
    offset: int


REFILL_VECTORS = (
    RefillVector('alternate instruction TLB', 'i', 'AsteriaIa64RuntimeAlternateInstructionTlbStart',
                 'AsteriaIa64RuntimeAlternateInstructionTlbEnd', 0x0c00),
    RefillVector('alternate data TLB', 'd', 'AsteriaIa64RuntimeAlternateDataTlbStart',
                 'AsteriaIa64RuntimeAlternateDataTlbEnd', 0x1000),
)


@dataclasses.dataclass(frozen=True)
class Trampoline:
    name: str
    offset: int
    selector: int | None    # The immutable r16 selector, None for `nop.i`
    entry: str
    vector: int | None = None   # The vector offset >> 8 it leaves in ar.k4, None for `nop.m`


# The native PL3 fault and trap slots that branch to the user exception entry
# with their offset in ar.k4 (IA-64 fault delivery; the alternate TLB slots escape from
# their refill handlers and the break slot through AsteriaIa64UserBreakEntry).
NATIVE_USER_VECTORS = (
    ('instruction TLB', 0x0400), ('data TLB', 0x0800), ('instruction key miss', 0x1800),
    ('data key miss', 0x1c00), ('dirty bit', 0x2000), ('instruction access bit', 0x2400),
    ('data access bit', 0x2800), ('page not present', 0x5000), ('key permission', 0x5100),
    ('instruction access rights', 0x5200), ('data access rights', 0x5300), ('general exception', 0x5400),
    ('disabled FP register', 0x5500), ('NaT consumption', 0x5600), ('speculation', 0x5700),
    ('debug', 0x5900), ('unaligned reference', 0x5a00), ('unsupported data reference', 0x5b00),
    ('floating-point fault', 0x5c00), ('floating-point trap', 0x5d00), ('lower privilege transfer', 0x5e00),
    ('taken branch', 0x5f00), ('single step', 0x6000))

TRAMPOLINES = (
    Trampoline('break', 0x2c00, None, 'AsteriaIa64UserBreakEntry'),
    Trampoline('external interrupt', 0x3000, None, 'AsteriaIa64ExternalInterruptDispatchEntry'),
    *(Trampoline(name, offset, 6, 'AsteriaIa64UserExceptionEntry', offset >> 8)
      for name, offset in NATIVE_USER_VECTORS),
)
STOP_VECTOR_OFFSETS = (0x0000, 0x1400, *range(0x3400, 0x5000, 0x400), 0x5800, *range(0x6100, 0x8000, 0x100))
STOP_TABLE_RELOCATIONS = tuple(RelocationFact(offset + 2, 'PCREL21B', 'AsteriaIa64StopFromIvt', 0) for offset in STOP_VECTOR_OFFSETS)
EXPECTED_TABLE_RELOCATIONS = tuple(sorted(RelocationFact(t.offset + 2, 'PCREL21B', t.entry, 0) for t in TRAMPOLINES))
# The refill handlers' ledger-miss escapes: the data handler's to the user
# fault entry (selector 4), the instruction handler's to the user exception
# entry (selector 6). Each bundle lies wherever the reviewed handler places it
# inside its slot; the relocation check accepts exactly one in each, in
# bundle slot 2.
USER_FAULT_ENTRY = 'AsteriaIa64UserFaultEntry'
ESCAPE_MNEMONICS = ('br.many', 'br.sptk.many')


def is_escape(step) -> bool:
    """A direct branch, by relocation or symbol, to the user-fault entry."""
    target = step.operation.target
    return (step.operation.is_branch and not step.operation.is_call and target is not None
            and target.kind != 'internal' and target.symbol in (USER_FAULT_ENTRY, 'AsteriaIa64UserExceptionEntry') and target.offset == 0)

BANK_ZERO_REGISTERS = Ia64.BANKED_GENERAL | {'r0'}
_GENERAL_REGISTER = re.compile(r'(?<![\w.])r\d+\b')
_BRANCH_REGISTER = re.compile(r'b[0-7]$')
_FLOAT_REGISTER = re.compile(r'f\d+$')


class _IvtProperty(Property):
    isas = ('Ia64',)


class _ArtifactProperty(_IvtProperty):
    """Runs on the object that defines the table and on the linked images."""

    def subjects(self, context):
        return Subjects.artifacts(context, TABLE, images=True)

    def describe(self, subject):
        return subject.label


def unique_definition(elf, name: str) -> Symbol | None:
    definitions = elf.definitions(name)
    return definitions[0] if len(definitions) == 1 else None


def _table_symbol(elf) -> Symbol:
    table = unique_definition(elf, TABLE)
    if table is None:
        raise ValueError('%s does not define %s exactly once' % (elf.path, TABLE))
    return table


def shape(instruction: Instruction) -> str:
    """Instruction text with every general register renamed to `r`."""
    return _GENERAL_REGISTER.sub('r', instruction.text)


def _control_writes(operation: Ia64.Operation) -> set[str]:
    return {r for r in operation.writes if r.startswith('cr.')}


def control_register_serialization_problems(instructions) -> list[str]:
    """Every write of a control register (`mov cr.X=`) is followed by a
    `srlz.i`/`srlz.d` in a later instruction group, and every later
    instruction that reads or writes cr.X, branches or accesses memory lies
    in an instruction group after that srlz (P-SERIALIZE, including its rule
    that a dependent read in the srlz's own group is rejected)."""
    return Properties.serialization_problems(
        list(instructions), is_write=lambda operation: bool(_control_writes(operation)),
        is_dependent=lambda write, later: bool(_control_writes(write) & (later.reads | later.writes))
        or later.is_branch or later.accesses_memory)


# Section contract -------------------------------------------------------------

@register
class Ia64RuntimeIvtSection(_IvtProperty):
    """The object defining the runtime IVT has exactly one
    `.text.ia64_runtime_ivt` section: PROGBITS, flags exactly ALLOC|EXECINSTR
    (never writable), 0x8000 bytes, aligned to 32 KiB, because cr.iva
    ignores the low 15 bits of the table address and the table must not be
    mutable."""

    def extract(self, context, variant):
        return tuple(section for section in load(context.defining_object(variant, TABLE)).sections
                     if section.name == SECTION)

    def verify(self, facts):
        if len(facts) != 1:
            return ['%d sections named %s, expected exactly one' % (len(facts), SECTION)]
        section = facts[0]
        problems = []
        if section.type != SHT_PROGBITS:
            problems.append('%s has type %d, expected PROGBITS' % (SECTION, section.type))
        if section.flags != SHF_ALLOC | SHF_EXECINSTR:
            problems.append('%s has flags %#x, expected exactly ALLOC|EXECINSTR' % (SECTION, section.flags))
        if section.size != TABLE_SIZE:
            problems.append('%s is %#x bytes, expected %#x' % (SECTION, section.size, TABLE_SIZE))
        if section.alignment != TABLE_SIZE:
            problems.append('%s is aligned to %d, expected %d' % (SECTION, section.alignment, TABLE_SIZE))
        return problems

    def mutants(self, facts):
        section = facts[0]
        yield 'NOBITS section', (dataclasses.replace(section, type=SHT_NOBITS),)
        yield 'writable section', (dataclasses.replace(section, flags=section.flags | SHF_WRITE),)
        yield '16-byte alignment', (dataclasses.replace(section, alignment=16),)
        yield 'short section', (dataclasses.replace(section, size=TABLE_SIZE - BUNDLE),)
        yield 'duplicate section', (section, dataclasses.replace(section, index=section.index + 1))
        yield 'missing section', ()


# Symbols and vector geometry ------------------------------------------

@dataclasses.dataclass(frozen=True)
class TableSymbols:
    section: Section | None
    definitions: tuple[tuple[str, tuple[Symbol,...]],...]

    def lookup(self, name: str) -> tuple[Symbol,...]:
        return dict(self.definitions).get(name, ())

    def one(self, name: str) -> Symbol | None:
        found = self.lookup(name)
        return found[0] if len(found) == 1 else None

    def edited(self, name: str, entries: tuple[Symbol,...]) -> 'TableSymbols':
        return dataclasses.replace(self, definitions=tuple(
            (other, entries if other == name else found) for other, found in self.definitions))

    def moved(self, name: str, delta: int) -> 'TableSymbols':
        symbol = self.lookup(name)[0]
        return self.edited(name, (dataclasses.replace(symbol, value=symbol.value + delta),))


class _TableProperty(_IvtProperty):
    NAMES = (TABLE,) + tuple(FIXED_MARKERS) + tuple(vector.end for vector in REFILL_VECTORS)

    def extract(self, context, variant):
        elf = load(context.defining_object(variant, TABLE))
        sections = [section for section in elf.sections if section.name == SECTION]
        return TableSymbols(sections[0] if len(sections) == 1 else None,
                            tuple((name, elf.definitions(name)) for name in self.NAMES))


@register
class Ia64RuntimeIvtSymbols(_TableProperty):
    """The table symbol and every vector marker are defined exactly
    once, in the IVT section, with DEFAULT visibility; the table is a GLOBAL
    OBJECT of 0x8000 bytes that starts the section. The installers and the
    final-image checks locate the table and the handler ranges through these
    symbols."""

    def verify(self, facts):
        if facts.section is None:
            return ['the object has no single %s section' % SECTION]
        problems = []
        for name, found in facts.definitions:
            if len(found) != 1:
                problems.append('%s has %d definitions, expected exactly one' % (name, len(found)))
                continue
            if found[0].section_index != facts.section.index:
                problems.append('%s lies in section %d, not in %s' % (name, found[0].section_index, SECTION))
            if found[0].visibility != STV_DEFAULT:
                problems.append('%s visibility is %d, expected DEFAULT' % (name, found[0].visibility))
        table = facts.one(TABLE)
        if table is not None:
            if table.type != STT_OBJECT or table.binding != STB_GLOBAL:
                problems.append('%s is type %d binding %d, expected a GLOBAL OBJECT'
                                % (TABLE, table.type, table.binding))
            if table.size != TABLE_SIZE:
                problems.append('%s has size %#x, expected %#x' % (TABLE, table.size, TABLE_SIZE))
            if table.value != facts.section.address:
                problems.append('%s at %#x does not start %s' % (TABLE, table.value, SECTION))
        return problems

    def mutants(self, facts):
        table = facts.lookup(TABLE)[0]
        marker = 'AsteriaIa64RuntimeBreakStart'
        found = facts.lookup(marker)[0]
        yield 'short table', facts.edited(TABLE, (dataclasses.replace(table, size=TABLE_SIZE - BUNDLE),))
        yield 'table is a function', facts.edited(TABLE, (dataclasses.replace(table, type=STT_FUNC),))
        yield 'table is local', facts.edited(TABLE, (dataclasses.replace(table, binding=STB_LOCAL),))
        yield 'table does not start the section', facts.moved(TABLE, BUNDLE)
        yield 'missing break marker', facts.edited(marker, ())
        yield 'duplicate break marker', facts.edited(marker, (found, found))
        yield 'break marker in another section', facts.edited(
            marker, (dataclasses.replace(found, section_index=found.section_index + 1),))
        yield 'hidden break marker', facts.edited(marker, (dataclasses.replace(found, visibility=STV_HIDDEN),))


@register
class Ia64RuntimeIvtGeometry(_TableProperty):
    """The vector markers sit at their architected offsets from the
    table: alternate ITLB +0x0c00, alternate DTLB +0x1000 (slot end
    +0x1400), break +0x2c00..+0x2c10 (slot end +0x3000), external interrupt
    +0x3000..+0x3010 (slot end +0x3400), the four page-fault vectors
    +0x5000/+0x5100/+0x5200/+0x5300 (one trampoline bundle each, 0x100-byte
    slots) and the table end +0x8000; each refill handler ends inside its
    0x400-byte slot after at least one bundle, on a bundle boundary. A
    drifted marker would dispatch a miss, break or interrupt into the wrong
    code."""

    def verify(self, facts):
        table = facts.one(TABLE)
        if table is None:
            return ['%s is not defined exactly once' % TABLE]
        problems = []
        for name, expected in FIXED_MARKERS.items():
            marker = facts.one(name)
            if marker is None:
                problems.append('%s is not defined exactly once' % name)
            elif marker.value - table.value != expected:
                problems.append('%s is at +%#x, expected +%#x' % (name, marker.value - table.value, expected))
        for vector in REFILL_VECTORS:
            end = facts.one(vector.end)
            if end is None:
                problems.append('%s is not defined exactly once' % vector.end)
                continue
            length = end.value - table.value - vector.offset
            if not 0 < length <= VECTOR_SLOT or length % BUNDLE:
                problems.append('the %s handler is %#x bytes; expected a whole number of bundles in (0, %#x]'
                                % (vector.label, length, VECTOR_SLOT))
        return problems

    def mutants(self, facts):
        table = facts.one(TABLE)
        instruction_end, data_end = (facts.one(vector.end) for vector in REFILL_VECTORS)
        yield 'alternate ITLB vector drifted', facts.moved('AsteriaIa64RuntimeAlternateInstructionTlbStart', BUNDLE)
        yield 'alternate DTLB vector drifted', facts.moved('AsteriaIa64RuntimeAlternateDataTlbStart', -BUNDLE)
        yield 'two-bundle break trampoline', facts.moved('AsteriaIa64RuntimeBreakEnd', BUNDLE)
        yield 'external interrupt slot end drifted', facts.moved('AsteriaIa64RuntimeExternalInterruptSlotEnd', BUNDLE)
        yield 'page-fault vector drifted', facts.moved('AsteriaIa64RuntimeKeyPermissionStart', 0x100)
        yield 'short table end', facts.moved('AsteriaIa64RuntimeInterruptVectorTableImageEnd', -BUNDLE)
        yield 'ITLB handler exceeds its slot', facts.moved(
            REFILL_VECTORS[0].end, table.value + 0x0c00 + VECTOR_SLOT + BUNDLE - instruction_end.value)
        yield 'empty DTLB handler', facts.moved(REFILL_VECTORS[1].end, table.value + 0x1000 - data_end.value)
        yield 'ITLB handler end off a bundle boundary', facts.moved(REFILL_VECTORS[0].end, 8)


# Relocations -------------------------------------------------------------------------

@register
class Ia64RuntimeIvtRelocations(_IvtProperty):
    """The only relocations of the IVT section are the six PCREL21B
    branches in slot 2 of the trampoline bundles, to the system-call entry
    (+0x2c02), the external-interrupt dispatcher (+0x3002) and the page-fault
    entry (+0x5002/+0x5102/+0x5202/+0x5302), and the one PCREL21B of the
    data handler's escape to the user-fault entry, in slot 2 of a bundle
    inside the data slot [+0x1000, +0x1400), each with addend 0. Any other
    relocation (GP-relative, absolute, LTOFF) would need data or GP access
    while an interruption runs with PSR.ic=0."""

    def extract(self, context, variant):
        elf = load(context.defining_object(variant, TABLE))
        return tuple(sorted(fact for section in elf.sections if section.name == SECTION
                            for fact in Properties.relocations_in_extent(elf, section.index, 0, section.size)))

    def verify(self, facts):
        stops = [fact for fact in facts if fact.target == 'AsteriaIa64StopEntry']
        facts = tuple(fact for fact in facts if fact.target != 'AsteriaIa64StopEntry')
        escapes = [fact for fact in facts if 0xc00 <= fact.offset < 0x1400]
        problems = Properties.multiset_problems([fact for fact in facts if fact not in escapes],
                                                tuple(sorted(EXPECTED_TABLE_RELOCATIONS + STOP_TABLE_RELOCATIONS)), 'relocation')
        for start, end, target in ((0xc00, 0x1000, 'AsteriaIa64UserExceptionEntry'),
                                   (0x1000, 0x1400, USER_FAULT_ENTRY)):
            found = [fact for fact in escapes if start <= fact.offset < end]
            if len(found) != 1 or any(fact.type != 'PCREL21B' or fact.addend
                or fact.offset & 15 != 2 or fact.target != target for fact in found):
                problems.append('expected one direct %s escape in refill slot %#x' % (target, start))
        if len(stops) != 2 or any(fact.type != 'PCREL21B' or fact.addend or fact.offset & 15 != 2
            or not 0xc00 <= fact.offset < 0x1400 for fact in stops) or len({fact.offset // 0x400 for fact in stops}) != 2:
            problems.append('expected exactly one terminal PCREL21B in each refill slot')
        return problems

    def mutants(self, facts):
        stops = tuple(fact for fact in facts if fact.target == 'AsteriaIa64StopEntry')
        for label, mutant in self.core_mutants(tuple(fact for fact in facts if fact.target != 'AsteriaIa64StopEntry')):
            yield label, tuple(sorted(mutant + stops))
        yield 'missing terminal refill edge', tuple(fact for fact in facts if fact != stops[0])

    def core_mutants(self, facts):
        fixed = tuple(fact for fact in facts if not 0xc00 <= fact.offset < 0x1400)
        escapes = tuple(fact for fact in facts if 0xc00 <= fact.offset < 0x1400)
        for description, mutant in Mutate.relocation_mutants(fixed, sorted(
                {trampoline.entry for trampoline in TRAMPOLINES})):
            yield description, tuple(sorted(mutant + escapes))
        escape = escapes[-1]
        other_escapes = tuple(fact for fact in escapes if fact != escape)
        for description, changes in (('in the instruction slot', dict(offset=escape.offset - 0x400)),
                                     ('in bundle slot 1', dict(offset=escape.offset - 1)),
                                     ('with an addend', dict(addend=8)), ('as LTOFF22X', dict(type='LTOFF22X'))):
            yield 'escape %s' % description, tuple(sorted(fixed + other_escapes + (dataclasses.replace(escape, **changes),)))
        yield 'second escape', tuple(sorted(facts + (dataclasses.replace(escape, offset=escape.offset + 0x10),)))
        yield 'no escape', fixed


# Fail-stop image ---------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class FailStopFacts:
    bundles: tuple[bytes,...]
    handlers: tuple[tuple[str, int, int],...]      # Label, start and end offset from the table


def fail_stop_problems(facts: FailStopFacts) -> list[str]:
    if len(facts.bundles) != TABLE_SIZE // BUNDLE or any(len(bundle) != BUNDLE for bundle in facts.bundles):
        return ['the table holds %d bundles, expected %d' % (len(facts.bundles), TABLE_SIZE // BUNDLE)]
    exempt: set[int] = set()
    required: dict[int, str] = {}
    for label, start, end in facts.handlers:
        exempt.update(range(start, min(max(end, start), start + VECTOR_SLOT), BUNDLE))
        required[start] = 'the %s refill handler' % label
    for offset in STOP_VECTOR_OFFSETS:
        exempt.add(offset)
        required[offset] = 'the terminal vector'
    for trampoline in TRAMPOLINES:
        exempt.add(trampoline.offset)
        required[trampoline.offset] = 'the %s trampoline' % trampoline.name
    problems, stray = [], []
    for index, bundle in enumerate(facts.bundles):
        offset = index * BUNDLE
        if offset in required and bundle == FAIL_STOP_BUNDLE:
            problems.append('%s at +%#x is the fail-stop bundle' % (required[offset], offset))
        elif offset not in exempt and bundle != FAIL_STOP_BUNDLE:
            stray.append(offset)
    if stray:
        problems.append('%d bundles outside the handler bodies and trampolines are not the fail-stop bundle: %s'
                        % (len(stray), ', '.join('+%#x' % offset for offset in stray[:8])))
    return problems


@register
class Ia64RuntimeIvtFailStopImage(_ArtifactProperty):
    """The table is 2048 bundles, and every bundle outside the
    refill handler bodies [Start, End) and the nine trampoline bundles is the
    fail-stop MIB self-loop `11000000 01000000 00020000 00000040`, so an
    unexpected interruption parks the processor instead of executing stale
    bytes; the first bundle of each handler and every trampoline bundle is
    real code. Each refill-slot tail must fail-stop too. Checked in the
    object and linked images."""

    def extract(self, context, subject):
        elf = load(subject.path)
        table = _table_symbol(elf)
        data = elf.symbol_bytes(table)
        handlers = []
        for vector in REFILL_VECTORS:
            end = unique_definition(elf, vector.end)
            handlers.append((vector.label, vector.offset,
                             end.value - table.value if end is not None else vector.offset))
        return FailStopFacts(tuple(data[offset:offset + BUNDLE] for offset in range(0, len(data), BUNDLE)),
                             tuple(handlers))

    def verify(self, facts):
        return fail_stop_problems(facts)

    def mutants(self, facts):
        def with_bundle(offset, bundle):
            index = offset // BUNDLE
            return dataclasses.replace(facts, bundles=facts.bundles[:index] + (bundle,) + facts.bundles[index + 1:])

        (_, instruction_start, instruction_end), (_, data_start, data_end) = facts.handlers
        code = facts.bundles[instruction_start // BUNDLE]
        yield 'zeroed filler bundle', with_bundle(0x10, bytes(BUNDLE))
        yield 'code in the ITLB slot tail', with_bundle(instruction_end, code)
        yield 'code in the DTLB slot tail', with_bundle(data_end, code)
        yield 'code after the break trampoline', with_bundle(0x2c10, code)
        yield 'code in the last table bundle', with_bundle(TABLE_SIZE - BUNDLE, code)
        yield 'missing ITLB handler', with_bundle(instruction_start, FAIL_STOP_BUNDLE)
        yield 'missing DTLB handler', with_bundle(data_start, FAIL_STOP_BUNDLE)
        for trampoline in TRAMPOLINES:
            yield 'missing %s trampoline' % trampoline.name, with_bundle(trampoline.offset, FAIL_STOP_BUNDLE)
        yield 'truncated table', dataclasses.replace(facts, bundles=facts.bundles[:-1])


# T1: direct IVT trampolines (objects and linked images) ------------------------------------------

@dataclasses.dataclass(frozen=True)
class SlotFacts:
    name: str
    offset: int
    selector: int | None
    entry: str
    bundle_address: int
    vector: int | None
    instructions: tuple[Instruction,...]         # The decoded slot bundle, nops included
    relocations: tuple[RelocationFact,...]       # Offsets from the bundle
    relocatable: bool                             # An object: the target is its relocation
    keeps_relocations: bool                       # A linked image built with --emit-relocs
    resolved_target: int | None                   # Linked image: decoded from the bundle bytes
    entry_address: int | None                     # Linked image: the entry symbol's value


def trampoline_problems(slot: SlotFacts) -> list[str]:
    label = '%s trampoline (+%#x)' % (slot.name, slot.offset)
    instructions = slot.instructions
    if [instruction.slot for instruction in instructions] != [0, 1, 2]:
        return ['%s: the slot does not decode as one bundle of three instructions: %s'
                % (label, '; '.join(instruction.rendered() for instruction in instructions))]
    problems = []
    templates = sorted({instruction.template for instruction in instructions})
    if templates != ['MIB']:
        problems.append('%s: template %s, expected MIB' % (label, '/'.join(map(str, templates))))
    memory, integer, branch = instructions
    # The exception vectors share one entry, which learns from ar.k4 (PL3
    # cannot write it) which vector it serves.
    expected_memory = 'nop.m 0x0' if slot.vector is None else 'mov.m ar.k4=%d' % slot.vector
    if memory.rendered() != expected_memory:
        problems.append('%s: slot 0 is %s, expected %s' % (label, memory.rendered(), expected_memory))
    expected = 'nop.i 0x0' if slot.selector is None else 'mov r16=%d' % slot.selector
    if integer.rendered() != expected:
        problems.append('%s: slot 1 is %s, expected %s' % (label, integer.rendered(), expected))
    operation = Ia64.parse(branch)
    if branch.predicate is not None or operation.is_call or branch.mnemonic not in ('br.many', 'br.sptk.many'):
        problems.append('%s: slot 2 is %s, not an unpredicated br(.sptk).many' % (label, branch.rendered()))
    if not branch.stop_after:
        problems.append('%s: no stop after the branch' % label)
    target = operation.target
    if (target is None or target.kind == 'internal' or target.symbol != slot.entry or target.offset
            or target.kind == 'reloc' and target.relocation != 'PCREL21B'):
        problems.append('%s: the branch does not name %s' % (label, slot.entry))
    expected_relocations = ((RelocationFact(2, 'PCREL21B', slot.entry, 0),)
                            if slot.relocatable or slot.keeps_relocations else ())
    if slot.relocations != expected_relocations:
        problems.append('%s: relocations %s, expected %s' % (label, list(slot.relocations), list(expected_relocations)))
    if not slot.relocatable:
        if slot.entry_address is None:
            problems.append('%s: %s is not defined exactly once in the image' % (label, slot.entry))
        elif slot.resolved_target != slot.entry_address:
            problems.append('%s: the branch reaches %s, but %s is at %#x' % (
                label, 'nothing' if slot.resolved_target is None else '%#x' % slot.resolved_target,
                slot.entry, slot.entry_address))
        elif not PCREL21B_RANGE[0] <= slot.entry_address - slot.bundle_address <= PCREL21B_RANGE[1]:
            problems.append('%s: displacement %#x to %s exceeds the PCREL21B range'
                            % (label, slot.entry_address - slot.bundle_address, slot.entry))
    return problems


@register
class Ia64IvtDirectTrampolines(_ArtifactProperty):
    """T1 (shared direct-IVT-trampoline property). Break (+0x2c00) and
    external interrupt (+0x3000) are `{ nop.m; nop.i; br.many entry;; }`,
    the four page-fault vectors (+0x5000 + 0x100*s) `{ nop.m; mov r16=s;
    br.many AsteriaIa64PageFaultNativeEntry;; }` with s = 0..3, and the data
    handler's only escape, inside the data slot, `{ nop.m; mov r16=4;
    br.many AsteriaIa64UserFaultEntry;; }`, so each is one direct jump (b0
    survives, nothing is called or relaxed into brl) that changes nothing
    but the immutable selector: an unpredicated
    `br.many` or `br.sptk.many` with a stop after it. The target is the
    PCREL21B relocation in slot 2 in the object (kept by an image linked with
    --emit-relocs), and the resolved displacement in the linked
    `NucleusCore.elf` O0/O2 and firmware `AsteriaBoot.elf`, which must reach
    exactly the entry symbol within +-16 MiB."""

    def extract(self, context, subject):
        elf = load(subject.path)
        table = _table_symbol(elf)
        code = Subjects.code(context, subject.path, TABLE)
        relocatable = elf.type == ET_REL
        keeps = any(section.info == table.section_index for section in elf.relocation_sections())
        escape_routes = []
        for start, end, selector, target, vector in ((0xc00, 0x1000, 6, 'AsteriaIa64UserExceptionEntry', 0xc),
                                                    (0x1000, 0x1400, 4, USER_FAULT_ENTRY, None)):
            escapes = [step.bundle for step in code.steps if is_escape(step) and start <= step.offset < end]
            escape_routes.append(Trampoline('user escape %#x' % start, escapes[0] if len(escapes) == 1 else end,
                                             selector, target, vector))
        slots = []
        for trampoline in TRAMPOLINES + tuple(escape_routes):
            address = table.value + trampoline.offset
            resolved = entry_address = None
            if not relocatable:
                resolved = Ia64.branch_target(elf.read_address(address, BUNDLE), 2, address)
                entry = unique_definition(elf, trampoline.entry)
                entry_address = entry.value if entry is not None else None
            slots.append(SlotFacts(
                trampoline.name, trampoline.offset, trampoline.selector, trampoline.entry, address, trampoline.vector,
                tuple(i for i in code.instructions if address <= i.address < address + BUNDLE),
                tuple(Properties.relocations_in_extent(elf, table.section_index, address, address + BUNDLE)),
                relocatable, keeps, resolved, entry_address))
        return tuple(slots)

    def verify(self, facts):
        return [problem for slot in facts for problem in trampoline_problems(slot)]

    def mutants(self, facts):
        by_name = {slot.name: index for index, slot in enumerate(facts)}

        def edited(name, **changes):
            return Mutate.replace_at(facts, by_name[name], **changes)

        def with_slot(name, position, **changes):
            return edited(name, instructions=Mutate.replace_at(facts[by_name[name]].instructions, position, **changes))

        external = facts[by_name['external interrupt']]
        branch = external.instructions[2]
        yield 'external vector calls', with_slot('external interrupt', 2, text=branch.text.replace(
            branch.mnemonic, 'br.call.sptk.many b0=', 1).replace('= ', '='))
        for slot in facts:
            if slot.selector is not None:
                yield 'wrong %s selector' % slot.name, with_slot(
                    slot.name, 1, text='mov r16=%d' % ((slot.selector + 1) % 4))
        yield 'page-not-present selector not passed', with_slot('page not present', 1, text='nop.i 0x0')
        yield 'selector in the break vector', with_slot('break', 1, text='mov r16=0')
        yield 'state change in slot 0', with_slot('break', 0, text='mov r16=r0')
        yield 'floating-point fault recorded as a trap', with_slot('floating-point fault', 0, text='mov.m ar.k4=93')
        yield 'general exception without its vector', with_slot('general exception', 0, text='nop.m 0x0')
        yield 'unstopped break branch', with_slot('break', 2, stop_after=False)
        yield 'predicated external branch', with_slot(
            'external interrupt', 2, predicate='p06', text=branch.text.replace(branch.mnemonic, 'br.cond.sptk.many', 1))
        memory = external.instructions[0]
        long_branch = (dataclasses.replace(memory, template='MLX'),
                       dataclasses.replace(branch, template='MLX', slot=1, address=memory.address + 6,
                                           text=branch.text.replace(branch.mnemonic, 'brl.sptk.many', 1)))
        yield 'external vector relaxed into brl', edited('external interrupt', instructions=long_branch)
        yield 'break vector with template MII', with_slot('break', 2, template='MII')
        other = 'AsteriaIa64ExternalInterruptDispatchEntry'
        system_call = facts[by_name['break']]
        retarget = dict(instructions=Mutate.replace_at(system_call.instructions, 2, text=system_call.instructions[
            2].text.replace(system_call.entry, other)),
                        relocations=tuple(dataclasses.replace(fact, target=other) for fact in system_call.relocations))
        if not system_call.relocatable:
            retarget['resolved_target'] = external.entry_address
        yield 'break vector branches to the external dispatcher', edited('break', **retarget)
        if system_call.relocatable or system_call.keeps_relocations:
            fact = system_call.relocations[0]
            yield 'break relocation in slot 1', edited('break', relocations=(dataclasses.replace(fact, offset=1),))
            yield 'second relocation in the break bundle', edited(
                'break', relocations=system_call.relocations + (RelocationFact(0, 'LTOFF22', '__gp', 0),))
        if not system_call.relocatable:
            yield 'break branch reaches the entry plus one bundle', edited(
                'break', resolved_target=system_call.resolved_target + BUNDLE)
            far = system_call.bundle_address + PCREL21B_RANGE[1] + BUNDLE
            yield 'break entry beyond the PCREL21B range', edited('break', resolved_target=far, entry_address=far)


# O-row items 1 and table placement in the linked images ------------------------------------------

@dataclasses.dataclass(frozen=True)
class ImageAnchors:
    global_pointer: tuple[Symbol,...]            # Every symbol-table entry named __gp
    table: tuple[Symbol,...]                     # Definitions of the runtime IVT
    table_section: Section | None


@register
class Ia64FinalIvtAnchors(_ArtifactProperty):
    """In each linked image (`NucleusCore.elf` O0/O2 and the firmware
    `AsteriaBoot.elf`), `__gp` is one absolute (SHN_ABS) symbol, the fixed
    GP the system-call entry loads, and the runtime IVT is one GLOBAL OBJECT of 0x8000 bytes at a
    32 KiB-aligned address inside a non-writable executable section, as
    cr.iva requires."""

    def subjects(self, context):
        return Subjects.artifacts(context, TABLE, objects=False, images=True)

    def extract(self, context, subject):
        elf = load(subject.path)
        tables = elf.definitions(TABLE)
        return ImageAnchors(tuple(elf.symbols_named('__gp')), tables,
                            elf.section_of(tables[0]) if len(tables) == 1 else None)

    def verify(self, facts):
        problems = []
        defined = [s for s in facts.global_pointer if s.section_index != SHN_UNDEF]
        if len(facts.global_pointer) != 1 or len(defined) != 1:
            problems.append('__gp has %d symbol-table entries and %d definitions, expected one definition'
                            % (len(facts.global_pointer), len(defined)))
        elif defined[0].section_index != SHN_ABS:
            problems.append('__gp is relative to section %d, not absolute' % defined[0].section_index)
        if len(facts.table) != 1:
            return problems + ['%s has %d definitions, expected one' % (TABLE, len(facts.table))]
        table, section = facts.table[0], facts.table_section
        if table.type != STT_OBJECT or table.binding != STB_GLOBAL or table.size != TABLE_SIZE:
            problems.append('%s is type %d binding %d size %#x, expected a GLOBAL OBJECT of %#x bytes'
                            % (TABLE, table.type, table.binding, table.size, TABLE_SIZE))
        if table.value % TABLE_SIZE:
            problems.append('%s at %#x is not 32 KiB aligned' % (TABLE, table.value))
        if section is None or section.flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR) != SHF_ALLOC | SHF_EXECINSTR:
            problems.append('%s does not lie in a non-writable executable section' % TABLE)
        elif not section.address <= table.value <= table.value + table.size <= section.address + section.size:
            problems.append('%s extends beyond section %s' % (TABLE, section.name))
        return problems

    def mutants(self, facts):
        pointer, table = facts.global_pointer[0], facts.table[0]
        for description, changes in (
                ('__gp relative to .text', dict(global_pointer=(dataclasses.replace(
                    pointer, section_index=table.section_index),))),
                ('__gp undefined', dict(global_pointer=(dataclasses.replace(pointer, section_index=SHN_UNDEF),))),
                ('missing __gp', dict(global_pointer=())), ('duplicate __gp', dict(global_pointer=(pointer, pointer))),
                ('misaligned table', dict(table=(dataclasses.replace(table, value=table.value + VECTOR_SLOT),))),
                ('short table', dict(table=(dataclasses.replace(table, size=TABLE_SIZE - BUNDLE),))),
                ('table in a writable section', dict(table_section=dataclasses.replace(
                    facts.table_section, flags=facts.table_section.flags | SHF_WRITE))),
                ('missing table', dict(table=(), table_section=None))):
            yield description, dataclasses.replace(facts, **changes)


# Refill handlers ----------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class RefillHandler:
    label: str
    kind: str
    code: Code


def _is_decrement(step, register: str | None = None) -> bool:
    operation = step.operation
    written = Ia64.single_write(operation)
    return (operation.base == 'adds' and operation.immediates == (-1,) and written is not None
            and operation.reads == {written} and (register is None or written == register))


def _fail_address(handler: RefillHandler) -> int | None:
    """The offset of the fail-stop bundle: the handler ends in `br.few` to
    its own bundle."""
    last = handler.code.steps[-1] if handler.code.steps else None
    if (last is not None and last.mnemonic in ESCAPE_MNEMONICS and not last.operation.is_predicated
            and last.operation.target is not None and last.operation.target.symbol == 'AsteriaIa64StopEntry'
            and not last.operation.target.offset):
        return last.bundle
    return None


FORBIDDEN_REFILL_BASES = frozenset({'alloc', 'cover', 'flushrs', 'loadrs', 'invala', 'chk', 'epc', 'clrrrb',
                                    'vmsw', 'brp', 'brl'})
FRAME_REGISTERS = frozenset({'ar.rsc', 'ar.bsp', 'ar.bspstore', 'ar.rnat', 'ar.pfs'})
ARCHITECTURAL_BASES = frozenset({
    'break', 'fc', 'itc', 'itr', 'probe', 'ptc', 'ptr', 'rfi', 'rsm', 'ssm', 'rum', 'sum', 'sync', 'srlz',
    'tak', 'thash', 'ttag', 'setf', 'getf', 'xma', 'fadd', 'fclass', 'fcmp', 'fmax', 'fmerge', 'fmin',
    'fmpy', 'fpcmp', 'frcpa', 'frsqrta', 'fselect', 'fsub'})
# Per refill kind: both handlers add the escape's PL3 guard and the scan's
# exhaustion branch to their Miss block, which re-reads cr.ipsr and restores
# PR before the escape; the instruction handler's also leaves 0x0c in ar.k4.
REFILL_GUARDS = {'i': 17, 'd': 17}
REFILL_BRANCH_ROLES = {kind: {'fail': 14, 'next': 2, 'miss': 1, 'scan': 1, 'fail loop': 1, 'escape': 1}
                       for kind in ('i', 'd')}
REFILL_PR_WRITES = {'i': 2, 'd': 2}
ESCAPE_ARCHITECTURAL = ['mov r=cr.ipsr', 'mov pr=r,0xfffffffffffffffe']


def is_architectural(operation: Ia64.Operation) -> bool:
    """An ar./cr. operand, a psr/pr/branch/floating-point register, or a
    privileged, translation, serialization or floating-point mnemonic."""
    registers = operation.reads | operation.writes
    return (operation.base in ARCHITECTURAL_BASES
            or any(r.startswith(('ar.', 'cr.', 'psr')) or r in ('pr', 'pr.rot') for r in registers)
            or any(_BRANCH_REGISTER.match(r) or _FLOAT_REGISTER.match(r) for r in registers))


class _RefillProperty(_IvtProperty):
    def extract(self, context, variant):
        path = context.defining_object(variant, TABLE)
        return tuple(RefillHandler(vector.label, vector.kind, Ia64StopViews.core(path, Subjects.code(context, path, start=vector.start,
                                                                            end=vector.end)))
                     for vector in REFILL_VECTORS)

    def verify(self, facts):
        return ['%s handler: %s' % (handler.label, problem) for handler in facts for problem in self.judge(handler)]

    def judge(self, handler: RefillHandler) -> list[str]:
        raise NotImplementedError

    def mutants(self, facts):
        for index, handler in enumerate(facts):
            for description, code in self.handler_mutants(handler, handler.code, _Sites(handler.code)):
                yield '%s handler: %s' % (handler.label, description), Mutate.replace_at(facts, index, code=code)


class _Sites:
    """Mutation sites located from the decoded facts of one handler."""

    def __init__(self, code: Code):
        self.code = code
        self.save = code.first(lambda step: step.operation.reads == {'pr'})
        self.save_register = Ia64.single_write(self.save.operation)
        self.restore = code.first(lambda step: step.operation.writes == {'pr'})
        self.restore_register = Ia64.single_read(self.restore.operation)
        self.insert = code.first(lambda step: step.operation.base == 'itc')
        self.serialize = code.first(lambda step: Ia64.is_serialize(step.operation))
        self.return_ = code.first(lambda step: step.operation.base == 'rfi')
        self.nop_m = code.free_slot(kind='nop.m')
        self.nop_i_after_restore = code.free_slot(self.restore, 'nop.i')
        self.nop_m_after_restore = code.free_slot(self.restore, 'nop.m')

    def at_nop(self, text: str) -> Code:
        return self.code.rewrite(self.nop_m, text)


def _load_through_sp(code: Code) -> Code:
    load = code.first(lambda step: step.operation.memory == 'load')
    return code.rewrite(load, re.sub(r'\[r\d+\]', '[r12]', load.text))


@register
class Ia64RuntimeRefillRegisterDomain(_RefillProperty):
    """The alternate ITLB and DTLB refill handlers use only r0 and
    the bank-0 registers r16..r31; the interrupted r1..r15 and the stacked
    registers stay untouched."""

    def judge(self, handler):
        return Properties.register_domain_problems(list(handler.code.instructions), BANK_ZERO_REGISTERS,
                                                   allow_other_classes=True)

    def handler_mutants(self, handler, code, sites):
        yield 'PR saved in r15', code.rewrite(sites.save, 'mov r15=pr')
        yield 'ISR read into a stacked register', code.rewrite(
            code.first(lambda step: step.operation.reads == {'cr.isr'}), 'mov r32=cr.isr')
        yield 'load through sp', _load_through_sp(code)


@register
class Ia64RuntimeRefillForbiddenForms(_RefillProperty):
    """With PSR.ic=0 there is no stack, register stack, call, store
    or speculation recovery: the refill handlers contain no store or
    semaphore, alloc, call, cover/flushrs/loadrs/invala, register-stack or
    frame register (ar.rsc/bsp/bspstore/rnat/pfs), branch register, sp,
    floating-point register, chk/epc/clrrrb/vmsw/brp/brl, and no branch form
    other than the predicated `br.cond.dpnt.few`, unpredicated `br.few`, and
    in the data handler the unpredicated `br.many` escape to the user-fault
    entry."""

    def judge(self, handler):
        problems = []
        for step in handler.code.steps:
            operation = step.operation
            registers = operation.reads | operation.writes
            reasons = [reason for reason, applies in (
                ('writes memory', operation.memory in ('store', 'atomic')),
                ('is a forbidden %s' % operation.base, operation.base in FORBIDDEN_REFILL_BASES),
                ('calls', operation.is_call),
                ('uses register-stack or frame state', registers & FRAME_REGISTERS),
                ('uses a branch register', any(_BRANCH_REGISTER.match(r) for r in registers)),
                ('uses sp', Ia64.STACK_POINTER in registers),
                ('uses a floating-point register', any(_FLOAT_REGISTER.match(r) for r in registers)),
                ('is not a refill branch form', operation.base == 'br' and step.mnemonic != (
                    'br.cond.dpnt.few' if operation.is_predicated else 'br.few') and not (
                    is_escape(step) and not operation.is_predicated
                    and step.mnemonic in ESCAPE_MNEMONICS) and not (step is handler.code.steps[-1] and _fail_address(handler) is not None))) if applies]
            if reasons:
                problems.append('%s (#%d) %s' % (operation.text, step.position, ' and '.join(reasons)))
        return problems

    def handler_mutants(self, handler, code, sites):
        register = sites.save_register
        for description, text in (
                ('alloc', 'alloc %s=ar.pfs,0,0,0' % register), ('store', 'st8 [%s]=r0' % register),
                ('call', 'br.call.sptk.many b0=<+0x0>'), ('cover', 'cover'), ('flushrs', 'flushrs'),
                ('loadrs', 'loadrs'), ('invala', 'invala'), ('ar.rsc write', 'mov.m ar.rsc=%s' % register),
                ('speculation check', 'chk.s.i %s,0' % register), ('advanced-load check', 'chk.a.clr %s,0' % register),
                ('branch predict', 'brp.loop.imp 0,0'), ('privileged-code entry', 'epc'),
                ('register-rotation clear', 'clrrrb'), ('virtual-machine switch', 'vmsw.0'),
                ('setf', 'setf.sig f6=%s' % register), ('floating-point arithmetic', 'fadd f6=f6,f6')):
            yield '%s inserted' % description, sites.at_nop(text)
        yield 'branch-register write after the PR restore', code.rewrite(
            sites.nop_i_after_restore, 'mov b0=%s' % sites.restore_register)
        yield 'load through sp', _load_through_sp(code)
        scan = code.first(lambda step: step.mnemonic == 'br.few' and step.target != step.bundle)
        yield 'counted-loop branch', code.rewrite(scan, scan.text.replace('br.few', 'br.wtop.sptk.few', 1))


@register
class Ia64RuntimeRefillArchitecturalOperations(_RefillProperty):
    """The architectural operations of each refill handler are
    exactly, each once and unpredicated: `mov r=pr`, `mov r=psr`,
    `mov r=cr.ipsr`, `mov r=cr.isr`, `mov.m r=ar.k3`, `mov r=cr.ifa`,
    `mov cr.ifa=r`, `mov cr.itir=r`, `itc.K r`, `srlz.K`,
    `mov pr=r,0xfffffffffffffffe` and `rfi`, with K = i in the alternate
    ITLB handler and d in the alternate DTLB handler; the DTLB handler's
    Miss block adds one more `mov r=cr.ipsr`, `mov r=cr.isr` and
    `mov pr=r,0xfffffffffffffffe`. An extra cr/psr/itc/srlz/rfi changes
    interruption state."""

    def judge(self, handler):
        architectural = [step for step in handler.code.steps if is_architectural(step.operation)]
        problems = ['%s (#%d) is predicated' % (step.operation.text, step.position) for step in architectural
                    if step.operation.is_predicated]
        return problems + Properties.multiset_problems((shape(step.instruction) for step in architectural), [
            'mov r=pr', 'mov r=psr', 'mov r=cr.ipsr', 'mov r=cr.isr', 'mov.m r=ar.k3', 'mov r=cr.ifa',
            'mov cr.ifa=r', 'mov cr.itir=r', 'itc.%s r' % handler.kind, 'srlz.%s' % handler.kind,
            'mov pr=r,0xfffffffffffffffe', 'rfi'] + ESCAPE_ARCHITECTURAL + (['mov.m ar.k4=12'] if handler.kind == 'i' else []),
            'architectural operation')

    def handler_mutants(self, handler, code, sites):
        register = sites.save_register
        for description, text in (
                ('break', 'break.i 0x100000'), ('ar.lc write', 'mov.i ar.lc=%s' % register),
                ('ar.fpsr write', 'mov.m ar.fpsr=%s' % register), ('ssm', 'ssm 0x6000'), ('rsm', 'rsm 0x6000'),
                ('cr.iip write', 'mov cr.iip=%s' % register), ('cr.ipsr write', 'mov cr.ipsr=%s' % register),
                ('cr.ifs write', 'mov cr.ifs=%s' % register), ('setf', 'setf.sig f6=%s' % register),
                ('floating-point arithmetic', 'fadd f6=f6,f6'), ('cache flush', 'fc.i %s' % register)):
            yield '%s inserted' % description, sites.at_nop(text)
        yield 'PR restore removed', code.rewrite(sites.restore, 'nop.i 0x0')
        yield 'serialization removed', code.rewrite(sites.serialize, 'nop.m 0x0')
        yield 'rfi removed', code.rewrite(sites.return_, 'nop.b 0x0')
        other = 'd' if handler.kind == 'i' else 'i'
        yield 'itc.%s instead of itc.%s' % (other, handler.kind), code.rewrite(
            sites.insert, sites.insert.text.replace('itc.' + handler.kind, 'itc.' + other, 1))
        yield 'second PR write after the restore', code.rewrite(
            sites.nop_i_after_restore, 'mov pr=%s,0x2' % sites.restore_register)
        yield 'predicated rfi', code.replace(sites.return_, predicate='p06')


@register
class Ia64RuntimeRefillComparisonControl(_RefillProperty):
    """The ITLB handler has exactly 16 unpredicated guard compares
    and the DTLB handler 18 (its escape's two); each is immediately (after
    nop removal) consumed by a conditional branch on a predicate it writes,
    and every conditional branch is immediately preceded by its compare, so
    no guard is skipped or detached. The exact compare texts are in the
    golden."""

    def judge(self, handler):
        steps = handler.code.steps
        problems = []
        compares = [index for index, step in enumerate(steps) if step.operation.base == 'cmp']
        if len(compares) != REFILL_GUARDS[handler.kind]:
            problems.append('%d compares, expected %d' % (len(compares), REFILL_GUARDS[handler.kind]))
        for index in compares:
            compare = steps[index]
            if compare.operation.is_predicated:
                problems.append('%s (#%d) is predicated' % (compare.operation.text, compare.position))
            consumer = steps[index + 1] if index + 1 < len(steps) else None
            if consumer is None or not (consumer.operation.is_branch and consumer.operation.is_predicated):
                problems.append('%s (#%d) is not immediately consumed by a conditional branch'
                                % (compare.operation.text, compare.position))
            elif consumer.operation.predicate not in compare.operation.writes:
                problems.append('%s (#%d) branches on %s, which %s does not write'
                                % (consumer.operation.text, consumer.position, consumer.operation.predicate,
                                   compare.operation.text))
        problems += ['%s (#%d) is not immediately preceded by a compare' % (step.operation.text, step.position)
                     for index, step in enumerate(steps) if step.operation.is_branch and step.operation.is_predicated
                     and (index == 0 or steps[index - 1].operation.base != 'cmp')]
        return problems

    def handler_mutants(self, handler, code, sites):
        compares = code.all(lambda step: step.operation.base == 'cmp')
        for number, compare in enumerate(compares, 1):
            # Inserted into the guard's instruction group, which it now ends.
            yield 'compare inserted after guard %d' % number, code.insert(
                compare, 'cmp.eq p6,p7=r0,r0', after=True, stop=compare.instruction.stop_after).replace(
                compare, stop_after=False)
        yield 'predicated guard compare', code.replace(compares[0], predicate='p07')
        yield 'guard compare removed', code.rewrite(compares[0], 'nop.m 0x0')


@register
class Ia64RuntimeRefillControlFlow(_RefillProperty):
    """Every failure converges on one fail-stop self-loop (the
    last bundle, `br.few` to itself); the only other branches are predicated
    `br.cond.dpnt.few` to Fail (14 in the ITLB handler, 15 in the DTLB
    handler), 2 to Next (the second decrement of the publication count,
    immediately followed by the Scan branch) and one unpredicated backward
    `br.few` to Scan (its zero test); the DTLB handler adds one branch to
    its Miss block (which starts by re-reading cr.ipsr) and the one
    unpredicated escape to the user-fault entry. Every other target lies in
    [Start, End): no other branch leaves the vector, and the ITLB handler
    has none."""

    def judge(self, handler):
        steps = handler.code.steps
        fail = _fail_address(handler)
        if fail is None:
            return ['the handler does not end in the fail-stop self-loop']
        problems = []
        roles = collections.Counter()
        next_targets = set()
        scan = None
        for index, step in enumerate(steps):
            operation = step.operation
            if not operation.is_branch:
                continue
            target = step.target
            if is_escape(step) and not operation.is_predicated and step.mnemonic in ESCAPE_MNEMONICS:
                roles['escape'] += 1
            elif index == len(steps) - 1 and _fail_address(handler) is not None:
                roles['fail loop'] += 1
            elif target is None or not 0 <= target < handler.code.end - handler.code.start:
                problems.append('%s (#%d) leaves the handler' % (operation.text, step.position))
            elif index == len(steps) - 1:
                roles['fail loop'] += 1
            elif operation.is_predicated and step.mnemonic == 'br.cond.dpnt.few':
                landing = handler.code.at_offset(target)
                if target == fail:
                    roles['fail'] += 1
                elif landing is not None and landing.operation.reads == {'cr.ipsr'}:
                    roles['miss'] += 1
                else:
                    roles['next'] += 1
                    next_targets.add(target)
            elif not operation.is_predicated and step.mnemonic == 'br.few' and target < step.bundle:
                roles['scan'] += 1
                scan = (index, target)
            else:
                problems.append('%s (#%d) is not a refill branch form' % (operation.text, step.position))
        expected = REFILL_BRANCH_ROLES[handler.kind]
        problems += ['%d %s branches, expected %d' % (roles[role], role, expected.get(role, 0))
                     for role in sorted(set(expected) | set(roles)) if roles[role] != expected.get(role, 0)]
        if len(next_targets) > 1:
            problems.append('the Next branches reach %d different targets' % len(next_targets))
        if scan is not None and len(next_targets) == 1:
            index, scan_target = scan
            decrement = steps[index - 1]
            if handler.code.at_offset(next(iter(next_targets))) is not decrement or not _is_decrement(decrement):
                problems.append('Next is not the decrement immediately before the Scan branch')
            else:
                head = handler.code.at_offset(scan_target)
                count = Ia64.single_write(decrement.operation)
                if (head is None or head.operation.base != 'cmp' or 'eq' not in head.operation.completers
                        or head.operation.reads != {count, 'r0'}):
                    problems.append('Scan is not the zero test of %s' % count)
        return problems

    def handler_mutants(self, handler, code, sites):
        fail = _fail_address(handler)
        branches = code.all(lambda step: step.operation.is_branch)
        fail_branch = next(step for step in branches if step.operation.is_predicated and step.target == fail)
        next_branch = next(step for step in branches if step.operation.is_predicated and step.target != fail)
        scan_branch = next(step for step in branches if not step.operation.is_predicated and step.target != step.bundle)
        fail_loop = branches[-1]
        for role, step in (('fail', fail_branch), ('next', next_branch), ('scan', scan_branch),
                           ('fail-loop', fail_loop)):
            text = step.text.replace(step.mnemonic, 'br.wtop.sptk.few', 1)
            yield '%s branch as predicated br.wtop' % role, code.rewrite(step, '(p06) ' + text)
            yield '%s branch as unpredicated br.wtop' % role, code.rewrite(step, text)
        yield 'guard branches to Next', code.retarget(fail_branch, next_branch.target)
        yield 'fail loop re-enters Scan', code.retarget(fail_loop, scan_branch.target)
        yield 'guard leaves the handler', code.retarget(fail_branch, 'AsteriaIa64RuntimeBreakStart')
        yield 'Next reaches the first decrement', code.retarget(next_branch, code.first(_is_decrement).bundle)


@register
class Ia64RuntimeRefillMemoryLoads(_RefillProperty):
    """Each refill handler performs exactly the 7 authorized loads
    (context binding, processor index, ledger, publication count and three
    post-increment record loads), all `ld8`/`ld8.acq`, and no other memory
    operation: any other access could fault with PSR.ic=0."""

    def judge(self, handler):
        memory = [step for step in handler.code.steps if step.operation.memory is not None]
        problems = [] if len(memory) == 7 else ['%d memory operations, expected the 7 authorized loads' % len(memory)]
        problems += ['%s (#%d) is not an unpredicated ld8 or ld8.acq' % (step.operation.text, step.position)
                     for step in memory if step.mnemonic not in ('ld8', 'ld8.acq') or step.operation.is_predicated]
        incremented = [step for step in memory if step.operation.memory_base in step.operation.writes]
        if len(incremented) != 3 or [step.operation.immediates for step in incremented] != [(-16,), (8,), (32,)]:
            problems.append('record loads must visit PageCount, VirtualStart, BaseTranslationWord in that order')
        elif incremented[0].mnemonic != 'ld8.acq':
            problems.append('PageCount must be acquired before either payload word')
        return problems

    def handler_mutants(self, handler, code, sites):
        register = sites.save_register
        context_read = code.first(lambda step: step.operation.reads == {'ar.k3'})
        context = Ia64.single_write(context_read.operation)
        carrier = code.first(lambda step: step is context_read, lambda step: context in step.operation.reads
                             and step.operation.memory is None and Ia64.single_write(step.operation) is not None)
        yield 'post-incremented context carrier', code.rewrite(
            carrier, 'ld8 %s=[%s],8' % (Ia64.single_write(carrier.operation), context))
        yield 'unauthorized load', sites.at_nop('ld8 %s=[r0]' % register)
        yield 'faultable load after the PR restore', code.rewrite(sites.nop_m_after_restore, 'ld8 %s=[r0]' % register)
        yield 'store', sites.at_nop('st8 [%s]=r0' % register)
        yield 'prefetch', sites.at_nop('lfetch [%s]' % register)
        first = code.first(lambda step: step.mnemonic == 'ld8')
        yield 'speculative load', code.rewrite(first, first.text.replace('ld8', 'ld8.s', 1))
        yield 'narrow load', code.rewrite(first, first.text.replace('ld8', 'ld4', 1))
        last = code.all(lambda step: step.operation.memory_base in step.operation.writes)[-1]
        yield 'record load without post-increment', code.rewrite(last, last.text.rsplit(',', 1)[0])
        count = code.all(lambda step: step.operation.memory_base in step.operation.writes)[0]
        yield 'PageCount without acquire', code.rewrite(count, count.text.replace('ld8.acq', 'ld8'))
        payload = code.all(lambda step: step.operation.memory_base in step.operation.writes)[1]
        yield 'payload before PageCount', code.swap(count, payload)


@register
class Ia64RuntimeRefillSuccessTail(_RefillProperty):
    """The success tail of each refill handler is contiguous:
    `mov cr.ifa=a; mov cr.itir=b` (in groups before the insert), `itc.K c`,
    `srlz.K` in a later group, `mov pr=s,0xfffffffffffffffe`, `rfi`; PR is
    written only there (and, in the DTLB handler, once more before its
    escape), from the entry save `mov s=pr` (s untouched in
    between); c is the last `or` result before the insert, and a is
    `dep a=r0,x,0,12` of the `mov x=cr.ifa` read. The translation is
    serialized before rfi, PR is restored, and nothing in between can fault
    or branch."""

    def judge(self, handler):
        steps = handler.code.steps
        kind = handler.kind
        inserts = [index for index, step in enumerate(steps) if step.operation.base == 'itc']
        if len(inserts) != 1:
            return ['%d translation inserts, expected one' % len(inserts)]
        index = inserts[0]
        if index < 2 or index + 3 >= len(steps):
            return ['the translation insert lacks a complete success tail']
        ifa, itir, insert, serialize, restore, return_ = steps[index - 2:index + 4]
        problems = []
        if ifa.operation.base != 'mov' or ifa.operation.writes != {'cr.ifa'}:
            problems.append('%s precedes the insert instead of mov cr.ifa=' % ifa.operation.text)
        if itir.operation.base != 'mov' or itir.operation.writes != {'cr.itir'}:
            problems.append('%s precedes the insert instead of mov cr.itir=' % itir.operation.text)
        if insert.mnemonic != 'itc.' + kind:
            problems.append('%s is not itc.%s' % (insert.operation.text, kind))
        if serialize.mnemonic != 'srlz.' + kind:
            problems.append('the insert is followed by %s, not srlz.%s' % (serialize.operation.text, kind))
        restored = Ia64.single_read(restore.operation)
        if (restore.operation.base != 'mov' or restore.operation.writes != {'pr'} or restored is None
                or restore.operation.immediates != (0xfffffffffffffffe,)):
            problems.append('%s is not the PR restore mov pr=s,0xfffffffffffffffe' % restore.operation.text)
        if return_.operation.base != 'rfi':
            problems.append('the PR restore is followed by %s, not rfi' % return_.operation.text)
        problems += ['%s (#%d) is predicated' % (step.operation.text, step.position)
                     for step in (ifa, itir, insert, serialize, restore, return_) if step.operation.is_predicated]
        if not ifa.group < insert.group or not itir.group < insert.group:
            problems.append('cr.ifa and cr.itir are not written in an instruction group before the insert')
        if not insert.group < serialize.group:
            problems.append('no stop between %s and %s' % (insert.operation.text, serialize.operation.text))
        predicate_writers = [step for step in steps if step.operation.writes & {'pr', 'pr.rot'}]
        if len(predicate_writers) != REFILL_PR_WRITES[handler.kind] or predicate_writers[0] is not restore:
            problems.append('%d PR writes, expected the restore%s' % (
                len(predicate_writers), ' and the escape restore' if handler.kind == 'd' else ''))
        saves = [i for i, step in enumerate(steps) if step.operation.base == 'mov' and step.operation.reads == {'pr'}]
        if len(saves) != 1:
            problems.append('%d PR saves, expected one' % len(saves))
        else:
            saved = Ia64.single_write(steps[saves[0]].operation)
            if saved != restored:
                problems.append('PR is restored from %s but saved in %s' % (restored, saved))
            elif writes_between(steps, saved, saves[0], index + 2):
                problems.append('the PR save %s is overwritten before the restore' % saved)
        translation = Ia64.single_read(insert.operation)
        producers = [i for i in range(index) if steps[i].operation.base == 'or']
        if (translation is None or not producers or Ia64.single_write(steps[producers[-1]].operation) != translation
                or writes_between(steps, translation, producers[-1], index)):
            problems.append('the inserted translation is not the result of the last or before the insert')
        address = Ia64.single_read(ifa.operation)
        reads = [i for i, step in enumerate(steps) if step.operation.reads == {'cr.ifa'}]
        deposits = [i for i in range(index - 2) if steps[i].operation.base == 'dep'
                    and Ia64.single_write(steps[i].operation) == address]
        if address is None or len(reads) != 1 or not deposits:
            problems.append('cr.ifa is not written from the page-aligned fault address')
        else:
            deposit = steps[deposits[-1]]
            fault = Ia64.single_write(steps[reads[0]].operation)
            if (deposit.operation.immediates != (0, 12) or deposit.operation.reads != {'r0', fault}
                    or not reads[0] < deposits[-1] or writes_between(steps, fault, reads[0], deposits[-1])
                    or writes_between(steps, address, deposits[-1], index - 2)):
                problems.append('cr.ifa is not dep %s=r0,%s,0,12 of the cr.ifa read' % (address, fault))
        return problems

    def handler_mutants(self, handler, code, sites):
        kind = handler.kind
        translation = Ia64.single_read(sites.insert.operation)
        itir = code.first(lambda step: step.operation.writes == {'cr.itir'})
        producer = code.all(lambda step: step.operation.base == 'or', before=sites.insert)[-1]
        deposit = code.first(lambda step: step.operation.base == 'dep')
        yield 'PR restore removed', code.rewrite(sites.restore, 'nop.i 0x0')
        yield 'itc operand r0', code.rewrite(sites.insert, 'itc.%s r0' % kind)
        yield 'serialization removed', code.rewrite(sites.serialize, 'nop.m 0x0')
        yield 'rfi removed', code.rewrite(sites.return_, 'nop.b 0x0')
        yield 'compare after the PR restore', code.rewrite(sites.nop_i_after_restore, 'cmp.eq p6,p7=r0,r0')
        yield 'branch-register write after the PR restore', code.rewrite(
            sites.nop_i_after_restore, 'mov b0=%s' % sites.restore_register)
        yield 'faultable load after the PR restore', code.rewrite(
            sites.nop_m_after_restore, 'ld8 %s=[r0]' % sites.save_register)
        yield 'no stop after the insert', code.without_stops(sites.insert, sites.serialize)
        yield 'cr.ifa from r0', code.rewrite(code.first(lambda step: step.operation.writes == {'cr.ifa'}),
                                             'mov cr.ifa=r0')
        yield 'PR restored from the translation register', code.rewrite(
            sites.restore, 'mov pr=%s,0xfffffffffffffffe' % translation)
        yield 'PR save overwritten', code.rewrite(code.free_slot(sites.save, 'nop.i'),
                                                  'mov %s=r0' % sites.save_register)
        yield 'insert in the cr.itir group', code.without_stops(itir, sites.insert)
        yield 'srlz before itc', code.swap(sites.insert, sites.serialize)
        yield 'translation overwritten after its or', code.rewrite(code.free_slot(producer), 'mov %s=r0' % translation)
        yield 'fault address not page-aligned', code.rewrite(deposit, deposit.text.replace(',0,12', ',0,11'))


@register
class Ia64RuntimeRefillBoundedScan(_RefillProperty):
    """The publication scan terminates: its only backward branch is
    the unpredicated `br.few` to Scan immediately after `adds n=-1,n`; Scan
    tests n against zero; n comes from one acquire load, is written
    otherwise only by its two decrements, and is tested against zero and
    against an immediate upper bound before the loop, both failing into the
    fail-stop loop. A corrupt ledger cannot hang the processor inside the
    miss handler."""

    def judge(self, handler):
        steps = handler.code.steps
        fail = _fail_address(handler)
        backward = [(index, step, step.target) for index, step in enumerate(steps) if step.operation.is_branch
                    and step.target is not None and step.target < step.bundle]
        if len(backward) != 1:
            return ['%d backward branches besides the fail-stop loop, expected one' % len(backward)]
        index, branch, target = backward[0]
        problems = []
        if branch.operation.is_predicated or branch.mnemonic != 'br.few':
            problems.append('the backward branch %s is not an unpredicated br.few' % branch.operation.text)
        if index == 0 or not _is_decrement(steps[index - 1]):
            return problems + ['the backward branch is not immediately preceded by a decrement']
        count = Ia64.single_write(steps[index - 1].operation)
        head = handler.code.at_offset(target)
        head_index = steps.index(head) if head is not None else None
        if head is None or head.operation.base != 'cmp' or head.operation.reads != {count, 'r0'}:
            problems.append('the loop head does not test %s against zero' % count)
        loads = [i for i, step in enumerate(steps)
                 if step.operation.memory == 'load' and count in step.operation.writes]
        if len(loads) != 1 or 'acq' not in steps[loads[0]].operation.completers:
            return problems + ['%s is not loaded by exactly one acquire load' % count]
        writers = [step for step in steps if count in step.operation.writes]
        if len(writers) != 3 or len([step for step in writers if _is_decrement(step, count)]) != 2:
            problems.append('%s is written %d times, expected its load and two decrements' % (count, len(writers)))
        if head_index is None:
            return problems

        def guarded(test) -> bool:
            return any(steps[i].operation.base == 'cmp' and test(i, steps[i]) and i + 1 < len(steps)
                       and steps[i + 1].operation.is_branch and steps[i + 1].target == fail
                       for i in range(loads[0] + 1, head_index))

        def bounded(i, step) -> bool:
            others = step.operation.reads - {count}
            if count not in step.operation.reads or len(others) != 1:
                return False
            bound = next(iter(others))
            writer = next((steps[j] for j in range(i - 1, -1, -1) if bound in steps[j].operation.writes), None)
            return (writer is not None and writer.operation.base == 'mov' and writer.operation.immediates
                    and not writer.operation.reads and writer.operation.immediates[0] >= 1)

        if not guarded(lambda i, step: step.operation.reads == {count, 'r0'}):
            problems.append('%s is not tested against zero before the loop' % count)
        if not guarded(bounded):
            problems.append('%s is not bounded by an immediate before the loop' % count)
        return problems

    def handler_mutants(self, handler, code, sites):
        scan = code.first(lambda step: step.mnemonic == 'br.few' and (step.target or 0) < step.bundle)
        decrement = code.previous(scan)
        count = Ia64.single_write(decrement.operation)
        head = code.at_offset(scan.target)
        load = code.first(lambda step: step.operation.memory == 'load' and count in step.operation.writes)
        bound = code.first(lambda step: step is load, lambda step: step.operation.base == 'cmp' and count in
                           step.operation.reads and len(step.operation.reads - {count, 'r0'}) == 1)
        yield 'second decrement adds 0', code.rewrite(decrement, decrement.text.replace('=-1,', '=0,', 1))
        yield 'Scan skips the termination test', code.retarget(scan, head.bundle + BUNDLE)
        yield 'no upper bound', code.rewrite(bound, 'nop.m 0x0')
        yield 'count loaded without acquire', code.rewrite(load, load.text.replace('ld8.acq', 'ld8', 1))
        yield 'count rewritten in the loop', code.rewrite(code.free_slot(head, 'nop.i'), 'mov %s=r0' % count)
        yield 'first decrement removed', code.rewrite(code.first(lambda step: _is_decrement(step, count)), 'nop.i 0x0')


@register
class Ia64RuntimeRefillUserFaultEscape(_RefillProperty):
    """Only a PL3 data access that is not an RSE reference leaves the
    vector when the ledger has no record for it. The ITLB handler has no
    escape. In the DTLB handler, the only branch to its Miss block is the
    predicated branch right after the Scan zero test (the exhausted scan),
    so no admission guard and no ledger hit reaches it. The Miss block is
    exactly, in order: `mov a=cr.ipsr` and `mov b=cr.isr` (re-read: the scan
    reuses their registers), `extr.u c=a,32,2` (IPSR.cpl), `extr.u d=b,37,2`
    (ISR.rs and ISR.ir), `cmp.eq p7,p6=3,c` and `cmp.eq p7,p6=d,r0` each
    immediately followed by `(p6) br.cond.dpnt.few Fail`, then
    `mov pr=s,0xfffffffffffffffe` from the entry save `mov s=pr` with s not
    written in between, and in a later group the escape bundle (
    `mov r16=4` and the unpredicated branch to the user-fault entry). The
    branch to Fail on a kernel or RSE-origin miss keeps those fail-stop."""

    def judge(self, handler):
        code = handler.code
        escapes = code.all(is_escape)
        if len(escapes) != 1:
            return ['%d escapes, expected one' % len(escapes)]
        escape, fail = escapes[0], _fail_address(handler)
        misses = [step for step in code.steps if step.operation.is_branch and step.target is not None
                  and (code.at_offset(step.target) or step).operation.reads == {'cr.ipsr'}]
        if len(misses) != 1:
            return ['%d branches reach a Miss block, expected the exhausted scan alone' % len(misses)]
        branch = misses[0]
        test = code.previous(branch)
        scan = code.first(lambda step: step.mnemonic == 'br.few' and step.target is not None
                          and step.target < step.bundle)
        if (not branch.operation.is_predicated or test is None or scan is None
                or code.at_offset(scan.target) is not test):
            return ['the Miss block is not reached from the Scan zero test']
        block = code.window(after=code.previous(code.at_offset(branch.target)), before=escape)
        shapes = [shape(step.instruction) if not step.operation.is_branch else
                  ('fail' if step.target == fail and step.operation.is_predicated else step.rendered)
                  for step in block]
        expected = ['mov r=cr.ipsr', 'extr.u r=r,32,2', 'cmp.eq p7,p6=3,r',
                    'fail', 'mov pr=r,0xfffffffffffffffe']
        expected += ['mov.m ar.k4=12', 'mov r=6'] if handler.kind == 'i' else ['mov r=4']
        if shapes != expected:
            return ['the Miss block is %s, expected %s' % (shapes, expected)]
        status, privilege, privilege_test, _, restore = block[:5]
        problems = []
        if (Ia64.single_read(privilege.operation) != Ia64.single_write(status.operation)
                or Ia64.single_write(privilege.operation) not in privilege_test.operation.reads):
            problems.append('the escape does not test CPL from the re-read IPSR')
        target = 'AsteriaIa64UserExceptionEntry' if handler.kind == 'i' else USER_FAULT_ENTRY
        if escape.operation.target.symbol != target:
            problems.append('the escape targets %s instead of %s' % (escape.operation.target.symbol, target))
        save = code.first(lambda step: step.operation.base == 'mov' and step.operation.reads == {'pr'})
        saved = save and Ia64.single_write(save.operation)
        if saved is None or Ia64.single_read(restore.operation) != saved:
            problems.append('the escape does not restore PR from the entry save')
        elif any(step.writes(saved) for step in code.window(after=save, before=restore)):
            problems.append('the PR save is overwritten before the escape')
        if not restore.group < escape.group:
            problems.append('the PR restore is not in a group before the escape')
        return problems

    def handler_mutants(self, handler, code, sites):
        escape = code.site(is_escape)
        privilege = code.site(lambda step: step.text.startswith('cmp.eq p7,p6=3,'))
        restore = code.site(lambda step: step is privilege, lambda step: step.operation.writes == {'pr'})
        guard = code.site(_is_rejection_guard)
        head = code.all(lambda step: step.operation.reads == {'cr.ipsr'})[-1]
        miss_branch = code.site(lambda step: step.operation.is_branch and step.target == head.bundle)
        yield 'a kernel miss escapes', code.replace(privilege, text=privilege.text.replace('=3,', '=2,'))
        yield 'the privilege guard removed', code.nop(privilege)
        yield 'PR not restored', code.nop(restore)
        yield 'PR restored from the ISR copy', code.replace(restore, text='mov pr=r29,0xfffffffffffffffe')
        yield 'a guard reaches the escape', code.retarget(guard, head.bundle)
        yield 'the exhausted scan stops', code.retarget(miss_branch, _fail_address(handler))
        yield 'IPSR not re-read', code.replace(head, text='mov r30=r0')
        yield 'a second escape', code.insert(escape, escape.rendered)
        selector = code.previous(escape)
        yield 'wrong selector', code.replace(selector, text='mov r16=7')


def _is_rejection_guard(step) -> bool:
    return step.operation.is_predicated and step.mnemonic == 'br.cond.dpnt.few'


# IVT install leaves --------------------------------------------------------------------------------

INSTALLERS = (
    'AsteriaIa64InstallRuntimeInterruptVectorTable',
    'AsteriaIa64InstallInitialInterruptVectorTable',
    'AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable',
)
INSTALLER_CONTRACT = Properties.FunctionContract(Properties.CALLABLE, alignment=16, size_multiple=16)


@dataclasses.dataclass(frozen=True)
class LeafFacts:
    symbol: str
    definitions: tuple[Properties.SymbolFacts,...]
    code: Code


@register
class Ia64IvtInstallers(_IvtProperty):
    """The three cr.iva install leaves (the runtime installer, which also
    commits ar.k3, and the BSP and AP initial-IVT installers) are GLOBAL
    functions (DEFAULT or HIDDEN visibility: the C++ callers may live in
    another object), bundle-aligned with a whole number of bundles, carry no
    relocation (they run physical, before a GP exists), and
    serialize the cr.iva write: a `srlz` follows in a later instruction
    group, and the readback and every later branch lie in groups after that
    srlz. The exact commit sequences and status codes are golden."""

    def extract(self, context, variant):
        return tuple(LeafFacts(symbol, tuple(Properties.symbol_facts(load(path), symbol)),
                               Subjects.code(context, path, symbol))
                     for symbol in INSTALLERS for path in (context.defining_object(variant, symbol),))

    def verify(self, facts):
        problems = []
        for leaf in facts:
            problems += INSTALLER_CONTRACT.problems(leaf.definitions, leaf.symbol)
            problems += ['%s: relocation %s in a leaf that runs before a GP exists' % (leaf.symbol, fact,)
                         for fact in leaf.code.relocations if not (fact.type == 'PCREL21B'
                            and fact.target == 'AsteriaIa64StopEntry' and not fact.addend)]
            problems += ['%s: %s' % (leaf.symbol, problem)
                         for problem in control_register_serialization_problems(leaf.code.instructions)]
        return problems

    def mutants(self, facts):
        for index, leaf in enumerate(facts):
            code = leaf.code
            write = code.first(lambda step: _control_writes(step.operation))
            serializer = code.first(lambda step: step is write, lambda step: Ia64.is_serialize(step.operation)
                                    and step.group > write.group)
            readback = code.first(lambda step: step is serializer,
                                  lambda step: _control_writes(write.operation) & step.operation.reads)
            for description, changed in (('srlz dropped', code.rewrite(serializer, 'nop.m 0x0')),
                                         ('cr.iva write and srlz in one group', code.without_stops(write, serializer)),
                                         ('readback in the srlz group', code.without_stops(serializer, readback))):
                yield '%s: %s' % (leaf.symbol, description), Mutate.replace_at(facts, index, code=changed)
        leaf = facts[-1]
        for description, definitions in INSTALLER_CONTRACT.mutants(leaf.definitions):
            yield 'AP installer: %s' % description, Mutate.replace_at(facts, len(facts) - 1, definitions=definitions)
        yield 'AP installer with a GP-relative relocation', Mutate.replace_at(
            facts, len(facts) - 1, code=dataclasses.replace(leaf.code, relocations=(
                RelocationFact(0x10, 'GPREL22', '__gp', 0),)))


@dataclasses.dataclass(frozen=True)
class BootstrapTableFacts:
    address: int
    size: int
    flags: int
    data: bytes


@register
class Ia64BootstrapIvtStaticImage(Property):
    """The bootstrap fail-stop IVT is aligned immutable text, sharing the image ITR."""
    isas = ('Ia64',)
    variants = ('O0', 'O2', 'firmware', 'firmware-test')

    def extract(self, context, variant):
        if variant in ('firmware', 'firmware-test'):
            path = context.artifact('%s:kernel_elf' % variant)
        else:
            path = context.object(variant, 'Source/Platform/Ia64/Memory/InitialInterruptVectorTableInstaller.S')
        elf = load(path)
        symbol = unique_definition(elf, 'AsteriaIa64BootstrapInterruptVectorTableImage')
        if symbol is None:
            return BootstrapTableFacts(1, 0, 0, b'')
        section = elf.sections[symbol.section_index]
        return BootstrapTableFacts(symbol.value, symbol.size, section.flags, elf.symbol_bytes(symbol))

    def verify(self, facts):
        problems = []
        if facts.address % TABLE_SIZE or facts.size != TABLE_SIZE:
            problems.append('the static bootstrap IVT is not an aligned 32 KiB table')
        if facts.flags != SHF_ALLOC | SHF_EXECINSTR:
            problems.append('the static bootstrap IVT is not immutable executable text')
        if facts.data != FAIL_STOP_BUNDLE * (TABLE_SIZE // BUNDLE):
            problems.append('the static bootstrap IVT has a non-self-loop bundle')
        return problems

    def mutants(self, facts):
        yield 'misaligned IVA', dataclasses.replace(facts, address=facts.address + BUNDLE)
        yield 'truncated table', dataclasses.replace(facts, size=facts.size - BUNDLE)
        yield 'writable bootstrap IVT', dataclasses.replace(facts, flags=facts.flags | SHF_WRITE)
        yield 'non-executable bootstrap IVT', dataclasses.replace(facts, flags=SHF_ALLOC)
        yield 'zero first bundle', dataclasses.replace(facts, data=bytes(BUNDLE) + facts.data[BUNDLE:])
        yield 'zero last bundle', dataclasses.replace(facts, data=facts.data[:-BUNDLE] + bytes(BUNDLE))
