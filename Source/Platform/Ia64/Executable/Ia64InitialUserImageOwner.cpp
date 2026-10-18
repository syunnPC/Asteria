// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Executable/Ia64InitialUserImageOwner.hpp"

#include "Foundation/Compiler.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"
#include "Runtime/Memory.hpp"

namespace Asteria::Platform::Ia64::Executable
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UIntPointer MaximumUIntPointer{~Foundation::UIntPointer{0U}};
constexpr Foundation::UInt64 PageSizeBytes{Ia64UserPageSizeBytes};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumSegmentAlignmentBytes{64U * 1024U};
constexpr Foundation::UInt64 MaximumSegmentPageCount{Ia64InitialUserImageMaximumSegmentPages};
constexpr Foundation::UInt64 MaximumAggregateLoadPageCount{Ia64InitialUserImageMaximumLoadPages};
constexpr Foundation::UInt64 AdditionalStoragePageCount{Ia64InitialUserMemoryStackPageCount +
                                                        Ia64InitialUserRseBackingStorePageCount};
constexpr Foundation::UInt64 MaximumArenaPageCount{MaximumAggregateLoadPageCount + AdditionalStoragePageCount};
constexpr Foundation::UInt8 MinimumPhysicalAddressBits{32U};
constexpr Foundation::UInt8 MaximumPhysicalAddressBits{63U};
constexpr Foundation::UInt8 EncoderPhysicalAddressBits{50U};
constexpr Foundation::UInt64 FirmwareWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 FirmwareRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt32 ConventionalFirmwareMemoryType{7U};

static_assert(sizeof(Foundation::Size) >= sizeof(Foundation::UInt64));
static_assert(PageSizeBytes != 0U);
static_assert((PageSizeBytes & PageOffsetMask) == 0U);
static_assert(PageSizeBytes == Kernel::Memory::PhysicalPageSizeBytes);

struct GuardedVirtualRange
{
    Foundation::UInt64 Start;
    Foundation::UInt64 SizeBytes;
};

constexpr GuardedVirtualRange GuardedVirtualRanges[]{
    {
        .Start = Ia64UserExecutionProbeCodeGuardedVirtualStart,
        .SizeBytes = Ia64UserExecutionProbeCodeGuardedVirtualSizeBytes,
    },
    {
        .Start = Ia64UserExecutionProbeMemoryStackGuardedVirtualStart,
        .SizeBytes = Ia64UserExecutionProbeMemoryStackGuardedVirtualSizeBytes,
    },
    {
        .Start = Ia64UserExecutionProbeRseBackingStoreGuardedVirtualStart,
        .SizeBytes = Ia64UserExecutionProbeRseBackingStoreGuardedVirtualSizeBytes,
    },
    {
        .Start = Ia64InitialUserMemoryStackGuardedVirtualStart,
        .SizeBytes = Ia64InitialUserMemoryStackGuardedVirtualSizeBytes,
    },
    {
        .Start = Ia64InitialUserRseBackingStoreGuardedVirtualStart,
        .SizeBytes = Ia64InitialUserRseBackingStoreGuardedVirtualSizeBytes,
    },
};

[[nodiscard]] constexpr bool TryAdd(const Foundation::UInt64 first, const Foundation::UInt64 second,
                                    Foundation::UInt64& result) noexcept
{
    if (first > (MaximumUInt64 - second))
    {
        return false;
    }
    result = first + second;
    return true;
}

