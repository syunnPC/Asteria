#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the verification checks of one target build tree.

Usage: VerifyTarget.py --build <tree> [--check NAME]... [--list]
                       [--update-golden] [--require-variants firmware,O0,O2]

Each check reports its problems; the run fails if any check reports one.
Properties also run their mutation self-tests, and the run fails if any
mutant is accepted.

`--update-golden` rewrites the golden disassembly files from the current
build instead of comparing them, for review as an ordinary data diff. To
record a new golden, create the file with its header lines (see
AsteriaVerify/Golden.py) and no instruction lines, then run with
`--update-golden --check GoldenDisassembly`.

`--golden PATH` (repeatable) restricts the golden check, and an update, to
the named files.

`--require-variants` fails the run when the tree lacks one of the listed
variants, so a check that needs them cannot be skipped silently.
"""
from __future__ import annotations

import argparse
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from AsteriaVerify.Context import TargetContext  # Noqa: E402
from AsteriaVerify import Checks  # Noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=pathlib.Path, required=True)
    parser.add_argument('--check', action='append', default=[])
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--update-golden', action='store_true')
    parser.add_argument('--golden', action='append', default=[],
                        help='restrict GoldenDisassembly to these golden files (repository-relative paths)')
    parser.add_argument('--require-variants', default='')
    parser.add_argument('--verbose', action='store_true',
                        help='print timing, self-test counts and the notes of passing properties')
    args = parser.parse_args()

    context = TargetContext.load(args.build)
    required = [variant for variant in args.require_variants.split(',') if variant]
    missing = [variant for variant in required if not context.has_variant(variant)]
    if missing:
        print('FAIL: %s lacks the variants %s' % (args.build, ', '.join(missing)), file=sys.stderr)
        return 2

    checks = [check for check in Checks.registry() if check.applies_to(context)]
    if args.list:
        for check in checks:
            print(check.name)
        return 0
    if args.check:
        unknown = set(args.check) - {check.name for check in checks}
        if unknown:
            print('FAIL: unknown checks: %s' % ', '.join(sorted(unknown)), file=sys.stderr)
            return 2
        checks = [check for check in checks if check.name in args.check]

    failures = 0
    mutants = 0
    for check in checks:
        started = time.monotonic()
        try:
            outcome = check.run(context, update_golden=args.update_golden, goldens=args.golden)
        except Exception as error:  # A crashing check is a failing check
            outcome = Checks.Outcome(['%s: %s' % (type(error).__name__, error)])
        elapsed = time.monotonic() - started
        mutants += outcome.mutants_rejected
        suffix = ''
        if args.verbose:
            suffix = ' (%.2fs, %d mutants rejected)' % (elapsed, outcome.mutants_rejected)
        if args.verbose:
            for note in outcome.notes:
                print('  %s %s' % (check.name, note))
        if outcome.skipped:
            print('SKIP %s: %s' % (check.name, outcome.skipped))
        elif outcome.problems:
            failures += 1
            print('FAIL %s%s' % (check.name, suffix), file=sys.stderr)
            for problem in outcome.problems:
                print('  ' + problem.replace('\n', '\n  '), file=sys.stderr)
        else:
            print('PASS %s%s' % (check.name, suffix))
    print('%d of %d checks passed for %s (%s); %d mutants rejected'
          % (len(checks) - failures, len(checks), context.isa, ', '.join(context.variants), mutants))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
