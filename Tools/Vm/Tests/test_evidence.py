# SPDX-License-Identifier: Apache-2.0
"""The evidence record reader (AsteriaVm.Evidence): fact line, layout from
header assertions, header validation, decoding and reading through QMP."""
from __future__ import annotations

import pathlib
import struct
import tempfile
import unittest

from AsteriaVm import Evidence
from AsteriaVm.Observers import Ia64Evidence

ROOT = pathlib.Path(__file__).resolve().parents[3]
COMMON = '''
struct EvidenceRecordHeader
{
    Foundation::UInt64 Magic;
    Foundation::UInt32 Schema;
    Foundation::UInt32 SizeBytes;
};
inline constexpr Foundation::UInt64 EvidenceRecordMagic{0x5244'4956'4554'5341ULL};
static_assert(__builtin_offsetof(EvidenceRecordHeader, Magic) == 0U);
static_assert(__builtin_offsetof(EvidenceRecordHeader, Schema) == 8U);
static_assert(__builtin_offsetof(EvidenceRecordHeader, SizeBytes) == 12U);
static_assert(sizeof(EvidenceRecordHeader) == 16U);
'''
RECORD = '''
inline constexpr Foundation::UInt32 TestSchema{3U};
inline constexpr Foundation::Size Slots{2U};
struct Part
{
    Foundation::UInt64 Published;  // set last
    Foundation::UInt8 Bytes[3U];
    Foundation::UInt16 Word;
};
struct TestRecord
{
    Kernel::Diagnostics::EvidenceRecordHeader Header;
    /// Two parts.
    Part Parts[Slots];
    Foundation::UInt32 Values[2U];
};
static_assert(__builtin_offsetof(Part, Published) == 0U);
static_assert(__builtin_offsetof(Part, Bytes) == 8U);
static_assert(__builtin_offsetof(Part, Word) == 12U);
static_assert(sizeof(Part) == 16U);
static_assert(__builtin_offsetof(TestRecord, Header) == 0U);
static_assert(__builtin_offsetof(TestRecord, Parts) == 16U);
static_assert(__builtin_offsetof(TestRecord, Values) == 48U);
static_assert(sizeof(TestRecord) == 56U);
'''


