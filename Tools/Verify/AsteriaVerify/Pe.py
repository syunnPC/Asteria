# SPDX-License-Identifier: Apache-2.0
"""Minimal PE32+ reader for verification checks.

Decodes the parts of a UEFI PE32+ image that the firmware checks need: the
DOS stub pointer, the COFF file header, the PE32+ optional header with its
data directories, the section table, and the base-relocation blocks. Every
read is bounds-checked; a malformed image raises PeError.
"""
from __future__ import annotations

import dataclasses
import pathlib
import struct

MACHINE_IA64 = 0x0200
MACHINE_AMD64 = 0x8664
MAGIC_PE32 = 0x10B
MAGIC_PE32_PLUS = 0x20B
SUBSYSTEM_EFI_APPLICATION = 10

SCN_CNT_CODE = 0x00000020
SCN_CNT_INITIALIZED_DATA = 0x00000040
SCN_MEM_EXECUTE = 0x20000000
SCN_MEM_READ = 0x40000000
SCN_MEM_WRITE = 0x80000000

DIRECTORY_BASE_RELOCATION = 5

REL_BASED_HIGHLOW = 3
REL_BASED_DIR64 = 10

# Offsets inside the PE32+ optional header.
_OPTIONAL_ENTRY = 16
_OPTIONAL_IMAGE_BASE = 24
_OPTIONAL_ALIGNMENTS = 32
_OPTIONAL_SIZES = 56
_OPTIONAL_SUBSYSTEM = 68
_OPTIONAL_DIRECTORY_COUNT = 108
_OPTIONAL_DIRECTORIES = 112


class PeError(Exception):
    """Raised when a file is not a well-formed PE32+ image."""


@dataclasses.dataclass(frozen=True)
class Header:
    """COFF file header and PE32+ optional-header fields."""
    machine: int
    section_count: int
    optional_header_size: int
    characteristics: int
    magic: int
    entry_rva: int
    image_base: int
    section_alignment: int
    file_alignment: int
    image_size: int
    headers_size: int
    checksum: int
    subsystem: int
    dll_characteristics: int
    directories: tuple[tuple[int, int], ...]    # (rva, size) per data directory

    def directory(self, index: int) -> tuple[int, int]:
        return self.directories[index] if index < len(self.directories) else (0, 0)


@dataclasses.dataclass(frozen=True)
class Section:
    name: str
    virtual_size: int
    rva: int
    raw_size: int
    raw_offset: int
    characteristics: int


@dataclasses.dataclass(frozen=True)
class RelocationBlock:
    """One base-relocation block: the page RVA and its raw 16-bit entries."""
    page_rva: int
    size: int
    entries: tuple[int, ...]


class PeFile:
    def __init__(self, source: str | pathlib.Path | bytes):
        if isinstance(source, (bytes, bytearray)):
            self.path = pathlib.Path('<memory>')
            self.data = bytes(source)
        else:
            self.path = pathlib.Path(source)
            self.data = self.path.read_bytes()
        if self.data[:2] != b'MZ':
            raise PeError('%s lacks the MZ signature' % self.path)
        (pe_offset,) = self._unpack('<I', 0x3C)
        if self._slice(pe_offset, 4) != b'PE\0\0':
            raise PeError('%s lacks the PE signature' % self.path)
        machine, section_count, _, _, _, optional_size, characteristics = self._unpack('<HHIIIHH', pe_offset + 4)
        optional = pe_offset + 24
        (magic,) = self._unpack('<H', optional)
        if magic != MAGIC_PE32_PLUS:
            # Keep the header readable so a check can report the wrong magic.
            self.header = Header(machine, section_count, optional_size, characteristics, magic,
                                 0, 0, 0, 0, 0, 0, 0, 0, 0, ())
            self.sections = []
            return
        if optional_size < _OPTIONAL_DIRECTORIES:
            raise PeError('%s has a truncated PE32+ optional header' % self.path)
        (entry,) = self._unpack('<I', optional + _OPTIONAL_ENTRY)
        (image_base,) = self._unpack('<Q', optional + _OPTIONAL_IMAGE_BASE)
        section_alignment, file_alignment = self._unpack('<II', optional + _OPTIONAL_ALIGNMENTS)
        image_size, headers_size, checksum = self._unpack('<III', optional + _OPTIONAL_SIZES)
        subsystem, dll_characteristics = self._unpack('<HH', optional + _OPTIONAL_SUBSYSTEM)
        (directory_count,) = self._unpack('<I', optional + _OPTIONAL_DIRECTORY_COUNT)
        if _OPTIONAL_DIRECTORIES + 8 * directory_count > optional_size:
            raise PeError('%s declares %d data directories beyond its optional header'
                          % (self.path, directory_count))
        directories = tuple(self._unpack('<II', optional + _OPTIONAL_DIRECTORIES + 8 * index)
                            for index in range(directory_count))
        self.header = Header(machine, section_count, optional_size, characteristics, magic, entry, image_base,
                             section_alignment, file_alignment, image_size, headers_size, checksum, subsystem,
                             dll_characteristics, directories)
        table = optional + optional_size
        self.sections = []
        for index in range(section_count):
            raw_name, virtual_size, rva, raw_size, raw_offset, _, _, _, _, section_characteristics = \
                self._unpack('<8sIIIIIIHHI', table + 40 * index)
            name = raw_name.rstrip(b'\0').decode('ascii', errors='replace')
            self.sections.append(Section(name, virtual_size, rva, raw_size, raw_offset, section_characteristics))

    def _unpack(self, form: str, offset: int):
        size = struct.calcsize(form)
        if offset < 0 or offset + size > len(self.data):
            raise PeError('%s is truncated at offset %#x' % (self.path, offset))
        return struct.unpack_from(form, self.data, offset)

    def _slice(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise PeError('%s is truncated at offset %#x' % (self.path, offset))
        return self.data[offset:offset + size]

    def section(self, name: str) -> Section | None:
        return next((section for section in self.sections if section.name == name), None)

    def section_containing(self, rva: int) -> Section | None:
        for section in self.sections:
            if section.rva <= rva < section.rva + max(section.virtual_size, section.raw_size):
                return section
        return None

    def read_rva(self, rva: int, size: int) -> bytes:
        """Bytes at an RVA inside the raw data of one section."""
        section = self.section_containing(rva)
        if section is None:
            raise PeError('%s: no section contains RVA %#x' % (self.path, rva))
        offset = rva - section.rva
        if offset + size > section.raw_size:
            raise PeError('%s: RVA %#x + %#x exceeds the raw data of %s' % (self.path, rva, size, section.name))
        return self._slice(section.raw_offset + offset, size)

    def base_relocation_data(self) -> bytes:
        """The bytes the base-relocation data directory names."""
        rva, size = self.header.directory(DIRECTORY_BASE_RELOCATION)
        if not size:
            return b''
        return self.read_rva(rva, size)


def relocation_blocks(data: bytes) -> list[RelocationBlock]:
    """Splits base-relocation data into blocks; raises PeError on malformed framing."""
    blocks = []
    cursor = 0
    while cursor < len(data):
        if cursor + 8 > len(data):
            raise PeError('trailing partial base-relocation block header at %#x' % cursor)
        page_rva, size = struct.unpack_from('<II', data, cursor)
        if size < 8 or size & 1 or cursor + size > len(data):
            raise PeError('base-relocation block at %#x has invalid size %#x' % (cursor, size))
        entries = struct.unpack_from('<%dH' % ((size - 8) // 2), data, cursor + 8)
        blocks.append(RelocationBlock(page_rva, size, tuple(entries)))
        cursor += size
    return blocks
