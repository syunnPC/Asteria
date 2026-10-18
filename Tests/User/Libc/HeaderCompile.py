#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the actual public surface as C17 and C++23, one header at a time and together."""
import pathlib
import shutil
import subprocess
import sys
import tempfile

include = pathlib.Path(sys.argv[1]).resolve()
cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
if cc is None:
    print('SKIP: no host C compiler')
    sys.exit(0)
cxx = sys.argv[2]
headers = ['string.h', 'strings.h', 'ctype.h', 'stdlib.h', 'stdio.h', 'errno.h', 'inttypes.h', 'assert.h']
with tempfile.TemporaryDirectory(prefix='asteria-libc-headers-') as temporary:
    for compiler, language, standard in [(cc, 'c', 'c17'), (cxx, 'c++', 'c++23')]:
        for selection in [[header] for header in headers] + [headers]:
            unit = pathlib.Path(temporary) / ('Headers.c' if language == 'c' else 'Headers.cpp')
            unit.write_text(''.join(f'#include <{header}>\n' for header in selection))
            subprocess.run([compiler, '-x', language, f'-std={standard}', '-ffreestanding',
                            '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                            '-DASTERIA_TARGET_AMD64=1', '-I', str(include), str(unit)], check=True)
print('PASS: 18 public header compilations (C17 and C++23)')
