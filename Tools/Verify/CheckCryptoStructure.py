#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Structural review inventory, not a proof of machine-code constant time.

Inventory every branch/loop condition and subscript in the hand-written crypto
units and shared SHA projection source. A change requires reviewing its secrecy
classification against Crypto.md's checklist and updating the manifest. This
keeps review coverage from silently disappearing when algorithms change.
"""
import argparse
import json
import re
from pathlib import Path

FILES = ('Source/User/Crypto/Hash.cpp', 'Source/User/Crypto/Symmetric.cpp',
         'Source/User/Crypto/Curve.cpp', 'Source/User/Crypto/Argon2.cpp',
         'Source/User/Crypto/Drbg.cpp', 'Source/Kernel/Security/Sha256.cpp',
         'Source/User/Crypto/Internal.hpp', 'Source/User/Crypto/Crypto.hpp')


def inventory(root):
    result = {}
    for relative in FILES:
        text = (root / relative).read_text(encoding='utf-8')
        # Strip comments and strings so example prose never counts as control flow.
        text = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"', '', text)
        tokens = re.findall(r'[A-Za-z_][A-Za-z_0-9]*|[0-9][A-Za-z_0-9\x27]*|\S', text)
        expressions = {'conditions': [], 'subscripts': [], 'selections': []}
        for index, token in enumerate(tokens):
            if token in ('if', 'for', 'while') and index + 1 < len(tokens) and tokens[index + 1] == '(':
                nesting, stop = 1, index + 2
                while stop < len(tokens) and nesting:
                    nesting += (tokens[stop] == '(') - (tokens[stop] == ')')
                    stop += 1
                expressions['conditions'].append(token + ' ' + ' '.join(tokens[index + 2:stop - 1]))
            elif token == '[':
                nesting, stop = 1, index + 1
                while stop < len(tokens) and nesting:
                    nesting += (tokens[stop] == '[') - (tokens[stop] == ']')
                    stop += 1
                expressions['subscripts'].append(' '.join(tokens[max(0, index - 1):stop]))
            elif token == '?':
                # Record enough surrounding tokens to catch a changed selector.
                expressions['selections'].append(' '.join(tokens[max(0, index - 12):index + 12]))
        result[relative] = expressions
    return result


def check(root):
    expected = json.loads((root / 'Tools/Verify/CryptoReview.json').read_text(encoding='utf-8'))
    actual = inventory(root)
    errors = []
    for name in FILES:
        for kind in ('conditions', 'subscripts', 'selections'):
            if actual[name][kind] != expected['inventory'][name][kind]:
                errors.append(f'{name}: {kind} changed; repeat the Crypto.md secrecy review')
    symmetric = (root / FILES[1]).read_text(encoding='utf-8')
    argon = (root / FILES[3]).read_text(encoding='utf-8')
    hashing = (root / FILES[0]).read_text(encoding='utf-8')
    if 'volatile UInt8*' not in hashing:
        errors.append('volatile secret erasure removed')
    if re.search(r'\b(?:Sbox|SBox|SBOX|TTable)\b', symmetric):
        errors.append('AES substitution table requires review; secret indexing prohibited')
    # Only RFC 9106's reference-block address has a reviewed indexing exception.
    # Keep the exact hybrid boundary and all other checks unchanged.
    if 'const bool independent = pass == 0U && slice < 2U;' not in argon or argon.count('memory[reference]') != 1:
        errors.append('Argon2id reviewed addressing exception/phase boundary changed')
    helpers = (root / 'Source/User/Crypto/Internal.hpp').read_text(encoding='utf-8')
    if 'volatile UInt32 opaque' not in helpers or 'volatile UInt64 opaque' not in helpers or len(re.findall(r'\bopaque\s*=\s*0U;', helpers)) != 2:
        errors.append('erased volatile secret-selection mask barrier removed')
    projection = (root / 'Source/User/Crypto/SharedSha256.cmake').read_text(encoding='utf-8')
    if 'volatile UInt32* clear = schedule' not in projection or 'reinterpret_cast<volatile UInt8*>(this)' not in projection:
        errors.append('shared SHA-256 projection erasure removed')
    if not expected.get('review') or not expected.get('classification'):
        errors.append('review/checklist classification missing')
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(error)
    if not errors:
        print('Crypto structural inventory matches the timing review; machine-code audit remains required.')
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
