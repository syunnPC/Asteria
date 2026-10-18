# SPDX-License-Identifier: Apache-2.0
"""Minimal little-endian ELF64 reader for verification checks.

Only the structures the checks need are decoded: the file header, section
headers, program headers, symbols, and RELA relocations. Every read is
bounds-checked; a malformed file raises ElfError.
"""
from __future__ import annotations

import bisect
import dataclasses
import functools
import pathlib
import struct

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
STT_NOTYPE = 0
STT_OBJECT = 1
STT_FUNC = 2
STT_SECTION = 3
STT_FILE = 4
STB_LOCAL = 0
STB_GLOBAL = 1
STB_WEAK = 2
STV_DEFAULT = 0
STV_INTERNAL = 1
STV_HIDDEN = 2
STV_PROTECTED = 3
PT_LOAD = 1
PT_DYNAMIC = 2
PF_X = 0x1
PF_W = 0x2
PF_R = 0x4
EM_IA_64 = 50
EM_X86_64 = 62
ET_REL = 1
ET_EXEC = 2
EF_IA_64_ABI64 = 0x10

BINDING_NAMES = {STB_LOCAL: 'LOCAL', STB_GLOBAL: 'GLOBAL', STB_WEAK: 'WEAK'}
TYPE_NAMES = {STT_NOTYPE: 'NOTYPE', STT_OBJECT: 'OBJECT', STT_FUNC: 'FUNC', STT_SECTION: 'SECTION',
              STT_FILE: 'FILE'}
VISIBILITY_NAMES = {STV_DEFAULT: 'DEFAULT', STV_INTERNAL: 'INTERNAL', STV_HIDDEN: 'HIDDEN',
                    STV_PROTECTED: 'PROTECTED'}


class ElfError(Exception):
    """Raised when a file is not a well-formed little-endian ELF64 object."""


@dataclasses.dataclass(frozen=True)
class Section:
    index: int
    name: str
    type: int
    flags: int
    address: int
    offset: int
    size: int
    link: int
    info: int
    alignment: int
    entry_size: int


@dataclasses.dataclass(frozen=True)
class Segment:
    type: int
    flags: int
    offset: int
    virtual_address: int
    physical_address: int
    file_size: int
    memory_size: int
    alignment: int


@dataclasses.dataclass(frozen=True)
class Symbol:
    name: str
    value: int
    size: int
    type: int
    binding: int
    visibility: int
    section_index: int


@dataclasses.dataclass(frozen=True)
class Relocation:
    offset: int
    type: int
    symbol: Symbol | None
    addend: int


