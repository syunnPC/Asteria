#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the public C23 ABI with both cross compilers, without enabling C in CMake."""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

include = pathlib.Path(sys.argv[1]).resolve()
headers = sorted(path.name for path in include.glob('*.h'))
compilers = [
    os.environ.get('AMD64_CC', 'x86_64-elf-gcc'),
    os.environ.get('IA64_CC', 'ia64-linux-gnu-gcc'),
]
# Missing tools are a failure: both ISAs are part of this ABI contract.
for name, compiler in zip(('AMD64_CC', 'IA64_CC'), compilers):
    if not shutil.which(compiler):
        raise RuntimeError(f'required C23 cross compiler unavailable: {compiler}; set {name} or add it to PATH')
with tempfile.TemporaryDirectory(prefix='asteria-status-headers-') as temporary:
    unit = pathlib.Path(temporary) / 'Headers.c'
    for compiler in compilers:
        for selection in [[header] for header in headers] + [headers]:
            unit.write_text(''.join(f'#include "{header}"\n' for header in selection)
                            + 'static_assert(sizeof(AsStatus) == 4U);\n'
                            + ''.join(f'static_assert(sizeof(enum {header[:-2]}) == 4U);\n'
                                      for header in selection if header != 'AsStatus.h'))
            subprocess.run([compiler, '-x', 'c', '-std=c23', '-ffreestanding',
                            '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsyntax-only',
                            '-I', str(include), str(unit)], check=True)
        unit.write_text('#include "AsFileStatus.h"\n'
                        'static_assert(AsFileNotFound == 0x85000001U);\n'
                        'static_assert(sizeof(enum AsFileStatus) == sizeof(AsStatus));\n'
                        'AsStatus Test(void) { return AsFileNotFound; }\n')
        subprocess.run([compiler, '-x', 'c', '-std=c23', '-ffreestanding',
                        '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsyntax-only',
                        '-I', str(include), str(unit)], check=True)
print(f'PASS: {2 * (len(headers) + 2)} cross-compiler C23 checks')
