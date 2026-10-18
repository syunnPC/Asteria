# SPDX-License-Identifier: Apache-2.0
"""processor stop policy terminal diagnostics; vocabulary comes from the kernel table."""
from __future__ import annotations

import pathlib
import re

from AsteriaVm import Evidence
from AsteriaVm.Observers import Amd64Evidence, Ia64Evidence

PREFIX = 'ASTERIA:STOP:'
LINE = re.compile(r'^ASTERIA:STOP:CPU=(\d+):CODE=(0x[0-9A-F]+):([A-Za-z]+):P='
                  r'(0x[0-9A-F]+),(0x[0-9A-F]+),(0x[0-9A-F]+),(0x[0-9A-F]+)$')


def vocabulary(root: pathlib.Path) -> dict[str, int]:
    text = (root / 'Source/Kernel/Diagnostics/StopCode.hpp').read_text()
    values = {name: int(number) for name, number in re.findall(r'^    (\w+) = (\d+)U,', text, re.M)}
    table = dict(re.findall(r'\{StopCode::(\w+), "(\w+)"\}', text))
    if set(values) != set(table) or any(name != table[name] for name in table):
        raise ValueError('stop name table disagrees with StopCode')
    if any(any(token in name.upper() for token in ('FAIL', 'ERROR', 'FAULT')) for name in table):
        raise ValueError('stop name contains a failure token')
    return values


def problems(lines: list[str], expected: str, cpus: int, root: pathlib.Path, stops, online_mask: int | None = None) -> list[str]:
    found = [line for line in lines if line.startswith(PREFIX)]
    if len(found) != 1:
        return ['%d STOP lines, expected exactly one' % len(found)]
    match = LINE.fullmatch(found[0])
    if match is None:
        return ['malformed STOP line: %r' % found[0]]
    cpu, code, name, *parameters = match.groups()
    values = vocabulary(root)
    issues = []
    if name != expected or int(code, 16) != values.get(expected):
        issues.append('STOP is %s/%s, expected %s' % (name, code, expected))
    mask = (1 << cpus) - 1 if online_mask is None else online_mask
    if stops.Published != 1 or stops.OnlineMask != mask:
        issues.append('stop not published or online mask differs from CPU count')
    if stops.ConfirmedMask & stops.UnconfirmedMask or stops.ConfirmedMask | stops.UnconfirmedMask != mask:
        issues.append('confirmation masks do not partition the online CPUs')
    winner = int(cpu)
    if not 0 <= winner < cpus or stops.Winner != winner:
        issues.append('STOP CPU differs from evidence winner')
        return issues
    record = stops.Processors[winner].Stop
    if record.Code != int(code, 16) or record.Parameters != [int(value, 16) for value in parameters]:
        issues.append('STOP payload differs from evidence')
    for processor in range(cpus):
        if not mask & (1 << processor):
            continue
        state = stops.Processors[processor].Stop.State
        if state not in (0, 1, 2, 3):
            issues.append('CPU %d has an unknown record state %d' % (processor, state))
        frozen = state == 2
        unconfirmed = bool(stops.UnconfirmedMask & (1 << processor))
        if not frozen and not unconfirmed:
            issues.append('CPU %d is neither frozen nor recorded unconfirmed' % processor)
        if stops.ConfirmedMask & (1 << processor) and not frozen:
            issues.append('CPU %d confirmation has no frozen record' % processor)
    return issues


def lifecycle(context) -> None:
    data = context.artifact(Evidence.ARTIFACT).read_bytes()
    observer = Ia64Evidence if context.case.isa == "Ia64" else Amd64Evidence
    stops = observer.schema(context.target.root).decode(data).Stops
    issues = problems(context.console.lines, context.case.expected_stop, context.case.cpus, context.target.root, stops, context.case.expected_online_mask)
    issues.extend(context.console.failures())
    context.check_problems('lifecycle.expected-stop', issues)
