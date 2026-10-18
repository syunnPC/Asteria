# SPDX-License-Identifier: Apache-2.0
"""Disassembly extraction and normalization for IA-64 and x86-64 artifacts.

The decoder takes the extent of a symbol (or an explicit range) from the ELF
symbol table, disassembles exactly that extent of the containing section,
and attaches relocations read from the ELF `.rela` sections — never from
objdump's tool-dependent annotation text. Branch targets are rendered from their
numeric value: inside the range as `<+0xOFF>`, outside it as the containing
function symbol, so the same routine normalizes identically in a relocatable
object at O0 or O2 and in a linked image.

For IA-64 the decoder records the bundle template, the slot, and whether an
instruction-group stop follows the instruction. `normalize_ia64_groups` drops
nop slots while carrying their stops, which keeps the group structure that
IA-64 dependency correctness depends on and ignores bundle padding.
"""
from __future__ import annotations

import dataclasses
import re
import subprocess

from AsteriaVerify import Relocations
from AsteriaVerify.Elf import EM_IA_64, ET_REL, STT_FUNC, STT_SECTION, ElfFile, Symbol, load

_LINE = re.compile(r'^\s*(?P<address>[0-9a-f]+):\t(?P<rest>.*)$')
_TEMPLATE = re.compile(r'^\[(?P<template>[A-Z]+)\]\s*')
_PREDICATE = re.compile(r'^\((?P<predicate>p\d+)\)\s*')
# A direct branch or call target as objdump prints it: `<hex> <label>`,
# optionally after `b0=`, or a speculation check's recovery target after its
# register (`chk.s.i r15,<hex> <label>`).
_TARGET = re.compile(r'(?P<prefix>(?:=|\s|,))(?P<address>[0-9a-f]+) <[^>]*>$')


class DisassemblyError(Exception):
    """Raised when objdump fails or its output cannot be decoded."""


@dataclasses.dataclass(frozen=True)
class Instruction:
    address: int
    text: str
    template: str | None = None
    slot: int = 0
    stop_after: bool = False
    predicate: str | None = None
    relocation: str | None = None

    @property
    def mnemonic(self) -> str:
        return self.text.split(None, 1)[0] if self.text else ''

    @property
    def is_nop(self) -> bool:
        return self.mnemonic.startswith('nop')

    def rendered(self) -> str:
        """Text with the predicate prefix, as it appears in golden files."""
        return '(%s) %s' % (self.predicate, self.text) if self.predicate else self.text


@dataclasses.dataclass
class DecodedRange:
    path: str
    section: str
    start: int
    end: int
    label: str
    instructions: list[Instruction]


