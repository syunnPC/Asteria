# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the PE32+ reader (synthetic images, no build tree).

Run with: python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify
"""
from __future__ import annotations

import struct
import unittest

from AsteriaVerify import Pe

PE_OFFSET = 0x80
FILE_ALIGNMENT = 0x200


def encode_relocation_blocks(blocks) -> bytes:
    """The inverse of Pe.relocation_blocks."""
    return b''.join(struct.pack('<II%dH' % len(block.entries), block.page_rva, block.size, *block.entries)
                    for block in blocks)


def image(*, machine=Pe.MACHINE_AMD64, magic=Pe.MAGIC_PE32_PLUS, subsystem=Pe.SUBSYSTEM_EFI_APPLICATION,
          sections=(('.text', 0x1000, b'\x90' * 0x20, Pe.SCN_CNT_CODE | Pe.SCN_MEM_READ | Pe.SCN_MEM_EXECUTE),
                    ('.reloc', 0x2000, b'', Pe.SCN_CNT_INITIALIZED_DATA | Pe.SCN_MEM_READ)),
          relocations=b'', entry=0x1000, image_base=0x4000000) -> bytes:
    """Builds a small PE32+ image; `.reloc` receives `relocations`."""
    sections = [(name, rva, relocations if name == '.reloc' else data, flags) for name, rva, data, flags in sections]
    optional_size = 112 + 16 * 8
    table = PE_OFFSET + 24 + optional_size
    raw = -(-(table + 40 * len(sections)) // FILE_ALIGNMENT) * FILE_ALIGNMENT
    layout = []
    for name, rva, data, flags in sections:
        size = -(-len(data) // FILE_ALIGNMENT) * FILE_ALIGNMENT
        layout.append((name, rva, data, flags, raw if size else 0, size))
        raw += size
    blob = bytearray(raw)
    blob[0:2] = b'MZ'
    struct.pack_into('<I', blob, 0x3C, PE_OFFSET)
    blob[PE_OFFSET:PE_OFFSET + 4] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', blob, PE_OFFSET + 4, machine, len(sections), 0, 0, 0, optional_size, 0x22E)
    optional = PE_OFFSET + 24
    struct.pack_into('<H', blob, optional, magic)
    struct.pack_into('<I', blob, optional + 16, entry)
    struct.pack_into('<Q', blob, optional + 24, image_base)
    struct.pack_into('<II', blob, optional + 32, 0x1000, FILE_ALIGNMENT)
    struct.pack_into('<III', blob, optional + 56, 0x3000, 0x400, 0)
    struct.pack_into('<HH', blob, optional + 68, subsystem, 0x160)
    struct.pack_into('<I', blob, optional + 108, 16)
    reloc = next((rva for name, rva, *_ in layout if name == '.reloc'), 0)
    struct.pack_into('<II', blob, optional + 112 + 5 * 8, reloc, len(relocations))
    for index, (name, rva, data, flags, offset, size) in enumerate(layout):
        struct.pack_into('<8sIIIIIIHHI', blob, table + 40 * index, name.encode(), max(len(data), 1), rva,
                         size, offset, 0, 0, 0, 0, flags)
        blob[offset:offset + len(data)] = data
    return bytes(blob)


BLOCKS = (Pe.RelocationBlock(0x1000, 12, ((Pe.REL_BASED_DIR64 << 12) | 0x8, 0)),
          Pe.RelocationBlock(0x2000, 12, ((Pe.REL_BASED_DIR64 << 12) | 0x10, (Pe.REL_BASED_DIR64 << 12) | 0x18)))


class HeaderTests(unittest.TestCase):
    def test_header_and_sections(self):
        pe = Pe.PeFile(image(relocations=encode_relocation_blocks(BLOCKS)))
        header = pe.header
        self.assertEqual((header.machine, header.magic, header.subsystem, header.entry_rva, header.image_base),
                         (Pe.MACHINE_AMD64, Pe.MAGIC_PE32_PLUS, 10, 0x1000, 0x4000000))
        self.assertEqual((header.section_alignment, header.file_alignment, header.optional_header_size),
                         (0x1000, 0x200, 240))
        self.assertEqual([s.name for s in pe.sections], ['.text', '.reloc'])
        text = pe.section('.text')
        self.assertEqual(text.characteristics & (Pe.SCN_MEM_EXECUTE | Pe.SCN_MEM_READ | Pe.SCN_MEM_WRITE),
                         Pe.SCN_MEM_EXECUTE | Pe.SCN_MEM_READ)
        self.assertEqual(pe.read_rva(0x1004, 4), b'\x90' * 4)
        self.assertIs(pe.section_containing(0x1010), text)
        self.assertEqual(header.directory(Pe.DIRECTORY_BASE_RELOCATION), (0x2000, 24))
        self.assertEqual(header.directory(40), (0, 0))

    def test_other_magic_keeps_the_header_readable(self):
        pe = Pe.PeFile(image(magic=Pe.MAGIC_PE32))
        self.assertEqual((pe.header.magic, pe.sections), (Pe.MAGIC_PE32, []))

    def test_malformed_images(self):
        good = image()
        for broken in (b'ZM' + good[2:], good[:PE_OFFSET] + b'NE\0\0' + good[PE_OFFSET + 4:], good[:PE_OFFSET + 30]):
            with self.assertRaises(Pe.PeError):
                Pe.PeFile(broken)
        pe = Pe.PeFile(good)
        with self.assertRaises(Pe.PeError):
            pe.read_rva(0x9000, 4)
        with self.assertRaises(Pe.PeError):
            pe.read_rva(0x1000, 0x400)


class RelocationTests(unittest.TestCase):
    def test_blocks_round_trip(self):
        data = encode_relocation_blocks(BLOCKS)
        pe = Pe.PeFile(image(relocations=data))
        self.assertEqual(pe.base_relocation_data(), data)
        self.assertEqual(tuple(Pe.relocation_blocks(data)), BLOCKS)
        self.assertEqual(Pe.relocation_blocks(b''), [])

    def test_malformed_framing(self):
        data = encode_relocation_blocks(BLOCKS)
        for broken in (data + b'\0\0\0\0',                            # trailing partial header
                       data[:4] + struct.pack('<I', 6) + data[8:],     # block smaller than its header
                       data[:4] + struct.pack('<I', 13) + data[8:],    # odd size
                       data[:4] + struct.pack('<I', 0x100) + data[8:]):    # beyond the data
            with self.assertRaises(Pe.PeError):
                Pe.relocation_blocks(broken)


if __name__ == '__main__':
    unittest.main()
