# SPDX-License-Identifier: Apache-2.0
"""The rendering corpora of the desktop session.

The C++ host renderers (Tests/Support/RenderCorpus.hpp) and the runner's
models (Session.py, Terminal.py) read the same corpus text, render each case to
a binary PPM and check its SHA-256 against one committed digest file, so the
two produce byte-identical images; later VM screenshots are compared with the
same models. The lexical rules are RenderCorpus.hpp's:

- one statement per line; blank lines and '#' comment lines are ignored;
- tokens are separated by blanks; a token starting with '"' is a quoted string
  with the escapes \\\\ \\" \\n \\r \\t \\e (0x1B) and \\xHH, returned as bytes;
- a case runs from `case <kind> <name>...` to `end`.
"""
from __future__ import annotations

import dataclasses
import hashlib
import os
import pathlib
import re

Token = str | bytes
OUTPUT_VARIABLE = 'ASTERIA_RENDER_OUTPUT_DIR'


class CorpusError(Exception):
    """A corpus or digest file is malformed."""


def _unescape(text: str, line: int) -> bytes:
    result = bytearray()
    at = 0
    while at < len(text):
        char = text[at]
        if char != '\\':
            result += char.encode('latin-1')
            at += 1
            continue
        at += 1
        if at >= len(text):
            raise CorpusError('line %d: a string ends with a backslash' % line)
        escape = text[at]
        simple = {'\\': b'\\', '"': b'"', 'n': b'\n', 'r': b'\r', 't': b'\t', 'e': b'\x1b'}
        if escape in simple:
            result += simple[escape]
            at += 1
        elif escape == 'x':
            digits = text[at + 1:at + 3]
            if len(digits) != 2 or any(digit not in '0123456789abcdefABCDEF' for digit in digits):
                raise CorpusError('line %d: malformed \\x escape' % line)
            result.append(int(digits, 16))
            at += 3
        else:
            raise CorpusError('line %d: unknown escape \\%s' % (line, escape))
    return bytes(result)


def tokenize(line: str, number: int = 0) -> list[Token]:
    """The tokens of one statement: str for words, bytes for quoted strings."""
    tokens: list[Token] = []
    at = 0
    while at < len(line):
        if line[at] in ' \t\r':
            at += 1
            continue
        if line[at] == '"':
            end = at + 1
            while end < len(line) and line[end] != '"':
                end += 2 if line[end] == '\\' else 1
            if end >= len(line):
                raise CorpusError('line %d: unterminated string' % number)
            tokens.append(_unescape(line[at + 1:end], number))
            at = end + 1
        else:
            end = at
            while end < len(line) and line[end] not in ' \t\r':
                end += 1
            tokens.append(line[at:end])
            at = end
    return tokens


@dataclasses.dataclass
class Case:
    kind: str
    name: str
    arguments: list[Token]
    statements: list[tuple[int, list[Token]]]


def read_cases(path: pathlib.Path) -> list[Case]:
    """Every case of a corpus file, in order."""
    cases: list[Case] = []
    current: Case | None = None
    for number, raw in enumerate(pathlib.Path(path).read_text(encoding='latin-1').split('\n'), start=1):
        tokens = tokenize(raw, number)
        if not tokens or (isinstance(tokens[0], str) and tokens[0].startswith('#')):
            continue
        if tokens[0] == 'case':
            if current is not None or len(tokens) < 3:
                raise CorpusError('line %d: malformed case' % number)
            current = Case(str(tokens[1]), str(tokens[2]), tokens[3:], [])
        elif tokens[0] == 'end':
            if current is None:
                raise CorpusError('line %d: end outside a case' % number)
            cases.append(current)
            current = None
        elif current is None:
            raise CorpusError('line %d: statement outside a case' % number)
        else:
            current.statements.append((number, tokens))
    if current is not None:
        raise CorpusError('case %s has no end' % current.name)
    return cases


