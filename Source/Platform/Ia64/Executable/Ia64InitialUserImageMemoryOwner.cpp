// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Executable/Ia64InitialUserImageMemoryOwner.hpp"

#include "Kernel/Object/ObjectPin.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"

namespace Asteria::Platform::Ia64::Executable
{
namespace
{
using MemoryAccessError = Ia64InitialUserImageMemoryAccessError;
using MemoryOwnerError = Ia64InitialUserImageMemoryOwnerError;
using MemoryPurpose = Ia64InitialUserImageMemoryPurpose;
using GuardPolicy = Ia64InitialUserImageGuardPolicy;
using RangePlan = Ia64InitialUserImageMemoryRangePlan;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageSizeBytes{Ia64UserPageSizeBytes};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumAggregateLoadPageCount{Ia64InitialUserImageMaximumMaterializedPageCount -
                                                           Ia64InitialUserMemoryStackPageCount -
                                                           Ia64InitialUserRseBackingStorePageCount};
constexpr Foundation::UInt64 MaximumPhysicalAddressExclusive{Foundation::UInt64{1U} << 50U};
constexpr Foundation::UInt64 FirmwareWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 FirmwareRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt32 ConventionalFirmwareMemoryType{7U};
constexpr Foundation::UInt8 KnownElfPermissionBits{
    static_cast<Foundation::UInt8>(::Asteria::Executable::Elf::Elf64SegmentPermission::Read) |
    static_cast<Foundation::UInt8>(::Asteria::Executable::Elf::Elf64SegmentPermission::Write) |
    static_cast<Foundation::UInt8>(::Asteria::Executable::Elf::Elf64SegmentPermission::Execute)};

static_assert(PageSizeBytes == Kernel::Memory::MappingPageSizeBytes);
static_assert(MaximumAggregateLoadPageCount == Ia64InitialUserImageMaximumLoadPages);

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

[[nodiscard]] constexpr bool TryGetPageRangeEnd(const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                                Foundation::UInt64& end) noexcept
{
    if ((pages == 0U) || (pages > (MaximumUInt64 / PageSizeBytes)))
    {
        return false;
    }
    return TryAdd(start, pages * PageSizeBytes, end);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
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

[[nodiscard]] constexpr bool AllocationAttributesAreValid(const Kernel::Memory::PhysicalPageRange& allocation) noexcept
{
    return ((allocation.Attributes & FirmwareWriteBackAttribute) != 0U) &&
           ((allocation.Attributes & FirmwareRuntimeAttribute) == 0U) &&
           (allocation.FirmwareType == ConventionalFirmwareMemoryType) && (allocation.Reserved == 0U);
}

[[nodiscard]] constexpr bool PhysicalRangeAttributesMatch(const Kernel::Memory::PhysicalPageRange& first,
                                                          const Kernel::Memory::PhysicalPageRange& second) noexcept
{
    return (first.Attributes == second.Attributes) && (first.FirmwareType == second.FirmwareType) &&
           (first.Reserved == second.Reserved);
}

[[nodiscard]] constexpr bool PhysicalRangesAreEqual(const Kernel::Memory::PhysicalPageRange& first,
                                                    const Kernel::Memory::PhysicalPageRange& second) noexcept
{
    return (first.PhysicalStart == second.PhysicalStart) && (first.PageCount == second.PageCount) &&
           PhysicalRangeAttributesMatch(first, second);
}

[[nodiscard]] constexpr bool ImageInformationIsEqual(const Ia64P64ImageInformation& first,
                                                     const Ia64P64ImageInformation& second) noexcept
{
    return (first.EntryDescriptorAddress == second.EntryDescriptorAddress) &&
           (first.EntryBundleAddress == second.EntryBundleAddress) && (first.GlobalPointer == second.GlobalPointer) &&
           (first.Flags == second.Flags) && (first.Machine == second.Machine) &&
           (first.ProgramHeaderCount == second.ProgramHeaderCount);
}

[[nodiscard]] constexpr bool StageSummariesAreEqual(const Ia64InitialUserImageStageSummary& first,
                                                    const Ia64InitialUserImageStageSummary& second) noexcept
{
    return ImageInformationIsEqual(first.Image, second.Image) &&
           PhysicalRangesAreEqual(first.AggregateArena, second.AggregateArena) &&
           PhysicalRangesAreEqual(first.MemoryStack, second.MemoryStack) &&
           PhysicalRangesAreEqual(first.RseBackingStore, second.RseBackingStore) &&
           (first.AggregateLoadPageCount == second.AggregateLoadPageCount) &&
           (first.TotalBssSizeBytes == second.TotalBssSizeBytes) && (first.SegmentCount == second.SegmentCount);
}

[[nodiscard]] constexpr bool LoadSegmentsAreEqual(const ::Asteria::Executable::Elf::Elf64LoadSegment& first,
                                                  const ::Asteria::Executable::Elf::Elf64LoadSegment& second) noexcept
{
    return (first.FileOffset == second.FileOffset) && (first.FileSizeBytes == second.FileSizeBytes) &&
           (first.VirtualStart == second.VirtualStart) && (first.MemorySizeBytes == second.MemorySizeBytes) &&
           (first.BssStart == second.BssStart) && (first.BssSizeBytes == second.BssSizeBytes) &&
           (first.PageStart == second.PageStart) && (first.PageCount == second.PageCount) &&
           (first.AlignmentBytes == second.AlignmentBytes) &&
           (first.Permissions.GetBits() == second.Permissions.GetBits());
}

[[nodiscard]] constexpr bool SegmentPlansAreEqual(const Ia64InitialUserImageSegmentPlan& first,
                                                  const Ia64InitialUserImageSegmentPlan& second) noexcept
{
    return LoadSegmentsAreEqual(first.Segment, second.Segment) &&
           PhysicalRangesAreEqual(first.PhysicalSlice, second.PhysicalSlice);
}

[[nodiscard]] constexpr bool TranslatePermissions(const ::Asteria::Executable::Elf::Elf64SegmentPermissions source,
                                                  Kernel::Memory::PagePermissions& output) noexcept
{
    using ElfPermission = ::Asteria::Executable::Elf::Elf64SegmentPermission;
    using PagePermission = Kernel::Memory::PagePermission;

    const Foundation::UInt8 bits = source.GetBits();
    const bool readable = source.Contains(ElfPermission::Read);
    const bool writable = source.Contains(ElfPermission::Write);
    const bool executable = source.Contains(ElfPermission::Execute);
    if (((bits & static_cast<Foundation::UInt8>(~KnownElfPermissionBits)) != 0U) || !readable ||
        (writable && executable))
    {
        return false;
    }

    output = Kernel::Memory::PagePermissions{PagePermission::Read}.With(PagePermission::User);
    if (writable)
    {
        output = output.With(PagePermission::Write);
    }
    if (executable)
    {
        output = output.With(PagePermission::Execute);
    }
    return true;
}

[[nodiscard]] constexpr Kernel::Memory::PagePermissions WritableUserPermissions() noexcept
{
    return Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}
        .With(Kernel::Memory::PagePermission::Write)
        .With(Kernel::Memory::PagePermission::User);
}

[[nodiscard]] constexpr bool
SegmentGeometryIsValid(const ::Asteria::Executable::Elf::Elf64LoadSegment& segment) noexcept
{
    if (segment.PageCount == 0U)
    {
        return segment.MemorySizeBytes == 0U;
    }
    if ((segment.MemorySizeBytes == 0U) || ((segment.PageStart & PageOffsetMask) != 0U) ||
        (segment.VirtualStart < segment.PageStart) || (segment.FileSizeBytes > segment.MemorySizeBytes) ||
        (segment.BssStart < segment.VirtualStart))
    {
        return false;
    }

    Foundation::UInt64 pageEnd{0U};
    Foundation::UInt64 memoryEnd{0U};
    Foundation::UInt64 fileBackedEnd{0U};
    Foundation::UInt64 bssEnd{0U};
    return TryGetPageRangeEnd(segment.PageStart, segment.PageCount, pageEnd) &&
           TryAdd(segment.VirtualStart, segment.MemorySizeBytes, memoryEnd) &&
           TryAdd(segment.VirtualStart, segment.FileSizeBytes, fileBackedEnd) &&
           TryAdd(segment.BssStart, segment.BssSizeBytes, bssEnd) && (memoryEnd <= pageEnd) &&
           (fileBackedEnd == segment.BssStart) && (bssEnd == memoryEnd);
}

[[nodiscard]] constexpr bool FrameSnapshotMatches(const Kernel::Memory::FrameSnapshot& snapshot,
                                                  const Kernel::Memory::PhysicalPageRange& arena,
                                                  const Kernel::Object::ResourceDomainId chargeOwner) noexcept
{
    return (snapshot.State == Kernel::Memory::FrameState::Ready) &&
           (snapshot.Descriptor.PhysicalStart.GetValue() == arena.PhysicalStart) &&
           (snapshot.Descriptor.Pages.GetValue() == arena.PageCount) &&
           (snapshot.Descriptor.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (snapshot.Descriptor.Kind == Kernel::Memory::FrameKind::Ram) &&
           BytesAreZero(snapshot.Descriptor.Reserved, sizeof(snapshot.Descriptor.Reserved)) &&
           BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved)) && chargeOwner.IsValid();
}

[[nodiscard]] constexpr bool ResourceCapacityIsSufficient(const Kernel::Resource::ResourceDomainSnapshot& snapshot,
                                                          const Foundation::UInt64 pages,
                                                          const Foundation::UInt64 metadataBytes) noexcept
{
    if ((snapshot.Usage.CommittedMemoryPages > (MaximumUInt64 - pages)) ||
        (snapshot.Usage.ResidentMemoryPages > (MaximumUInt64 - pages)) ||
        (snapshot.Usage.KernelMetadataBytes > (MaximumUInt64 - metadataBytes)))
    {
        return false;
    }
    return ((snapshot.Usage.CommittedMemoryPages + pages) <= snapshot.Limits.CommittedMemoryPages) &&
           ((snapshot.Usage.ResidentMemoryPages + pages) <= snapshot.Limits.ResidentMemoryPages) &&
           ((snapshot.Usage.KernelMetadataBytes + metadataBytes) <= snapshot.Limits.KernelMetadataBytes);
}

[[nodiscard]] constexpr bool ResourceUsageHasExactIncrease(const Kernel::Resource::ResourceDomainSnapshot& initial,
                                                           const Kernel::Resource::ResourceDomainSnapshot& final,
                                                           const Foundation::UInt64 pages,
                                                           const Foundation::UInt64 metadataBytes) noexcept
{
    return (final.Usage.CommittedMemoryPages == (initial.Usage.CommittedMemoryPages + pages)) &&
           (final.Usage.ResidentMemoryPages == (initial.Usage.ResidentMemoryPages + pages)) &&
           (final.Usage.KernelMetadataBytes == (initial.Usage.KernelMetadataBytes + metadataBytes)) &&
           (final.Usage.CapabilityHandles == initial.Usage.CapabilityHandles) &&
           (final.Usage.IpcQueueBytes == initial.Usage.IpcQueueBytes) &&
           (final.Usage.Threads == initial.Usage.Threads) && (final.Usage.ChildDomains == initial.Usage.ChildDomains);
}

[[nodiscard]] constexpr bool
ResourceDomainEnvelopeIsUnchanged(const Kernel::Resource::ResourceDomainSnapshot& initial,
                                  const Kernel::Resource::ResourceDomainSnapshot& final) noexcept
{
    return (final.Identity.GetValue() == initial.Identity.GetValue()) && final.Parent.IsEqualTo(initial.Parent) &&
           (final.Limits.CommittedMemoryPages == initial.Limits.CommittedMemoryPages) &&
           (final.Limits.ResidentMemoryPages == initial.Limits.ResidentMemoryPages) &&
           (final.Limits.KernelMetadataBytes == initial.Limits.KernelMetadataBytes) &&
           (final.Limits.CapabilityHandles == initial.Limits.CapabilityHandles) &&
           (final.Limits.IpcQueueBytes == initial.Limits.IpcQueueBytes) &&
           (final.Limits.Threads == initial.Limits.Threads) &&
           (final.Limits.ChildDomains == initial.Limits.ChildDomains) && (final.Depth == initial.Depth) &&
           (final.State == Kernel::Resource::ResourceDomainState::Ready) &&
           BytesAreZero(final.Reserved, sizeof(final.Reserved));
}

[[nodiscard]] constexpr bool
MemoryObjectSnapshotMatches(const Kernel::Memory::MemoryObjectSnapshot& snapshot, const RangePlan& plan,
                            const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                            const Kernel::Object::ResourceDomainId resourceDomainIdentifier) noexcept
{
    return (snapshot.State == Kernel::Memory::MemoryObjectState::Ready) &&
           (snapshot.Descriptor.Pages.GetValue() == plan.Pages.GetValue()) &&
           (snapshot.Descriptor.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (snapshot.Descriptor.MaximumPermissions.GetBits() == plan.Permissions.GetBits()) &&
           (snapshot.Descriptor.Commitment == Kernel::Memory::MemoryCommitment::Pinned) &&
           BytesAreZero(snapshot.Descriptor.Reserved, sizeof(snapshot.Descriptor.Reserved)) &&
           snapshot.ResourceDomainIdentity.IsEqualTo(resourceDomainIdentity) &&
           (snapshot.ResourceDomain.GetValue() == resourceDomainIdentifier.GetValue()) &&
           (snapshot.ActiveMappings == 0U) && BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}
} // namespace

void Ia64InitialUserImageMemoryOwner::ResetPreflightStorage() noexcept
{
    for (Foundation::Size index{0U}; index < Ia64InitialUserImageMaximumMemoryObjectCount; ++index)
    {
        m_rangePlans[index] = Ia64InitialUserImageMemoryRangePlan{};
    }
    for (Foundation::Size index{0U}; index < Ia64InitialUserImageMaximumSegmentCount; ++index)
    {
        m_stagedSegments[index] = Ia64InitialUserImageSegmentPlan{};
    }
    m_stagedSummary = Ia64InitialUserImageStageSummary{};
    m_summary = Ia64InitialUserImageMemorySummary{};
    m_failure = Ia64InitialUserImageMemoryFailure{};
    m_initialResourceDomain = Kernel::Resource::ResourceDomainSnapshot{};
    m_initialRegistryObjectCount = 0U;
    m_initializedFrameCount = 0U;
    m_initializedMemoryObjectCount = 0U;
    m_registeredObjectCount = 0U;
    m_hasFailure = false;
}

MemoryOwnerError Ia64InitialUserImageMemoryOwner::PreflightSegments(const Ia64InitialUserImageOwner& stagedOwner,
                                                                    const Kernel::Memory::PhysicalPageRange& arena,
                                                                    Foundation::UInt64& packedPages,
                                                                    Foundation::Size& rangeCount) noexcept
{
    Foundation::UInt64 previousVirtualEnd{0U};
    bool hasPreviousVirtualRange{false};
    for (Foundation::Size index{0U}; index < m_stagedSummary.SegmentCount; ++index)
    {
        const auto segmentResult = stagedOwner.GetSegmentPlan(index);
        if (!segmentResult.HasValue())
        {
            m_failure.SourceAccess = segmentResult.GetError();
            m_failure.OriginalSegmentIndex = index;
            return MemoryOwnerError::SegmentPlanRejected;
        }
        const Ia64InitialUserImageSegmentPlan& staged = segmentResult.GetValue();
        m_stagedSegments[index] = staged;
        const ::Asteria::Executable::Elf::Elf64LoadSegment& segment = staged.Segment;

        Foundation::UInt64 expectedPhysicalStart{0U};
        if ((packedPages > MaximumAggregateLoadPageCount) ||
            !TryAdd(arena.PhysicalStart, packedPages * PageSizeBytes, expectedPhysicalStart) ||
            (staged.PhysicalSlice.PhysicalStart != expectedPhysicalStart) ||
            (staged.PhysicalSlice.PageCount != segment.PageCount) ||
            !PhysicalRangeAttributesMatch(staged.PhysicalSlice, arena) || !SegmentGeometryIsValid(segment))
        {
            m_failure.OriginalSegmentIndex = index;
            return MemoryOwnerError::InvalidPhysicalPacking;
        }
        if (segment.PageCount > (MaximumAggregateLoadPageCount - packedPages))
        {
            m_failure.OriginalSegmentIndex = index;
            return MemoryOwnerError::InvalidStageSummary;
        }
        packedPages += segment.PageCount;
        if (segment.PageCount == 0U)
        {
            continue;
        }

        Kernel::Memory::PagePermissions permissions{};
        if (!TranslatePermissions(segment.Permissions, permissions))
        {
            m_failure.OriginalSegmentIndex = index;
            return MemoryOwnerError::PermissionRejected;
        }
        Foundation::UInt64 virtualEnd{0U};
        if (!TryGetPageRangeEnd(segment.PageStart, segment.PageCount, virtualEnd))
        {
            m_failure.OriginalSegmentIndex = index;
            return MemoryOwnerError::InvalidVirtualRange;
        }
        if (hasPreviousVirtualRange && (segment.PageStart < previousVirtualEnd))
        {
            m_failure.OriginalSegmentIndex = index;
            return MemoryOwnerError::VirtualRangeOrderInvalid;
        }
        if (rangeCount >= Ia64InitialUserImageMaximumMemoryObjectCount)
        {
            m_failure.OriginalSegmentIndex = index;
            return MemoryOwnerError::RangeCapacityExceeded;
        }
        m_rangePlans[rangeCount] = Ia64InitialUserImageMemoryRangePlan{
            .Purpose = MemoryPurpose::LoadSegment,
            .GuardPolicy = GuardPolicy::None,
            .Permissions = permissions,
            .Reserved = {},
            .OriginalSegmentIndex = index,
            .VirtualStart = segment.PageStart,
            .Pages = Kernel::Memory::PageCount{segment.PageCount},
            .FrameOffsetPages = packedPages - segment.PageCount,
            .MemoryObject = {},
        };
        previousVirtualEnd = virtualEnd;
        hasPreviousVirtualRange = true;
        ++rangeCount;
    }

    if (packedPages != m_stagedSummary.AggregateLoadPageCount)
    {
        return MemoryOwnerError::InvalidStageSummary;
    }
    return MemoryOwnerError::None;
}

MemoryOwnerError Ia64InitialUserImageMemoryOwner::PreflightFixedRanges(const Kernel::Memory::PhysicalPageRange& arena,
                                                                       const Foundation::UInt64 packedPages,
                                                                       Foundation::Size& rangeCount) noexcept
{
    Foundation::UInt64 expectedStackPhysicalStart{0U};
    Foundation::UInt64 expectedRsePhysicalStart{0U};
    if (!TryAdd(arena.PhysicalStart, packedPages * PageSizeBytes, expectedStackPhysicalStart) ||
        !TryAdd(expectedStackPhysicalStart, Ia64InitialUserMemoryStackPageCount * PageSizeBytes,
                expectedRsePhysicalStart) ||
        (m_stagedSummary.MemoryStack.PhysicalStart != expectedStackPhysicalStart) ||
        (m_stagedSummary.MemoryStack.PageCount != Ia64InitialUserMemoryStackPageCount) ||
        !PhysicalRangeAttributesMatch(m_stagedSummary.MemoryStack, arena) ||
        (m_stagedSummary.RseBackingStore.PhysicalStart != expectedRsePhysicalStart) ||
        (m_stagedSummary.RseBackingStore.PageCount != Ia64InitialUserRseBackingStorePageCount) ||
        !PhysicalRangeAttributesMatch(m_stagedSummary.RseBackingStore, arena) ||
        (arena.PageCount !=
         (packedPages + Ia64InitialUserMemoryStackPageCount + Ia64InitialUserRseBackingStorePageCount)))
    {
        return MemoryOwnerError::InvalidPhysicalPacking;
    }

    constexpr Foundation::UInt64 stackStart{Ia64InitialUserMemoryStackVirtualStart};
    constexpr Foundation::UInt64 rseStart{Ia64InitialUserRseBackingStoreVirtualStart};
    Foundation::UInt64 stackEnd{0U};
    Foundation::UInt64 rseEnd{0U};
    if (!TryGetPageRangeEnd(stackStart, Ia64InitialUserMemoryStackPageCount, stackEnd) ||
        !TryGetPageRangeEnd(rseStart, Ia64InitialUserRseBackingStorePageCount, rseEnd) ||
        (stackStart < PageSizeBytes) || (rseStart < PageSizeBytes) || (stackEnd > (MaximumUInt64 - PageSizeBytes)) ||
        (rseEnd > (MaximumUInt64 - PageSizeBytes)) ||
        RangesOverlap(stackStart - PageSizeBytes, stackEnd + PageSizeBytes, rseStart - PageSizeBytes,
                      rseEnd + PageSizeBytes) ||
        ((rangeCount + 2U) > Ia64InitialUserImageMaximumMemoryObjectCount))
    {
        return MemoryOwnerError::VirtualRangeOrderInvalid;
    }

    const Foundation::UInt64 stackGuardedStart{stackStart - PageSizeBytes};
    const Foundation::UInt64 stackGuardedEnd{stackEnd + PageSizeBytes};
    const Foundation::UInt64 rseGuardedStart{rseStart - PageSizeBytes};
    const Foundation::UInt64 rseGuardedEnd{rseEnd + PageSizeBytes};
    for (Foundation::Size index{0U}; index < rangeCount; ++index)
    {
        const RangePlan& load = m_rangePlans[index];
        Foundation::UInt64 loadEnd{0U};
        if (!TryGetPageRangeEnd(load.VirtualStart, load.Pages.GetValue(), loadEnd) ||
            RangesOverlap(load.VirtualStart, loadEnd, stackGuardedStart, stackGuardedEnd) ||
            RangesOverlap(load.VirtualStart, loadEnd, rseGuardedStart, rseGuardedEnd))
        {
            m_failure.RangeIndex = index;
            m_failure.OriginalSegmentIndex = load.OriginalSegmentIndex;
            return MemoryOwnerError::VirtualRangeOrderInvalid;
        }
    }

    const Kernel::Memory::PagePermissions writablePermissions = WritableUserPermissions();
    m_rangePlans[rangeCount] = Ia64InitialUserImageMemoryRangePlan{
        .Purpose = MemoryPurpose::MemoryStack,
        .GuardPolicy = GuardPolicy::OnePageBeforeAndAfter,
        .Permissions = writablePermissions,
        .Reserved = {},
        .OriginalSegmentIndex = Ia64InitialUserImageNoSegment,
        .VirtualStart = stackStart,
        .Pages = Kernel::Memory::PageCount{Ia64InitialUserMemoryStackPageCount},
        .FrameOffsetPages = packedPages,
        .MemoryObject = {},
    };
    ++rangeCount;
    m_rangePlans[rangeCount] = Ia64InitialUserImageMemoryRangePlan{
        .Purpose = MemoryPurpose::RseBackingStore,
        .GuardPolicy = GuardPolicy::OnePageBeforeAndAfter,
        .Permissions = writablePermissions,
        .Reserved = {},
        .OriginalSegmentIndex = Ia64InitialUserImageNoSegment,
        .VirtualStart = rseStart,
        .Pages = Kernel::Memory::PageCount{Ia64InitialUserRseBackingStorePageCount},
        .FrameOffsetPages = packedPages + Ia64InitialUserMemoryStackPageCount,
        .MemoryObject = {},
    };
    ++rangeCount;
    return MemoryOwnerError::None;
}

MemoryOwnerError Ia64InitialUserImageMemoryOwner::PreflightSource(const Ia64InitialUserImageOwner& stagedOwner) noexcept
{
    if (stagedOwner.GetState() != Ia64InitialUserImageOwnerState::Staged)
    {
        return MemoryOwnerError::SourceNotStaged;
    }
    const auto summaryResult = stagedOwner.GetStageSummary();
    if (!summaryResult.HasValue())
    {
        m_failure.SourceAccess = summaryResult.GetError();
        return MemoryOwnerError::SourceAccessRejected;
    }
    m_stagedSummary = summaryResult.GetValue();

    const Kernel::Memory::PhysicalPageRange& arena = m_stagedSummary.AggregateArena;
    Foundation::UInt64 arenaEnd{0U};
    if ((m_stagedSummary.SegmentCount > Ia64InitialUserImageMaximumSegmentCount) ||
        (m_stagedSummary.AggregateLoadPageCount > MaximumAggregateLoadPageCount) ||
        (m_stagedSummary.TotalBssSizeBytes == 0U) || (m_stagedSummary.Image.Machine != Ia64ElfMachine) ||
        (m_stagedSummary.Image.Flags != Ia64P64ElfFlags) ||
        (m_stagedSummary.Image.ProgramHeaderCount < m_stagedSummary.SegmentCount) || (arena.PhysicalStart == 0U) ||
        ((arena.PhysicalStart & PageOffsetMask) != 0U) || (arena.PageCount == 0U) ||
        (arena.PageCount > Ia64InitialUserImageMaximumMaterializedPageCount) ||
        !TryGetPageRangeEnd(arena.PhysicalStart, arena.PageCount, arenaEnd) ||
        (arenaEnd > MaximumPhysicalAddressExclusive) || !AllocationAttributesAreValid(arena))
    {
        return MemoryOwnerError::InvalidStageSummary;
    }

    Foundation::UInt64 packedPages{0U};
    Foundation::Size rangeCount{0U};
    MemoryOwnerError error = PreflightSegments(stagedOwner, arena, packedPages, rangeCount);
    if (error == MemoryOwnerError::None)
    {
        error = PreflightFixedRanges(arena, packedPages, rangeCount);
    }
    if (error != MemoryOwnerError::None)
    {
        return error;
    }

    m_summary = Ia64InitialUserImageMemorySummary{
        .Image = m_stagedSummary.Image,
        .AggregateArena = arena,
        .AggregateFrame = {},
        .ResourceDomainGeneration = {},
        .RangeCount = rangeCount,
    };
    return MemoryOwnerError::None;
}

MemoryOwnerError
Ia64InitialUserImageMemoryOwner::PreflightObjects(Kernel::Object::ObjectRegistry& registry,
                                                  Kernel::Object::RootCapabilityIssuer& issuer,
                                                  const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                                  Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    if (!issuer.IsFromRegistry(registry))
    {
        return MemoryOwnerError::IssuerRegistryMismatch;
    }
    if (!resourceDomainIdentity.IsValid() || !resourceDomainIdentity.IsFromRegistry(registry))
    {
        return MemoryOwnerError::InvalidResourceDomain;
    }

    Kernel::Object::ObjectPin verification;
    m_failure.Pin = registry.PinObject(resourceDomainIdentity.GetObjectId(), Kernel::Object::ObjectType::ResourceDomain,
                                       verification);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
        (&verification.GetObject() != static_cast<Kernel::Object::IKernelObject*>(&resourceDomain)))
    {
        return MemoryOwnerError::InvalidResourceDomain;
    }
    verification.Reset();

