# SPDX-License-Identifier: Apache-2.0
"""Translation observers (Observers.Ia64Translation): translation capacity, mutation checks and the derived bootstrap base.

The recorded synthetic-probe dump passes; mutants
(TR/TC tuples, permissions, W^X, deletions, duplicates, aliases, region
registers, image geometry) fail. The bootstrap reservation's base is derived
from the run: moving the runtime DTR to another aligned base still passes;
moving one image TR independently breaks coverage.
"""
from __future__ import annotations

import dataclasses
import unittest

from AsteriaVm import Ia64Layout
from AsteriaVm.Drivers import Ia64SyntheticProbe
from AsteriaVm.Observers import Ia64Image, Ia64State, Ia64Translation
from AsteriaVm.Observers.Ia64State import TlbEntry
from Tests.test_ia64_fixtures import ROOT, expectation, lines, registers

TLB_FORMAT = '%s[%u] %s va=0x%016x pa=0x%016x ps=0x%016x rid=0x%06x key=0x%06x ar=%u pl=%u perm=0x%x pte=0x%016x'
BASE = 0x2000000


def render(entry: TlbEntry) -> str:
    """The line QEMU's ia64_dump_tlb prints for `entry`."""
    return TLB_FORMAT % (entry.kind, entry.index, 'TR' if entry.is_register else 'TC', entry.va, entry.pa,
                         entry.ps, entry.rid, entry.key, entry.ar, entry.pl, entry.perm, entry.pte)


def rewrite(text: str, transform) -> str:
    """Applies `transform(entry) -> entry | list | None` to every TLB line of a dump."""
    output = []
    for raw in text.split('\n'):
        line = raw.rstrip('\r')
        if line.startswith(('ITLB[', 'DTLB[')):
            entry = Ia64State.parse(line + '\n').tlb()[0]
            result = transform(entry)
            results = [] if result is None else result if isinstance(result, list) else [result]
            output.extend(render(item) for item in results)
        else:
            output.append(line)
    return '\n'.join(output)


def move(entry: TlbEntry, delta: int) -> TlbEntry:
    return dataclasses.replace(entry, va=entry.va + delta, pa=entry.pa + delta, pte=entry.pte + delta)