class TreeTest(unittest.TestCase):
    """A temporary tree with the common header and one record header."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        (self.root / 'Source/Kernel/Diagnostics').mkdir(parents=True)
        (self.root / Evidence.COMMON_HEADER).write_text(COMMON)

    def tearDown(self):
        self.directory.cleanup()

    def load(self, record: str = RECORD) -> Evidence.Schema:
        (self.root / 'Record.hpp').write_text(record)
        return Evidence.Schema.load(self.root, ['Record.hpp'], 'TestRecord', 'TestSchema')

    @staticmethod
    def data(schema=3, size=56, magic=0x5244495645545341) -> bytes:
        parts = b''.join(struct.pack('<Q3sxH2x', index + 1, bytes([index, 1, 2]), 0x1234 + index)
                         for index in range(2))
        return struct.pack('<QII', magic, schema, size) + parts + struct.pack('<2I', 7, 8)


class LayoutTests(TreeTest):
    def test_signed_clock_residuals_decode_as_signed_values(self):
        schema = self.load(RECORD.replace('Foundation::UInt64 Published;', 'Foundation::Int64 Published;'))
        data = bytearray(self.data())
        struct.pack_into('<q', data, 16, -1234)
        self.assertEqual(schema.decode(data).Parts[0].Published, -1234)

    def test_a_shared_header_supplies_an_array_bound(self):
        (self.root / 'Shared.hpp').write_text('inline constexpr Foundation::Size SharedSlots{2U};\n')
        (self.root / 'Record.hpp').write_text(RECORD.replace('Slots{2U}', 'Slots{SharedSlots}'))
        schema = Evidence.Schema.load(self.root, ['Record.hpp', 'Shared.hpp'], 'TestRecord', 'TestSchema')
        self.assertEqual(schema.record.field('Parts').count, 2)
        self.assertEqual(schema.record.size, 56)

    def test_the_layout_follows_the_declarations_and_assertions(self):
        schema = self.load()
        self.assertEqual((schema.magic, schema.version, schema.record.size), (0x5244495645545341, 3, 56))
        self.assertEqual([(field.name, field.offset, field.size) for field in schema.record.fields],
                         [('Header', 0, 16), ('Parts', 16, 32), ('Values', 48, 8)])
        record = schema.decode(self.data())
        self.assertEqual(record.Header.Schema, 3)
        self.assertEqual([(part.Published, part.Bytes, part.Word) for part in record.Parts],
                         [(1, b'\x00\x01\x02', 0x1234), (2, b'\x01\x01\x02', 0x1235)])
        self.assertEqual(record['Values'], [7, 8])

    def test_a_layout_the_assertions_contradict_is_refused(self):
        for old, new in (('== 12U);\nstatic_assert(sizeof(Part)', '== 11U);\nstatic_assert(sizeof(Part)'),
                         ('sizeof(TestRecord) == 56U', 'sizeof(TestRecord) == 64U'),
                         ('static_assert(__builtin_offsetof(Part, Word) == 12U);\n', ''),
                         ('Foundation::UInt16 Word;', 'Foundation::UInt16 Word, Other;'),
                         ('Part Parts[Slots];', 'Part Parts[Unknown];'),
                         ('struct TestRecord', 'struct TestRecord2')):
            with self.subTest(new):
                self.assertIn(old, RECORD)
                with self.assertRaises(Evidence.SchemaError):
                    self.load(RECORD.replace(old, new))

    def test_the_record_must_start_with_the_common_header(self):
        headless = RECORD.replace('    Kernel::Diagnostics::EvidenceRecordHeader Header;\n', '').replace(
            'static_assert(__builtin_offsetof(TestRecord, Header) == 0U);\n', '').replace(
            '(TestRecord, Parts) == 16U', '(TestRecord, Parts) == 0U').replace(
            '(TestRecord, Values) == 48U', '(TestRecord, Values) == 32U').replace(
            'sizeof(TestRecord) == 56U', 'sizeof(TestRecord) == 40U')
        with self.assertRaises(Evidence.SchemaError) as raised:
            self.load(headless)
        self.assertIn('does not start with', str(raised.exception))


class HeaderTests(TreeTest):
    def test_header_problems(self):
        schema = self.load()
        self.assertEqual(schema.header_problems(self.data()), [])
        for data in (self.data(schema=4), self.data(size=64), self.data(magic=1), self.data()[:8]):
            self.assertTrue(schema.header_problems(data))
            with self.assertRaises(Evidence.EvidenceError):
                schema.decode(data)

    def test_read_through_qmp(self):
        schema = self.load()
        memory = {0x4000: self.data()}
        calls = []

        class FakeQmp:
            def pmemsave(self, address, size):
                calls.append((address, size))
                return memory[address][:size]

        self.assertEqual(schema.read(FakeQmp(), 0x4000), self.data())
        self.assertEqual(calls, [(0x4000, 16), (0x4000, 56)])
        memory[0x4000] = self.data(schema=2)
        with self.assertRaises(Evidence.EvidenceError):
            schema.read(FakeQmp(), 0x4000)


class SaveTests(unittest.TestCase):
    def test_the_artifact_follows_the_record_header(self):
        record = struct.pack('<QII', 1, 1, 24) + bytes(range(8))
        line = 'ASTERIA:EVIDENCE:0x0000000000004000'

        class FakeQmp:
            def __init__(self, data):
                self.data = data

            def pmemsave(self, address, size):
                assert address == 0x4000
                return self.data[:size]

        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / Evidence.ARTIFACT
            self.assertIsNone(Evidence.save(FakeQmp(record + b'tail'), ['x', line], path))
            self.assertEqual(path.read_bytes(), record)
            path.unlink()
            self.assertIn('fact line', Evidence.save(FakeQmp(record), ['x'], path))
            self.assertIn('implausible', Evidence.save(FakeQmp(struct.pack('<QII', 1, 1, 8)), [line], path))
            self.assertFalse(path.exists())


class LocateTests(unittest.TestCase):
    def test_one_well_formed_fact_line(self):
        line = 'ASTERIA:EVIDENCE:0x000000000432B110'
        self.assertEqual(Evidence.locate(['ASTERIA:STAGE:BOOT', line]), (0x432B110, []))
        for lines in ([], [line, line], ['ASTERIA:EVIDENCE:0x432B110'], ['ASTERIA:EVIDENCE:0x000000000432b110']):
            address, problems = Evidence.locate(lines)
            self.assertIsNone(address, lines)
            self.assertTrue(problems, lines)


class Ia64SchemaTests(unittest.TestCase):
    def test_the_ia64_record_is_described_by_its_headers(self):
        schema = Ia64Evidence.schema(ROOT)
        self.assertEqual([field.name for field in schema.record.fields],
                         ['Header', 'Processors', 'Pci', 'Ahci', 'ApplicationProcessors', 'SharedRootShootdown',
                          'UserProbe', 'NativePreemption', 'UserPreemption', 'ObjectManagement', 'Threads',
                          'Domains', 'Components', 'Collection', 'DeviceControl', 'Services', 'Interrupts',
                          'Stops', 'Iommu', 'BootFiles', 'Block', 'IommuBackend', 'Metadata', 'Allocator', 'MemoryImport', 'ApplicationLaunch', 'Watchdog', 'Clocks', 'UserProcessors'])
        self.assertEqual(schema.version, 32)
        self.assertEqual(schema.record.field('Threads').element.field('LongestDeferral').offset, 136)
        self.assertEqual(schema.record.field('Stops').offset, 4048)
        self.assertEqual(schema.record.field('Iommu').offset, 4672)
        self.assertEqual(schema.record.field('Block').offset, 5848)
        self.assertEqual(schema.record.field('Block').element.field('Released').count, 2)
        self.assertEqual(schema.record.field('IommuBackend').offset, 6016)
        self.assertEqual(schema.record.size, 19256)
        self.assertEqual(schema.record.field('IommuBackend').element.field('Units').count, 4)
        processors = schema.record.field('ApplicationProcessors').element.field('Processors')
        self.assertEqual(processors.count, 4)
        self.assertEqual(set(Ia64Evidence.outcomes(ROOT)), {'None', 'NoDisk', 'Identified', 'Read'})


class IommuQueuePlacementTests(unittest.TestCase):
    def test_queues_must_stay_in_the_executing_copy(self):
        # Reject the firmware source and reservations crossing the copied
        # BSS boundary; neither may supply live DMA descriptors after relocation.
        record = Evidence.Record(
            Iommu=Evidence.Record(Published=1, Present=1, Ready=1, Bypass=0, Requester=56,
                                  SweptFunctions=7, QueuePages=8, QueuePhysical=0x2300000),
            IommuBackend=Evidence.Record(Backend=Ia64Evidence.BACKEND_VIRTIO),
            Pci=Evidence.Record(PublishedFunctionCount=7))
        pci = [{'devices': [{'bus': 0, 'slot': 7, 'function': 0,
                             'id': {'vendor': 0x1AF4, 'device': 0x1057}, 'class_info': {'class': 0}}]}]
        writable = (0x2300000, 0x2340000)
        self.assertEqual(Ia64Evidence.iommu_problems(record, pci, present=True, queue_range=writable), [])
        for address in (0x1DF00000, writable[0] - 4096, writable[1] - 4096):
            with self.subTest(address=address):
                record.Iommu['QueuePhysical'] = address
                self.assertIn('queue reservation is outside the executing image writable section',
                              Ia64Evidence.iommu_problems(record, pci, present=True, queue_range=writable))



class VtdBackendTests(unittest.TestCase):
    """The VT-d facts of a confined edu run on the 7500-class hub."""

    WRITABLE = (0x4C00000, 0x5000000)

    @staticmethod
    def unit(base: int, served: int, faults: int, flushes: int, coherent: bool = False) -> Evidence.Record:
        return Evidence.Record(RegisterBase=base, State=Ia64Evidence.VTD_TRANSLATING, DisabledReason=0,
                               ConfigureError=0, EnableError=0, Version=0x10, Levels=4, Status=1 << 31,
                               ExtendedCapability=0xF0F0A | int(coherent), ServedFunctions=served,
                               Faults=faults, TableFlushes=flushes)

    def record(self, coherent: bool = False) -> Evidence.Record:
        flushes = 0 if coherent else 40
        empty = Evidence.Record(RegisterBase=0, State=0)
        return Evidence.Record(
            Iommu=Evidence.Record(Published=1, Present=0, Ready=1, Bypass=0, SweptFunctions=2,
                                  DomainsCreated=1, DomainsReclaimed=1, Faults=1, LastFaultSource=6 << 3,
                                  LastFaultReason=5, LastFaultAddress=0x200000),
            IommuBackend=Evidence.Record(Published=1, Backend=Ia64Evidence.BACKEND_VTD, SelectionError=0,
                                         HostAddressBits=50, UnitCount=2, SweptFunctions=2,
                                         TablePhysical=0x4D00000, TablePages=74, LastFaultRead=0,
                                         Units=[self.unit(0xFED91000, 1, 0, flushes, coherent),
                                                self.unit(0xFED90000, 4, 1, flushes, coherent), empty, empty]),
            Pci=Evidence.Record(PublishedFunctionCount=5))

    PCI = [{'devices': [{'bus': 0, 'slot': slot, 'function': 0, 'id': {'vendor': vendor, 'device': device},
                         'class_info': {'class': kind}}
                        for slot, vendor, device, kind in ((0, 0x8086, 0x1237, 0x0600), (3, 0x1002, 0x515E, 0x0300),
                                                           (6, 0x1234, 0x11E8, 0x00FF), (7, 0x1B36, 0x0001, 0x0604))]}]

    def problems(self, record: Evidence.Record, vtd: Ia64Evidence.VtdExpectation) -> list[str]:
        return Ia64Evidence.iommu_problems(record, self.PCI, present=True, drivers=1, queue_range=self.WRITABLE,
                                           vtd=vtd)

    def test_a_confined_run_passes_with_and_without_coherency(self):
        self.assertEqual(self.problems(self.record(), Ia64Evidence.VtdExpectation()), [])
        self.assertEqual(self.problems(self.record(True), Ia64Evidence.VtdExpectation(coherent=True)), [])

    def test_flushes_must_follow_ecap_c(self):
        self.assertTrue(self.problems(self.record(True), Ia64Evidence.VtdExpectation()))
        self.assertTrue(self.problems(self.record(), Ia64Evidence.VtdExpectation(coherent=True)))
        record = self.record()
        record.IommuBackend.Units[1]['TableFlushes'] = 0
        self.assertTrue(self.problems(record, Ia64Evidence.VtdExpectation()))

    def test_each_backend_fact_is_judged(self):
        for part, field, value in (('IommuBackend', 'Backend', Ia64Evidence.BACKEND_VIRTIO),
                                   ('IommuBackend', 'HostAddressBits', 48),
                                   ('IommuBackend', 'SelectionError', 7),
                                   ('IommuBackend', 'SweptFunctions', 4),
                                   ('IommuBackend', 'TablePhysical', 0x1000000),
                                   ('Iommu', 'Present', 1),
                                   ('Iommu', 'LastFaultReason', 2)):
            with self.subTest(field=field):
                record = self.record()
                record[part][field] = value
                self.assertTrue(self.problems(record, Ia64Evidence.VtdExpectation()))
        record = self.record()
        record.IommuBackend.Units[0]['State'] = 1
        self.assertTrue(self.problems(record, Ia64Evidence.VtdExpectation()))
        record = self.record()
        record.IommuBackend.Units[0]['Faults'] = 1
        self.assertTrue(self.problems(record, Ia64Evidence.VtdExpectation()))
        high = Ia64Evidence.VtdExpectation(bases=(0x10000FED91000, 0x10000FED90000))
        self.assertTrue(self.problems(self.record(), high))


if __name__ == '__main__':
    unittest.main()