    m_initialResourceDomain = resourceDomain.GetSnapshot();
    const Kernel::Object::ResourceDomainId resourceDomainIdentifier = resourceDomain.GetIdentifier();
    if (!resourceDomainIdentifier.IsValid() ||
        (m_initialResourceDomain.State != Kernel::Resource::ResourceDomainState::Ready) ||
        m_initialResourceDomain.Parent.IsValid() || (m_initialResourceDomain.Depth != 0U) ||
        (m_initialResourceDomain.Identity.GetValue() != resourceDomainIdentifier.GetValue()) ||
        !BytesAreZero(m_initialResourceDomain.Reserved, sizeof(m_initialResourceDomain.Reserved)))
    {
        return MemoryOwnerError::InvalidResourceDomain;
    }

    m_initialRegistryObjectCount = registry.GetActiveCount();
    const Foundation::Size registryCapacity = registry.GetCapacity();
    const Foundation::Size requiredRegistrations = m_summary.RangeCount + 1U;
    if ((m_initialRegistryObjectCount > registryCapacity) ||
        (requiredRegistrations > (registryCapacity - m_initialRegistryObjectCount)))
    {
        return MemoryOwnerError::RegistryCapacityExceeded;
    }

    const Foundation::UInt64 requiredPages = m_summary.AggregateArena.PageCount;
    const Foundation::UInt64 requiredMetadataBytes =
        static_cast<Foundation::UInt64>(m_summary.RangeCount) * sizeof(Kernel::Memory::MemoryObject);
    if (!ResourceCapacityIsSufficient(m_initialResourceDomain, requiredPages, requiredMetadataBytes))
    {
        return MemoryOwnerError::ResourceCapacityExceeded;
    }