class ElfFile:
    def __init__(self, path: str | pathlib.Path):
        self.path = pathlib.Path(path)
        self.data = self.path.read_bytes()
        if self.data[:4] != b'\x7fELF' or len(self.data) < 64:
            raise ElfError('%s is not an ELF file' % self.path)
        if self.data[4] != 2 or self.data[5] != 1:
            raise ElfError('%s is not a little-endian ELF64 file' % self.path)
        (self.type, self.machine, _, self.entry, self.program_header_offset,
         self.section_header_offset, self.flags, _, program_entry_size, program_count,
         section_entry_size, section_count, section_name_index) = self._unpack('<HHIQQQIHHHHHH', 16)
        if section_count and section_entry_size != 64:
            raise ElfError('%s has unexpected section header size' % self.path)
        if program_count and program_entry_size != 56:
            raise ElfError('%s has unexpected program header size' % self.path)
        raw = [self._unpack('<IIQQQQIIQQ', self.section_header_offset + i * 64) for i in range(section_count)]
        names = raw[section_name_index] if section_count else None
        self.sections = []
        for index, fields in enumerate(raw):
            name = self._string(names[4], names[5], fields[0]) if names else ''
            self.sections.append(Section(index, name, fields[1], fields[2], fields[3], fields[4],
                                         fields[5], fields[6], fields[7], fields[8], fields[9]))
        self.segments = [Segment(*self._unpack('<IIQQQQQQ', self.program_header_offset + i * 56))
                         for i in range(program_count)]
        self._symbols = None
        self._relocations: dict[int, tuple[list[int], list[Relocation]]] = {}

    def _unpack(self, form: str, offset: int):
        size = struct.calcsize(form)
        if offset < 0 or offset + size > len(self.data):
            raise ElfError('%s is truncated at offset %#x' % (self.path, offset))
        return struct.unpack_from(form, self.data, offset)

    def _string(self, table_offset: int, table_size: int, offset: int) -> str:
        if offset >= table_size:
            raise ElfError('%s has a string offset outside its table' % self.path)
        start = table_offset + offset
        end = self.data.find(b'\0', start, table_offset + table_size)
        if end < 0:
            raise ElfError('%s has an unterminated string' % self.path)
        return self.data[start:end].decode('utf-8', errors='replace')

    def section(self, name: str) -> Section | None:
        return next((s for s in self.sections if s.name == name), None)

    def section_bytes(self, section: Section) -> bytes:
        if section.type == SHT_NOBITS:
            return b''
        if section.offset + section.size > len(self.data):
            raise ElfError('%s section %s exceeds the file' % (self.path, section.name))
        return self.data[section.offset:section.offset + section.size]

    @property
    def symbols(self) -> list[Symbol]:
        if self._symbols is None:
            self._symbols = []
            table = next((s for s in self.sections if s.type == SHT_SYMTAB), None)
            if table is not None:
                strings = self.sections[table.link]
                for i in range(table.size // 24):
                    name, info, other, index, value, size = self._unpack('<IBBHQQ', table.offset + i * 24)
                    self._symbols.append(Symbol(self._string(strings.offset, strings.size, name),
                                                value, size, info & 0xF, info >> 4, other & 0x3, index))
        return self._symbols

    @functools.cached_property
    def _names(self) -> dict[str, list[Symbol]]:
        names: dict[str, list[Symbol]] = {}
        for symbol in self.symbols:
            names.setdefault(symbol.name, []).append(symbol)
        return names

    def symbol(self, name: str) -> Symbol | None:
        matches = self.symbols_named(name)
        if len(matches) > 1:
            raise ElfError('%s defines %s more than once' % (self.path, name))
        return matches[0] if matches else None

    def symbols_named(self, name: str) -> list[Symbol]:
        """Every symbol-table entry called `name` (definitions and references)."""
        return list(self._names.get(name, ()))

    def definitions(self, name: str) -> tuple[Symbol, ...]:
        """The entries called `name` that define it (section or absolute)."""
        return tuple(s for s in self.symbols_named(name) if s.section_index != SHN_UNDEF)

    def defined_symbols(self) -> list[Symbol]:
        """Named symbols defined in a section or absolute (no FILE/SECTION)."""
        return [s for s in self.symbols
                if s.name and s.section_index != SHN_UNDEF and s.type not in (STT_FILE, STT_SECTION)]

    def undefined_symbols(self) -> list[Symbol]:
        """Undefined symbol-table entries other than the null entry, weak ones included."""
        return [s for s in self.symbols[1:] if s.section_index == SHN_UNDEF]

    def section_of(self, symbol: Symbol) -> Section | None:
        if 0 < symbol.section_index < len(self.sections):
            return self.sections[symbol.section_index]
        return None

    def section_containing(self, address: int) -> Section | None:
        """The allocated section of a linked image that contains `address`."""
        for section in self.sections:
            if section.flags & SHF_ALLOC and section.address <= address < section.address + section.size:
                return section
        return None

    def read(self, section: Section, offset: int, size: int) -> bytes:
        """Bytes [offset, offset + size) of a section (offsets are section-relative)."""
        if offset < 0 or size < 0 or offset + size > section.size:
            raise ElfError('%s: read of %#x bytes at %s+%#x exceeds the section'
                           % (self.path, size, section.name, offset))
        if section.type == SHT_NOBITS:
            return bytes(size)
        return self.section_bytes(section)[offset:offset + size]

    def read_address(self, address: int, size: int) -> bytes:
        """Bytes at a virtual address of a linked image."""
        section = self.section_containing(address)
        if section is None:
            raise ElfError('%s: no allocated section contains %#x' % (self.path, address))
        return self.read(section, address - section.address, size)

    def symbol_bytes(self, symbol: Symbol) -> bytes:
        """The bytes of a defined symbol's extent."""
        section = self.section_of(symbol)
        if section is None:
            raise ElfError('%s: %s is not defined in a section' % (self.path, symbol.name))
        base = 0 if self.type == ET_REL else section.address
        return self.read(section, symbol.value - base, symbol.size)

    def relocations(self, section: Section) -> list[Relocation]:
        if section.type != SHT_RELA:
            raise ElfError('%s is not a RELA section' % section.name)
        result = []
        for i in range(section.size // 24):
            offset, info, addend = self._unpack('<QQq', section.offset + i * 24)
            index = info >> 32
            symbol = self.symbols[index] if 0 < index < len(self.symbols) else None
            result.append(Relocation(offset, info & 0xFFFFFFFF, symbol, addend))
        return result

    def relocation_sections(self) -> list[Section]:
        return [s for s in self.sections if s.type == SHT_RELA]

    def relocations_in(self, section_index: int, start: int, end: int) -> list[Relocation]:
        """The relocations of every RELA section for `section_index` whose
        r_offset lies in [start, end), by offset (file order among equals)."""
        if section_index not in self._relocations:
            ordered = sorted((relocation for rela in self.relocation_sections() if rela.info == section_index
                              for relocation in self.relocations(rela)), key=lambda relocation: relocation.offset)
            self._relocations[section_index] = ([relocation.offset for relocation in ordered], ordered)
        offsets, ordered = self._relocations[section_index]
        return ordered[bisect.bisect_left(offsets, start):bisect.bisect_left(offsets, end)]


@functools.lru_cache(maxsize=None)
def _load(path: str, stamp: tuple[int, int]) -> ElfFile:
    return ElfFile(path)


def load(path: str | pathlib.Path) -> ElfFile:
    """The parsed ELF file at `path`, shared by every check of a run (read
    again when the file changes)."""
    stat = pathlib.Path(path).stat()
    return _load(str(path), (stat.st_mtime_ns, stat.st_size))
