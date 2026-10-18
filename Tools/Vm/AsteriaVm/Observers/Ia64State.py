# SPDX-License-Identifier: Apache-2.0
"""IA-64 CPU state from the project QEMU's `info registers` text.

The IA-64 QEMU fork has no QMP register or TLB query,
so this module is the one place that uses `human-monitor-command`: `read`
runs `info registers` for one CPU and `parse` turns the text into a
`CpuState`. The text is QEMU's `ia64_cpu_dump_state` (target/ia64/dump.c):
human-readable lines (`IP:`, `CR.DCR:`, `IVA:`, `IHA:`, `CFM:`, `RSE:`,
`ITLB[n] TR|TC...`) followed by the versioned `IA64STATE` records
(schema 1).

Parsing is strict: the text must be complete
(newline-terminated) and free of NUL bytes; a record that a check reads must
appear exactly once and match its format completely; and every TLB line must
be well formed. A violation is reported by `CpuState.get` (and the typed
accessors) as a `StateError` naming the record, never as a silently missing
or partial value. Lines of other kinds (`r0...`, `RSE-PGR[..]`, `SAPIC...`)
are ignored.
"""
from __future__ import annotations

import dataclasses
import re

SCHEMA_VERSION = 1
GENERAL_REGISTERS = 128
BRANCH_REGISTERS = 8

_HEX16 = r'[0-9A-Fa-f]{16}'
_X16 = r'0x[0-9A-Fa-f]{16}'


def _record(pattern: str) -> re.Pattern:
    return re.compile('^' + pattern + '$')


# Single records: key -> (line prefix, complete-line pattern). Named groups
# become the record's fields; `_int` converts them.
_RECORDS: dict[str, tuple[str, re.Pattern]] = {
    'IP': ('IP: ', _record(r'IP: (?P<ip>%s)  PSR: (?P<psr>%s)  HALTED: (?P<halted>\d+)' % (_X16, _X16))),
    'SCHEMA': ('IA64STATE SCHEMA ', _record(r'IA64STATE SCHEMA version=(?P<version>%s)' % _HEX16)),
    'META': ('IA64STATE META ', _record(r'IA64STATE META ip=(?P<ip>%s) psr=(?P<psr>%s) halted=(?P<halted>\d+)'
                                        % (_HEX16, _HEX16))),
    'EXCEPTION': ('IA64STATE EXCEPTION ', _record(
        r'IA64STATE EXCEPTION code=(?P<code>%s) fault_code=(?P<fault_code>%s) fault_ip=(?P<fault_ip>%s)'
        r' fault_imm=(?P<fault_imm>%s)' % (_HEX16, _HEX16, _HEX16, _HEX16))),
    'GRNAT': ('IA64STATE GRNAT ', _record(r'IA64STATE GRNAT lo=(?P<lo>%s) hi=(?P<hi>%s)' % (_HEX16, _HEX16))),
    'PR': ('IA64STATE PR ', _record(r'IA64STATE PR value=(?P<value>%s)' % _HEX16)),
    'AR': ('IA64STATE AR ', _record(
        r'IA64STATE AR ccv=(?P<ccv>{0}) unat=(?P<unat>{0}) fpsr=(?P<fpsr>{0}) csd=(?P<csd>{0}) ssd=(?P<ssd>{0})'
        r' rsc=(?P<rsc>{0}) rnat=(?P<rnat>{0}) pfs=(?P<pfs>{0})'.format(_HEX16))),
    'CFM': ('IA64STATE CFM ', _record(
        r'IA64STATE CFM sof=(?P<sof>{0}) sol=(?P<sol>{0}) sor=(?P<sor>{0}) rrb_gr=(?P<rrb_gr>{0})'
        r' rrb_fr=(?P<rrb_fr>{0}) rrb_pr=(?P<rrb_pr>{0})'.format(_HEX16))),
    'CR.DCR': ('CR.DCR: ', _record(
        r'CR\.DCR: (?P<dcr>{0}) CR\.ITM: (?P<itm>{0}) ITV: (?P<itv>{0}) TPR: (?P<tpr>{0}) EOI: (?P<eoi>{0})'
.format(_X16))),
    'IIP': ('IIP: ', _record(r'IIP: (?P<iip>{0}) IFA: (?P<ifa>{0}) IPSR: (?P<ipsr>{0})'.format(_X16))),
    'ISR': ('ISR: ', _record(r'ISR: (?P<isr>{0}) IFS: (?P<ifs>{0}) IIM: (?P<iim>{0})'.format(_X16))),
    'IVA': ('IVA: ', _record(r'IVA: (?P<iva>{0}) IIPA: (?P<iipa>{0}) ITIR: (?P<itir>{0})'.format(_X16))),
    'IHA': ('IHA: ', _record(
        r'IHA: (?P<iha>{0}) PTA: (?P<pta>{0}) RR0: (?P<rr0>{0}) RR5: (?P<rr5>{0}) RR6: (?P<rr6>{0})'
        r' RR7: (?P<rr7>{0})'.format(_X16))),
    'CFM:': ('CFM: ', _record(
        r'CFM: sof=(?P<sof>\d+) sol=(?P<sol>\d+) sor=(?P<sor>\d+) rrb\.gr=(?P<rrb_gr>\d+) rrb\.fr=(?P<rrb_fr>\d+)'
        r' rrb\.pr=(?P<rrb_pr>\d+) AR\.PFS=(?P<pfs>{0}) BSP=(?P<bsp>{0}) BSPSTORE=(?P<bspstore>{0})'
.format(_X16))),
    'RSE': ('RSE: ', _record(
        r'RSE: bol=(?P<bol>\d+) dirty=(?P<dirty>-?\d+)/(?P<dirty_nat>-?\d+) clean=(?P<clean>-?\d+)'
        r'/(?P<clean_nat>-?\d+) invalid=(?P<invalid>-?\d+) RNAT=(?P<rnat>{0}) RNAT_ADDR=(?P<rnat_addr>{0})'
        r' RNAT_DEFINED=(?P<rnat_defined>{0}) RSC=(?P<rsc>{0})'.format(_X16))),
}