    m_summary.ResourceDomainGeneration = resourceDomainIdentity;
    return MemoryOwnerError::None;
}

MemoryOwnerError Ia64InitialUserImageMemoryOwner::InitializeObjects(
    Kernel::Object::ObjectRegistry& registry, const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    const Kernel::Object::ResourceDomainId chargeOwner = resourceDomain.GetIdentifier();
    m_failure.Frame = m_aggregateFrame.Initialize(
        chargeOwner, Kernel::Memory::FrameDescriptor{
                         .PhysicalStart = Kernel::Memory::PhysicalAddress{m_summary.AggregateArena.PhysicalStart},
                         .Pages = Kernel::Memory::PageCount{m_summary.AggregateArena.PageCount},
                         .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                         .Kind = Kernel::Memory::FrameKind::Ram,
                         .Reserved = {},
                     });
    if (m_failure.Frame != Kernel::Memory::FrameError::None)
    {
        return MemoryOwnerError::FrameInitializationRejected;
    }
    m_initializedFrameCount = 1U;

    for (Foundation::Size index{0U}; index < m_summary.RangeCount; ++index)
    {
        const RangePlan& plan = m_rangePlans[index];
        m_failure.RangeIndex = index;
        m_failure.OriginalSegmentIndex = plan.OriginalSegmentIndex;
        m_failure.MemoryObject =
            m_memoryObjects[index].Initialize(registry, resourceDomainIdentity, resourceDomain,
                                              Kernel::Memory::MemoryObjectDescriptor{
                                                  .Pages = plan.Pages,
                                                  .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                                  .MaximumPermissions = plan.Permissions,
                                                  .Commitment = Kernel::Memory::MemoryCommitment::Pinned,
                                                  .Reserved = {},
                                              });
        if (m_failure.MemoryObject.Error != Kernel::Memory::MemoryObjectError::None)
        {
            return MemoryOwnerError::MemoryObjectInitializationRejected;
        }
        ++m_initializedMemoryObjectCount;
    }
    return MemoryOwnerError::None;
}

