#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure how many source mutants the hosted tests detect.

Usage: Mutate.py --source Source/<file>.cpp --test <CTest name> [--test...]
                 [--build-dir Build/CMake/host-debug] [--work-dir DIR] [--jobs 2]
                 [--operators relational,equality,...] [--lines A-B] [--max-mutants N]
                 [--json FILE] [--list]

The tool generates mutants of one production translation unit (see
AsteriaMutation/Generator.py for the operators), compiles each mutated copy
with the unit's exact command from the build tree's compile database, relinks
every listed test with that object in front of the library that holds the
original, and runs the tests with a timeout scaled from an unmutated baseline.
A mutant is killed when a test fails, survives when all pass, times out, or
is a compile error (excluded from the kill rate).

The build tree must be configured and must have built the listed tests.
Outputs go to --work-dir (default Build/Mutation/<source stem>); Source/ and
the build tree are never modified.

Example:
  Tools/Mutation/Mutate.py --source Source/Kernel/Ipc/MessageCodec.cpp \\
      --test Kernel/Ipc/MessageCodecTests --test Fuzz/MessageCodecFuzz \\
      --json Build/Mutation/MessageCodec.json
"""
from __future__ import annotations

import argparse
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from AsteriaMutation import Generator, Report  # Noqa: E402
from AsteriaMutation.Build import BuildTree, BuildTreeError  # Noqa: E402
from AsteriaMutation.Runner import MutationRun  # Noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]


def _line_range(value: str) -> tuple[int, int]:
    first, _, last = value.partition('-')
    try:
        return int(first), int(last or first)
    except ValueError:
        raise argparse.ArgumentTypeError('expected A-B, got %r' % value) from None


def _select(mutants: list[Generator.Mutant], lines: tuple[int, int] | None,
            maximum: int | None) -> list[Generator.Mutant]:
    if lines is not None:
        mutants = [mutant for mutant in mutants if lines[0] <= mutant.line <= lines[1]]
    if maximum is not None and 0 < maximum < len(mutants):
        # An evenly spaced, deterministic sample over the whole file.
        mutants = [mutants[(index * len(mutants)) // maximum] for index in range(maximum)]
    return mutants


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', type=pathlib.Path, required=True, help='production source file under Source/')
    parser.add_argument('--test', action='append', default=[], help='CTest test name (repeatable)')
    parser.add_argument('--build-dir', type=pathlib.Path, default=ROOT / 'Build/CMake/host-debug')
    parser.add_argument('--work-dir', type=pathlib.Path)
    parser.add_argument('--jobs', type=int, default=2, help='mutants processed concurrently (default 2)')
    parser.add_argument('--operators', default=','.join(Generator.OPERATOR_GROUPS),
                        help='comma-separated operator groups: %s' % ', '.join(Generator.OPERATOR_GROUPS))
    parser.add_argument('--lines', type=_line_range, help='only mutants on lines A-B')
    parser.add_argument('--max-mutants', type=int, help='evenly spaced sample of at most N mutants')
    parser.add_argument('--list', action='store_true', help='list the mutants and exit')
    parser.add_argument('--json', type=pathlib.Path, help='also write the JSON report here')
    parser.add_argument('--compile-timeout', type=float, default=600.0)
    parser.add_argument('--timeout-factor', type=float, default=10.0,
                        help='test timeout = baseline seconds * factor + slack')
    parser.add_argument('--timeout-slack', type=float, default=5.0)
    parser.add_argument('--keep-artifacts', action='store_true', help='keep every mutant directory')
    parser.add_argument('--keep-werror', action='store_true',
                        help='compile mutants with -Werror intact (default: append -Wno-error)')
    parser.add_argument('--quiet', action='store_true', help='no progress lines on stderr')
    args = parser.parse_args()

    source = (args.source if args.source.is_absolute() else pathlib.Path.cwd() / args.source).resolve()
    source_root = (ROOT / 'Source').resolve()
    if not source.is_file() or source_root not in source.parents:
        parser.error('%s is not a file under %s' % (args.source, source_root))
    relative = source.relative_to(ROOT).as_posix()
    text = source.read_text()
    groups = tuple(group for group in args.operators.split(',') if group)
    try:
        mutants = _select(Generator.generate(text, groups), args.lines, args.max_mutants)
    except ValueError as error:
        parser.error(str(error))
    if args.list:
        print(Report.format_listing(mutants, text))
        print('%d mutants' % len(mutants))
        return 0
    if not args.test:
        parser.error('at least one --test is required')

    work = (args.work_dir or ROOT / 'Build/Mutation' / source.stem).resolve()
    if work == source_root or source_root in work.parents:
        parser.error('the work directory must not be inside Source/')
    started = time.monotonic()
    try:
        run = MutationRun(BuildTree(args.build_dir), source, args.test, work,
                          compile_timeout=args.compile_timeout, timeout_factor=args.timeout_factor,
                          timeout_slack=args.timeout_slack, keep_artifacts=args.keep_artifacts,
                          keep_werror=args.keep_werror)
        baseline = run.baseline()
    except BuildTreeError as error:
        print('FAIL: %s' % error, file=sys.stderr)
        return 2
    if not args.quiet:
        print('baseline passed (%s); %d mutants, %d jobs' % (
            ', '.join('%s %.2f s' % item for item in baseline.items()), len(mutants), args.jobs), file=sys.stderr)

    def progress(outcome, done, total):
        if not args.quiet:
            print('[%d/%d] %s %s:%d %s %s' % (done, total, outcome.mutant.identifier, relative, outcome.mutant.line,
                                              outcome.mutant.description, outcome.status),
                  file=sys.stderr, flush=True)

    outcomes = run.run(mutants, args.jobs, progress)
    seconds = time.monotonic() - started
    print(Report.format_text(relative, args.test, outcomes, text, seconds), end='')
    if args.json is not None:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(Report.to_json(relative, args.test, str(args.build_dir), outcomes, text, seconds))
    return 0


if __name__ == '__main__':
    sys.exit(main())
