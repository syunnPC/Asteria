# SPDX-License-Identifier: Apache-2.0
"""Independent physical-map admission and interval accounting."""
import struct

PAGE = 4096
LOW_END = 1 << 32
MAP_RECORD = struct.Struct('<QQQQII')
DUMP = 'MemoryMap'


def _subtract(ranges, exclusions):
    for low, high in sorted(exclusions):
        result = []
        for first, end in ranges:
            if high <= first or low >= end:
                result.append((first, end))
            else:
                if first < low:
                    result.append((first, low))
                if high < end:
                    result.append((high, end))
        ranges = result
    return ranges


def problems(record, data: bytes | None, *, require_all: bool = False) -> list[str]:
    e = record.MemoryImport
    errors = []
    if e.Published != 1:
        return ['memory import was not published']
    if data is None or len(data) != e.MapRegions * MAP_RECORD.size or e.MapRegions > 2048:
        return ['complete firmware map was not captured']
    if e.Exclusions > 256 or e.Segments > 64 or e.UnitCount > 8 or e.MasterRecorded > 64:
        return ['memory import inventory exceeds its bounds']
    if e.BootServicesAllowed and (not e.GateComplete or e.UnconfinedFunctions):
        errors.append('BootServices admitted without a complete bus-master gate')
    if e.MasterRecorded != min(e.MasteringFunctions, 64):
        errors.append('mastering-function outcomes were truncated before the recording bound')
    for i in range(e.MasterRecorded):
        if e.MasterOutcome[i] > 3 or e.MasterRequester[i] >= 1 << 32 or e.MasterClass[i] >= 1 << 24:
            errors.append('mastering-function %d has an invalid outcome/requester/class' % i)
    for i in range(e.UnitCount):
        if e.UnitOutcome[i] == 1:
            continue
        live = e.UnitOutcome[i] == 0 or e.UnitKind[i] == 2 or e.UnitStatus[i] & ((1 << 31) | (1 << 25))
        pending = e.UnitStatus[i] & (1 << 26) and e.UnitHead[i] != e.UnitTail[i]
        if e.BootServicesAllowed and (live or pending):
            errors.append('BootServices admitted while firmware unit %d can reach unenumerated pages' % i)
    sources = []
    for start, virtual, pages, attributes, firmware_type, kind in MAP_RECORD.iter_unpack(data):
        del virtual
        admitted_type = (firmware_type == 7 and kind == 4) or (
            e.BootServicesAllowed and firmware_type in (3, 4) and kind == 2)
        if admitted_type and attributes & 8 and not attributes & ((1 << 63) | 0x40000):
            if start % PAGE or start + pages * PAGE >= 1 << 64:
                errors.append('malformed source descriptor')
                continue
            first = max(start, PAGE)
            if first < start + pages * PAGE:
                sources.append((first, start + pages * PAGE))
    exclusions = [(e.ExclusionStart[i] // PAGE * PAGE,
                   (e.ExclusionStart[i] + e.ExclusionBytes[i] + PAGE - 1) // PAGE * PAGE)
                  for i in range(e.Exclusions)]
    ranges = _subtract(sources, exclusions)
    total = sum((end - first) // PAGE for first, end in ranges)
    managed = 0
    previous = 0
    for i in range(e.Segments):
        first, end = e.SegmentStart[i], e.SegmentStart[i] + e.SegmentPages[i] * PAGE
        if first % (2 << 20) or end % (2 << 20) or first < previous or first < LOW_END < end or first == end:
            errors.append('segment %d violates padding, ordering or the 4 GiB split' % i)
        previous = end
        managed += sum(max(0, min(end, high) - max(first, low)) // PAGE for low, high in ranges)
    if managed != e.ManagedPages or total - managed != e.UnmanagedPages:
        errors.append('map computes %d managed/%d unmanaged pages, import reports %d/%d' % (
            managed, total - managed, e.ManagedPages, e.UnmanagedPages))
    if require_all and (managed != total or e.UnmanagedPages):
        errors.append('large-memory case must manage the complete admitted map')
    if e.MetadataBytes > e.MetadataBudgetBytes:
        errors.append('segment metadata exceeds its arena budget')
    a = record.Allocator
    if a.ManagedPages != managed or a.AllocatablePages != managed - a.RetiredPages:
        errors.append('runtime allocator differs from the independently computed import')
    if a.CleanPages + a.DirtyPages + a.AllocatedPages + a.ClaimedPages + a.RetiredPages != managed:
        errors.append('allocator page partition differs from the independently computed import')
    return errors


def rmrr_problems(record, data: bytes, base: int, limit: int) -> list[str]:
    """Prove the authored reservation belongs to otherwise-admitted BSData."""
    e = record.MemoryImport
    if not e.BootServicesAllowed:
        return ['RMRR case must admit other BootServices pages']
    if not any(start <= base and limit < start + pages * PAGE and attributes & 8
               and firmware_type == 4 and kind == 2
               for start, _, pages, attributes, firmware_type, kind in MAP_RECORD.iter_unpack(data)):
        return ['authored RMRR does not lie entirely inside WB BootServicesData']
    if not any(e.ExclusionStart[i] <= base and limit < e.ExclusionStart[i] + e.ExclusionBytes[i]
               for i in range(e.Exclusions)):
        return ['authored RMRR is missing from the import exclusions']
    return problems(record, data, require_all=True)