[[nodiscard]] constexpr bool TryGetPointerEnd(const Foundation::UIntPointer start, const Foundation::Size sizeBytes,
                                              Foundation::UIntPointer& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUIntPointer - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] constexpr bool TryGetPageSize(const Foundation::UInt64 pageCount, Foundation::UInt64& sizeBytes) noexcept
{
    if (pageCount > (MaximumUInt64 / PageSizeBytes))
    {
        return false;
    }
    sizeBytes = pageCount * PageSizeBytes;
    return true;
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool
SegmentPrecedesOrEquals(const ::Asteria::Executable::Elf::Elf64LoadSegment& first,
                        const ::Asteria::Executable::Elf::Elf64LoadSegment& second) noexcept
{
    return (first.PageStart < second.PageStart) ||
           ((first.PageStart == second.PageStart) && (first.VirtualStart <= second.VirtualStart));
}

[[nodiscard]] constexpr bool
IntersectsGuardedVirtualRange(const ::Asteria::Executable::Elf::Elf64LoadSegment& segment) noexcept
{
    Foundation::UInt64 segmentSizeBytes{0U};
    Foundation::UInt64 segmentEnd{0U};
    if ((segment.PageCount == 0U) || !TryGetPageSize(segment.PageCount, segmentSizeBytes) ||
        !TryAdd(segment.PageStart, segmentSizeBytes, segmentEnd))
    {
        return segment.PageCount != 0U;
    }

    for (const GuardedVirtualRange& guarded : GuardedVirtualRanges)
    {
        Foundation::UInt64 guardedEnd{0U};
        if (!TryAdd(guarded.Start, guarded.SizeBytes, guardedEnd) ||
            RangesOverlap(segment.PageStart, segmentEnd, guarded.Start, guardedEnd))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool AllocationAttributesAreValid(const Kernel::Memory::PhysicalPageRange& allocation) noexcept
{
    return ((allocation.Attributes & FirmwareWriteBackAttribute) != 0U) &&
           ((allocation.Attributes & FirmwareRuntimeAttribute) == 0U) &&
           (allocation.FirmwareType == ConventionalFirmwareMemoryType) && (allocation.Reserved == 0U);
}

[[nodiscard]] constexpr Kernel::Memory::PhysicalPageRange
MakePhysicalSlice(const Kernel::Memory::PhysicalPageRange& arena, const Foundation::UInt64 physicalStart,
                  const Foundation::UInt64 pageCount) noexcept
{
    return Kernel::Memory::PhysicalPageRange{
        .PhysicalStart = physicalStart,
        .PageCount = pageCount,
        .Attributes = arena.Attributes,
        .FirmwareType = arena.FirmwareType,
        .Reserved = arena.Reserved,
    };
}

[[nodiscard]] constexpr bool PhysicalSliceIsValid(const Kernel::Memory::PhysicalPageRange& slice,
                                                  const Kernel::Memory::PhysicalPageRange& arena,
                                                  const Foundation::UInt64 arenaEnd) noexcept
{
    Foundation::UInt64 sliceSizeBytes{0U};
    Foundation::UInt64 sliceEnd{0U};
    return (slice.Attributes == arena.Attributes) && (slice.FirmwareType == arena.FirmwareType) &&
           (slice.Reserved == arena.Reserved) && ((slice.PhysicalStart & PageOffsetMask) == 0U) &&
           TryGetPageSize(slice.PageCount, sliceSizeBytes) && TryAdd(slice.PhysicalStart, sliceSizeBytes, sliceEnd) &&
           (slice.PhysicalStart >= arena.PhysicalStart) && (sliceEnd <= arenaEnd);
}

[[nodiscard]] bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

ASTERIA_NOINLINE_NOIPA void PublishExecutableSlices(const Foundation::UInt8* const writableArena,
                                                    const Kernel::Memory::PhysicalPageRange& arena,
                                                    const Ia64InitialUserImageSegmentPlan* const segments,
                                                    const Foundation::Size segmentCount) noexcept
{
#if defined(__ia64__)
    using ::Asteria::Executable::Elf::Elf64SegmentPermission;
    constexpr Foundation::Size BundleSizeBytes{16U};
    for (Foundation::Size index{0U}; index < segmentCount; ++index)
    {
        const Ia64InitialUserImageSegmentPlan& plan = segments[index];
        if (plan.Segment.Permissions.Contains(Elf64SegmentPermission::Execute)) [[likely]]
        {
            const Foundation::Size sliceOffset =
                static_cast<Foundation::Size>(plan.PhysicalSlice.PhysicalStart - arena.PhysicalStart);
            const Foundation::Size sliceSizeBytes =
                static_cast<Foundation::Size>(plan.PhysicalSlice.PageCount * PageSizeBytes);
            for (Foundation::Size offset{0U}; offset < sliceSizeBytes; offset += BundleSizeBytes)
            {
                __asm__ volatile("fc.i %0" : : "r"(writableArena + sliceOffset + offset) : "memory");
            }
        }
    }
    __asm__ volatile(";; sync.i\n;; srlz.i\n;; mf\n;;" ::: "memory");
#else
    static_cast<void>(writableArena);
    static_cast<void>(arena);
    static_cast<void>(segments);
    static_cast<void>(segmentCount);
#endif
}
} // namespace

void Ia64InitialUserImageOwner::RecordFailure(const Ia64InitialUserImageOwnerError error,
                                              const Foundation::Size segmentIndex,
                                              const Ia64InitialUserImageSourceError source,
                                              const Ia64P64ImageFailure p64, const Ia64P64LoadPlanAccessError loadPlan,
                                              const Kernel::Memory::BootstrapPageAllocatorError allocator) noexcept
{
    m_failure = Ia64InitialUserImageOwnerFailure{
        .Error = error,
        .Source = source,
        .P64 = p64,
        .LoadPlan = loadPlan,
        .Allocator = allocator,
        .SegmentIndex = segmentIndex,
    };
}

Ia64InitialUserImageOwner::StageResult
Ia64InitialUserImageOwner::Fail(const Ia64InitialUserImageOwnerError error, const Foundation::Size segmentIndex,
                                const Ia64InitialUserImageSourceError source, const Ia64P64ImageFailure p64,
                                const Ia64P64LoadPlanAccessError loadPlan,
                                const Kernel::Memory::BootstrapPageAllocatorError allocator) noexcept
{
    RecordFailure(error, segmentIndex, source, p64, loadPlan, allocator);
    return StageResult::Failure(m_failure);
}

bool Ia64InitialUserImageOwner::ValidateSourceExtentAndAliases(
    const ::Asteria::Executable::Elf::Elf64ByteView source,
    const Kernel::Memory::BootstrapPageAllocator& allocator) noexcept
{
    const Foundation::UIntPointer sourceStart = reinterpret_cast<Foundation::UIntPointer>(source.Data);
    Foundation::UIntPointer sourceEnd{0U};
    if ((source.Data == nullptr) || !TryGetPointerEnd(sourceStart, source.SizeBytes, sourceEnd))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::InvalidSourceExtent);
        return false;
    }

    const Foundation::UIntPointer ownerStart = reinterpret_cast<Foundation::UIntPointer>(this);
    const Foundation::UIntPointer loadPlanStart = reinterpret_cast<Foundation::UIntPointer>(&m_loadPlan);
    const Foundation::UIntPointer allocatorStart = reinterpret_cast<Foundation::UIntPointer>(&allocator);
    Foundation::UIntPointer ownerEnd{0U};
    Foundation::UIntPointer loadPlanEnd{0U};
    Foundation::UIntPointer allocatorEnd{0U};
    if (!TryGetPointerEnd(ownerStart, sizeof(*this), ownerEnd) ||
        !TryGetPointerEnd(loadPlanStart, sizeof(m_loadPlan), loadPlanEnd) ||
        !TryGetPointerEnd(allocatorStart, sizeof(allocator), allocatorEnd) ||
        RangesOverlap(sourceStart, sourceEnd, ownerStart, ownerEnd) ||
        RangesOverlap(sourceStart, sourceEnd, loadPlanStart, loadPlanEnd) ||
        RangesOverlap(sourceStart, sourceEnd, allocatorStart, allocatorEnd))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::SourceAliasesControlStorage);
        return false;
    }
    return true;
}

