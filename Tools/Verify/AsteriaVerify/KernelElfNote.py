# SPDX-License-Identifier: Apache-2.0
"""Asteria's independently checked ELF note and PT_LOAD ownership geometry."""
from __future__ import annotations

import struct

ROLES = ('.text', '.note.asteria', 'ReadOnlyBody', 'ReadOnlyTail', '.data', '.bss')


def ownership(elf):
    note = elf.section('.note.asteria')
    if note is None or note.type != 7:
        raise ValueError('missing typed kernel ABI note')
    blob = elf.data[note.offset:note.offset + note.size]
    if len(blob) < 380 or struct.unpack_from('<III', blob) != (8, 360, 0x41535401) or blob[12:20] != b'ASTERIA\0':
        raise ValueError('invalid kernel ABI note header')
    version, boot_version, arch, count = struct.unpack_from('<IIII', blob, 20)
    base, size, alignment = struct.unpack_from('<QQQ', blob, 36)
    if version != 1 or boot_version != 22 or arch != 2 or count != 6 or alignment != 0x1000000:
        raise ValueError('unsupported IA-64 kernel ABI note')
    records = [struct.unpack_from('<QQII', blob, 116 + index * 24) for index in range(count)]
    if any(reserved or purpose != (0 if index == 0 else 1 if index < 4 else 2)
           for index, (_, _, purpose, reserved) in enumerate(records)):
        raise ValueError('incorrect kernel ownership purposes')
    ranges = [(name, base + rva, pages * 4096) for name, (rva, pages, _, _) in zip(ROLES, records)]
    groups = [(ranges[0][1], ranges[0][2], 5),
              (ranges[1][1], sum(item[2] for item in ranges[1:4]), 4),
              (ranges[4][1], sum(item[2] for item in ranges[4:6]), 6)]
    loads = [(segment.virtual_address, segment.memory_size, segment.flags)
             for segment in elf.segments if segment.type == 1]
    if loads != groups or any(address != base + (index + 1) * 0x400000 or extent != 0x400000
                              for index, (address, extent, _) in enumerate(groups)) or size != 0x1000000:
        raise ValueError('kernel note differs from the three PT_LOAD permission groups')
    return tuple(ranges)
