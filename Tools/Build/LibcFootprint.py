#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure unlinked application libc at -Os using a configured AMD64 target's exact flags."""
import argparse
import json
import pathlib
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=pathlib.Path, required=True)
parser.add_argument('--size', required=True)
parser.add_argument('--nm', required=True)
args = parser.parse_args()
commands = json.loads((args.build / 'compile_commands.json').read_text())
units = [entry for entry in commands if 'AsteriaCompileCheckLibcO2.dir' in entry['command']]
if len(units) != 10:
    raise SystemExit(f'expected ten application libc units, found {len(units)}')
code = readonly = data = bss = 0
defined = set()
undefined = set()
public = set()
with tempfile.TemporaryDirectory(prefix='asteria-libc-footprint-') as temporary:
    for entry in sorted(units, key=lambda entry: entry['file']):
        command = shlex.split(entry['command'])
        output = pathlib.Path(temporary) / (pathlib.Path(entry['file']).stem + '.o')
        index = command.index('-o')
        command[index + 1] = str(output)
        command[command.index('-O2')] = '-Os'
        # No build-tree dependency files are touched by this scratch compile.
        for flag in ['-MD', '-MMD']:
            if flag in command:
                command.remove(flag)
        for flag in ['-MF', '-MT']:
            if flag in command:
                index = command.index(flag)
                del command[index:index + 2]
        subprocess.run(command, cwd=entry['directory'], check=True)
        symbols = subprocess.check_output([args.nm, '-g', '-P', str(output)], text=True)
        for line in symbols.splitlines():
            fields = line.split()
            if len(fields) >= 2:
                if fields[1] == 'U':
                    undefined.add(fields[0])
                else:
                    defined.add(fields[0])
                    if fields[1] == 'T' and not fields[0].startswith('_Z'):
                        public.add(fields[0])
        listing = subprocess.check_output([args.size, '-A', str(output)], text=True)
        values = {}
        for line in listing.splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[0].startswith('.'):
                values[fields[0]] = int(fields[1])
        text = sum(size for name, size in values.items() if name.startswith('.text'))
        ro = sum(size for name, size in values.items() if name.startswith('.rodata'))
        writable = sum(size for name, size in values.items() if name.startswith('.data'))
        zero = sum(size for name, size in values.items() if name.startswith('.bss'))
        print(f'{output.stem:12s} .text={text:6d} .rodata={ro:5d} .data={writable:5d} .bss={zero:5d}')
        code += text
        readonly += ro
        data += writable
        bss += zero
print(f'TOTAL .text={code} .rodata={readonly} .data={data} .bss={bss}')
if any(name.startswith('asteria_libc_') for name in public):
    raise SystemExit('target symbol isolation is incorrect')
print(f'Public C exports: {len(public)}; external dependencies: {sorted(undefined - defined)}')
