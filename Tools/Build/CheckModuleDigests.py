#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Checks WriteModuleDigests: the FIPS 180-4 vector for "abc" and, for a
module larger than the tool's read buffer, Python's hashlib over the same
bytes, in the tool's line format."""
from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys
import tempfile

ABC = 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'


def main() -> int:
    tool = sys.argv[1]
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        small = root / 'ABC.ELF'
        small.write_bytes(b'abc')
        large = root / 'COMP.ELF'
        payload = bytes((index * 7 + (index >> 9)) & 255 for index in range(3 * 65536 + 17))
        large.write_bytes(payload)
        output = root / 'ModuleDigests.txt'
        subprocess.run([tool, str(output), 'abc', 'ABC.ELF', str(small), 'component', 'COMP.ELF', str(large)],
                       check=True)
        expected = 'abc ABC.ELF sha256=%s\ncomponent COMP.ELF sha256=%s\n' % (
            ABC, hashlib.sha256(payload).hexdigest())
        actual = output.read_text()
        if actual != expected:
            print('WriteModuleDigests wrote %r, expected %r' % (actual, expected), file=sys.stderr)
            return 1
        refused = subprocess.run([tool, str(output), 'missing', 'NONE.ELF', str(root / 'NONE.ELF')])
        if refused.returncode == 0:
            print('WriteModuleDigests accepted a missing module', file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
