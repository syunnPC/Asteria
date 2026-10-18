#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""List the weak definitions of a closed relocatable object.

Used before the direct AMD64 PE link: GNU ld omits base relocations for
references to defined ELF weak symbols, so the resolved weak definitions of
the closed firmware core are localized first. The object must have no
undefined symbols.
"""
import argparse
import pathlib
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nm', required=True)
    parser.add_argument('--require-closed', action='store_true')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('object', type=pathlib.Path)
    args = parser.parse_args()

    if args.require_closed:
        undefined = subprocess.run([args.nm, '-u', str(args.object)], check=True,
                                   capture_output=True, text=True).stdout.strip()
        if undefined:
            print('FAIL: unresolved symbols in %s:\n%s' % (args.object, undefined), file=sys.stderr)
            return 1
    listing = subprocess.run([args.nm, '--defined-only', '-f', 'posix', str(args.object)],
                             check=True, capture_output=True, text=True).stdout
    weak = [fields[0] for fields in (line.split() for line in listing.splitlines())
            if len(fields) >= 2 and fields[1] in ('W', 'V')]
    args.output.write_text(''.join(name + '\n' for name in weak))
    return 0


if __name__ == '__main__':
    sys.exit(main())