bool Ia64InitialUserImageOwner::ValidateAndRetainSegments(const Foundation::Size segmentCount,
                                                          Foundation::UInt64& aggregateLoadPageCount,
                                                          Foundation::UInt64& totalBssSizeBytes) noexcept
{
    if (segmentCount > Ia64InitialUserImageMaximumSegmentCount)
    {
        RecordFailure(Ia64InitialUserImageOwnerError::LoadPlanAccessRejected);
        return false;
    }
    aggregateLoadPageCount = 0U;
    totalBssSizeBytes = 0U;
    for (Foundation::Size index{0U}; index < segmentCount; ++index)
    {
        const auto segmentResult = m_loadPlan.GetSegment(index);
        if (!segmentResult.HasValue())
        {
            RecordFailure(Ia64InitialUserImageOwnerError::LoadPlanAccessRejected, index,
                          Ia64InitialUserImageSourceError::None, {}, segmentResult.GetError());
            return false;
        }
        const ::Asteria::Executable::Elf::Elf64LoadSegment segment = segmentResult.GetValue();
        if ((index != 0U) && !SegmentPrecedesOrEquals(m_segmentPlans[index - 1U].Segment, segment))
        {
            RecordFailure(Ia64InitialUserImageOwnerError::SegmentOrderInvalid, index);
            return false;
        }
        if (segment.PageCount > MaximumSegmentPageCount)
        {
            RecordFailure(Ia64InitialUserImageOwnerError::SegmentPageCapacityExceeded, index);
            return false;
        }
        if (aggregateLoadPageCount > (MaximumAggregateLoadPageCount - segment.PageCount))
        {
            RecordFailure(Ia64InitialUserImageOwnerError::AggregatePageCapacityExceeded, index);
            return false;
        }
        aggregateLoadPageCount += segment.PageCount;
        if (totalBssSizeBytes > (MaximumUInt64 - segment.BssSizeBytes))
        {
            RecordFailure(Ia64InitialUserImageOwnerError::BssSizeOverflow, index);
            return false;
        }
        totalBssSizeBytes += segment.BssSizeBytes;
        if (IntersectsGuardedVirtualRange(segment))
        {
            RecordFailure(Ia64InitialUserImageOwnerError::GuardedVirtualRangeConflict, index);
            return false;
        }
        m_segmentPlans[index].Segment = segment;
    }
    if (totalBssSizeBytes == 0U)
    {
        RecordFailure(Ia64InitialUserImageOwnerError::MissingRealBss);
        return false;
    }
    return true;
}