MemoryOwnerError Ia64InitialUserImageMemoryOwner::RegisterAndVerify(Kernel::Object::ObjectRegistry& registry,
                                                                    Kernel::Object::RootCapabilityIssuer& issuer,
                                                                    Kernel::Object::IKernelObject& object,
                                                                    const Kernel::Object::ObjectType expectedType,
                                                                    Kernel::Object::ObjectId& identity) noexcept
{
    const auto registered = issuer.CreateObject(object);
    if (!registered.HasValue())
    {
        m_failure.Registry = registered.GetError();
        return MemoryOwnerError::ObjectRegistrationRejected;
    }
    ++m_registeredObjectCount;

    Kernel::Object::ObjectPin verification;
    m_failure.Pin = registry.PinObject(registered.GetValue(), expectedType, verification);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
        (verification.GetObjectId().GetValue() != registered.GetValue().GetValue()) ||
        (verification.GetObjectType() != expectedType) || (&verification.GetObject() != &object))
    {
        return MemoryOwnerError::RegistryBindingRejected;
    }
    identity = registered.GetValue();
    verification.Reset();
    return MemoryOwnerError::None;
}

MemoryOwnerError Ia64InitialUserImageMemoryOwner::RegisterObjects(Kernel::Object::ObjectRegistry& registry,
                                                                  Kernel::Object::RootCapabilityIssuer& issuer) noexcept
{
    Kernel::Object::ObjectId identity{};
    MemoryOwnerError error =
        RegisterAndVerify(registry, issuer, m_aggregateFrame, Kernel::Object::ObjectType::Frame, identity);
    if (error != MemoryOwnerError::None)
    {
        return error;
    }
    m_summary.AggregateFrame = Kernel::Memory::FrameId{registry, identity};

    for (Foundation::Size index{0U}; index < m_summary.RangeCount; ++index)
    {
        m_failure.RangeIndex = index;
        m_failure.OriginalSegmentIndex = m_rangePlans[index].OriginalSegmentIndex;
        identity = Kernel::Object::ObjectId{};
        error = RegisterAndVerify(registry, issuer, m_memoryObjects[index], Kernel::Object::ObjectType::MemoryObject,
                                  identity);
        if (error != MemoryOwnerError::None)
        {
            return error;
        }
        m_rangePlans[index].MemoryObject = Kernel::Memory::MemoryObjectId{registry, identity};
    }
    return MemoryOwnerError::None;
}

