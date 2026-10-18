# SPDX-License-Identifier: Apache-2.0
"""Independent map accounting rejects unsafe or inconsistent import records."""
import copy
import unittest
from types import SimpleNamespace as Record
from AsteriaVm.Observers import MemoryImport as M


def sample():
    data = M.MAP_RECORD.pack(0, 0, 512, 8, 7, 4) + M.MAP_RECORD.pack(512*M.PAGE, 0, 512, 8, 4, 2)
    e = Record(Published=1, MapRegions=2, Exclusions=1, Segments=1, UnitCount=0,
               BootServicesAllowed=1, GateComplete=1, UnconfinedFunctions=0, MasterRecorded=0, MasteringFunctions=0,
               MasterOutcome=[], MasterRequester=[], MasterClass=[],
               ExclusionStart=[600*M.PAGE], ExclusionBytes=[2*M.PAGE],
               SegmentStart=[0], SegmentPages=[1024], ManagedPages=1021, UnmanagedPages=0,
               MetadataBytes=2048, MetadataBudgetBytes=4096, UnitOutcome=[0], UnitKind=[1],
               UnitStatus=[0], UnitHead=[0], UnitTail=[0])
    a = Record(ManagedPages=1021, AllocatablePages=1021, RetiredPages=0,
               CleanPages=0, DirtyPages=1021, AllocatedPages=0, ClaimedPages=0)
    return Record(MemoryImport=e, Allocator=a), data


class MemoryImportTests(unittest.TestCase):
    def test_map_is_counted_with_outward_exclusions_and_page_zero_removed(self):
        record, data = sample()
        self.assertEqual(M.problems(record, data, require_all=True), [])
        record.MemoryImport.ExclusionStart[0] += 1
        record.MemoryImport.ExclusionBytes[0] -= 1
        self.assertEqual(M.problems(record, data, require_all=True), [])

    def test_authored_rmrr_requires_bootservices_provenance_and_exclusion(self):
        record, data = sample()
        self.assertEqual(M.rmrr_problems(record, data, 600*M.PAGE, 602*M.PAGE-1), [])
        self.assertTrue(M.rmrr_problems(record, data, 400*M.PAGE, 402*M.PAGE-1))
        record.MemoryImport.ExclusionBytes[0] = M.PAGE
        self.assertTrue(M.rmrr_problems(record, data, 600*M.PAGE, 602*M.PAGE-1))
        record.MemoryImport.BootServicesAllowed = 0
        self.assertTrue(M.rmrr_problems(record, data, 600*M.PAGE, 602*M.PAGE-1))

    def test_inconsistent_imports_are_rejected(self):
        original, data = sample()
        for part, field, value in (
                ('MemoryImport', 'Published', 0), ('MemoryImport', 'MapRegions', 3),
                ('MemoryImport', 'GateComplete', 0), ('MemoryImport', 'UnconfinedFunctions', 1),
                ('MemoryImport', 'ManagedPages', 1024), ('MemoryImport', 'Exclusions', 257),
                ('MemoryImport', 'Segments', 65), ('MemoryImport', 'MetadataBytes', 4097),
                ('Allocator', 'DirtyPages', 1020), ('Allocator', 'AllocatablePages', 1020)):
            with self.subTest(field=field):
                record = copy.deepcopy(original)
                setattr(getattr(record, part), field, value)
                self.assertTrue(M.problems(record, data))
        self.assertTrue(M.problems(original, None))

    def test_live_or_unaddressed_units_keep_bootservices_out(self):
        record, data = sample()
        record.MemoryImport.UnitCount = 1
        for status, head, tail in ((0, 0, 0), (1 << 31, 0, 0), (1 << 26, 0, 16)):
            record.MemoryImport.UnitStatus[0] = status
            record.MemoryImport.UnitHead[0] = head
            record.MemoryImport.UnitTail[0] = tail
            record.MemoryImport.UnitOutcome[0] = 0 if status == 0 else 2
            self.assertTrue(M.problems(record, data))
        record.MemoryImport.UnitOutcome[0] = 1
        self.assertEqual(M.problems(record, data), [])

    def test_metadata_limited_admission_is_accounted_but_six_gib_requires_all(self):
        record, data = sample()
        record.MemoryImport.SegmentPages[0] = 512
        record.MemoryImport.ManagedPages = 511
        record.MemoryImport.UnmanagedPages = 510
        record.Allocator.ManagedPages = record.Allocator.AllocatablePages = record.Allocator.DirtyPages = 511
        self.assertEqual(M.problems(record, data), [])
        self.assertTrue(M.problems(record, data, require_all=True))

    def test_non_wb_runtime_and_special_purpose_sources_are_not_managed(self):
        record, data = sample()
        record.MemoryImport.Segments = record.MemoryImport.ManagedPages = 0
        record.MemoryImport.UnmanagedPages = 0
        record.Allocator.ManagedPages = record.Allocator.AllocatablePages = record.Allocator.DirtyPages = 0
        for attr in (0, 8 | 1 << 63, 8 | 0x40000):
            excluded = M.MAP_RECORD.pack(0, 0, 1024, attr, 7, 4)
            record.MemoryImport.MapRegions = 1
            self.assertEqual(M.problems(record, excluded), [])