bool Ia64InitialUserImageOwner::ValidateArenaAllocation(const Kernel::Memory::BootstrapPageAllocator& allocator,
                                                        const ::Asteria::Executable::Elf::Elf64ByteView source,
                                                        const Foundation::UInt8 physicalAddressBits,
                                                        const Foundation::UInt64 arenaPageCount,
                                                        Foundation::UInt8*& writableArena,
                                                        Foundation::Size& writableSizeBytes,
                                                        Foundation::UInt64& arenaPhysicalEnd) noexcept
{
    Foundation::UInt64 arenaSizeBytes{0U};
    const Foundation::UInt8 effectivePhysicalAddressBits =
        physicalAddressBits < EncoderPhysicalAddressBits ? physicalAddressBits : EncoderPhysicalAddressBits;
    const Foundation::UInt64 effectivePhysicalAddressExclusive = Foundation::UInt64{1U} << effectivePhysicalAddressBits;
    if ((m_arenaAllocation.PhysicalStart == 0U) || ((m_arenaAllocation.PhysicalStart & PageOffsetMask) != 0U) ||
        (m_arenaAllocation.PageCount != arenaPageCount) || !TryGetPageSize(arenaPageCount, arenaSizeBytes) ||
        !TryAdd(m_arenaAllocation.PhysicalStart, arenaSizeBytes, arenaPhysicalEnd) ||
        (arenaPhysicalEnd > effectivePhysicalAddressExclusive))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::InvalidArenaAllocation);
        return false;
    }
    if (!AllocationAttributesAreValid(m_arenaAllocation))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::InvalidArenaAllocationAttributes);
        return false;
    }

    const Foundation::UIntPointer writableStart = static_cast<Foundation::UIntPointer>(m_arenaAllocation.PhysicalStart);
    writableArena = reinterpret_cast<Foundation::UInt8*>(writableStart);
    writableSizeBytes = static_cast<Foundation::Size>(arenaSizeBytes);
    Foundation::UIntPointer writableEnd{0U};
    if ((writableArena == nullptr) || (reinterpret_cast<Foundation::UIntPointer>(writableArena) != writableStart) ||
        ((writableStart & PageOffsetMask) != 0U) || !TryGetPointerEnd(writableStart, writableSizeBytes, writableEnd) ||
        (writableEnd != static_cast<Foundation::UIntPointer>(arenaPhysicalEnd)))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::InvalidArenaAlias);
        return false;
    }

    const Foundation::UIntPointer sourceStart = reinterpret_cast<Foundation::UIntPointer>(source.Data);
    const Foundation::UIntPointer ownerStart = reinterpret_cast<Foundation::UIntPointer>(this);
    const Foundation::UIntPointer loadPlanStart = reinterpret_cast<Foundation::UIntPointer>(&m_loadPlan);
    const Foundation::UIntPointer allocatorStart = reinterpret_cast<Foundation::UIntPointer>(&allocator);
    Foundation::UIntPointer sourceEnd{0U};
    Foundation::UIntPointer ownerEnd{0U};
    Foundation::UIntPointer loadPlanEnd{0U};
    Foundation::UIntPointer allocatorEnd{0U};
    if (!TryGetPointerEnd(sourceStart, source.SizeBytes, sourceEnd) ||
        !TryGetPointerEnd(ownerStart, sizeof(*this), ownerEnd) ||
        !TryGetPointerEnd(loadPlanStart, sizeof(m_loadPlan), loadPlanEnd) ||
        !TryGetPointerEnd(allocatorStart, sizeof(allocator), allocatorEnd))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::InvalidArenaAlias);
        return false;
    }
    if (RangesOverlap(writableStart, writableEnd, sourceStart, sourceEnd))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::ArenaAliasesSource);
        return false;
    }
    if (RangesOverlap(writableStart, writableEnd, ownerStart, ownerEnd) ||
        RangesOverlap(writableStart, writableEnd, loadPlanStart, loadPlanEnd) ||
        RangesOverlap(writableStart, writableEnd, allocatorStart, allocatorEnd))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::ArenaAliasesControlStorage);
        return false;
    }
    return true;
}

