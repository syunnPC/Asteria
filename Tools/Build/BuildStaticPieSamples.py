#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build real freestanding static PIEs with the project's native toolchains.

All compiler outputs and temporary files remain beneath --output (under Build).
The IA-64 entry script adapts GNU ld's code entry to Asteria's descriptor ABI;
no ELF bytes or relocation tables are synthesized or patched by this script.
"""
import argparse
import os
import pathlib
import shutil
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--amd64-cxx', default=os.environ.get(
        'AMD64_CXX', 'x86_64-elf-g++'))
    parser.add_argument('--ia64-cxx', default=os.environ.get(
        'IA64_CXX', 'ia64-linux-gnu-g++'))
    args = parser.parse_args()
    for name, compiler in (('AMD64_CXX', args.amd64_cxx), ('IA64_CXX', args.ia64_cxx)):
        if not shutil.which(compiler):
            parser.error(f'required cross compiler unavailable: {compiler}; set {name} or add it to PATH')
    root = pathlib.Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    if not output.is_relative_to(root / 'Build'):
        parser.error('--output must be under this worktree\'s Build/')
    output.mkdir(parents=True, exist_ok=True)
    temporary = root / 'Build' / 'Tmp'
    temporary.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, TMPDIR=str(temporary))
    source = root / 'Tests/User/Loader/Samples/StaticPie.cpp'
    for isa, compiler, pic in [('amd64', args.amd64_cxx, '-fpie'),
                               ('ia64', args.ia64_cxx, '-fpic')]:
        flags = ['-std=c++23', '-O2', '-ffreestanding', '-fno-exceptions',
                 '-fno-rtti', '-fno-stack-protector', pic, '-nostdlib',
                 '-Wl,-pie,--no-dynamic-linker,-z,text,-z,now,--build-id=none']
        if isa == 'ia64':
            flags += ['-Wl,-e,EntryDescriptor',
                      '-Wl,-T,' + str(root / 'Tests/User/Loader/Samples/Ia64Entry.lds')]
        else:
            flags += ['-Wl,-z,relro,-e,Entry']
        subprocess.run([compiler, *flags, str(source), '-o', str(output / (isa + '.elf'))],
                       check=True, env=environment)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