bool Ia64InitialUserImageMemoryOwner::StagedOwnerIsUnchanged(
    const Ia64InitialUserImageOwner& stagedOwner, const Ia64InitialUserImageOwnerState expectedState) const noexcept
{
    if (stagedOwner.GetState() != expectedState)
    {
        return false;
    }
    const auto summary = stagedOwner.GetStageSummary();
    if (!summary.HasValue() || !StageSummariesAreEqual(summary.GetValue(), m_stagedSummary))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < m_stagedSummary.SegmentCount; ++index)
    {
        const auto segment = stagedOwner.GetSegmentPlan(index);
        if (!segment.HasValue() || !SegmentPlansAreEqual(segment.GetValue(), m_stagedSegments[index]))
        {
            return false;
        }
    }
    return true;
}

MemoryOwnerError
Ia64InitialUserImageMemoryOwner::VerifyPostconditions(const Ia64InitialUserImageOwner& stagedOwner,
                                                      Kernel::Object::ObjectRegistry& registry,
                                                      Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    const Foundation::Size requiredRegistrations = m_summary.RangeCount + 1U;
    const Foundation::UInt64 requiredPages = m_summary.AggregateArena.PageCount;
    const Foundation::UInt64 requiredMetadataBytes =
        static_cast<Foundation::UInt64>(m_summary.RangeCount) * sizeof(Kernel::Memory::MemoryObject);
    const Kernel::Object::ResourceDomainId resourceDomainIdentifier = resourceDomain.GetIdentifier();
    const Kernel::Resource::ResourceDomainSnapshot finalResourceDomain = resourceDomain.GetSnapshot();
    if ((m_initializedFrameCount != 1U) || (m_initializedMemoryObjectCount != m_summary.RangeCount) ||
        (m_registeredObjectCount != requiredRegistrations) ||
        (registry.GetActiveCount() != (m_initialRegistryObjectCount + requiredRegistrations)) ||
        !resourceDomainIdentifier.IsValid() ||
        !ResourceDomainEnvelopeIsUnchanged(m_initialResourceDomain, finalResourceDomain) ||
        !ResourceUsageHasExactIncrease(m_initialResourceDomain, finalResourceDomain, requiredPages,
                                       requiredMetadataBytes) ||
        !m_summary.AggregateFrame.IsValid() || !m_summary.AggregateFrame.IsFromRegistry(registry) ||
        !m_summary.ResourceDomainGeneration.IsValid() || !m_summary.ResourceDomainGeneration.IsFromRegistry(registry) ||
        !FrameSnapshotMatches(m_aggregateFrame.GetSnapshot(), m_summary.AggregateArena, resourceDomainIdentifier) ||
        (m_aggregateFrame.GetChargeOwner().GetValue() != resourceDomainIdentifier.GetValue()) ||
        !StagedOwnerIsUnchanged(stagedOwner, Ia64InitialUserImageOwnerState::Claimed))
    {
        return MemoryOwnerError::PostconditionRejected;
    }

    Kernel::Object::ObjectPin verification;
    m_failure.Pin =
        registry.PinObject(m_summary.AggregateFrame.GetObjectId(), Kernel::Object::ObjectType::Frame, verification);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
        (&verification.GetObject() != static_cast<Kernel::Object::IKernelObject*>(&m_aggregateFrame)))
    {
        return MemoryOwnerError::PostconditionRejected;
    }
    verification.Reset();

    for (Foundation::Size index{0U}; index < m_summary.RangeCount; ++index)
    {
        const RangePlan& plan = m_rangePlans[index];
        m_failure.RangeIndex = index;
        m_failure.OriginalSegmentIndex = plan.OriginalSegmentIndex;
        if (!plan.MemoryObject.IsValid() || !plan.MemoryObject.IsFromRegistry(registry) ||
            !MemoryObjectSnapshotMatches(m_memoryObjects[index].GetSnapshot(), plan, m_summary.ResourceDomainGeneration,
                                         resourceDomainIdentifier))
        {
            return MemoryOwnerError::PostconditionRejected;
        }
        m_failure.Pin =
            registry.PinObject(plan.MemoryObject.GetObjectId(), Kernel::Object::ObjectType::MemoryObject, verification);
        if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
            (&verification.GetObject() != static_cast<Kernel::Object::IKernelObject*>(&m_memoryObjects[index])))
        {
            return MemoryOwnerError::PostconditionRejected;
        }
        verification.Reset();
    }
    return MemoryOwnerError::None;
}

