#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check held GIMPLE guard scopes over resolved calls.

Input is GCC pre-optimization dumps from the project build. Overloads and target variants
are merged by name (a conservative may-analysis). Class dumps provide each
base subobject's virtual table address point. Missing virtual targets
reachable from a held scope are errors, rather than exemptions.
"""
from __future__ import annotations
import argparse
import collections
import json
import pathlib
import re

LOCATION = re.compile(r'\[([^\]\n]+:\d+:\d+)\]')
NAME = r'(?:Asteria::[^;=]*?|Asteria\w+)'
CALL = re.compile(r'(' + NAME + r')\s+\(')
VIRTUAL = re.compile(r'OBJ_TYPE_REF\([^;]+;\((?:const )?struct ([^)]+)\).*?->(\d+)B\)')
GUARD = 'Asteria::Platform::Synchronization::SpinLockGuard::SpinLockGuard'
DESTRUCTOR = 'Asteria::Platform::Synchronization::SpinLockGuard::~SpinLockGuard'


def normalized(name: str) -> str:
    return ' '.join(name.split())


def forbidden(name: str) -> bool:
    # Terminal stop/freeze is the processor stop policy emergency path, not a returning wait.
    return (name.endswith(('::ProcessorWaitProgress', '::SwitchToIdleRoot',
                               '::ServiceThreadRequests', '::ServiceWaitingThreadRequests', '::KickProcessor'))
            or 'CrossProcessorWait::Poll' in name
            or name.endswith('::Wait') and ('Pager' in name or 'MappingProgress' in name)
            or 'TlbShootdown' in name and name.endswith(('::Publish', '::Drain', '::Notify'))
            or name.endswith(('BootRuntime::RunService', 'BootRuntime::StartProcessors',
                              'Amd64LocalApic::Send', '::DelayTscMicroseconds')))


class Analysis:
    def __init__(self):
        self.calls: dict[str, set[str]] = collections.defaultdict(set)
        self.virtuals: dict[tuple[str, int], set[str]] = collections.defaultdict(set)
        self.guards: dict[tuple[str, str], set[str]] = collections.defaultdict(set)
        self.dumps = 0
        self.pointer_bindings = collections.defaultdict(set)
        self.pointer_calls = collections.defaultdict(set)

    def classes(self, text: str) -> None:
        tables = {}
        for match in re.finditer(r'^Vtable for (.+)\n.*\n((?:\d+[^\n]*\n)+)', text, re.M):
            table = {}
            for row in match[2].splitlines():
                offset = int(row.split()[0])
                function = re.search(r'\)\)(.+)$', row)
                table[offset] = normalized(function[1]) if function else ''
            tables[match[1]] = table
        for match in re.finditer(r'^Class ([^\n]+)\n(.*?)(?=^Class |^Vtable |\Z)', text, re.M | re.S):
            owner, body = match[1], match[2]
            current = owner
            points = {}
            for row in body.splitlines():
                base = re.match(r'(Asteria::.*?) \(0x', row)
                if base:
                    current = base[1]
                point = re.search(r'vptr=.*? (Asteria::.*?)::_ZTV.*? \+ (\d+)\)', row)
                if point:
                    points[current] = (point[1], int(point[2]))
                primary = re.search(r'primary-for (Asteria::.*?) \(0x', row)
                if primary and primary[1] in points:
                    points[current] = points[primary[1]]
            for base, (table_name, start) in points.items():
                table = tables.get(table_name, {})
                rows = sorted((offset, fn) for offset, fn in table.items() if offset >= start)
                for slot, (_, function) in enumerate(rows):
                    if function in ('0', '') or function.startswith('(-'):
                        break  # Next secondary table header
                    if function.startswith('Asteria'):
                        self.virtuals[base.rsplit('::', 1)[-1], slot].add(function)

    def gimple(self, text: str, native_ia64: bool = False) -> None:
        self.dumps += 1
        function = None
        depth = 0
        active = {}
        for raw in text.splitlines():
            located = LOCATION.findall(raw)
            line = LOCATION.sub('', raw).strip()
            if function is None:
                if raw and not raw[0].isspace():
                    header = CALL.search(line)
                    if header:
                        function = normalized(header[1])
                        depth = 0
                        active = {}
                continue
            depth += line.count('{') - line.count('}')
            calls = {normalized(m[1]) for m in CALL.finditer(line)}
            # The only function pointers reachable from lock acquisition are
            # identity, monotonic clock and terminal stop hooks. Derive their
            # possible bindings from the same dumps; do not trust a whitelist
            # of implementations. All other unresolved pointer calls are
            # forbidden if reachable from a held scope.
            field = re.search(r'\.(Ticks|Stop) = ([^;]+);', line)
            if field and '(' not in field[2]:
                self.pointer_bindings[field[1]].add(normalized(field[2]))
            identity = re.search(r'::InstallCurrentProcessor \(\s*([^);]+)\);', line)
            if identity:
                self.pointer_bindings['Current'].add(normalized(identity[1]))
            if 'OBJ_TYPE_REF' not in line and re.search(r'\b(?:_\d+|current(?:\.\d+)?) \(', line):
                hook = next((key for suffix, key in (
                    ('::ProcessorWaitTicks', 'Ticks'), ('::StopProcessorWait', 'Stop'),
                    ('::CurrentProcessor', 'Current')) if function.endswith(suffix)), None)
                if function == 'Asteria::Boot::Uefi::{anonymous}::FirmwarePci::Read32':
                    # Source/Boot/Uefi/TopologyReport.cpp calls only UEFI
                    # HandleProtocol, Pci.GetLocation and Pci.Read here.
                    # UEFI Specification 2.10, section 14.4 (PCI I/O):
                    # these are firmware-owned synchronous protocol calls.
                    # TopologyReport executes before ExitBootServices and AP
                    # release; no Asteria callback is passed to the firmware.
                    marker = 'foreign UEFI PCI topology reads'
                else:
                    marker = 'pointer ' + hook if hook else 'unresolved pointer in ' + function
                calls.add(marker)
                self.pointer_calls[marker].add(function)
            for virtual in VIRTUAL.finditer(line):
                slot = int(virtual[2])
                if native_ia64:
                    if slot % 2:
                        raise ValueError("odd IA-64 descriptor slot")
                    slot //= 2
                key = normalized(virtual[1]), slot
                targets = self.virtuals.get(key)
                if targets:
                    calls.update(targets)
                else:
                    calls.add('unresolved virtual ' + str(key))
            self.calls[function].update(calls - {function})
            for scope in active.values():
                self.guards[scope].update(calls)
            if GUARD in calls:
                variable = re.search(r'SpinLockGuard::SpinLockGuard \(\s*&([^,]+),', line)
                if not variable:
                    raise ValueError('unparsed guard constructor: ' + raw)
                location = located[0] if located else line
                active[variable[1]] = (function, location)
                self.guards[function, location]  # Count empty held scopes, too
            elif DESTRUCTOR in calls:
                variable = re.search(r'~SpinLockGuard \(\s*&([^),]+)', line)
                if variable:
                    active.pop(variable[1], None)
            if depth == 0 and line == '}':
                if active:
                    raise ValueError('unclosed guard scope in ' + function)
                function = None

    def violations(self) -> list[dict]:
        for marker in self.pointer_calls:
            if marker.startswith('pointer '):
                key = marker.split(' ', 1)[1]
                names = self.pointer_bindings[key]
                targets = {fn for fn in self.calls
                           if fn in names or any(fn.endswith('::' + n) for n in names)}
                if not targets:
                    self.calls[marker].add('unresolved ' + marker)
                else:
                    self.calls[marker].update(targets)
        reverse = collections.defaultdict(set)
        for caller, callees in self.calls.items():
            for callee in callees:
                reverse[callee].add(caller)
        paths = {fn: [fn] for fn in reverse if forbidden(fn) or fn.startswith('unresolved ')}
        pending = collections.deque(paths)
        while pending:
            callee = pending.popleft()
            for caller in reverse[callee]:
                if caller not in paths:
                    paths[caller] = [caller] + paths[callee]
                    pending.append(caller)
        failures = []
        for (owner, location), callees in sorted(self.guards.items()):
            for callee in sorted(callees):
                if callee in paths:
                    failures.append(dict(held=owner, location=location, path=paths[callee]))
        return failures


def native_ia64(directory: pathlib.Path) -> bool:
    """Read the compiler target, including renamed probe/build directories.

    Hosted IA-64 analysis units use the host ABI. Native IA-64 virtual slots
    use descriptor pairs, independently of the preset's directory name.
    """
    for parent in (directory, *directory.parents):
        cache = parent / 'CMakeCache.txt'
        if cache.is_file():
            target = re.search(r'^ASTERIA_TARGET:[^=\n]+=([^\n]+)$', cache.read_text(), re.M)
            if target is not None:
                return target[1] == 'Ia64'
    # Keep support for standalone historic dumps without a build cache.
    return 'ia64' in directory.parts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dumps', action='append', required=True, type=pathlib.Path)
    parser.add_argument('--json', type=pathlib.Path)
    args = parser.parse_args()
    analysis = Analysis()
    files = []
    for directory in args.dumps:
        if not directory.is_dir():
            parser.error('missing dump tree: ' + str(directory))
        found = [p for p in directory.rglob('*.gimple') if '/Source/' in str(p)]
        if not found:
            parser.error('no GIMPLE dumps in ' + str(directory))
        files.extend((path, native_ia64(directory)) for path in found)
        for path in directory.rglob('*.class'):
            if '/Source/' in str(path):
                analysis.classes(path.read_text())
    for path, native in files:
        analysis.gimple(path.read_text(), native)
    failures = analysis.violations()
    report = dict(dumps=analysis.dumps, functions=len(analysis.calls),
                  guard_scopes=len(analysis.guards), virtual_slots=len(analysis.virtuals),
                  violations=failures)
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n')
    print('LockOrder: %d dumps, %d functions, %d guard scopes, %d virtual slots, %d violations'
          % (analysis.dumps, len(analysis.calls), len(analysis.guards), len(analysis.virtuals), len(failures)))
    for failure in failures[:30]:
        print(failure['location'] + ': ' + failure['held'])
        print('  ' + ' -> '.join(failure['path']))
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
