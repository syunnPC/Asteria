// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_PHYSICAL_PAGE_IMPORT_EVIDENCE_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_PHYSICAL_PAGE_IMPORT_EVIDENCE_HPP_INCLUDED
#include "Foundation/Types.hpp"
namespace Asteria::Kernel::Memory
{
/// The map remains excluded and readable by the external runner. Raw intervals allow its own
/// admission computation.
struct PhysicalPageImportEvidence
{
    Foundation::UInt64 Published;
    Foundation::UInt64 MapPhysical;
    Foundation::UInt64 MapRegions;
    Foundation::UInt64 BootServicesAllowed;
    Foundation::UInt64 GateComplete;
    Foundation::UInt64 FunctionsChecked;
    Foundation::UInt64 MasteringFunctions;
    Foundation::UInt64 UnconfinedFunctions;
    Foundation::UInt64 ManagedPages;
    Foundation::UInt64 UnmanagedPages;
    Foundation::UInt64 MetadataBytes;
    Foundation::UInt64 MetadataBudgetBytes;
    Foundation::UInt64 LowWatermarkBlocks;
    Foundation::UInt64 Segments;
    Foundation::UInt64 Exclusions;
    Foundation::UInt64 SegmentStart[64U];
    Foundation::UInt64 SegmentPages[64U];
    Foundation::UInt64 ExclusionStart[256U];
    Foundation::UInt64 ExclusionBytes[256U];
    Foundation::UInt64 MasterRecorded;
    Foundation::UInt64 MasterRequester[64U];
    Foundation::UInt64 MasterClass[64U];
    Foundation::UInt64 AmdBaseRegisters[120U];
    Foundation::UInt64 UnitCount;
    Foundation::UInt64 UnitBase[8U];
    Foundation::UInt64 UnitKind[8U];
    Foundation::UInt64 UnitOutcome[8U];
    Foundation::UInt64 UnitReason[8U];
    Foundation::UInt64 UnitStatus[8U];
    Foundation::UInt64 UnitFaultStatus[8U];
    Foundation::UInt64 UnitQueue[8U];
    Foundation::UInt64 UnitHead[8U];
    Foundation::UInt64 UnitTail[8U];
    Foundation::UInt64 UnitRoot[8U];
    Foundation::UInt64 UnitInterruptTable[8U];
    Foundation::UInt64 UnitPageRequestQueue[8U];
    Foundation::UInt64 UnitAdvancedFaultLog[8U];
    Foundation::UInt64 BootInfoFaultRanges;
    Foundation::UInt64 MasterOutcome[64U];
    // Per AMD-Vi unit: command, event, PPR, GA, PPR B, event B head/tail.
    Foundation::UInt64 AmdQueuePointers[96U];
};
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, Published) == 0U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, MapPhysical) == 8U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, MapRegions) == 16U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, BootServicesAllowed) == 24U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, GateComplete) == 32U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, FunctionsChecked) == 40U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, MasteringFunctions) == 48U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnconfinedFunctions) == 56U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, ManagedPages) == 64U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnmanagedPages) == 72U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, MetadataBytes) == 80U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, MetadataBudgetBytes) == 88U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, LowWatermarkBlocks) == 96U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, Segments) == 104U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, Exclusions) == 112U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, SegmentStart) == 120U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, SegmentPages) == 632U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, ExclusionStart) == 1144U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, ExclusionBytes) == 3192U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, MasterRecorded) == 5240U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, MasterRequester) == 5248U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, MasterClass) == 5760U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, AmdBaseRegisters) == 6272U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitCount) == 7232U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitBase) == 7240U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitKind) == 7304U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitOutcome) == 7368U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitReason) == 7432U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitStatus) == 7496U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitFaultStatus) == 7560U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitQueue) == 7624U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitHead) == 7688U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitTail) == 7752U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitRoot) == 7816U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitInterruptTable) == 7880U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitPageRequestQueue) == 7944U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, UnitAdvancedFaultLog) == 8008U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, BootInfoFaultRanges) == 8072U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, MasterOutcome) == 8080U);
static_assert(__builtin_offsetof(PhysicalPageImportEvidence, AmdQueuePointers) == 8592U);
static_assert(sizeof(PhysicalPageImportEvidence) == 9360U);
}
#endif