bool Ia64InitialUserImageOwner::PackArenaSlices(const Foundation::Size segmentCount,
                                                const Foundation::UInt64 arenaPhysicalEnd,
                                                Foundation::UInt64& memoryStackSizeBytes,
                                                Foundation::UInt64& rseBackingStoreSizeBytes) noexcept
{
    Foundation::UInt64 packedPhysicalStart = m_arenaAllocation.PhysicalStart;
    for (Foundation::Size index{0U}; index < segmentCount; ++index)
    {
        Ia64InitialUserImageSegmentPlan& plan = m_segmentPlans[index];
        plan.PhysicalSlice = MakePhysicalSlice(m_arenaAllocation, packedPhysicalStart, plan.Segment.PageCount);
        Foundation::UInt64 sliceSizeBytes{0U};
        if (!TryGetPageSize(plan.Segment.PageCount, sliceSizeBytes) ||
            !TryAdd(packedPhysicalStart, sliceSizeBytes, packedPhysicalStart) ||
            !PhysicalSliceIsValid(plan.PhysicalSlice, m_arenaAllocation, arenaPhysicalEnd))
        {
            RecordFailure(Ia64InitialUserImageOwnerError::InvalidPhysicalSlice, index);
            return false;
        }
    }

    const Kernel::Memory::PhysicalPageRange memoryStack =
        MakePhysicalSlice(m_arenaAllocation, packedPhysicalStart, Ia64InitialUserMemoryStackPageCount);
    if (!TryGetPageSize(Ia64InitialUserMemoryStackPageCount, memoryStackSizeBytes) ||
        !TryAdd(packedPhysicalStart, memoryStackSizeBytes, packedPhysicalStart) ||
        !PhysicalSliceIsValid(memoryStack, m_arenaAllocation, arenaPhysicalEnd))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::InvalidPhysicalSlice);
        return false;
    }
    const Kernel::Memory::PhysicalPageRange rseBackingStore =
        MakePhysicalSlice(m_arenaAllocation, packedPhysicalStart, Ia64InitialUserRseBackingStorePageCount);
    if (!TryGetPageSize(Ia64InitialUserRseBackingStorePageCount, rseBackingStoreSizeBytes) ||
        !TryAdd(packedPhysicalStart, rseBackingStoreSizeBytes, packedPhysicalStart) ||
        (packedPhysicalStart != arenaPhysicalEnd) ||
        !PhysicalSliceIsValid(rseBackingStore, m_arenaAllocation, arenaPhysicalEnd))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::InvalidPhysicalSlice);
        return false;
    }

    Foundation::UInt64 memoryStackEnd{0U};
    Foundation::UInt64 rseBackingStoreEnd{0U};
    if (!TryAdd(memoryStack.PhysicalStart, memoryStackSizeBytes, memoryStackEnd) ||
        !TryAdd(rseBackingStore.PhysicalStart, rseBackingStoreSizeBytes, rseBackingStoreEnd) ||
        RangesOverlap(memoryStack.PhysicalStart, memoryStackEnd, rseBackingStore.PhysicalStart, rseBackingStoreEnd))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::InvalidPhysicalSlice);
        return false;
    }

    for (Foundation::Size index{0U}; index < segmentCount; ++index)
    {
        const Ia64InitialUserImageSegmentPlan& plan = m_segmentPlans[index];
        Foundation::UInt64 sliceSizeBytes{0U};
        Foundation::UInt64 sliceEnd{0U};
        if (!TryGetPageSize(plan.PhysicalSlice.PageCount, sliceSizeBytes) ||
            !TryAdd(plan.PhysicalSlice.PhysicalStart, sliceSizeBytes, sliceEnd))
        {
            RecordFailure(Ia64InitialUserImageOwnerError::InvalidPhysicalSlice, index);
            return false;
        }
        for (Foundation::Size prior{0U}; prior < index; ++prior)
        {
            const Ia64InitialUserImageSegmentPlan& priorPlan = m_segmentPlans[prior];
            Foundation::UInt64 priorSizeBytes{0U};
            Foundation::UInt64 priorEnd{0U};
            if (!TryGetPageSize(priorPlan.PhysicalSlice.PageCount, priorSizeBytes) ||
                !TryAdd(priorPlan.PhysicalSlice.PhysicalStart, priorSizeBytes, priorEnd) ||
                RangesOverlap(plan.PhysicalSlice.PhysicalStart, sliceEnd, priorPlan.PhysicalSlice.PhysicalStart,
                              priorEnd))
            {
                RecordFailure(Ia64InitialUserImageOwnerError::InvalidPhysicalSlice, index);
                return false;
            }
        }
        if (RangesOverlap(plan.PhysicalSlice.PhysicalStart, sliceEnd, memoryStack.PhysicalStart, memoryStackEnd) ||
            RangesOverlap(plan.PhysicalSlice.PhysicalStart, sliceEnd, rseBackingStore.PhysicalStart,
                          rseBackingStoreEnd))
        {
            RecordFailure(Ia64InitialUserImageOwnerError::InvalidPhysicalSlice, index);
            return false;
        }
    }

    m_summary.MemoryStack = memoryStack;
    m_summary.RseBackingStore = rseBackingStore;
    return true;
}