Ia64InitialUserImageMemoryOwner::MaterializeResult
Ia64InitialUserImageMemoryOwner::Fail(const MemoryOwnerError error) noexcept
{
    m_failure.Error = error;
    m_hasFailure = true;
    return MaterializeResult::Failure(m_failure);
}

Ia64InitialUserImageMemoryOwner::MaterializeResult Ia64InitialUserImageMemoryOwner::Materialize(
    Ia64InitialUserImageOwner& stagedOwner, Kernel::Object::ObjectRegistry& registry,
    Kernel::Object::RootCapabilityIssuer& issuer, const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    if (m_state != Ia64InitialUserImageMemoryOwnerState::Fresh)
    {
        Ia64InitialUserImageMemoryFailure repeated{};
        repeated.Error = MemoryOwnerError::AlreadyAttempted;
        repeated.RangeIndex = Ia64InitialUserImageNoSegment;
        repeated.OriginalSegmentIndex = Ia64InitialUserImageNoSegment;
        return MaterializeResult::Failure(repeated);
    }

    ResetPreflightStorage();
    m_failure.RangeIndex = Ia64InitialUserImageNoSegment;
    m_failure.OriginalSegmentIndex = Ia64InitialUserImageNoSegment;
    MemoryOwnerError error = PreflightSource(stagedOwner);
    if (error == MemoryOwnerError::None)
    {
        error = PreflightObjects(registry, issuer, resourceDomainIdentity, resourceDomain);
    }
    if ((error == MemoryOwnerError::None) &&
        !StagedOwnerIsUnchanged(stagedOwner, Ia64InitialUserImageOwnerState::Staged))
    {
        error = MemoryOwnerError::StagedOwnerChanged;
    }
    if (error != MemoryOwnerError::None)
    {
        return Fail(error);
    }
    if (!stagedOwner.ClaimForMemoryOwner())
    {
        return Fail(MemoryOwnerError::StagedOwnerChanged);
    }

    m_state = Ia64InitialUserImageMemoryOwnerState::Failed;
    error = InitializeObjects(registry, resourceDomainIdentity, resourceDomain);
    if (error == MemoryOwnerError::None)
    {
        error = RegisterObjects(registry, issuer);
    }
    if (error == MemoryOwnerError::None)
    {
        error = VerifyPostconditions(stagedOwner, registry, resourceDomain);
    }
    if (error != MemoryOwnerError::None)
    {
        return Fail(error);
    }

    m_failure = Ia64InitialUserImageMemoryFailure{};
    m_hasFailure = false;
    m_state = Ia64InitialUserImageMemoryOwnerState::Ready;
    return MaterializeResult::Success(m_summary);
}