def read_digests(path: pathlib.Path) -> dict[str, str]:
    digests: dict[str, str] = {}
    for number, raw in enumerate(pathlib.Path(path).read_text().split('\n'), start=1):
        tokens = tokenize(raw, number)
        if not tokens or (isinstance(tokens[0], str) and tokens[0].startswith('#')):
            continue
        if len(tokens) != 2 or len(str(tokens[1])) != 64:
            raise CorpusError('line %d: malformed digest' % number)
        digests[str(tokens[0])] = str(tokens[1])
    return digests


def header_text(repository: pathlib.Path, relative: str) -> str:
    path = pathlib.Path(repository) / relative
    try:
        return path.read_text()
    except OSError as error:
        raise CorpusError('cannot read %s: %s' % (path, error)) from error


def header_constant(text: str, name: str) -> int:
    """An integer constant written `Name{<decimal or 0x hex>U}`."""
    match = re.search(r'\b%s\s*\{\s*(0[xX][0-9a-fA-F]+|\d+)[Uu]?\s*\}' % re.escape(name), text)
    if match is None:
        raise CorpusError('no constant %s' % name)
    return int(match.group(1), 0)


def header_color(text: str, name: str) -> tuple[int, int, int]:
    """A colour written `Name{.Red = 0x..U,.Green = 0x..U,.Blue = 0x..U}`."""
    match = re.search(r'\b%s\s*\{\s*\.Red\s*=\s*(\w+?)[Uu]?\s*,\s*\.Green\s*=\s*(\w+?)[Uu]?\s*,'
                      r'\s*\.Blue\s*=\s*(\w+?)[Uu]?\s*\}' % re.escape(name), text)
    if match is None:
        raise CorpusError('no colour %s' % name)
    return tuple(int(group, 0) for group in match.groups())


def ppm(width: int, height: int, rgb: bytes) -> bytes:
    """A binary PPM exactly as EncodeRenderPpm writes it."""
    return b'P6\n%d %d\n255\n' % (width, height) + rgb


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def first_difference(expected: bytes, actual: bytes, width: int) -> str:
    """Where two PPMs of one width first differ, for a failure message."""
    header = expected.index(b'255\n') + 4 if b'255\n' in expected else 0
    for offset in range(min(len(expected), len(actual))):
        if expected[offset] != actual[offset]:
            if offset < header or width <= 0:
                return 'byte %d' % offset
            pixel = (offset - header) // 3
            return 'pixel (%d, %d) channel %d: model %d, renderer %d' % (
                pixel % width, pixel // width, (offset - header) % 3, expected[offset], actual[offset])
    return 'lengths %d and %d' % (len(expected), len(actual))


def renderer_output(name: str) -> bytes | None:
    """The C++ renderer's PPM of a case when ASTERIA_RENDER_OUTPUT_DIR names
    the directory its corpus test wrote, else None."""
    directory = os.environ.get(OUTPUT_VARIABLE)
    if not directory:
        return None
    path = pathlib.Path(directory) / ('%s.ppm' % name)
    return path.read_bytes() if path.is_file() else None


def glyph_mapping(repository: pathlib.Path) -> tuple[dict[int, int], int]:
    """The shared Bold8x16 Unicode boundary, read from C++ character-set data."""
    text = header_text(repository, 'Source/User/Terminal/Cp437.hpp')
    table = text[text.index('Cp437Graphics['):]
    table = table[:table.index('};')]
    pairs = re.findall(r'\{\s*(0x[0-9A-Fa-f]+)U\s*,\s*(0x[0-9A-Fa-f]+)U\s*\}', table)
    if len(pairs) != header_constant(text, 'Cp437MappingCount'):
        raise CorpusError('incomplete Bold8x16 Unicode table')
    return ({int(point, 16): int(code, 16) for code, point in pairs}, header_constant(text, 'Cp437Replacement'))