bool Ia64InitialUserImageOwner::ValidateSegmentOffsets(const Foundation::Size segmentCount,
                                                       const Foundation::Size sourceSizeBytes) noexcept
{
    for (Foundation::Size index{0U}; index < segmentCount; ++index)
    {
        const ::Asteria::Executable::Elf::Elf64LoadSegment& segment = m_segmentPlans[index].Segment;
        if (segment.PageCount == 0U)
        {
            continue;
        }
        if ((segment.VirtualStart < segment.PageStart) || (segment.BssStart < segment.PageStart))
        {
            RecordFailure(Ia64InitialUserImageOwnerError::InvalidPhysicalSlice, index);
            return false;
        }

        Foundation::UInt64 sliceSizeBytes{0U};
        Foundation::UInt64 sourceFileEnd{0U};
        Foundation::UInt64 payloadEndOffset{0U};
        Foundation::UInt64 bssEndOffset{0U};
        const Foundation::UInt64 payloadOffset = segment.VirtualStart - segment.PageStart;
        const Foundation::UInt64 bssOffset = segment.BssStart - segment.PageStart;
        if (!TryGetPageSize(segment.PageCount, sliceSizeBytes) ||
            !TryAdd(segment.FileOffset, segment.FileSizeBytes, sourceFileEnd) ||
            (sourceFileEnd > static_cast<Foundation::UInt64>(sourceSizeBytes)) ||
            !TryAdd(payloadOffset, segment.FileSizeBytes, payloadEndOffset) || (payloadEndOffset > sliceSizeBytes) ||
            !TryAdd(bssOffset, segment.BssSizeBytes, bssEndOffset) || (bssEndOffset > sliceSizeBytes) ||
            (payloadEndOffset != bssOffset))
        {
            RecordFailure(Ia64InitialUserImageOwnerError::InvalidPhysicalSlice, index);
            return false;
        }
    }
    return true;
}

bool Ia64InitialUserImageOwner::InitializeAndVerifyArena(const ::Asteria::Executable::Elf::Elf64ByteView source,
                                                         Foundation::UInt8* const writableArena,
                                                         const Foundation::Size writableSizeBytes,
                                                         const Foundation::UInt64 memoryStackSizeBytes,
                                                         const Foundation::UInt64 rseBackingStoreSizeBytes) noexcept
{
    static_cast<void>(memset(writableArena, 0, writableSizeBytes));
    for (Foundation::Size index{0U}; index < m_summary.SegmentCount; ++index)
    {
        const Ia64InitialUserImageSegmentPlan& plan = m_segmentPlans[index];
        if (plan.Segment.PageCount == 0U)
        {
            continue;
        }
        const Foundation::Size sliceOffset =
            static_cast<Foundation::Size>(plan.PhysicalSlice.PhysicalStart - m_arenaAllocation.PhysicalStart);
        const Foundation::Size payloadOffset =
            static_cast<Foundation::Size>(plan.Segment.VirtualStart - plan.Segment.PageStart);
        static_cast<void>(memcpy(writableArena + sliceOffset + payloadOffset,
                                 source.Data + static_cast<Foundation::Size>(plan.Segment.FileOffset),
                                 static_cast<Foundation::Size>(plan.Segment.FileSizeBytes)));
    }

    for (Foundation::Size index{0U}; index < m_summary.SegmentCount; ++index)
    {
        const Ia64InitialUserImageSegmentPlan& plan = m_segmentPlans[index];
        if (plan.Segment.PageCount == 0U)
        {
            continue;
        }
        const Foundation::Size sliceOffset =
            static_cast<Foundation::Size>(plan.PhysicalSlice.PhysicalStart - m_arenaAllocation.PhysicalStart);
        const Foundation::Size sliceSizeBytes =
            static_cast<Foundation::Size>(plan.PhysicalSlice.PageCount * PageSizeBytes);
        const Foundation::Size payloadOffset =
            static_cast<Foundation::Size>(plan.Segment.VirtualStart - plan.Segment.PageStart);
        const Foundation::Size payloadSizeBytes = static_cast<Foundation::Size>(plan.Segment.FileSizeBytes);
        const Foundation::Size bssOffset =
            static_cast<Foundation::Size>(plan.Segment.BssStart - plan.Segment.PageStart);
        const Foundation::Size bssSizeBytes = static_cast<Foundation::Size>(plan.Segment.BssSizeBytes);
        const Foundation::Size bssEndOffset = bssOffset + bssSizeBytes;
        if (!BytesAreZero(writableArena + sliceOffset, payloadOffset) ||
            (memcmp(writableArena + sliceOffset + payloadOffset,
                    source.Data + static_cast<Foundation::Size>(plan.Segment.FileOffset), payloadSizeBytes) != 0) ||
            !BytesAreZero(writableArena + sliceOffset + bssOffset, bssSizeBytes) ||
            !BytesAreZero(writableArena + sliceOffset + bssEndOffset, sliceSizeBytes - bssEndOffset))
        {
            RecordFailure(Ia64InitialUserImageOwnerError::StagedBytesMismatch, index);
            return false;
        }
    }

    const Foundation::Size memoryStackOffset =
        static_cast<Foundation::Size>(m_summary.MemoryStack.PhysicalStart - m_arenaAllocation.PhysicalStart);
    const Foundation::Size rseBackingStoreOffset =
        static_cast<Foundation::Size>(m_summary.RseBackingStore.PhysicalStart - m_arenaAllocation.PhysicalStart);
    if (!BytesAreZero(writableArena + memoryStackOffset, static_cast<Foundation::Size>(memoryStackSizeBytes)) ||
        !BytesAreZero(writableArena + rseBackingStoreOffset, static_cast<Foundation::Size>(rseBackingStoreSizeBytes)))
    {
        RecordFailure(Ia64InitialUserImageOwnerError::StagedBytesMismatch);
        return false;
    }
    return true;
}

