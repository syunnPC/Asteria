#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bind a kernel-owned link script to compiler-generated reservation values."""
from __future__ import annotations

import pathlib
import re
import sys


def main() -> int:
    template, layout, output = map(pathlib.Path, sys.argv[1:])
    constants = dict(re.findall(r'^#define (ASTERIA_[A-Z0-9_]+) (-?\d+)$', layout.read_text(), re.MULTILINE))
    text = re.sub(r'@(ASTERIA_[A-Z0-9_]+)@', lambda match: constants[match[1]], template.read_text())
    output.write_text(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
