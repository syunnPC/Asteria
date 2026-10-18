# SPDX-License-Identifier: Apache-2.0
"""Kernel stage events on the debug channel.

The kernel reports its progress as neutral facts: one line
`ASTERIA:STAGE:<NAME>` when a stage of the boot path completes, where NAME
is upper-case letters and digits in hyphen-separated words (for example
`ASTERIA:STAGE:AP-RENDEZVOUS`). A stage event is no verdict. Failures are
reported by `...:FAIL`, `-ERROR` and `-FAULT` diagnostics, which end a case
(`Console.FAILURE`); what the kernel's scripted scenarios observed is in
its evidence record (`AsteriaVm.Evidence`).

Drivers wait on stage events to bound their phases (`predicate`) and check
which stages a boot reached, in which order where the order matters, and
which stages a configuration must not reach (`problems`).
"""
from __future__ import annotations

import re
from typing import Iterable, Sequence

PREFIX = 'ASTERIA:STAGE:'
_NAME = re.compile(r'[A-Z0-9]+(?:-[A-Z0-9]+)*')
_BOOT_MODULE = re.compile(r'BOOT-MODULE:[a-z0-9-]{1,15} [A-Z0-9_]{1,8}\.[A-Z0-9_]{1,3} size=[1-9][0-9]* crc32c=0x[0-9a-fA-F]{8}')
_BOOT_FILE = re.compile(r'BOOT-FILE:[A-Z0-9_]{1,8}\.[A-Z0-9_]{1,3} size=[1-9][0-9]* crc32c=0x[0-9a-fA-F]{8}')


def line(name: str) -> str:
    """The event line of stage `name`."""
    if not _NAME.fullmatch(name):
        raise ValueError('%r is not a stage name' % name)
    return PREFIX + name


def stages(lines: Iterable[str]) -> list[str]:
    """The names of the well-formed stage events, in the order they arrived."""
    return [text[len(PREFIX):] for text in lines
            if text.startswith(PREFIX) and _NAME.fullmatch(text[len(PREFIX):])]


def malformed(lines: Iterable[str]) -> list[str]:
    """Lines that start like a stage event but do not name a stage."""
    return [text for text in lines if text.startswith(PREFIX) and not _NAME.fullmatch(text[len(PREFIX):])
            and not _BOOT_FILE.fullmatch(text[len(PREFIX):]) and not _BOOT_MODULE.fullmatch(text[len(PREFIX):])]


def predicate(name: str):
    """A console predicate for the event of stage `name` (a complete line)."""
    expected = line(name)

    def matches(text: str) -> bool:
        return text == expected
    matches.__name__ = expected
    return matches


def problems(lines: Iterable[str], expected: Sequence[str], *, absent: Sequence[str] = ()) -> list[str]:
    """Every stage of `expected` reported exactly once and in this order
    (other stages may come in between), none of `absent`, and no malformed
    stage line."""
    lines = list(lines)
    observed = stages(lines)
    found = ['malformed stage event %r' % text for text in malformed(lines)]
    for name in expected:
        count = observed.count(name)
        if count != 1:
            found.append('stage %s reported %d times, expected once' % (name, count))
    positions = [observed.index(name) for name in expected if observed.count(name) == 1]
    if positions != sorted(positions):
        found.append('stages out of order: %s; expected the order %s' % (
            ', '.join(name for name in observed if name in expected), ', '.join(expected)))
    for name in absent:
        if name in observed:
            found.append('stage %s is reported, but this configuration must not reach it' % name)
    return found
