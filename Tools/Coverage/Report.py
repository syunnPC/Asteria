#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Report Clang source-based coverage of the hosted tests for Source/.

Usage: Report.py [--build DIR] [--prefix Source/Kernel] [--sort uncovered]
                 [--top N] [--directories] [--json FILE] [--html [DIR]]

Run the tests of the host-coverage preset first; every test process writes a
raw profile to <build>/Coverage/Profiles (see Tests/CMakeLists.txt). The report
merges those profiles with llvm-profdata, reads the coverage mapping of every
instrumented test executable in the build tree with llvm-cov, and prints line,
branch, region and function coverage per file below the prefix, followed by
the total. `--html` also writes llvm-cov's annotated sources, restricted to
the same files, under the build tree.

The LLVM tools default to llvm-profdata-21/llvm-cov-21 and can be selected
with --llvm-profdata/--llvm-cov or LLVM_PROFDATA/LLVM_COV. They run without
debuginfod, so no object is fetched from the network.

llvm-cov reports "functions have mismatched data" when several executables
are combined: an executable keeps placeholder records for inline functions it
never emits, while another executable instruments them, and test doubles or
`main` share names across executables. Coverage of such a function comes from
the executables that contain its code, so the report counts it correctly; the
warning is summarized instead of printed.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
METRICS = ('lines', 'branches', 'regions', 'functions')


def find_tool(explicit: str | None, variable: str, names: tuple[str,...]) -> str:
    for candidate in (explicit, os.environ.get(variable), *names):
        if candidate and shutil.which(candidate):
            return candidate
    raise SystemExit(f'error: none of {", ".join(names)} found; use --{names[0].rsplit("-", 1)[0]} or {variable}')


def elf_section_names(path: pathlib.Path) -> set[str] | None:
    """Returns the section names of a little-endian ELF64 executable, else None."""
    try:
        with path.open('rb') as file:
            header = file.read(64)
            if len(header) < 64 or header[:4] != b'\x7fELF' or header[4] != 2 or header[5] != 1:
                return None
            elf_type = struct.unpack_from('<H', header, 16)[0]
            if elf_type not in (2, 3):  # ET_EXEC, ET_DYN
                return None
            section_offset = struct.unpack_from('<Q', header, 0x28)[0]
            entry_size, count, names_index = struct.unpack_from('<HHH', header, 0x3A)
            if entry_size != 64 or count == 0 or names_index >= count:
                return None
            file.seek(section_offset)
            table = file.read(entry_size * count)
            offsets = [struct.unpack_from('<IIQQQQ', table, index * entry_size) for index in range(count)]
            _, _, _, _, names_offset, names_size = offsets[names_index]
            file.seek(names_offset)
            strings = file.read(names_size)
    except (OSError, struct.error):
        return None
    return {strings[entry[0]:strings.index(b'\0', entry[0])].decode() for entry in offsets}


def instrumented_executables(build: pathlib.Path) -> list[pathlib.Path]:
    executables = []
    for directory in (build / 'Tests', build / 'Tools'):
        for path in sorted(directory.rglob('*')):
            if not path.is_file() or not os.access(path, os.X_OK) or 'CMakeFiles' in path.parts:
                continue
            names = elf_section_names(path)
            if names is not None and '__llvm_covmap' in names:
                executables.append(path)
    return executables


def merge_profiles(profdata: str, profiles: list[pathlib.Path], output: pathlib.Path) -> None:
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as listing:
        listing.write('\n'.join(str(path) for path in profiles) + '\n')
    try:
        run_llvm([profdata, 'merge', '-sparse', '-f', listing.name, '-o', str(output)])
    finally:
        os.unlink(listing.name)


def object_arguments(executables: list[pathlib.Path]) -> list[str]:
    arguments = [str(executables[0])]
    for path in executables[1:]:
        arguments += ['-object', str(path)]
    return arguments


def run_llvm(arguments: list[str]) -> str:
    """Runs an LLVM tool without debuginfod and returns its standard output."""
    environment = {name: value for name, value in os.environ.items() if name != 'DEBUGINFOD_URLS'}
    result = subprocess.run(arguments, capture_output=True, text=True, env=environment)
    diagnostics = [line for line in result.stderr.splitlines() if line.strip()]
    mismatched = [line for line in diagnostics if 'functions have mismatched data' in line]
    for line in diagnostics:
        if line not in mismatched:
            print(line, file=sys.stderr)
    if mismatched:
        print(f'note: {mismatched[0].split("warning: ")[-1].strip()} (expected; see --help)', file=sys.stderr)
    if result.returncode != 0:
        raise SystemExit(f'error: {arguments[0]} {arguments[1]} failed with status {result.returncode}')
    return result.stdout


def export_summary(cov: str, profile: pathlib.Path, executables: list[pathlib.Path]) -> dict:
    return json.loads(run_llvm(
        [cov, 'export', '-format=text', '-summary-only', f'-instr-profile={profile}',
         '-ignore-filename-regex=^/usr/', *object_arguments(executables)]))