_GENERAL = re.compile(r'^IA64STATE GR index=(?P<index>[0-9a-f]{3}) value=(?P<value>%s) nat=(?P<nat>[01])$' % _HEX16)
_BRANCH = re.compile(r'^IA64STATE BR index=(?P<index>[0-9a-f]{3}) value=(?P<value>%s)$' % _HEX16)
_TLB_PREFIX = re.compile(r'^(?:ITLB|DTLB|DTLB1)\[')
_TLB = re.compile(
    r'^(?P<kind>ITLB|DTLB|DTLB1)\[(?P<index>\d+)\] (?P<type>TR|TC) va=(?P<va>{0}) pa=(?P<pa>{0}) ps=(?P<ps>{0})'
    r' rid=0x(?P<rid>[0-9A-Fa-f]{{6}}) key=0x(?P<key>[0-9A-Fa-f]{{6}}) ar=(?P<ar>\d) pl=(?P<pl>\d)'
    r' perm=0x(?P<perm>[0-9A-Fa-f]+) pte=(?P<pte>{0})$'.format(_X16))
_DECIMAL_FIELDS = frozenset({'halted', 'sof', 'sol', 'sor', 'rrb_gr', 'rrb_fr', 'rrb_pr', 'bol', 'dirty',
                             'dirty_nat', 'clean', 'clean_nat', 'invalid'})


class StateError(Exception):
    """A record the caller needs is missing, duplicated or malformed."""


@dataclasses.dataclass(frozen=True)
class TlbEntry:
    """One valid entry of QEMU's instruction or data TLB array.

    `index` is the array slot, which for a translation register is its TR
    number. `perm` is QEMU's derived r/w/x permission mask (1/2/4)."""
    kind: str        # 'ITLB' or 'DTLB'
    index: int
    is_register: bool
    va: int
    pa: int
    ps: int
    rid: int
    key: int
    ar: int
    pl: int
    perm: int
    pte: int

    def describe(self) -> str:
        return '%s[%d] %s va=%#x pa=%#x ps=%#x rid=%#08x key=%#x ar=%d pl=%d perm=%#x pte=%#018x' % (
            self.kind, self.index, 'TR' if self.is_register else 'TC', self.va, self.pa, self.ps, self.rid,
            self.key, self.ar, self.pl, self.perm, self.pte)


def _int(key: str, name: str, text: str) -> int:
    # Machine CFM uses hex; the human CFM: line uses decimal. META halted
    # is decimal even in the machine schema. Preserve the record identity.
    decimal = name in _DECIMAL_FIELDS and key != 'CFM'
    return int(text, 10 if decimal else 16)