class KernelTranslationTests(unittest.TestCase):
    def setUp(self):
        self.text = registers('SyntheticProbe')
        self.expected = expectation('SyntheticProbe')
        loaded, _ = Ia64Image.loaded_image(lines('SyntheticProbe'))
        self.layout = Ia64Image.KernelLayout(self.expected.facts, loaded)
        self.translations = Ia64SyntheticProbe.kernel_translations(self.expected.constants,
                                                                    self.expected.runtime_memory_page)
        self.base = next(entry.va for entry in Ia64State.parse(self.text).translation_registers('DTLB')
                         if entry.ps == self.expected.runtime_memory_page)

    def problems(self, text: str, layout=None, translations=None) -> tuple[list[str], int | None]:
        return Ia64Translation.kernel_translation_problems(Ia64State.parse(text), layout or self.layout,
                                                           translations or self.translations)

    def test_recorded(self):
        self.assertEqual(self.problems(self.text), ([], self.base))

    def test_the_bootstrap_base_is_derived_from_the_run(self):
        for delta in (-0x1000000, -0x2000000):
            moved = rewrite(self.text, lambda entry: move(entry, delta)
                            if entry.is_register and entry.kind == 'DTLB' and entry.ps == 0x1000000 else entry)
            self.assertNotEqual(Ia64State.parse(moved).tlb(), Ia64State.parse(self.text).tlb())
            self.assertEqual(self.problems(moved), ([], self.base + delta))

    def test_inconsistent_image_translations_fail(self):
        def only(kind, index, delta):
            return lambda entry: (move(entry, delta) if (entry.kind, entry.index) == (kind, index)
                                  and entry.is_register else entry)
        mutants = {
            'text ITR only moved': only('ITLB', 1, 0x400000),
            'text DTR only moved': only('DTLB', 1, 0x400000),
            'writable image DTR only moved': only('DTLB', 3, 0x400000),
            'misaligned runtime reservation': only('DTLB', 0, 0x4000),
        }
        for name, transform in mutants.items():
            changed = rewrite(self.text, transform)
            self.assertNotEqual(Ia64State.parse(changed).tlb(), Ia64State.parse(self.text).tlb(), name)
            self.assertTrue(self.problems(changed)[0], name)

    def test_h19_register_mutants(self):
        def change(kind, slot, **fields):
            return lambda entry: (dataclasses.replace(entry, **fields)
                                  if (entry.kind, entry.index) == (kind, slot) and entry.is_register else entry)
        first_read_only = next(e.index for e in Ia64State.parse(self.text).translation_registers('DTLB') if e.ar == 0)
        first_read_write = next(e.index for e in Ia64State.parse(self.text).translation_registers('DTLB')
                                if e.ar == 2 and e.ps != self.expected.runtime_memory_page)
        last_dtr = max(e.index for e in Ia64State.parse(self.text).translation_registers('DTLB') if e.is_register)
        mutants = {
            'ITR slot 0': change('ITLB', 1, index=0),
            'rid of every TR': lambda entry: dataclasses.replace(entry, rid=0x1000) if entry.is_register else entry,
            'key': change('ITLB', 1, key=1),
            'not identity (va)': change('ITLB', 1, va=0),
            'not identity (pa)': change('DTLB', 0, pa=0),
            'text ITR deleted': lambda entry: None if (entry.kind, entry.index) == ('ITLB', 1) else entry,
            'read-only image DTR deleted': lambda entry: None if (entry.kind, entry.index) == ('DTLB', 2) else entry,
            'text DTR deleted': lambda entry: None if (entry.kind, entry.index) == ('DTLB', 1) else entry,
            'runtime DTR deleted': lambda entry: None if (entry.kind, entry.index) == ('DTLB', 0) else entry,
            'writable image DTR deleted': lambda entry: None if (entry.kind, entry.index) == ('DTLB', 3) else entry,
            'text DTR page size': change('DTLB', 1, ps=0x1000),
            'text DTR read-only': change('DTLB', 1, ar=0, perm=1,
                                         pte=self.layout.section('.text')[0] | 0x0010000000000021),
            'non-insertable page size': change('DTLB', 0, ps=0x3000),
            'ITR access rights': change('ITLB', 1, ar=7, perm=7),
            'ITR at PL3': change('ITLB', 1, pl=3),
            'ITR pte flags': change('ITLB', 1, pte=self.layout.section('.text')[0] | 0x0010000000000220),
            'writable read-only data': change('DTLB', first_read_only, ar=2, perm=3,
                                              pte=self.layout.section('.note.asteria')[0] | 0x0010000000000461),
            'read-only data section': change('DTLB', first_read_write, ar=0, perm=1,
                                             pte=self.layout.section('.data')[0] | 0x0010000000000021),
            'duplicated DTR0': lambda entry: [entry, entry] if (entry.kind, entry.index) == ('DTLB', 0)
            and entry.is_register else entry,
            'extra DTR': lambda entry: [entry, dataclasses.replace(entry, index=entry.index + 1, va=0x5000000,
                                                                   pa=0x5000000, pte=0x0010000005000461)]
            if (entry.kind, entry.index) == ('DTLB', last_dtr) else entry,
            'header gap mapped': change('ITLB', 1, va=self.layout.loaded.base, pa=self.layout.loaded.base,
                                         ps=0x400000, pte=self.layout.loaded.base | 0x0010000000000221),
        }
        for name, transform in mutants.items():
            changed = rewrite(self.text, transform)
            self.assertNotEqual(Ia64State.parse(changed).tlb(), Ia64State.parse(self.text).tlb(), name)
            self.assertTrue(self.problems(changed)[0], name)
        self.assertTrue(self.problems(self.text + 'ITLB[999] TC va=0x0000000006000000 pa=0x0000000006000000\n')[0])

    def test_image_geometry_mutants(self):
        # Loaded-image mutants: another base, a zero or
        # different size, an image overlapping the bootstrap reservation.
        loaded = self.layout.loaded
        for image in (dataclasses.replace(loaded, base=0x1000),
                      dataclasses.replace(loaded, size=loaded.size + 0x1000),
                      dataclasses.replace(loaded, base=BASE + 0x8000)):
            layout = Ia64Image.KernelLayout(self.expected.facts, image)
            self.assertTrue(self.problems(self.text, layout)[0], image)

    def test_capacity(self):
        # The recorded TRs fit capacities of exactly their count (plus the
        # firmware's ITR); one register less does not.
        state = Ia64State.parse(self.text)
        instruction, data = (len(state.translation_registers(kind)) for kind in ('ITLB', 'DTLB'))
        tight = dataclasses.replace(self.translations, instruction_capacity=instruction + 1, data_capacity=data)
        self.assertEqual(self.problems(self.text, translations=tight)[0], [])
        for translations in (dataclasses.replace(tight, instruction_capacity=instruction),
                             dataclasses.replace(tight, data_capacity=data - 1),
                             dataclasses.replace(tight, firmware_instruction_registers=2)):
            self.assertTrue(self.problems(self.text, translations=translations)[0])