def empty_totals() -> dict:
    return {metric: {'count': 0, 'covered': 0} for metric in METRICS}


def accumulate(totals: dict, summary: dict) -> None:
    for metric in METRICS:
        totals[metric]['count'] += summary[metric]['count']
        totals[metric]['covered'] += summary[metric]['covered']


def cell(metric: dict) -> str:
    if metric['count'] == 0:
        return f'{"-":>18}'
    percent = 100.0 * metric['covered'] / metric['count']
    return f'{metric["covered"]:>6}/{metric["count"]:<6} {percent:5.1f}%'


def print_table(title: str, rows: list[tuple[str, dict]], totals: dict) -> None:
    width = max([len(title), len('Total')] + [len(name) for name, _ in rows])
    header = f'{title:<{width}}  ' + '  '.join(f'{metric.capitalize():>20}' for metric in METRICS)
    print(header)
    print('-' * len(header))
    for name, summary in rows:
        print(f'{name:<{width}}  ' + '  '.join(f'{cell(summary[metric]):>20}' for metric in METRICS))
    print('-' * len(header))
    print(f'{"Total":<{width}}  ' + '  '.join(f'{cell(totals[metric]):>20}' for metric in METRICS))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=pathlib.Path, default=ROOT / 'Build/CMake/host-coverage',
                        help='host-coverage build tree (default: %(default)s)')
    parser.add_argument('--prefix', default='Source/',
                        help='report files below this repository-relative path (default: %(default)s)')
    parser.add_argument('--sort', choices=('uncovered', 'name', 'lines', 'branches', 'regions'), default='name',
                        help='row order: most uncovered lines first, by name, or by ascending percentage')
    parser.add_argument('--top', type=int, help='print only the first N rows')
    parser.add_argument('--directories', action='store_true', help='aggregate rows per directory')
    parser.add_argument('--json', type=pathlib.Path, help='also write the per-file summary as JSON')
    parser.add_argument('--html', nargs='?', type=pathlib.Path, const=pathlib.Path(),
                        help='also write annotated sources (default directory: <build>/Coverage/Html)')
    parser.add_argument('--llvm-profdata', help='llvm-profdata executable')
    parser.add_argument('--llvm-cov', help='llvm-cov executable')
    args = parser.parse_args()

    profdata = find_tool(args.llvm_profdata, 'LLVM_PROFDATA', ('llvm-profdata-21', 'llvm-profdata'))
    cov = find_tool(args.llvm_cov, 'LLVM_COV', ('llvm-cov-21', 'llvm-cov'))
    build = args.build.resolve()
    profiles = sorted((build / 'Coverage/Profiles').rglob('*.profraw'))
    if not profiles:
        raise SystemExit(f'error: no raw profiles in {build}/Coverage/Profiles; '
                         'run `ctest --preset host-coverage` first')
    executables = instrumented_executables(build)
    if not executables:
        raise SystemExit(f'error: no instrumented executables in {build}')

    merged = build / 'Coverage/Merged.profdata'
    merge_profiles(profdata, profiles, merged)
    export = export_summary(cov, merged, executables)

    prefix = (ROOT / args.prefix).resolve()
    files = []
    for entry in export['data'][0]['files']:
        path = pathlib.Path(entry['filename'])
        if path.is_relative_to(prefix):
            files.append((path.relative_to(ROOT).as_posix(), entry['summary']))
    if not files:
        raise SystemExit(f'error: no coverage data for files below {args.prefix}')

    rows = files
    if args.directories:
        groups: dict[str, dict] = {}
        for name, summary in files:
            accumulate(groups.setdefault(name.rsplit('/', 1)[0], empty_totals()), summary)
        rows = sorted(groups.items())
    totals = empty_totals()
    for _, summary in files:
        accumulate(totals, summary)

    def ratio(summary: dict, metric: str) -> float:
        return summary[metric]['covered'] / summary[metric]['count'] if summary[metric]['count'] else 1.0

    if args.sort == 'uncovered':
        rows = sorted(rows, key=lambda row: (row[1]['lines']['covered'] - row[1]['lines']['count'], row[0]))
    elif args.sort != 'name':
        rows = sorted(rows, key=lambda row: (ratio(row[1], args.sort), row[0]))
    else:
        rows = sorted(rows)
    if args.top is not None:
        rows = rows[:args.top]

    print(f'{len(profiles)} profiles, {len(executables)} instrumented executables, '
          f'{len(files)} files below {args.prefix}')
    print_table('Directory' if args.directories else 'File', rows, totals)

    if args.json:
        args.json.write_text(json.dumps({'prefix': args.prefix, 'files': dict(files), 'totals': totals}, indent=1))
    if args.html is not None:
        output = args.html if args.html != pathlib.Path() else build / 'Coverage/Html'
        run_llvm(
            [cov, 'show', '-format=html', f'-output-dir={output}', f'-instr-profile={merged}',
             '-show-branches=count', '-show-line-counts-or-regions', *object_arguments(executables),
             *(str(ROOT / name) for name, _ in files)])
        print(f'HTML report: {output / "index.html"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
