#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Canonicalize Asteria's retained RELA order; never change loadable bytes.

The loader rejects overlapping effective fixups and multiple RELA tables for
one target. Sorting by virtual site makes that check linear. Input comes only
from our closed static linker; the runtime validates the result independently.
"""
from __future__ import annotations

import pathlib
import struct
import sys


def main() -> int:
    path = pathlib.Path(sys.argv[1])
    data = bytearray(path.read_bytes())
    section_offset, = struct.unpack_from('<Q', data, 40)
    section_count, = struct.unpack_from('<H', data, 60)
    for index in range(section_count):
        offset = section_offset + index * 64
        kind, = struct.unpack_from('<I', data, offset + 4)
        if kind == 4:
            file_offset, size = struct.unpack_from('<QQ', data, offset + 24)
            stride, = struct.unpack_from('<Q', data, offset + 56)
            if stride != 24 or size % stride or file_offset + size > len(data):
                raise ValueError('invalid retained RELA table')
            rows = [data[begin:begin + stride] for begin in range(file_offset, file_offset + size, stride)]
            rows.sort(key=lambda row: struct.unpack_from('<Q', row)[0])
            data[file_offset:file_offset + size] = b''.join(rows)
    path.write_bytes(data)
    return 0


if __name__ == '__main__':
    sys.exit(main())