Ia64InitialUserImageOwner::StageResult Ia64InitialUserImageOwner::Stage(
    Kernel::Memory::BootstrapPageAllocator& allocator, const Foundation::UInt8 physicalAddressBits,
    const Foundation::UInt8 implementedVirtualAddressMostSignificantBit,
    const ::Asteria::Executable::Elf::Elf64ByteView source, const Boot::BootFileRegion& region) noexcept
{
    if (m_state != Ia64InitialUserImageOwnerState::Fresh)
    {
        return StageResult::Failure(Ia64InitialUserImageOwnerFailure{
            .Error = Ia64InitialUserImageOwnerError::AlreadyAttempted,
            .Source = Ia64InitialUserImageSourceError::None,
            .P64 = {},
            .LoadPlan = Ia64P64LoadPlanAccessError::None,
            .Allocator = Kernel::Memory::BootstrapPageAllocatorError::None,
            .SegmentIndex = Ia64InitialUserImageNoSegment,
        });
    }
    m_state = Ia64InitialUserImageOwnerState::Failed;

    {
        const auto sourceResult = Ia64InitialUserImageSource::CreateView(region, source);
        if (!sourceResult.HasValue())
        {
            return Fail(Ia64InitialUserImageOwnerError::SourceRejected, Ia64InitialUserImageNoSegment,
                        sourceResult.GetError());
        }
        if (!ValidateSourceExtentAndAliases(source, allocator))
        {
            return StageResult::Failure(m_failure);
        }
    }

    constexpr ::Asteria::Executable::Elf::Elf64LoadPlanLimits limits{
        .PageSizeBytes = PageSizeBytes,
        .MaximumSegmentAlignmentBytes = MaximumSegmentAlignmentBytes,
        .LoadSegmentCapacity = Ia64InitialUserImageMaximumSegmentCount,
    };
    {
        const auto p64Result = Ia64P64Image::DecodeLoadPlan(source, limits, implementedVirtualAddressMostSignificantBit,
                                                            Ia64InitialUserVirtualRegion, m_loadPlan);
        if (!p64Result.HasValue())
        {
            return Fail(Ia64InitialUserImageOwnerError::P64ImageRejected, Ia64InitialUserImageNoSegment,
                        Ia64InitialUserImageSourceError::None, p64Result.GetError());
        }
        m_summary.Image = p64Result.GetValue();
    }
    if ((physicalAddressBits < MinimumPhysicalAddressBits) || (physicalAddressBits > MaximumPhysicalAddressBits))
    {
        return Fail(Ia64InitialUserImageOwnerError::InvalidPhysicalAddressProfile);
    }

    const Foundation::Size segmentCount = m_loadPlan.GetSegmentCount();
    Foundation::UInt64 aggregateLoadPageCount{0U};
    Foundation::UInt64 totalBssSizeBytes{0U};
    if (!ValidateAndRetainSegments(segmentCount, aggregateLoadPageCount, totalBssSizeBytes))
    {
        return StageResult::Failure(m_failure);
    }

    const Foundation::UInt64 arenaPageCount = aggregateLoadPageCount + AdditionalStoragePageCount;
    if ((arenaPageCount == 0U) || (arenaPageCount > MaximumArenaPageCount))
    {
        return Fail(Ia64InitialUserImageOwnerError::AggregatePageCapacityExceeded);
    }
    {
        const auto allocationResult = allocator.Allocate(arenaPageCount, 1U);
        if (!allocationResult.HasValue())
        {
            return Fail(Ia64InitialUserImageOwnerError::ArenaAllocationRejected, Ia64InitialUserImageNoSegment,
                        Ia64InitialUserImageSourceError::None, {}, Ia64P64LoadPlanAccessError::None,
                        allocationResult.GetError());
        }
        m_arenaAllocation = allocationResult.GetValue();
    }

    Foundation::UInt64 arenaPhysicalEnd{0U};
    Foundation::UInt8* writableArena{nullptr};
    Foundation::Size writableSizeBytes{0U};
    if (!ValidateArenaAllocation(allocator, source, physicalAddressBits, arenaPageCount, writableArena,
                                 writableSizeBytes, arenaPhysicalEnd))
    {
        return StageResult::Failure(m_failure);
    }

    Foundation::UInt64 memoryStackSizeBytes{0U};
    Foundation::UInt64 rseBackingStoreSizeBytes{0U};
    if (!PackArenaSlices(segmentCount, arenaPhysicalEnd, memoryStackSizeBytes, rseBackingStoreSizeBytes))
    {
        return StageResult::Failure(m_failure);
    }

    if (!ValidateSegmentOffsets(segmentCount, source.SizeBytes))
    {
        return StageResult::Failure(m_failure);
    }

    m_summary.AggregateArena = m_arenaAllocation;
    m_summary.AggregateLoadPageCount = aggregateLoadPageCount;
    m_summary.TotalBssSizeBytes = totalBssSizeBytes;
    m_summary.SegmentCount = segmentCount;

    if (!InitializeAndVerifyArena(source, writableArena, writableSizeBytes, memoryStackSizeBytes,
                                  rseBackingStoreSizeBytes))
    {
        return StageResult::Failure(m_failure);
    }

    PublishExecutableSlices(writableArena, m_arenaAllocation, m_segmentPlans, segmentCount);
    m_state = Ia64InitialUserImageOwnerState::Staged;
    return StageResult::Success(m_summary);
}