class CpuState:
    """The parsed `info registers` text of one CPU.

    Build it with `parse`. Accessors raise `StateError`; checks turn the
    error into a problem string, so one unreadable record fails exactly the
    checks that need it.
    """

    def __init__(self, text: str, cpu: int):
        self.text = text
        self.cpu = cpu
        self.text_problems: list[str] = []
        self._lines: dict[str, list[str]] = {key: [] for key in _RECORDS}
        self._general: dict[int, list[tuple[int, int]]] = {}
        self._branch: dict[int, list[int]] = {}
        self._malformed: dict[str, list[str]] = {}
        self._tlb: list[TlbEntry] = []
        self._tlb_problems: list[str] = []

    # Records -----------------------------------------------------------------

    def _fail(self, what: str) -> None:
        if self.text_problems:
            raise StateError('CPU %d register dump is unusable (%s); %s cannot be read'
                             % (self.cpu, '; '.join(self.text_problems), what))

    def get(self, key: str) -> dict[str, int]:
        """The fields of the single record `key` (see `_RECORDS`)."""
        self._fail('the %s record' % key)
        lines = self._lines[key]
        if len(lines) != 1:
            raise StateError('CPU %d has %d %r records, expected exactly one%s' % (
                self.cpu, len(lines), _RECORDS[key][0].strip(),
                '' if not lines else ': ' + ' | '.join(lines[:3])))
        match = _RECORDS[key][1].match(lines[0])
        if match is None:
            raise StateError('CPU %d has a malformed %r record: %r' % (self.cpu, _RECORDS[key][0].strip(), lines[0]))
        try:
            return {name: _int(key, name, value) for name, value in match.groupdict().items()}
        except ValueError as error:
            raise StateError('CPU %d has an invalid numeric %r record: %r'
                             % (self.cpu, key, lines[0])) from error

    def value(self, key: str, field: str) -> int:
        return self.get(key)[field]

    def general(self, index: int) -> tuple[int, int]:
        """(value, NaT bit) of GR `index` from its `IA64STATE GR` record."""
        self._fail('r%d' % index)
        if 'GR' in self._malformed:
            raise StateError('CPU %d has malformed IA64STATE GR records: %r' % (self.cpu, self._malformed['GR'][0]))
        records = self._general.get(index, [])
        if len(records) != 1:
            raise StateError('CPU %d has %d IA64STATE GR records for r%d, expected exactly one'
                             % (self.cpu, len(records), index))
        return records[0]

    def branch(self, index: int) -> int:
        self._fail('b%d' % index)
        if 'BR' in self._malformed:
            raise StateError('CPU %d has malformed IA64STATE BR records: %r' % (self.cpu, self._malformed['BR'][0]))
        records = self._branch.get(index, [])
        if len(records) != 1:
            raise StateError('CPU %d has %d IA64STATE BR records for b%d, expected exactly one'
                             % (self.cpu, len(records), index))
        return records[0]

    def tlb(self) -> list[TlbEntry]:
        """Every valid TLB entry, in dump order; fails if any TLB line is malformed."""
        self._fail('the TLB')
        if self._tlb_problems:
            raise StateError('CPU %d has malformed TLB records: %s' % (self.cpu, '; '.join(self._tlb_problems[:3])))
        return list(self._tlb)

    def translation_registers(self, kind: str) -> list[TlbEntry]:
        return [entry for entry in self.tlb() if entry.kind == kind and entry.is_register]

    def translation_cache(self) -> list[TlbEntry]:
        return [entry for entry in self.tlb() if not entry.is_register]

    # Frequently used values --------------------------------------------------

    @property
    def ip(self) -> int:
        return self.value('IP', 'ip')

    @property
    def psr(self) -> int:
        return self.value('IP', 'psr')


def parse(text: str, cpu: int = 0) -> CpuState:
    state = CpuState(text, cpu)
    if not text.endswith('\n'):
        state.text_problems.append('the dump is incomplete (no final newline)')
    if '\0' in text:
        state.text_problems.append('the dump contains NUL bytes')
    for raw in text.split('\n'):
        line = raw[:-1] if raw.endswith('\r') else raw
        if line.startswith('IA64STATE GR '):
            match = _GENERAL.match(line)
            if match is None:
                state._malformed.setdefault('GR', []).append(line)
            else:
                state._general.setdefault(int(match.group('index'), 16), []).append(
                    (int(match.group('value'), 16), int(match.group('nat'))))
            continue
        if line.startswith('IA64STATE BR '):
            match = _BRANCH.match(line)
            if match is None:
                state._malformed.setdefault('BR', []).append(line)
            else:
                state._branch.setdefault(int(match.group('index'), 16), []).append(int(match.group('value'), 16))
            continue
        if _TLB_PREFIX.match(line):
            match = _TLB.match(line)
            if match is None:
                state._tlb_problems.append('malformed TLB line %r' % line)
            else:
                fields = match.groupdict()
                state._tlb.append(TlbEntry(
                    fields['kind'], int(fields['index']), fields['type'] == 'TR', int(fields['va'], 16),
                    int(fields['pa'], 16), int(fields['ps'], 16), int(fields['rid'], 16), int(fields['key'], 16),
                    int(fields['ar']), int(fields['pl']), int(fields['perm'], 16), int(fields['pte'], 16)))
            continue
        for key, (prefix, _) in _RECORDS.items():
            if line.startswith(prefix):
                state._lines[key].append(line)
                break
    return state


def schema_problems(state: CpuState) -> list[str]:
    try:
        version = state.value('SCHEMA', 'version')
    except StateError as error:
        return [str(error)]
    return [] if version == SCHEMA_VERSION else ['IA64STATE schema version %d, this parser reads %d'
                                                 % (version, SCHEMA_VERSION)]


# QMP access (the only human-monitor-command use of the IA-64 observers) ------

def read_text(qmp, cpu: int = 0) -> str:
    """`info registers` of CPU `cpu`, as QEMU prints it."""
    return qmp.hmp('info registers', cpu=cpu)


def read(qmp, cpu: int = 0) -> CpuState:
    return parse(read_text(qmp, cpu), cpu)
