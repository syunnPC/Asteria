# SPDX-License-Identifier: Apache-2.0
"""golden snapshots of normalized disassembly.

A golden file records one symbol, or one range between two marker symbols,
of hand-written assembly:

    # golden: 2
    # binutils: <first line of `objdump --version` used to record it>
    # symbol: <symbol name> or # range: <start symbol>..<end symbol>
    # size: <extent size in bytes, or "any">
    # artifacts: <artifact specifications>
    <one normalized instruction per line>

Each artifact specification is either an object group (`O0`, `O2`,
`firmware`, `O2-probe`,...), meaning the object of that group that defines
the symbol, or an artifact key such as `O2:user_elf`. A specification may
end with `[option]`, a configuration variable of the manifest: the artifact
then exists only in trees built with that option `1` (for example
`firmware[ia64_native_preemption_probe]` for code only a probe firmware
compiles). Every listed artifact must produce exactly the recorded lines;
artifacts of variants a build tree does not contain, or whose option it
does not set, are skipped.

Comparison reports a unified diff. Recording is explicit (`--update-golden`)
so that a code-generation change is reviewed as a data diff. Goldens depend
on the assembler and objdump, so a golden recorded with another binutils
version is reported as stale instead of being compared.
"""
from __future__ import annotations

import dataclasses
import difflib
import pathlib

FORMAT = '2'


@dataclasses.dataclass
class GoldenRecord:
    binutils: str
    symbol: str | None
    range: tuple[str, str] | None
    size: int | None
    artifacts: list[str]
    lines: list[str]

    @property
    def label(self) -> str:
        return self.symbol if self.symbol is not None else '%s..%s' % self.range

    def render(self) -> str:
        header = ['# golden: %s' % FORMAT, '# binutils: %s' % self.binutils]
        if self.symbol is not None:
            header.append('# symbol: %s' % self.symbol)
        else:
            header.append('# range: %s..%s' % self.range)
        header.append('# size: %s' % ('any' if self.size is None else '%#x' % self.size))
        header.append('# artifacts: %s' % ' '.join(self.artifacts))
        return '\n'.join(header + self.lines) + '\n'


class GoldenError(Exception):
    pass


def split_condition(specification: str) -> tuple[str, str | None]:
    """(artifact specification, configuration option or None) of `spec[option]`."""
    if specification.endswith(']') and '[' in specification:
        base, _, option = specification[:-1].partition('[')
        if base and option:
            return base, option
        raise GoldenError('malformed conditional artifact %s' % specification)
    return specification, None


def parse(path: pathlib.Path) -> GoldenRecord:
    fields: dict[str, str] = {}
    lines = []
    for line in path.read_text().splitlines():
        if line.startswith('# ') and ':' in line and not lines:
            key, value = line[2:].split(':', 1)
            fields[key.strip()] = value.strip()
        else:
            lines.append(line)
    if fields.get('golden') != FORMAT:
        raise GoldenError('%s is not a format-%s golden file' % (path, FORMAT))
    for key in ('binutils', 'size', 'artifacts'):
        if key not in fields:
            raise GoldenError('%s lacks the "%s" header' % (path, key))
    if ('symbol' in fields) == ('range' in fields):
        raise GoldenError('%s must name exactly one of "symbol" and "range"' % path)
    symbol_range = None
    if 'range' in fields:
        start, _, end = fields['range'].partition('..')
        if not start or not end:
            raise GoldenError('%s has a malformed range' % path)
        symbol_range = (start, end)
    size = None if fields['size'] == 'any' else int(fields['size'], 0)
    artifacts = fields['artifacts'].split()
    if not artifacts:
        raise GoldenError('%s names no artifacts' % path)
    return GoldenRecord(fields['binutils'], fields.get('symbol'), symbol_range, size, artifacts, lines)


def compare(expected: GoldenRecord, lines: list[str], size: int, artifact: str, path: pathlib.Path) -> list[str]:
    """Returns human-readable differences; an empty list means a match."""
    problems = []
    if expected.size is not None and expected.size != size:
        problems.append('%s: %s in %s has size %#x, golden records %#x'
                        % (path, expected.label, artifact, size, expected.size))
    if expected.lines != lines:
        diff = difflib.unified_diff(expected.lines, lines, 'golden', artifact, lineterm='', n=2)
        problems.append('%s: disassembly of %s in %s differs:\n%s'
                        % (path, expected.label, artifact, '\n'.join(diff)))
    return problems