Foundation::Result<Ia64InitialUserImageMemorySummary, MemoryAccessError>
Ia64InitialUserImageMemoryOwner::GetSummary() const noexcept
{
    using ResultType = Foundation::Result<Ia64InitialUserImageMemorySummary, MemoryAccessError>;
    if (m_state != Ia64InitialUserImageMemoryOwnerState::Ready)
    {
        return ResultType::Failure(MemoryAccessError::NotReady);
    }
    return ResultType::Success(m_summary);
}

Foundation::Result<Ia64InitialUserImageMemoryRangePlan, MemoryAccessError>
Ia64InitialUserImageMemoryOwner::GetRangePlan(const Foundation::Size index) const noexcept
{
    using ResultType = Foundation::Result<Ia64InitialUserImageMemoryRangePlan, MemoryAccessError>;
    if (m_state != Ia64InitialUserImageMemoryOwnerState::Ready)
    {
        return ResultType::Failure(MemoryAccessError::NotReady);
    }
    if (index >= m_summary.RangeCount)
    {
        return ResultType::Failure(MemoryAccessError::IndexOutOfBounds);
    }
    return ResultType::Success(m_rangePlans[index]);
}

Foundation::Result<Ia64InitialUserImageMemoryRangeView, MemoryAccessError>
Ia64InitialUserImageMemoryOwner::BorrowRange(const Foundation::Size index) noexcept
{
    using ResultType = Foundation::Result<Ia64InitialUserImageMemoryRangeView, MemoryAccessError>;
    if (m_state != Ia64InitialUserImageMemoryOwnerState::Ready)
    {
        return ResultType::Failure(MemoryAccessError::NotReady);
    }
    if (index >= m_summary.RangeCount)
    {
        return ResultType::Failure(MemoryAccessError::IndexOutOfBounds);
    }
    return ResultType::Success(Ia64InitialUserImageMemoryRangeView{
        .Plan = m_rangePlans[index],
        .FrameStorage = &m_aggregateFrame,
        .MemoryObjectStorage = &m_memoryObjects[index],
    });
}

Foundation::Result<Ia64InitialUserImageMemoryFailure, MemoryAccessError>
Ia64InitialUserImageMemoryOwner::GetFailure() const noexcept
{
    using ResultType = Foundation::Result<Ia64InitialUserImageMemoryFailure, MemoryAccessError>;
    if (!m_hasFailure)
    {
        return ResultType::Failure(MemoryAccessError::NoFailure);
    }
    return ResultType::Success(m_failure);
}
} // namespace Asteria::Platform::Ia64::Executable
