#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Enforce the boundary between production sources and verification code.

Production code under Source/ must not know about tests: it must not include
test headers, grant friendship to test types, or branch on test-only
configuration macros. Violations are reported with their locations.
"""
import argparse
import pathlib
import re
import sys

FORBIDDEN_MACROS = ('ASTERIA_HOST_TEST', 'ASTERIA_UI_TEXT_TRACE',
                    'ASTERIA_SESSION_LIVENESS_FAULT', 'ASTERIA_SESSION_SEAT')

RULES = [
    ('includes a Tests/ header', re.compile(r'#\s*include\s*"(?:Support|Tests)/')),
    ('grants friendship to a test type', re.compile(r'\bfriend\b[^;]*Test')),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=pathlib.Path, required=True, help='repository root')
    parser.add_argument('--forbid-macro', action='append', default=[],
                        help='a preprocessor symbol that Source/ must not reference')
    args = parser.parse_args()

    rules = list(RULES)
    for macro in dict.fromkeys((*FORBIDDEN_MACROS, *args.forbid_macro)):
        rules.append(('references test-only macro %s' % macro, re.compile(r'\b%s\b' % re.escape(macro))))

    violations = []
    source = args.root / 'Source'
    files = sorted(p for p in source.rglob('*') if p.suffix in {'.cpp', '.hpp', '.h', '.S', '.inc', '.lds'})
    for path in files:
        for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
            for description, pattern in rules:
                if pattern.search(line):
                    violations.append('%s:%d: %s' % (path.relative_to(args.root), number, description))
    for violation in violations:
        print('FAIL:', violation, file=sys.stderr)
    if violations:
        print('FAIL: %d separation violations in Source/' % len(violations), file=sys.stderr)
        return 1
    print('PASS: %d Source/ files contain no test dependencies' % len(files))
    return 0


if __name__ == '__main__':
    sys.exit(main())