def _objdump(objdump: str, path: str, section: str, start: int, end: int) -> str:
    # The IVT is an OBJECT symbol whose initial bundles are executable; -d
    # prints those as hex data. Decode the explicitly requested extent.
    command = [objdump, '-D', '--no-show-raw-insn', '-j', section,
               '--start-address=%#x' % start, '--stop-address=%#x' % end, str(path)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise DisassemblyError('%s failed for %s: %s' % (objdump, path, result.stderr.strip()))
    return result.stdout


def raw_instructions(objdump: str, path, section: str, start: int, end: int) -> list[Instruction]:
    """Decodes [start, end) of an IA-64 section without symbol-relative
    rendering or relocations (for code no symbol labels)."""
    return _decode_lines(_objdump(objdump, path, section, start, end), True)


def _decode_lines(text: str, isa_ia64: bool) -> list[Instruction]:
    instructions: list[Instruction] = []
    template = None
    slot = 0
    for line in text.splitlines():
        match = _LINE.match(line)
        if not match:
            continue
        body = match.group('rest').strip()
        if not body:
            continue
        address = int(match.group('address'), 16)
        if isa_ia64:
            template_match = _TEMPLATE.match(body)
            if template_match:
                template = template_match.group('template')
                slot = 0
                body = body[template_match.end():]
            else:
                slot += 1
        stop = body.endswith(';;')
        if stop:
            body = body[:-2].rstrip()
        predicate = None
        predicate_match = _PREDICATE.match(body)
        if predicate_match:
            predicate = predicate_match.group('predicate')
            body = body[predicate_match.end():]
        instructions.append(Instruction(address, ' '.join(body.split()), template, slot, stop, predicate))
    return instructions


def _containing_symbol(elf: ElfFile, section_index: int, address: int) -> Symbol | None:
    best = None
    for symbol in elf.symbols:
        if symbol.section_index != section_index or symbol.type not in (STT_FUNC, 1):
            continue
        if symbol.value <= address < symbol.value + max(symbol.size, 1):
            if best is None or symbol.value > best.value:
                best = symbol
    return best


def describe_address(elf: ElfFile, section_index: int, address: int) -> str:
    """Names an address of a section as `symbol` or `symbol+0xN`.

    The containing FUNC or OBJECT symbol is preferred; without one the
    address is given relative to its section.
    """
    symbol = _containing_symbol(elf, section_index, address)
    if symbol is None:
        return '%s+%#x' % (elf.sections[section_index].name, address - elf.sections[section_index].address) \
            if elf.type != ET_REL else '%s+%#x' % (elf.sections[section_index].name, address)
    offset = address - symbol.value
    return symbol.name if offset == 0 else '%s+%#x' % (symbol.name, offset)


def _render_relocation(elf: ElfFile, relocation) -> str:
    kind = Relocations.name(elf.machine, relocation.type)
    symbol = relocation.symbol
    if symbol is None:
        target = '%#x' % relocation.addend
    elif symbol.type == STT_SECTION:
        section = elf.sections[symbol.section_index]
        base = 0 if elf.type == ET_REL else section.address
        target = describe_address(elf, symbol.section_index, base + relocation.addend)
    else:
        target = symbol.name
        if relocation.addend:
            target += '%+#x' % relocation.addend
    return '<reloc:%s %s>' % (kind, target)


def _relocations_by_instruction(elf: ElfFile, section_index: int, start: int, end: int) -> dict[int, str]:
    """Maps instruction addresses in [start, end) to rendered relocations."""
    result: dict[int, str] = {}
    for relocation in elf.relocations_in(section_index, start, end):
        if elf.machine == EM_IA_64:
            # IA-64 relocations name bundle + slot; objdump prints slot N
            # at bundle + 6 * N.
            address = (relocation.offset & ~0xF) + 6 * (relocation.offset & 0xF)
        else:
            address = relocation.offset
        result[address] = _render_relocation(elf, relocation)
    return result


def _instruction_start_for(instructions: list[Instruction], address: int) -> int | None:
    """x86 relocations point inside an instruction; find the containing one."""
    candidate = None
    for instruction in instructions:
        if instruction.address <= address:
            candidate = instruction.address
        else:
            break
    return candidate


def _apply_relocation(text: str, token: str, kind: str) -> str:
    if kind in Relocations.IA64_BRANCH or _TARGET.search(text):
        substituted, count = _TARGET.subn(lambda m: m.group('prefix') + token, text)
        if count:
            return substituted
    # Immediate operand: replace the first numeric immediate after '='.
    substituted, count = re.subn(r'=(-?0x[0-9a-f]+|-?\d+)', '=' + token, text, count=1)
    if count:
        return substituted
    return text + ' ' + token


def _normalize_target(text: str, elf: ElfFile, section_index: int, start: int, end: int) -> str:
    match = _TARGET.search(text)
    if not match:
        return text
    address = int(match.group('address'), 16)
    if start <= address < end:
        replacement = '<+%#x>' % (address - start)
    else:
        replacement = '<%s>' % describe_address(elf, section_index, address)
    return text[:match.start()] + match.group('prefix') + replacement


def _symbolize_immediate(text: str, elf: ElfFile) -> str:
    """In a linked image, renders a `movl` immediate that equals the value of
    a defined symbol as `<symbol>`, matching the relocation form of objects."""
    match = re.match(r'^(movl \S+=)(0x[0-9a-f]+)$', text)
    if not match:
        return text
    value = int(match.group(2), 16)
    candidates = sorted((s for s in elf.defined_symbols() if s.value == value),
                        key=lambda s: (s.binding != 1, s.name))
    if not candidates:
        return text
    return '%s<%s>' % (match.group(1), candidates[0].name)


def disassemble(objdump: str, path: str, *, symbol: str | None = None,
                start_symbol: str | None = None, end_symbol: str | None = None) -> DecodedRange:
    """Disassembles one symbol's extent, or the range [start_symbol, end_symbol)."""
    elf = load(path)
    if symbol is not None:
        found = elf.symbol(symbol)
        if found is None:
            raise DisassemblyError('%s does not define %s' % (path, symbol))
        if found.size == 0:
            raise DisassemblyError('%s has zero size in %s' % (symbol, path))
        section_index, start, end, label = found.section_index, found.value, found.value + found.size, symbol
    else:
        first, last = elf.symbol(start_symbol), elf.symbol(end_symbol)
        if first is None or last is None or first.section_index != last.section_index:
            raise DisassemblyError('range %s..%s is not defined in one section of %s'
                                   % (start_symbol, end_symbol, path))
        section_index, start, end, label = first.section_index, first.value, last.value, '%s..%s' % (
            start_symbol, end_symbol)
    section = elf.sections[section_index]
    ia64 = elf.machine == EM_IA_64
    raw = _decode_lines(_objdump(objdump, path, section.name, start, end), ia64)
    relocations = _relocations_by_instruction(elf, section_index, start, end)
    if not ia64:
        rebased = {}
        for address, token in relocations.items():
            owner = _instruction_start_for(raw, address)
            if owner is not None:
                rebased[owner] = token
        relocations = rebased
    decoded = []
    for instruction in raw:
        text = instruction.text
        token = relocations.get(instruction.address)
        if token is not None:
            kind = token[len('<reloc:'):].split(' ', 1)[0]
            text = _apply_relocation(text, token, kind)
        else:
            text = _normalize_target(text, elf, section_index, start, end)
            if elf.type != ET_REL:
                text = _symbolize_immediate(text, elf)
        decoded.append(dataclasses.replace(instruction, text=text, relocation=token))
    if ia64 and decoded and decoded[0].address != start:
        raise DisassemblyError('%s: first bundle of %s is at %#x, not %#x'
                               % (path, label, decoded[0].address, start))
    return DecodedRange(str(path), section.name, start, end, label, decoded)


def normalize_ia64_groups(instructions: list[Instruction]) -> list[str]:
    """Formats IA-64 instructions as `stopBefore:stopAfter:text`.

    Nop slots are dropped; a stop that followed a dropped nop is carried to the
    next meaningful instruction as stopBefore, so the instruction-group
    structure survives the removal of bundle padding.
    """
    lines = []
    stop_pending = False
    for instruction in instructions:
        if instruction.is_nop:
            stop_pending = stop_pending or instruction.stop_after
            continue
        lines.append('%d:%d:%s' % (int(stop_pending), int(instruction.stop_after), instruction.rendered()))
        stop_pending = instruction.stop_after
    return lines


def group_indices(instructions: list[Instruction]) -> list[int]:
    """Instruction-group index of each instruction (nops included)."""
    indices = []
    group = 0
    for instruction in instructions:
        indices.append(group)
        if instruction.stop_after:
            group += 1
    return indices


def normalize_x86(instructions: list[Instruction]) -> list[str]:
    """Formats x86-64 instructions without addresses or padding nops."""
    return [instruction.rendered() for instruction in instructions if not instruction.is_nop]


def tool_version(tool: str) -> str:
    """First line of `tool --version`, recorded in golden files."""
    result = subprocess.run([tool, '--version'], capture_output=True, text=True)
    return result.stdout.splitlines()[0].strip() if result.returncode == 0 and result.stdout else 'unknown'