def with_runtime_reservation(text: str, *entries: TlbEntry) -> str:
    """The dump with its run-time memory reservation DTRs (read/write data TRs
    of 16 MiB) replaced by `entries`, the data TRs in address order and every
    DTLB line numbered again from zero, TRs first, as QEMU prints them."""
    state = Ia64State.parse(text)
    data = [entry for entry in state.translation_registers('DTLB') if (entry.ps, entry.ar) != (RUNTIME_PAGE, 2)]
    data = sorted(data + list(entries), key=lambda entry: entry.va)
    cache = [entry for entry in state.translation_cache() if entry.kind == 'DTLB']
    numbered = [render(dataclasses.replace(entry, index=index)) for index, entry in enumerate(data + cache)]
    other = [line for line in text.split('\n') if not line.startswith('DTLB[')]
    position = next(index for index, line in enumerate(text.split('\n')) if line.startswith('DTLB['))
    return '\n'.join(other[:position] + numbered + other[position:])


RUNTIME_PAGE = 0x1000000
RUNTIME_BASE = 0x1000000


def runtime_dtr(va: int = RUNTIME_BASE, ps: int = RUNTIME_PAGE, ar: int = 2, perm: int = 3,
                flags: int = 0x461) -> TlbEntry:
    """A kernel run-time memory reservation DTR as the initial encoder writes it."""
    return TlbEntry('DTLB', 0, True, va, va, ps, 0x2000, 0, ar, 0, perm, 0x0010000000000000 | va | flags)


class RuntimeReservationTests(unittest.TestCase):
    """The run-time memory reservation's DTR (Ia64TranslationRegisterBudget.md)
    on the recorded synthetic-probe dump: exactly one read/write 16 MiB DTR
    outside the image, containing the bootstrap data, wherever it sorts."""

    def setUp(self):
        self.text = registers('SyntheticProbe')
        self.expected = expectation('SyntheticProbe')
        loaded, _ = Ia64Image.loaded_image(lines('SyntheticProbe'))
        self.layout = Ia64Image.KernelLayout(self.expected.facts, loaded)
        self.translations = Ia64SyntheticProbe.kernel_translations(self.expected.constants,
                                                                    self.expected.runtime_memory_page)
        self.base = next(entry.va for entry in Ia64State.parse(self.text).translation_registers('DTLB')
                         if entry.ps == self.expected.runtime_memory_page)

    def problems(self, *entries: TlbEntry) -> list[str]:
        return Ia64Translation.kernel_translation_problems(
            Ia64State.parse(with_runtime_reservation(self.text, *entries)), self.layout, self.translations)[0]

    def test_the_page_comes_from_the_tree(self):
        self.assertEqual(self.expected.runtime_memory_page, RUNTIME_PAGE)

    def test_one_reservation_passes_wherever_it_sorts(self):
        for base in (RUNTIME_BASE, 0x8000000, 0x20000000):    # before and after the recorded image
            self.assertEqual(self.problems(runtime_dtr(base)), [], hex(base))

    def test_mutants(self):
        mutants = {
            'missing': (),
            'two': (runtime_dtr(), runtime_dtr(0x8000000)),
            'a 4 MiB page': (runtime_dtr(ps=0x400000),),
            'read-only': (runtime_dtr(ar=0, perm=1, flags=0x21),),
            'executable': (runtime_dtr(ar=3, perm=7, flags=0x661),),
            'not identity': (dataclasses.replace(runtime_dtr(), pa=0x2000000),),
            'not naturally aligned': (runtime_dtr(0x1800000),),
            'inside the image': (runtime_dtr(self.layout.loaded.base),),
        }
        for name, entries in mutants.items():
            self.assertTrue(self.problems(*entries), name)


