# SPDX-License-Identifier: Apache-2.0
"""TAP version 14 parser for the guest test program's output.

The guest writes one TAP line per `USER:` debug line. The stream is complete
when the plan (`1..N`) has been seen and N test points have arrived, or on
`Bail out!`. Subtests and YAML blocks are not used by the guest and are
rejected, so a malformed stream can never be mistaken for a pass. The runner
is stricter than TAP 14 in one respect: the guest has no expected failures,
so a `not ok` point fails even with a TODO or SKIP directive; a skipped
point is `ok # SKIP`.
"""
from __future__ import annotations

import dataclasses
import re

_VERSION = re.compile(r'^TAP version 14$')
_PLAN = re.compile(r'^1\.\.(?P<count>\d+)(?:\s*#\s*(?P<reason>.*))?$')
_POINT = re.compile(r'^(?P<status>ok|not ok)(?:\s+(?P<number>\d+))?(?:\s*-?\s*(?P<description>[^#]*?))?'
                    r'(?:\s*#\s*(?P<directive>SKIP|TODO)\b\s*(?P<reason>.*))?$', re.IGNORECASE)
_BAIL = re.compile(r'^Bail out!(?:\s*(?P<reason>.*))?$')


@dataclasses.dataclass
class Point:
    number: int
    ok: bool
    description: str
    directive: str | None = None
    reason: str = ''
    diagnostics: list[str] = dataclasses.field(default_factory=list)

    @property
    def passed(self) -> bool:
        return self.ok

    @property
    def skipped(self) -> bool:
        return self.ok and self.directive == 'SKIP'


@dataclasses.dataclass
class TapStream:
    version_seen: bool = False
    plan: int | None = None
    points: list[Point] = dataclasses.field(default_factory=list)
    bail_out: str | None = None
    errors: list[str] = dataclasses.field(default_factory=list)

    @property
    def complete(self) -> bool:
        return self.bail_out is not None or (self.plan is not None and len(self.points) >= self.plan)

    @property
    def passed(self) -> bool:
        return (self.version_seen and self.bail_out is None and not self.errors and self.plan is not None
                and len(self.points) == self.plan and all(point.passed for point in self.points))

    def structure_problems(self) -> list[str]:
        """What makes the stream itself invalid, apart from failing points."""
        problems = list(self.errors)
        if not self.version_seen:
            problems.append('no "TAP version 14" line')
        if self.bail_out is not None:
            problems.append('bail out: %s' % self.bail_out)
        if self.plan is None:
            problems.append('no plan line')
        elif len(self.points) != self.plan:
            problems.append('plan 1..%d but %d test points' % (self.plan, len(self.points)))
        return problems

    def problems(self) -> list[str]:
        return self.structure_problems() + [
            'not ok %d - %s%s' % (p.number, p.description, ''.join('\n    ' + d for d in p.diagnostics))
            for p in self.points if not p.passed]


def parse(lines: list[str]) -> TapStream:
    stream = TapStream()
    expected = 1
    for line in lines:
        if not stream.version_seen:
            if _VERSION.match(line):
                stream.version_seen = True
            else:
                stream.errors.append('text before the version line: %r' % line)
            continue
        if line.startswith('#'):
            if stream.points:
                stream.points[-1].diagnostics.append(line[1:].strip())
            continue
        if line.startswith(' '):
            stream.errors.append('indented TAP (subtest or YAML) is not supported: %r' % line)
            continue
        match = _BAIL.match(line)
        if match:
            stream.bail_out = match.group('reason') or ''
            break
        match = _PLAN.match(line)
        if match:
            if stream.plan is not None:
                stream.errors.append('second plan line: %r' % line)
            stream.plan = int(match.group('count'))
            continue
        match = _POINT.match(line)
        if match:
            number = int(match.group('number')) if match.group('number') else expected
            if number != expected:
                stream.errors.append('test point %d arrived, expected %d' % (number, expected))
            expected = number + 1
            directive = match.group('directive').upper() if match.group('directive') else None
            stream.points.append(Point(number, match.group('status').lower() == 'ok',
                                       (match.group('description') or '').strip(), directive,
                                       (match.group('reason') or '').strip()))
            continue
        stream.errors.append('unrecognized TAP line: %r' % line)
    return stream
