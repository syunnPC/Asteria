# SPDX-License-Identifier: Apache-2.0
"""Reject PIE dynamic relocations into executable segments.

IA-64 ld's -z text also rejects its read-only function descriptors, even
in a writable gate PT_LOAD. Inspect the dynamic relocations of the second
link instead; the fixed-address production ELF is never changed.
"""
import pathlib
import struct
import sys


def check(data: bytes) -> None:
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected little-endian ELF64')
    phoff, shoff = struct.unpack_from('<QQ', data, 32)
    phsize, phcount, shsize, shcount = struct.unpack_from('<HHHH', data, 54)
    executable = []
    for index in range(phcount):
        kind, flags, _, address, _, _, size, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + index * phsize)
        if kind == 1 and flags & 3 == 3:
            raise ValueError('writable executable segment')
        if kind == 1 and flags & 1:
            executable.append((address, address + size))
    if not executable:
        raise ValueError('no executable segment')
    dynamic = False
    for index in range(shcount):
        _, kind, flags, _, offset, size, _, _, _, stride = struct.unpack_from(
            '<IIQQQQIIQQ', data, shoff + index * shsize)
        dynamic = dynamic or kind == 6  # SHT_DYNAMIC proves this was a dynamic second link.
        # Only allocated RELA/REL tables describe load-time relocations.
        if kind not in (4, 9) or not flags & 2:
            continue
        if stride != (24 if kind == 4 else 16) or size % stride:
            raise ValueError('malformed relocation table')
        for entry in range(offset, offset + size, stride):
            target, = struct.unpack_from('<Q', data, entry)
            if any(start <= target < limit for start, limit in executable):
                raise ValueError('dynamic relocation into executable text at %#x' % target)

    if not dynamic:
        raise ValueError('no SHT_DYNAMIC section; PIE link required')


if __name__ == '__main__':
    for argument in sys.argv[1:]:
        try:
            check(pathlib.Path(argument).read_bytes())
        except (ValueError, struct.error) as error:
            sys.exit('%s: %s' % (argument, error))