class ConsolidatedKernelTranslationTests(unittest.TestCase):
    """The four-DTR plan also fits a CPU with only eight slots of each kind."""

    def setUp(self):
        recorded = expectation('SyntheticProbe')
        self.expected = Ia64SyntheticProbe.kernel_translations(recorded.constants, RUNTIME_PAGE)
        self.expected = dataclasses.replace(self.expected, instruction_capacity=8, data_capacity=8)
        loaded = Ia64Image.LoadedImage(0x10000000, 0x1000000, 0x4000000)
        sections = {'.text': (0x10400000, 0x10800000), '.note.asteria': (0x10800000, 0x10801000),
                    'ReadOnlyBody': (0x10801000, 0x10bff000), 'ReadOnlyTail': (0x10bff000, 0x10c00000), '.data': (0x10c00000, 0x10fff000),
                    '.bss': (0x10fff000, 0x11000000)}
        from types import SimpleNamespace
        self.layout = SimpleNamespace(loaded=loaded, section=lambda name: sections[name])
        grants = (self.expected.text, self.expected.read_only, self.expected.read_write)
        data = [runtime_dtr()] + [
            TlbEntry('DTLB', index + 1, True, address, address, 0x400000, 0x2000, 0,
                     grant.ar, 0, grant.perm, address | self.expected.exception_deferral | grant.flags)
            for index, (address, grant) in enumerate(zip((0x10400000, 0x10800000, 0x10c00000), grants))]
        instruction = dataclasses.replace(data[1], kind='ITLB', index=1)
        self.entries = data + [instruction]

    def problems(self, entries):
        return Ia64Translation.kernel_translation_problems(
            Ia64State.parse('\n'.join(render(entry) for entry in entries) + '\n'), self.layout, self.expected)[0]

    def test_architectural_budget_and_mutants(self):
        self.assertEqual(self.problems(self.entries), [])
        mutants = (self.entries[1:], self.entries[:-1], self.entries + [self.entries[-1]],
                   [dataclasses.replace(e, ps=0x1000) if e.index == 0 else e for e in self.entries],
                   [dataclasses.replace(e, perm=7) if e.index == 0 else e for e in self.entries])
        for entries in mutants:
            self.assertTrue(self.problems(entries))


class SectionTests(unittest.TestCase):
    def test_recorded_and_mutants(self):
        facts = expectation('SyntheticProbe').facts
        self.assertEqual(Ia64Translation.kernel_section_problems(facts, 0x8000), [])
        sections = dict(facts.kernel_sections)
        for name, extent in (('ReadOnlyBody', (sections['ReadOnlyBody'][0] + 0x1000, sections['ReadOnlyBody'][1])),
                             ('.data', (sections['.data'][0] + 0x10, sections['.data'][1])),
                             ('.bss', (sections['.bss'][0], 0))):
            broken = dataclasses.replace(facts, kernel_sections={**sections, name: extent})
            self.assertTrue(Ia64Translation.kernel_section_problems(broken, 0x8000), name)
        symbols = dict(facts.kernel_symbols)
        symbols['AsteriaIa64KernelIdle'] = Ia64Image.Symbol(sections['ReadOnlyBody'][0], 0x10)
        self.assertTrue(Ia64Translation.kernel_section_problems(dataclasses.replace(facts, kernel_symbols=symbols),
                                                                0x8000))


