#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check GCC -fstack-usage reports against a per-function byte budget.

The arguments are the object files whose reports are checked (a CMake
`$<TARGET_OBJECTS:...>` list may arrive as one semicolon-separated argument).
GCC writes the report of `X.cpp.o` (or `X.cpp.obj`) to `X.cpp.su`; objects
assembled from `.S` files have none. A missing report fails, so an unprobed
unit cannot pass silently, and reports of units no longer built are ignored.

Each report line has the form `location:function<TAB>bytes<TAB>kind`. Every
function must use static or dynamic-but-bounded stack, and no function may
exceed the budget.
"""
import argparse
import pathlib
import sys

ASSEMBLY_SUFFIXES = ('.S', '.s')


def report_of(object_path: pathlib.Path) -> pathlib.Path | None:
    source = object_path.with_suffix('')
    if source.suffix in ASSEMBLY_SUFFIXES:
        return None
    return source.with_name(source.name + '.su')


def check_report(path: pathlib.Path, limit: int, allow_bounded: bool) -> list[str]:
    errors = []
    allowed = {'static'} | ({'dynamic,bounded'} if allow_bounded else set())
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split('\t')
        if len(fields) != 3 or not fields[1].isdigit():
            errors.append('%s:%d: unparseable stack-usage line: %r' % (path, number, line))
            continue
        location, size, kind = fields
        if kind not in allowed:
            errors.append('%s: %s has %s stack usage' % (path, location, kind))
        if int(size) > limit:
            errors.append('%s: %s uses %s bytes; the budget is %d' % (path, location, size, limit))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--limit', type=int, required=True, help='maximum bytes per function')
    parser.add_argument('--static-only', action='store_true',
                        help='reject dynamic,bounded frames as well as unbounded ones')
    parser.add_argument('objects', nargs='+', help='object files (semicolon-separated lists are split)')
    args = parser.parse_args()

    objects = sorted({pathlib.Path(item) for argument in args.objects for item in argument.split(';') if item})
    reports = [report for report in map(report_of, objects) if report is not None]
    errors = ['missing stack-usage report %s' % report for report in reports if not report.is_file()]
    for report in reports:
        if report.is_file():
            errors.extend(check_report(report, args.limit, not args.static_only))
    for error in errors:
        print('FAIL:', error, file=sys.stderr)
    if errors:
        return 1
    print('PASS: %d stack-usage reports within %d bytes per function' % (len(reports), args.limit))
    return 0


if __name__ == '__main__':
    sys.exit(main())