bool Ia64InitialUserImageOwner::ClaimForMemoryOwner() noexcept
{
    if (m_state != Ia64InitialUserImageOwnerState::Staged)
    {
        return false;
    }
    m_state = Ia64InitialUserImageOwnerState::Claimed;
    return true;
}

Foundation::Result<Ia64InitialUserImageStageSummary, Ia64InitialUserImageOwnerAccessError>
Ia64InitialUserImageOwner::GetStageSummary() const noexcept
{
    using ResultType = Foundation::Result<Ia64InitialUserImageStageSummary, Ia64InitialUserImageOwnerAccessError>;
    if ((m_state != Ia64InitialUserImageOwnerState::Staged) && (m_state != Ia64InitialUserImageOwnerState::Claimed))
    {
        return ResultType::Failure(Ia64InitialUserImageOwnerAccessError::NotStaged);
    }
    return ResultType::Success(m_summary);
}

Foundation::Result<Ia64InitialUserImageSegmentPlan, Ia64InitialUserImageOwnerAccessError>
Ia64InitialUserImageOwner::GetSegmentPlan(const Foundation::Size index) const noexcept
{
    using ResultType = Foundation::Result<Ia64InitialUserImageSegmentPlan, Ia64InitialUserImageOwnerAccessError>;
    if ((m_state != Ia64InitialUserImageOwnerState::Staged) && (m_state != Ia64InitialUserImageOwnerState::Claimed))
    {
        return ResultType::Failure(Ia64InitialUserImageOwnerAccessError::NotStaged);
    }
    if (index >= m_summary.SegmentCount)
    {
        return ResultType::Failure(Ia64InitialUserImageOwnerAccessError::IndexOutOfBounds);
    }
    return ResultType::Success(m_segmentPlans[index]);
}

Foundation::Result<Ia64InitialUserImageOwnerFailure, Ia64InitialUserImageOwnerAccessError>
Ia64InitialUserImageOwner::GetFailure() const noexcept
{
    using ResultType = Foundation::Result<Ia64InitialUserImageOwnerFailure, Ia64InitialUserImageOwnerAccessError>;
    if (m_state != Ia64InitialUserImageOwnerState::Failed)
    {
        return ResultType::Failure(Ia64InitialUserImageOwnerAccessError::NoFailure);
    }
    return ResultType::Success(m_failure);
}
} // namespace Asteria::Platform::Ia64::Executable