class UserTranslationTests(unittest.TestCase):
    def setUp(self):
        self.text = registers('SyntheticProbe')
        self.constants = expectation('SyntheticProbe').constants
        self.pages = Ia64SyntheticProbe.user_pages(self.constants)

    def problems(self, text: str) -> list[str]:
        return Ia64Translation.user_translation_cache_problems(Ia64State.parse(text), self.pages,
                                                               self.constants.user_rid(1),
                                                               self.constants.pte_exception_deferral)

    def test_recorded(self):
        self.assertEqual(self.problems(self.text), [])
        self.assertTrue(self.problems(registers('Smp4')))

    def test_h19_cache_mutants(self):
        def at(va, **fields):
            return lambda entry: (dataclasses.replace(entry, **fields)
                                  if entry.va == va and not entry.is_register else entry)
        code, stack, spill, result = 0x2000000001001000, 0x2000000002004000, 0x2000000003001000, 0x2000000003004000
        cache = {entry.va: entry for entry in Ia64State.parse(self.text).translation_cache()}
        mutants = {'code deleted': lambda entry: None if entry.va == code else entry,
                   'stack deleted': lambda entry: None if entry.va == stack else entry,
                   'spill deleted': lambda entry: None if entry.va == spill else entry,
                   'result deleted': lambda entry: None if entry.va == result else entry,
                   'code at PL0': at(code, pl=0), 'code writable': at(code, ar=2, perm=3),
                   'code RWX': at(code, perm=7), 'stack execute': at(stack, ar=1, perm=5),
                   'spill read-only': at(spill, perm=1), 'result read-only': at(result, perm=1),
                   'code in the kernel region': at(code, rid=0x2000),
                   'code pte flags': at(code, pte=cache[code].pte ^ 1),
                   'stack aliases code': at(stack, pa=cache[code].pa,
                                            pte=cache[code].pa | self.constants.pte_exception_deferral
                                            | self.constants.pte_user_rw),
                   'result aliases spill': at(result, pa=cache[spill].pa, pte=cache[spill].pte),
                   'code in the DTLB': at(code, kind='DTLB'), 'large page': at(stack, ps=0x4000),
                   'duplicate': lambda entry: [entry, entry] if entry.va == stack else entry}
        for name, transform in mutants.items():
            changed = rewrite(self.text, transform)
            self.assertNotEqual(Ia64State.parse(changed).tlb(), Ia64State.parse(self.text).tlb(), name)
            self.assertTrue(self.problems(changed), name)
        extra = ('ITLB[99] TC va=0x2000000004001000 pa=0x0000000001003000 ps=0x0000000000001000 rid=0x002011'
                 ' key=0x000000 ar=1 pl=3 perm=0x5 pte=0x00100000010033a1\n')
        self.assertTrue(self.problems(self.text + extra))


class RegionRegisterTests(unittest.TestCase):
    def test_recorded_and_mutants(self):
        constants = expectation('SyntheticProbe').constants
        expected = {0: Ia64Translation.region_register(constants.kernel_rid, 12)}
        expected.update({r: Ia64Translation.region_register(constants.user_rid(r), 12) for r in (5, 6, 7)})
        self.assertEqual(expected, {0: 0x200030, 5: 0x201530, 6: 0x201630, 7: 0x201730})
        text = registers('SyntheticProbe')
        self.assertEqual(Ia64Translation.region_register_problems(Ia64State.parse(text), expected), [])
        for old, new in (('RR0: 0x0000000000200030', 'RR0: 0x0000000000100030'),
                         ('RR6: 0x0000000000201630', 'RR6: 0x0000000000201631'), (' RR0: 0x0000000000200030', '')):
            state = Ia64State.parse(text.replace(old, new))
            self.assertTrue(Ia64Translation.region_register_problems(state, expected), old)

    def test_access_is_derived_from_the_pte_flags(self):
        constants = Ia64Layout.headers(ROOT)
        self.assertEqual(Ia64Translation.access('RX', constants.pte_kernel_rx),
                         Ia64Translation.Access('RX', 1, 5, 0x221))
        self.assertEqual(Ia64Translation.access('R', constants.pte_kernel_r), Ia64Translation.Access('R', 0, 1, 0x21))
        self.assertEqual(Ia64Translation.access('RW', constants.pte_user_rw), Ia64Translation.Access('RW', 2, 3, 0x5E1))
        with self.assertRaises(ValueError):
            Ia64Translation.access('bad', 0x7 << 9)


if __name__ == '__main__':
    unittest.main()
