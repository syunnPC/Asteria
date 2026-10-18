// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Executable/Ia64InitialUserImageMappingOwner.hpp"

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
void* operator new(Asteria::Foundation::Size sizeBytes, void* storage) noexcept;
#else
#include <new>
#endif

#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeTranslationEncoder.hpp"

namespace Asteria::Platform::Ia64::Executable
{
namespace
{
using AccessError = Ia64InitialUserImageMappingAccessError;
using Failure = Ia64InitialUserImageMappingFailure;
using GuardPolicy = Ia64InitialUserImageGuardPolicy;
using InstalledMapping = Ia64InitialUserImageInstalledMapping;
using MappingOwnerError = Ia64InitialUserImageMappingOwnerError;
using MappingPurpose = Ia64InitialUserImageMemoryPurpose;
using MappingResult = Foundation::Result<Ia64InitialUserImageMappingSummary, Failure>;
using RangePlan = Ia64InitialUserImageMemoryRangePlan;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageSizeBytes{Kernel::Memory::MappingPageSizeBytes};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt8 MinimumPhysicalAddressBits{32U};
constexpr Foundation::UInt8 MaximumPhysicalAddressBits{63U};
constexpr Foundation::UInt8 MinimumImplementedVirtualAddressBit{50U};
constexpr Foundation::UInt8 MaximumImplementedVirtualAddressBit{60U};
constexpr Foundation::UInt8 RuntimeTranslationPhysicalAddressBits{50U};

static_assert(PageSizeBytes == Ia64UserPageSizeBytes);

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

[[nodiscard]] constexpr bool BytesAreZero(const Foundation::UInt8* const bytes,
                                          const Foundation::Size sizeBytes) noexcept
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

[[nodiscard]] constexpr bool ImageInformationIsEqual(const Ia64P64ImageInformation& first,
                                                     const Ia64P64ImageInformation& second) noexcept
{
    return (first.EntryDescriptorAddress == second.EntryDescriptorAddress) &&
           (first.EntryBundleAddress == second.EntryBundleAddress) && (first.GlobalPointer == second.GlobalPointer) &&
           (first.Flags == second.Flags) && (first.Machine == second.Machine) &&
           (first.ProgramHeaderCount == second.ProgramHeaderCount);
}

[[nodiscard]] constexpr bool PhysicalRangesAreEqual(const Kernel::Memory::PhysicalPageRange& first,
                                                    const Kernel::Memory::PhysicalPageRange& second) noexcept
{
    return (first.PhysicalStart == second.PhysicalStart) && (first.PageCount == second.PageCount) &&
           (first.Attributes == second.Attributes) && (first.FirmwareType == second.FirmwareType) &&
           (first.Reserved == second.Reserved);
}

[[nodiscard]] constexpr bool MemorySummariesAreEqual(const Ia64InitialUserImageMemorySummary& first,
                                                     const Ia64InitialUserImageMemorySummary& second) noexcept
{
    return ImageInformationIsEqual(first.Image, second.Image) &&
           PhysicalRangesAreEqual(first.AggregateArena, second.AggregateArena) &&
           first.AggregateFrame.IsEqualTo(second.AggregateFrame) &&
           first.ResourceDomainGeneration.IsEqualTo(second.ResourceDomainGeneration) &&
           (first.RangeCount == second.RangeCount);
}

[[nodiscard]] constexpr bool RangePlansAreEqual(const RangePlan& first, const RangePlan& second) noexcept
{
    return (first.Purpose == second.Purpose) && (first.GuardPolicy == second.GuardPolicy) &&
           (first.Permissions.GetBits() == second.Permissions.GetBits()) &&
           BytesAreZero(first.Reserved, sizeof(first.Reserved)) &&
           BytesAreZero(second.Reserved, sizeof(second.Reserved)) &&
           (first.OriginalSegmentIndex == second.OriginalSegmentIndex) && (first.VirtualStart == second.VirtualStart) &&
           (first.Pages.GetValue() == second.Pages.GetValue()) && (first.FrameOffsetPages == second.FrameOffsetPages) &&
           first.MemoryObject.IsEqualTo(second.MemoryObject);
}

[[nodiscard]] constexpr bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
                                                         const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (implementedMostSignificantBit == MaximumImplementedVirtualAddressBit)
    {
        return true;
    }
    const Foundation::UInt8 unimplementedBitCount =
        static_cast<Foundation::UInt8>(MaximumImplementedVirtualAddressBit - implementedMostSignificantBit);
    const Foundation::UInt64 mask = (Foundation::UInt64{1U} << unimplementedBitCount) - 1U;
    const Foundation::UInt64 actual = (address >> (implementedMostSignificantBit + 1U)) & mask;
    const Foundation::UInt64 expected =
        ((address & (Foundation::UInt64{1U} << implementedMostSignificantBit)) != 0U) ? mask : 0U;
    return actual == expected;
}

[[nodiscard]] constexpr bool IsImplementedVirtualRange(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                                       const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    return (start < end) && ((start >> 61U) == Ia64InitialUserVirtualRegion) &&
           (((end - 1U) >> 61U) == Ia64InitialUserVirtualRegion) &&
           IsVirtualAddressImplemented(start, implementedMostSignificantBit) &&
           IsVirtualAddressImplemented(end - 1U, implementedMostSignificantBit);
}

[[nodiscard]] constexpr bool PermissionsAreValid(const Kernel::Memory::PagePermissions permissions) noexcept
{
    using PagePermission = Kernel::Memory::PagePermission;
    return permissions.HasOnlyKnownBits() && permissions.Contains(PagePermission::Read) &&
           permissions.Contains(PagePermission::User) &&
           !(permissions.Contains(PagePermission::Write) && permissions.Contains(PagePermission::Execute));
}

[[nodiscard]] constexpr Kernel::Memory::PagePermissions WritableUserPermissions() noexcept
{
    using PagePermission = Kernel::Memory::PagePermission;
    return Kernel::Memory::PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
}

[[nodiscard]] constexpr bool TryGetGuardedVirtualRange(const RangePlan& plan, Foundation::UInt64& start,
                                                       Foundation::UInt64& end) noexcept
{
    Foundation::UInt64 mappingEnd{0U};
    if (!TryGetPageRangeEnd(plan.VirtualStart, plan.Pages.GetValue(), mappingEnd))
    {
        return false;
    }
    switch (plan.GuardPolicy)
    {
    case GuardPolicy::None:
        start = plan.VirtualStart;
        end = mappingEnd;
        return true;
    case GuardPolicy::OnePageBeforeAndAfter:
        if ((plan.VirtualStart < PageSizeBytes) || (mappingEnd > (MaximumUInt64 - PageSizeBytes)))
        {
            return false;
        }
        start = plan.VirtualStart - PageSizeBytes;
        end = mappingEnd + PageSizeBytes;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool TryGetPhysicalRange(const Ia64InitialUserImageMemorySummary& summary,
                                                 const RangePlan& plan, Foundation::UInt64& start,
                                                 Foundation::UInt64& end) noexcept
{
    if ((plan.FrameOffsetPages > summary.AggregateArena.PageCount) ||
        (plan.Pages.GetValue() > (summary.AggregateArena.PageCount - plan.FrameOffsetPages)) ||
        (plan.FrameOffsetPages > (MaximumUInt64 / PageSizeBytes)))
    {
        return false;
    }
    return TryAdd(summary.AggregateArena.PhysicalStart, plan.FrameOffsetPages * PageSizeBytes, start) &&
           TryGetPageRangeEnd(start, plan.Pages.GetValue(), end);
}

[[nodiscard]] constexpr bool RootIsReadyAndNonresident(const Kernel::Memory::AddressSpaceSnapshot& snapshot,
                                                       const Kernel::Memory::AddressSpaceId identity) noexcept
{
    return snapshot.Identity.IsEqualTo(identity) && (snapshot.State == Kernel::Memory::AddressSpaceState::Ready) &&
           (snapshot.ResidentProcessorMask == 0U) && !snapshot.ActivatingProcessor.IsValid() &&
           !snapshot.TransitionProcessor.IsValid() && (snapshot.TransitionSequence == 0U) &&
           (snapshot.ShootdownSequence == 0U) && (snapshot.TargetProcessorMask == 0U) &&
           (snapshot.AcknowledgedProcessorMask == 0U) && BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

[[nodiscard]] constexpr bool RootSnapshotsAreEqual(const Kernel::Memory::AddressSpaceSnapshot& first,
                                                   const Kernel::Memory::AddressSpaceSnapshot& second) noexcept
{
    return first.Identity.IsEqualTo(second.Identity) && (first.MappingCount == second.MappingCount) &&
           (first.ShootdownSequence == second.ShootdownSequence) &&
           (first.TransitionSequence == second.TransitionSequence) && (first.State == second.State) &&
           (first.ResidentProcessorMask == second.ResidentProcessorMask) &&
           (first.ActivatingProcessor.GetValue() == second.ActivatingProcessor.GetValue()) &&
           (first.TransitionProcessor.GetValue() == second.TransitionProcessor.GetValue()) &&
           (first.TargetProcessorMask == second.TargetProcessorMask) &&
           (first.AcknowledgedProcessorMask == second.AcknowledgedProcessorMask) &&
           BytesAreZero(first.Reserved, sizeof(first.Reserved)) &&
           BytesAreZero(second.Reserved, sizeof(second.Reserved));
}

[[nodiscard]] constexpr bool FrameSnapshotMatches(const Kernel::Memory::FrameSnapshot& snapshot,
                                                  const Ia64InitialUserImageMemorySummary& summary) noexcept
{
    return (snapshot.State == Kernel::Memory::FrameState::Ready) &&
           (snapshot.Descriptor.PhysicalStart.GetValue() == summary.AggregateArena.PhysicalStart) &&
           (snapshot.Descriptor.Pages.GetValue() == summary.AggregateArena.PageCount) &&
           (snapshot.Descriptor.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (snapshot.Descriptor.Kind == Kernel::Memory::FrameKind::Ram) &&
           BytesAreZero(snapshot.Descriptor.Reserved, sizeof(snapshot.Descriptor.Reserved)) &&
           BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

[[nodiscard]] constexpr bool MemoryObjectSnapshotMatches(const Kernel::Memory::MemoryObjectSnapshot& snapshot,
                                                         const RangePlan& plan,
                                                         const Kernel::Resource::ResourceDomainObjectId resourceDomain,
                                                         const Foundation::UInt64 activeMappings) noexcept
{
    return (snapshot.State == Kernel::Memory::MemoryObjectState::Ready) &&
           (snapshot.Descriptor.Pages.GetValue() == plan.Pages.GetValue()) &&
           (snapshot.Descriptor.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (snapshot.Descriptor.MaximumPermissions.GetBits() == plan.Permissions.GetBits()) &&
           (snapshot.Descriptor.Commitment == Kernel::Memory::MemoryCommitment::Pinned) &&
           BytesAreZero(snapshot.Descriptor.Reserved, sizeof(snapshot.Descriptor.Reserved)) &&
           snapshot.ResourceDomainIdentity.IsEqualTo(resourceDomain) && snapshot.ResourceDomain.IsValid() &&
           (snapshot.ActiveMappings == activeMappings) && BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

[[nodiscard]] constexpr bool MappingRecordMatches(const Kernel::Memory::AddressSpaceMappingRecord& record,
                                                  const RangePlan& plan, const Kernel::Memory::FrameId frame,
                                                  const Foundation::UInt64 physicalStart) noexcept
{
    return record.MemoryObject.IsEqualTo(plan.MemoryObject) && !record.DeviceRegion.IsValid() &&
           record.Frame.IsEqualTo(frame) && (record.VirtualStart.GetValue() == plan.VirtualStart) &&
           (record.PhysicalStart.GetValue() == physicalStart) && (record.Pages.GetValue() == plan.Pages.GetValue()) &&
           (record.MemoryObjectOffsetPages == 0U) && (record.Permissions.GetBits() == plan.Permissions.GetBits()) &&
           (record.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (record.Kind == Kernel::Memory::FrameKind::Ram) &&
           (record.Commitment == Kernel::Memory::MemoryCommitment::Pinned) && record.RuntimeMapping &&
           BytesAreZero(record.Reserved, sizeof(record.Reserved));
}
} // namespace

void Ia64InitialUserImageMappingOwner::ResetPreflightStorage() noexcept
{
    for (Foundation::Size index{0U}; index < Ia64InitialUserImageMaximumMemoryObjectCount; ++index)
    {
        m_mappings[index] = Ia64InitialUserImageInstalledMapping{};
        m_memoryObjectStorage[index] = nullptr;
    }
    m_memorySummary = Ia64InitialUserImageMemorySummary{};
    ::new (static_cast<void*>(&m_bspSnapshot)) Memory::Ia64BspRuntimeMemorySnapshot{};
    ::new (static_cast<void*>(&m_initialComponentSnapshot)) Memory::Ia64RuntimeAddressSpaceOwnerSnapshot{};
    ::new (static_cast<void*>(&m_initialRootSnapshot)) Kernel::Memory::AddressSpaceSnapshot{};
    ::new (static_cast<void*>(&m_finalRootSnapshot)) Kernel::Memory::AddressSpaceSnapshot{};
    m_summary = Ia64InitialUserImageMappingSummary{};
    m_failure = Ia64InitialUserImageMappingFailure{};
    m_failure.RangeIndex = Ia64InitialUserImageNoSegment;
    m_registry = nullptr;
    m_rootAddressSpace = nullptr;
    m_aggregateFrameStorage = nullptr;
    m_controllerStorage = nullptr;
    m_resourceDomainIdentifier = Kernel::Object::ResourceDomainId{};
    m_bspProcessor = Kernel::Scheduler::CpuId{};
    m_commonPhysicalAddressBits = 0U;
    m_commonImplementedVirtualAddressMostSignificantBit = 0U;
    m_hasFailure = false;
}

MappingOwnerError
Ia64InitialUserImageMappingOwner::CaptureAndValidateOwners(Ia64InitialUserImageMemoryOwner& memoryOwner,
                                                           Memory::Ia64BspRuntimeMemory& bspRuntimeMemory,
                                                           Kernel::Object::ObjectRegistry& registry) noexcept
{
    if (memoryOwner.GetState() != Ia64InitialUserImageMemoryOwnerState::Ready)
    {
        return MappingOwnerError::MemoryOwnerNotReady;
    }
    const auto memorySummary = memoryOwner.GetSummary();
    if (!memorySummary.HasValue())
    {
        m_failure.MemoryAccess = memorySummary.GetError();
        return MappingOwnerError::MemoryOwnerAccessRejected;
    }
    m_memorySummary = memorySummary.GetValue();
    if ((m_memorySummary.RangeCount < 3U) ||
        (m_memorySummary.RangeCount > Ia64InitialUserImageMaximumMemoryObjectCount) ||
        (m_memorySummary.AggregateArena.PhysicalStart == 0U) ||
        ((m_memorySummary.AggregateArena.PhysicalStart & PageOffsetMask) != 0U) ||
        (m_memorySummary.AggregateArena.PageCount == 0U) || !m_memorySummary.AggregateFrame.IsValid() ||
        !m_memorySummary.AggregateFrame.IsFromRegistry(registry) ||
        !m_memorySummary.ResourceDomainGeneration.IsValid() ||
        !m_memorySummary.ResourceDomainGeneration.IsFromRegistry(registry))
    {
        return MappingOwnerError::InvalidMemorySummary;
    }

    ::new (static_cast<void*>(&m_bspSnapshot)) Memory::Ia64BspRuntimeMemorySnapshot{bspRuntimeMemory.GetSnapshot()};
    if (m_bspSnapshot.State != Memory::Ia64BspRuntimeMemoryState::Ready)
    {
        return MappingOwnerError::BspRuntimeMemoryNotReady;
    }
    const auto rootResult = bspRuntimeMemory.BorrowReadyUserRoot(m_domain);
    if (!rootResult.HasValue())
    {
        m_failure.RootBorrow = rootResult.GetError();
        return MappingOwnerError::InitialComponentRootBorrowRejected;
    }
    const Memory::Ia64BspRuntimeRootView& root = rootResult.GetValue();
    ::new (static_cast<void*>(&m_initialComponentSnapshot))
        Memory::Ia64RuntimeAddressSpaceOwnerSnapshot{bspRuntimeMemory.GetUserAddressSpaceSnapshot(m_domain)};
    if ((m_bspSnapshot.AddressSpaceStorage == nullptr) || !m_bspSnapshot.Summary.AddressSpaceIdentity.IsValid() ||
        !m_bspSnapshot.Summary.AddressSpaceIdentity.IsFromRegistry(registry) ||
        !m_bspSnapshot.AddressSpace.Identity.IsEqualTo(m_bspSnapshot.Summary.AddressSpaceIdentity) ||
        (m_bspSnapshot.Summary.BspProcessorIndex >= Kernel::Scheduler::MaximumProcessorCount) ||
        ((m_bspSnapshot.Summary.ProfiledProcessorMask &
          static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << m_bspSnapshot.Summary.BspProcessorIndex)) == 0U))
    {
        return MappingOwnerError::InvalidBspRuntime;
    }
    if ((root.AddressSpaceStorage == nullptr) || (root.ControllerStorage == nullptr) ||
        !root.AddressSpaceIdentity.IsValid() || !root.AddressSpaceIdentity.IsFromRegistry(registry) ||
        !root.Processor.IsValid() || (root.Processor.GetValue() != m_bspSnapshot.Summary.BspProcessorIndex) ||
        (m_initialComponentSnapshot.State != Memory::Ia64RuntimeAddressSpaceOwnerState::Ready) ||
        (m_initialComponentSnapshot.Summary.State != Memory::Ia64RuntimeAddressSpaceOwnerState::Ready) ||
        !m_initialComponentSnapshot.Summary.BackendClaimed ||
        (m_initialComponentSnapshot.AddressSpaceStorage != root.AddressSpaceStorage) ||
        !m_initialComponentSnapshot.Summary.AddressSpaceIdentity.IsEqualTo(root.AddressSpaceIdentity) ||
        !m_initialComponentSnapshot.AddressSpace.Identity.IsEqualTo(root.AddressSpaceIdentity))
    {
        return MappingOwnerError::InvalidInitialComponentRoot;
    }

    m_registry = &registry;
    m_rootAddressSpace = root.AddressSpaceStorage;
    m_controllerStorage = root.ControllerStorage;
    m_bspProcessor = root.Processor;
    ::new (static_cast<void*>(&m_initialRootSnapshot))
        Kernel::Memory::AddressSpaceSnapshot{m_rootAddressSpace->GetSnapshot()};
    m_finalRootSnapshot = m_initialRootSnapshot;
    if (!RootIsReadyAndNonresident(m_initialRootSnapshot, root.AddressSpaceIdentity) ||
        !RootSnapshotsAreEqual(m_initialRootSnapshot, m_initialComponentSnapshot.AddressSpace))
    {
        return MappingOwnerError::InvalidInitialComponentRoot;
    }

    Kernel::Object::ObjectPin rootPin;
    m_failure.Pin =
        registry.PinObject(root.AddressSpaceIdentity.GetObjectId(), Kernel::Object::ObjectType::AddressSpace, rootPin);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !rootPin.IsValid() ||
        (&rootPin.GetObject() != static_cast<Kernel::Object::IKernelObject*>(root.AddressSpaceStorage)))
    {
        return MappingOwnerError::RegistryBindingRejected;
    }
    rootPin.Reset();

    m_commonPhysicalAddressBits = m_bspSnapshot.Summary.CommonCapabilities.PhysicalAddressBits;
    m_commonImplementedVirtualAddressMostSignificantBit =
        m_bspSnapshot.Summary.CommonCapabilities.ImplementedVirtualAddressMostSignificantBit;
    if ((m_commonPhysicalAddressBits < MinimumPhysicalAddressBits) ||
        (m_commonPhysicalAddressBits > MaximumPhysicalAddressBits) ||
        (m_commonImplementedVirtualAddressMostSignificantBit < MinimumImplementedVirtualAddressBit) ||
        (m_commonImplementedVirtualAddressMostSignificantBit > MaximumImplementedVirtualAddressBit))
    {
        return MappingOwnerError::InvalidCommonAddressWidth;
    }

    const Foundation::Size seedMappingCount = m_initialComponentSnapshot.Summary.MappingCount;
    if ((seedMappingCount > m_initialRootSnapshot.MappingCount) ||
        ((m_initialRootSnapshot.MappingCount - seedMappingCount) >
         Kernel::Memory::MaximumRuntimeAddressSpaceMappings) ||
        (m_memorySummary.RangeCount > (Kernel::Memory::MaximumRuntimeAddressSpaceMappings -
                                       (m_initialRootSnapshot.MappingCount - seedMappingCount))) ||
        (m_initialRootSnapshot.MappingCount > Kernel::Memory::MaximumAddressSpaceMappings) ||
        (m_memorySummary.RangeCount >
         (Kernel::Memory::MaximumAddressSpaceMappings - m_initialRootSnapshot.MappingCount)))
    {
        return MappingOwnerError::MappingCapacityExceeded;
    }

    const Foundation::UInt8 encodablePhysicalAddressBits =
        m_commonPhysicalAddressBits < RuntimeTranslationPhysicalAddressBits ? m_commonPhysicalAddressBits
                                                                            : RuntimeTranslationPhysicalAddressBits;
    const Foundation::UInt64 physicalLimit = Foundation::UInt64{1U} << encodablePhysicalAddressBits;
    Foundation::UInt64 arenaEnd{0U};
    if (!TryGetPageRangeEnd(m_memorySummary.AggregateArena.PhysicalStart, m_memorySummary.AggregateArena.PageCount,
                            arenaEnd) ||
        (arenaEnd > physicalLimit))
    {
        return MappingOwnerError::InvalidCommonAddressWidth;
    }

    m_summary.AddressSpaceIdentity = root.AddressSpaceIdentity;
    m_summary.AggregateFrame = m_memorySummary.AggregateFrame;
    m_summary.InitialMappingCount = m_initialRootSnapshot.MappingCount;
    m_summary.FinalMappingCount = m_initialRootSnapshot.MappingCount;
    m_summary.RangeCount = m_memorySummary.RangeCount;
    return MappingOwnerError::None;
}

MappingOwnerError Ia64InitialUserImageMappingOwner::ValidateRangeGeometry(const Foundation::Size index) noexcept
{
    const RangePlan& plan = m_mappings[index].Plan;
    if (!plan.Permissions.HasOnlyKnownBits() || !PermissionsAreValid(plan.Permissions) ||
        !BytesAreZero(plan.Reserved, sizeof(plan.Reserved)) || !plan.MemoryObject.IsValid() ||
        !plan.MemoryObject.IsFromRegistry(*m_registry) || ((plan.VirtualStart & PageOffsetMask) != 0U) ||
        (plan.Pages.GetValue() == 0U) || (plan.Pages.GetValue() > Memory::MaximumIa64RuntimeMappingPages))
    {
        return MappingOwnerError::InvalidRange;
    }

    Foundation::UInt64 virtualEnd{0U};
    Foundation::UInt64 guardedStart{0U};
    Foundation::UInt64 guardedEnd{0U};
    Foundation::UInt64 physicalStart{0U};
    Foundation::UInt64 physicalEnd{0U};
    if (!TryGetPageRangeEnd(plan.VirtualStart, plan.Pages.GetValue(), virtualEnd) ||
        !IsImplementedVirtualRange(plan.VirtualStart, virtualEnd,
                                   m_commonImplementedVirtualAddressMostSignificantBit) ||
        !TryGetGuardedVirtualRange(plan, guardedStart, guardedEnd) ||
        !IsImplementedVirtualRange(guardedStart, guardedEnd, m_commonImplementedVirtualAddressMostSignificantBit) ||
        !TryGetPhysicalRange(m_memorySummary, plan, physicalStart, physicalEnd))
    {
        return MappingOwnerError::InvalidRange;
    }

    Foundation::UInt64 expectedFrameOffset{0U};
    for (Foundation::Size previous{0U}; previous < index; ++previous)
    {
        const Foundation::UInt64 pages = m_mappings[previous].Plan.Pages.GetValue();
        if (expectedFrameOffset > (MaximumUInt64 - pages))
        {
            return MappingOwnerError::InvalidRange;
        }
        expectedFrameOffset += pages;
    }
    if (plan.FrameOffsetPages != expectedFrameOffset)
    {
        return MappingOwnerError::InvalidRange;
    }

    const Foundation::Size stackIndex = m_summary.RangeCount - 2U;
    if (index < stackIndex)
    {
        if ((plan.Purpose != MappingPurpose::LoadSegment) || (plan.GuardPolicy != GuardPolicy::None) ||
            (plan.OriginalSegmentIndex == Ia64InitialUserImageNoSegment) ||
            (plan.OriginalSegmentIndex >= m_memorySummary.Image.ProgramHeaderCount) ||
            ((index != 0U) && (plan.OriginalSegmentIndex <= m_mappings[index - 1U].Plan.OriginalSegmentIndex)))
        {
            return MappingOwnerError::InvalidRange;
        }
    }
    else if (index == stackIndex)
    {
        if ((plan.Purpose != MappingPurpose::MemoryStack) || (plan.GuardPolicy != GuardPolicy::OnePageBeforeAndAfter) ||
            (plan.OriginalSegmentIndex != Ia64InitialUserImageNoSegment) ||
            (plan.VirtualStart != Ia64InitialUserMemoryStackVirtualStart) ||
            (plan.Pages.GetValue() != Ia64InitialUserMemoryStackPageCount) ||
            (plan.Permissions.GetBits() != WritableUserPermissions().GetBits()) ||
            (plan.FrameOffsetPages != (m_memorySummary.AggregateArena.PageCount - Ia64InitialUserMemoryStackPageCount -
                                       Ia64InitialUserRseBackingStorePageCount)))
        {
            return MappingOwnerError::InvalidRange;
        }
    }
    else if ((plan.Purpose != MappingPurpose::RseBackingStore) ||
             (plan.GuardPolicy != GuardPolicy::OnePageBeforeAndAfter) ||
             (plan.OriginalSegmentIndex != Ia64InitialUserImageNoSegment) ||
             (plan.VirtualStart != Ia64InitialUserRseBackingStoreVirtualStart) ||
             (plan.Pages.GetValue() != Ia64InitialUserRseBackingStorePageCount) ||
             (plan.Permissions.GetBits() != WritableUserPermissions().GetBits()) ||
             (plan.FrameOffsetPages !=
              (m_memorySummary.AggregateArena.PageCount - Ia64InitialUserRseBackingStorePageCount)))
    {
        return MappingOwnerError::InvalidRange;
    }
    return MappingOwnerError::None;
}

MappingOwnerError Ia64InitialUserImageMappingOwner::ValidateRanges(Ia64InitialUserImageMemoryOwner& memoryOwner,
                                                                   Kernel::Object::ObjectRegistry& registry) noexcept
{
    for (Foundation::Size index{0U}; index < m_summary.RangeCount; ++index)
    {
        m_failure.RangeIndex = index;
        const auto planResult = memoryOwner.GetRangePlan(index);
        const auto rangeResult = memoryOwner.BorrowRange(index);
        if (!planResult.HasValue())
        {
            m_failure.MemoryAccess = planResult.GetError();
            return MappingOwnerError::MemoryOwnerAccessRejected;
        }
        if (!rangeResult.HasValue())
        {
            m_failure.MemoryAccess = rangeResult.GetError();
            return MappingOwnerError::MemoryOwnerAccessRejected;
        }
        const Ia64InitialUserImageMemoryRangeView& range = rangeResult.GetValue();
        if (!RangePlansAreEqual(planResult.GetValue(), range.Plan) || (range.FrameStorage == nullptr) ||
            (range.MemoryObjectStorage == nullptr))
        {
            return MappingOwnerError::InvalidRange;
        }
        m_mappings[index].Plan = range.Plan;
        m_memoryObjectStorage[index] = range.MemoryObjectStorage;
        if (index == 0U)
        {
            m_aggregateFrameStorage = range.FrameStorage;
        }
        else if (range.FrameStorage != m_aggregateFrameStorage)
        {
            return MappingOwnerError::InvalidAggregateFrame;
        }

        MappingOwnerError error = ValidateRangeGeometry(index);
        if (error != MappingOwnerError::None)
        {
            return error;
        }
        const Kernel::Memory::MemoryObjectSnapshot memoryObject = range.MemoryObjectStorage->GetSnapshot();
        if (!MemoryObjectSnapshotMatches(memoryObject, range.Plan, m_memorySummary.ResourceDomainGeneration, 0U) ||
            (range.MemoryObjectStorage->GetChargeOwner().GetValue() != memoryObject.ResourceDomain.GetValue()))
        {
            return MappingOwnerError::InvalidMemoryObject;
        }
        if (index == 0U)
        {
            m_resourceDomainIdentifier = memoryObject.ResourceDomain;
        }
        else if (memoryObject.ResourceDomain.GetValue() != m_resourceDomainIdentifier.GetValue())
        {
            return MappingOwnerError::InvalidMemoryObject;
        }

        Kernel::Object::ObjectPin verification;
        m_failure.Pin = registry.PinObject(range.Plan.MemoryObject.GetObjectId(),
                                           Kernel::Object::ObjectType::MemoryObject, verification);
        if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
            (&verification.GetObject() != static_cast<Kernel::Object::IKernelObject*>(range.MemoryObjectStorage)))
        {
            return MappingOwnerError::RegistryBindingRejected;
        }
        verification.Reset();

        for (Foundation::Size previous{0U}; previous < index; ++previous)
        {
            if (range.Plan.MemoryObject.IsEqualTo(m_mappings[previous].Plan.MemoryObject) ||
                (range.MemoryObjectStorage == m_memoryObjectStorage[previous]))
            {
                return MappingOwnerError::InvalidMemoryObject;
            }
        }
    }

    const RangePlan& last = m_mappings[m_summary.RangeCount - 1U].Plan;
    if ((last.FrameOffsetPages > (MaximumUInt64 - last.Pages.GetValue())) ||
        ((last.FrameOffsetPages + last.Pages.GetValue()) != m_memorySummary.AggregateArena.PageCount))
    {
        return MappingOwnerError::InvalidRange;
    }
    return MappingOwnerError::None;
}

MappingOwnerError
Ia64InitialUserImageMappingOwner::ValidateAggregateFrame(Kernel::Object::ObjectRegistry& registry) noexcept
{
    Kernel::Memory::Frame* const frame = m_aggregateFrameStorage;
    if (frame == nullptr)
    {
        return MappingOwnerError::InvalidAggregateFrame;
    }

    Kernel::Object::ObjectPin verification;
    m_failure.Pin =
        registry.PinObject(m_summary.AggregateFrame.GetObjectId(), Kernel::Object::ObjectType::Frame, verification);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
        (&verification.GetObject() != static_cast<Kernel::Object::IKernelObject*>(frame)))
    {
        return MappingOwnerError::RegistryBindingRejected;
    }
    verification.Reset();

    if (!FrameSnapshotMatches(frame->GetSnapshot(), m_memorySummary) ||
        (frame->GetChargeOwner().GetValue() != m_resourceDomainIdentifier.GetValue()))
    {
        return MappingOwnerError::InvalidAggregateFrame;
    }
    return MappingOwnerError::None;
}

MappingOwnerError Ia64InitialUserImageMappingOwner::ValidatePairwiseRanges() noexcept
{
    for (Foundation::Size first{0U}; first < m_summary.RangeCount; ++first)
    {
        Foundation::UInt64 firstVirtualStart{0U};
        Foundation::UInt64 firstVirtualEnd{0U};
        Foundation::UInt64 firstPhysicalStart{0U};
        Foundation::UInt64 firstPhysicalEnd{0U};
        if (!TryGetGuardedVirtualRange(m_mappings[first].Plan, firstVirtualStart, firstVirtualEnd) ||
            !TryGetPhysicalRange(m_memorySummary, m_mappings[first].Plan, firstPhysicalStart, firstPhysicalEnd))
        {
            m_failure.RangeIndex = first;
            return MappingOwnerError::InvalidRange;
        }
        for (Foundation::Size second{first + 1U}; second < m_summary.RangeCount; ++second)
        {
            Foundation::UInt64 secondVirtualStart{0U};
            Foundation::UInt64 secondVirtualEnd{0U};
            Foundation::UInt64 secondPhysicalStart{0U};
            Foundation::UInt64 secondPhysicalEnd{0U};
            if (!TryGetGuardedVirtualRange(m_mappings[second].Plan, secondVirtualStart, secondVirtualEnd) ||
                !TryGetPhysicalRange(m_memorySummary, m_mappings[second].Plan, secondPhysicalStart, secondPhysicalEnd))
            {
                m_failure.RangeIndex = second;
                return MappingOwnerError::InvalidRange;
            }
            if (RangesOverlap(firstVirtualStart, firstVirtualEnd, secondVirtualStart, secondVirtualEnd))
            {
                m_failure.RangeIndex = second;
                return MappingOwnerError::VirtualRangeOverlap;
            }
            if (RangesOverlap(firstPhysicalStart, firstPhysicalEnd, secondPhysicalStart, secondPhysicalEnd))
            {
                m_failure.RangeIndex = second;
                return MappingOwnerError::PhysicalRangeOverlap;
            }
        }
    }
    return MappingOwnerError::None;
}

MappingOwnerError Ia64InitialUserImageMappingOwner::ValidateExistingMappings() noexcept
{
    for (Foundation::Size existingIndex{0U}; existingIndex < m_initialRootSnapshot.MappingCount; ++existingIndex)
    {
        const auto existingResult = m_rootAddressSpace->TryGetMapping(existingIndex);
        if (!existingResult.HasValue())
        {
            return MappingOwnerError::InvalidExistingMapping;
        }
        const Kernel::Memory::AddressSpaceMappingRecord& existing = existingResult.GetValue();
        Foundation::UInt64 existingVirtualEnd{0U};
        Foundation::UInt64 existingPhysicalEnd{0U};
        if (((existing.VirtualStart.GetValue() & PageOffsetMask) != 0U) ||
            ((existing.PhysicalStart.GetValue() & PageOffsetMask) != 0U) ||
            !TryGetPageRangeEnd(existing.VirtualStart.GetValue(), existing.Pages.GetValue(), existingVirtualEnd) ||
            !TryGetPageRangeEnd(existing.PhysicalStart.GetValue(), existing.Pages.GetValue(), existingPhysicalEnd) ||
            !BytesAreZero(existing.Reserved, sizeof(existing.Reserved)))
        {
            return MappingOwnerError::InvalidExistingMapping;
        }

        for (Foundation::Size index{0U}; index < m_summary.RangeCount; ++index)
        {
            Foundation::UInt64 guardedStart{0U};
            Foundation::UInt64 guardedEnd{0U};
            Foundation::UInt64 physicalStart{0U};
            Foundation::UInt64 physicalEnd{0U};
            if (!TryGetGuardedVirtualRange(m_mappings[index].Plan, guardedStart, guardedEnd) ||
                !TryGetPhysicalRange(m_memorySummary, m_mappings[index].Plan, physicalStart, physicalEnd))
            {
                m_failure.RangeIndex = index;
                return MappingOwnerError::InvalidRange;
            }
            if (RangesOverlap(existing.VirtualStart.GetValue(), existingVirtualEnd, guardedStart, guardedEnd))
            {
                m_failure.RangeIndex = index;
                return MappingOwnerError::GuardedVirtualRangeOccupied;
            }
            if (RangesOverlap(existing.PhysicalStart.GetValue(), existingPhysicalEnd, physicalStart, physicalEnd))
            {
                m_failure.RangeIndex = index;
                return MappingOwnerError::PhysicalSliceOccupied;
            }
        }
    }
    return MappingOwnerError::None;
}

MappingOwnerError
Ia64InitialUserImageMappingOwner::RevalidateAuthoritativeState(Ia64InitialUserImageMemoryOwner& memoryOwner,
                                                               Memory::Ia64BspRuntimeMemory& bspRuntimeMemory,
                                                               Kernel::Object::ObjectRegistry& registry) noexcept
{
    if ((m_registry != &registry) || (memoryOwner.GetState() != Ia64InitialUserImageMemoryOwnerState::Ready))
    {
        return MappingOwnerError::AuthoritativeStateChanged;
    }
    const auto currentMemorySummary = memoryOwner.GetSummary();
    if (!currentMemorySummary.HasValue() || !MemorySummariesAreEqual(currentMemorySummary.GetValue(), m_memorySummary))
    {
        if (!currentMemorySummary.HasValue())
        {
            m_failure.MemoryAccess = currentMemorySummary.GetError();
        }
        return MappingOwnerError::AuthoritativeStateChanged;
    }

    ::new (static_cast<void*>(&m_bspSnapshot)) Memory::Ia64BspRuntimeMemorySnapshot{bspRuntimeMemory.GetSnapshot()};
    const auto rootResult = bspRuntimeMemory.BorrowReadyUserRoot(m_domain);
    if (!rootResult.HasValue())
    {
        m_failure.RootBorrow = rootResult.GetError();
        return MappingOwnerError::AuthoritativeStateChanged;
    }
    const Memory::Ia64BspRuntimeRootView& root = rootResult.GetValue();
    const Kernel::Memory::AddressSpaceSnapshot currentRoot = m_rootAddressSpace->GetSnapshot();
    if ((m_bspSnapshot.State != Memory::Ia64BspRuntimeMemoryState::Ready) ||
        (m_bspSnapshot.Summary.CommonCapabilities.PhysicalAddressBits != m_commonPhysicalAddressBits) ||
        (m_bspSnapshot.Summary.CommonCapabilities.ImplementedVirtualAddressMostSignificantBit !=
         m_commonImplementedVirtualAddressMostSignificantBit) ||
        (m_bspSnapshot.Summary.BspProcessorIndex != m_bspProcessor.GetValue()) ||
        (root.AddressSpaceStorage != m_rootAddressSpace) ||
        !root.AddressSpaceIdentity.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (root.ControllerStorage != m_controllerStorage) || (root.Processor.GetValue() != m_bspProcessor.GetValue()) ||
        !RootSnapshotsAreEqual(currentRoot, m_initialRootSnapshot))
    {
        return MappingOwnerError::AuthoritativeStateChanged;
    }
    return ValidateExistingMappings();
}

MappingOwnerError
Ia64InitialUserImageMappingOwner::PrepareAndBeginMapping(const Foundation::Size index,
                                                         Kernel::Memory::AddressSpaceShootdownRequest& request) noexcept
{
    const RangePlan& plan = m_mappings[index].Plan;
    Kernel::Memory::PinnedFrameMappingPreparation preparation;
    m_failure.MappingPreparation =
        preparation.Prepare(*m_registry, plan.MemoryObject, *m_memoryObjectStorage[index], m_summary.AggregateFrame,
                            *m_aggregateFrameStorage,
                            Kernel::Memory::PinnedFrameMappingRequest{
                                .VirtualStart = Kernel::Memory::VirtualAddress{plan.VirtualStart},
                                .Pages = plan.Pages,
                                .MemoryObjectOffsetPages = 0U,
                                .FrameOffsetPages = plan.FrameOffsetPages,
                                .Permissions = plan.Permissions,
                                .Reserved = {},
                            });
    if (m_failure.MappingPreparation != Kernel::Memory::PinnedFrameMappingError::None)
    {
        return MappingOwnerError::MappingPreparationRejected;
    }

    Foundation::UInt64 physicalStart{0U};
    Foundation::UInt64 physicalEnd{0U};
    if (!TryGetPhysicalRange(m_memorySummary, plan, physicalStart, physicalEnd) ||
        !MappingRecordMatches(preparation.GetMapping(), plan, m_summary.AggregateFrame, physicalStart))
    {
        return MappingOwnerError::MappingPreparationRejected;
    }

    const auto begin = m_rootAddressSpace->BeginPinnedFrameMapping(m_summary.AddressSpaceIdentity, preparation);
    if (!begin.HasValue())
    {
        m_failure.AddressSpace = begin.GetError();
        m_finalRootSnapshot = m_rootAddressSpace->GetSnapshot();
        if ((begin.GetError().PageTableError == Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown) ||
            (m_finalRootSnapshot.State == Kernel::Memory::AddressSpaceState::Faulted))
        {
            m_failure.RootMayBeFaulted = true;
        }
        return MappingOwnerError::AddressSpaceMappingRejected;
    }
    request = begin.GetValue();
    return MappingOwnerError::None;
}

MappingOwnerError Ia64InitialUserImageMappingOwner::CompleteAndValidateMapping(
    const Foundation::Size index, const Kernel::Memory::AddressSpaceShootdownRequest& request) noexcept
{
    const RangePlan& plan = m_mappings[index].Plan;
    const Kernel::Memory::AddressSpaceSnapshot beginSnapshot = m_rootAddressSpace->GetSnapshot();
    const Kernel::Memory::AddressSpaceMappingToken token = request.GetToken();
    const Foundation::Size expectedPriorCount = m_summary.InitialMappingCount + index;
    if (!token.IsValid() || !token.IsFor(*m_rootAddressSpace) ||
        (request.GetInvalidationStart().GetValue() != plan.VirtualStart) ||
        (request.GetInvalidationPages().GetValue() != plan.Pages.GetValue()) ||
        !beginSnapshot.Identity.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (beginSnapshot.MappingCount != expectedPriorCount) ||
        (beginSnapshot.State != Kernel::Memory::AddressSpaceState::ShootdownPending) ||
        (beginSnapshot.ResidentProcessorMask != 0U) || beginSnapshot.ActivatingProcessor.IsValid() ||
        beginSnapshot.TransitionProcessor.IsValid() || (beginSnapshot.TransitionSequence != 0U) ||
        (beginSnapshot.ShootdownSequence != token.GetSequence()) || (beginSnapshot.TargetProcessorMask != 0U) ||
        (beginSnapshot.AcknowledgedProcessorMask != 0U) ||
        !BytesAreZero(beginSnapshot.Reserved, sizeof(beginSnapshot.Reserved)))
    {
        m_failure.AddressSpace.Error = m_rootAddressSpace->FailShootdown(token);
        m_failure.RootMayBeFaulted = true;
        return MappingOwnerError::NonemptyShootdown;
    }

    const auto completed = m_rootAddressSpace->CompleteMapping(token);
    if (!completed.HasValue())
    {
        m_failure.AddressSpace = completed.GetError();
        m_failure.RootMayBeFaulted = true;
        return MappingOwnerError::MappingCompletionRejected;
    }
    m_mappings[index].Receipt = completed.GetValue();

    Foundation::UInt64 physicalStart{0U};
    Foundation::UInt64 physicalEnd{0U};
    const auto record = m_rootAddressSpace->TryGetMapping(m_summary.InitialMappingCount + index);
    const Kernel::Memory::AddressSpaceMappingReceipt& receipt = m_mappings[index].Receipt;
    const Foundation::UInt64 previousSequence = index == 0U ? 0U : m_mappings[index - 1U].Receipt.MappingSequence;
    if (!TryGetPhysicalRange(m_memorySummary, plan, physicalStart, physicalEnd) || !record.HasValue() ||
        (receipt.MappingSequence == 0U) || ((index != 0U) && (receipt.MappingSequence != (previousSequence + 1U))) ||
        (receipt.MappingCount != (m_summary.InitialMappingCount + index + 1U)) ||
        (receipt.VirtualStart.GetValue() != plan.VirtualStart) || (receipt.Pages.GetValue() != plan.Pages.GetValue()) ||
        !MappingRecordMatches(record.HasValue() ? record.GetValue() : Kernel::Memory::AddressSpaceMappingRecord{}, plan,
                              m_summary.AggregateFrame, physicalStart) ||
        !MemoryObjectSnapshotMatches(m_memoryObjectStorage[index]->GetSnapshot(), plan,
                                     m_memorySummary.ResourceDomainGeneration, 1U))
    {
        return MappingOwnerError::PostconditionRejected;
    }
    return MappingOwnerError::None;
}

MappingOwnerError Ia64InitialUserImageMappingOwner::InstallMapping(const Foundation::Size index) noexcept
{
    Kernel::Memory::AddressSpaceShootdownRequest request;
    MappingOwnerError error = PrepareAndBeginMapping(index, request);
    if (error == MappingOwnerError::None)
    {
        error = CompleteAndValidateMapping(index, request);
    }
    return error;
}

MappingOwnerError Ia64InitialUserImageMappingOwner::InstallMappings() noexcept
{
    for (Foundation::Size index{0U}; index < m_summary.RangeCount; ++index)
    {
        m_failure.RangeIndex = index;
        const MappingOwnerError error = InstallMapping(index);
        if (error != MappingOwnerError::None)
        {
            return error;
        }
    }
    return MappingOwnerError::None;
}

MappingOwnerError Ia64InitialUserImageMappingOwner::ValidateFinalGuardVacancy() noexcept
{
    for (Foundation::Size recordIndex{0U}; recordIndex < m_finalRootSnapshot.MappingCount; ++recordIndex)
    {
        const auto recordResult = m_rootAddressSpace->TryGetMapping(recordIndex);
        if (!recordResult.HasValue())
        {
            return MappingOwnerError::PostconditionRejected;
        }
        const Kernel::Memory::AddressSpaceMappingRecord& record = recordResult.GetValue();
        Foundation::UInt64 recordEnd{0U};
        if (!TryGetPageRangeEnd(record.VirtualStart.GetValue(), record.Pages.GetValue(), recordEnd))
        {
            return MappingOwnerError::PostconditionRejected;
        }
        for (Foundation::Size index{0U}; index < m_summary.RangeCount; ++index)
        {
            const RangePlan& plan = m_mappings[index].Plan;
            if (plan.GuardPolicy != GuardPolicy::OnePageBeforeAndAfter)
            {
                continue;
            }
            Foundation::UInt64 guardedStart{0U};
            Foundation::UInt64 guardedEnd{0U};
            Foundation::UInt64 mappingEnd{0U};
            if (!TryGetGuardedVirtualRange(plan, guardedStart, guardedEnd) ||
                !TryGetPageRangeEnd(plan.VirtualStart, plan.Pages.GetValue(), mappingEnd) ||
                RangesOverlap(record.VirtualStart.GetValue(), recordEnd, guardedStart, plan.VirtualStart) ||
                RangesOverlap(record.VirtualStart.GetValue(), recordEnd, mappingEnd, guardedEnd))
            {
                m_failure.RangeIndex = index;
                return MappingOwnerError::PostconditionRejected;
            }
        }
    }
    return MappingOwnerError::None;
}

MappingOwnerError
Ia64InitialUserImageMappingOwner::ValidateFinalOwnerBoundary(Ia64InitialUserImageMemoryOwner& memoryOwner,
                                                             Memory::Ia64BspRuntimeMemory& bspRuntimeMemory) noexcept
{
    const auto currentMemorySummary = memoryOwner.GetSummary();
    const auto rootResult = bspRuntimeMemory.BorrowReadyUserRoot(m_domain);
    ::new (static_cast<void*>(&m_bspSnapshot)) Memory::Ia64BspRuntimeMemorySnapshot{bspRuntimeMemory.GetSnapshot()};
    if (!currentMemorySummary.HasValue() ||
        !MemorySummariesAreEqual(currentMemorySummary.GetValue(), m_memorySummary) || !rootResult.HasValue() ||
        (rootResult.GetValue().AddressSpaceStorage != m_rootAddressSpace) ||
        !rootResult.GetValue().AddressSpaceIdentity.IsEqualTo(m_summary.AddressSpaceIdentity) ||
        (rootResult.GetValue().ControllerStorage != m_controllerStorage) ||
        (rootResult.GetValue().Processor.GetValue() != m_bspProcessor.GetValue()) ||
        (m_bspSnapshot.State != Memory::Ia64BspRuntimeMemoryState::Ready) ||
        (m_bspSnapshot.Summary.CommonCapabilities.PhysicalAddressBits != m_commonPhysicalAddressBits) ||
        (m_bspSnapshot.Summary.CommonCapabilities.ImplementedVirtualAddressMostSignificantBit !=
         m_commonImplementedVirtualAddressMostSignificantBit) ||
        !FrameSnapshotMatches(m_aggregateFrameStorage->GetSnapshot(), m_memorySummary) ||
        (m_aggregateFrameStorage->GetChargeOwner().GetValue() != m_resourceDomainIdentifier.GetValue()))
    {
        if (!currentMemorySummary.HasValue())
        {
            m_failure.MemoryAccess = currentMemorySummary.GetError();
        }
        if (!rootResult.HasValue())
        {
            m_failure.RootBorrow = rootResult.GetError();
        }
        return MappingOwnerError::PostconditionRejected;
    }
    return MappingOwnerError::None;
}

MappingOwnerError
Ia64InitialUserImageMappingOwner::VerifyPostconditions(Ia64InitialUserImageMemoryOwner& memoryOwner,
                                                       Memory::Ia64BspRuntimeMemory& bspRuntimeMemory) noexcept
{
    m_finalRootSnapshot = m_rootAddressSpace->GetSnapshot();
    const Foundation::Size expectedFinalCount = m_summary.InitialMappingCount + m_summary.RangeCount;
    if (!RootIsReadyAndNonresident(m_finalRootSnapshot, m_summary.AddressSpaceIdentity) ||
        (m_finalRootSnapshot.MappingCount != expectedFinalCount) ||
        (memoryOwner.GetState() != Ia64InitialUserImageMemoryOwnerState::Ready) ||
        (ValidateFinalOwnerBoundary(memoryOwner, bspRuntimeMemory) != MappingOwnerError::None))
    {
        return MappingOwnerError::PostconditionRejected;
    }

    Foundation::UInt64 priorSequence{0U};
    for (Foundation::Size index{0U}; index < m_summary.RangeCount; ++index)
    {
        m_failure.RangeIndex = index;
        Foundation::UInt64 physicalStart{0U};
        Foundation::UInt64 physicalEnd{0U};
        const auto record = m_rootAddressSpace->TryGetMapping(m_summary.InitialMappingCount + index);
        const Kernel::Memory::AddressSpaceMappingReceipt& receipt = m_mappings[index].Receipt;
        if (!TryGetPhysicalRange(m_memorySummary, m_mappings[index].Plan, physicalStart, physicalEnd) ||
            !record.HasValue() ||
            !MappingRecordMatches(record.HasValue() ? record.GetValue() : Kernel::Memory::AddressSpaceMappingRecord{},
                                  m_mappings[index].Plan, m_summary.AggregateFrame, physicalStart) ||
            (receipt.MappingSequence == 0U) || ((index != 0U) && (receipt.MappingSequence != (priorSequence + 1U))) ||
            (receipt.MappingCount != (m_summary.InitialMappingCount + index + 1U)) ||
            (receipt.VirtualStart.GetValue() != m_mappings[index].Plan.VirtualStart) ||
            (receipt.Pages.GetValue() != m_mappings[index].Plan.Pages.GetValue()) ||
            !MemoryObjectSnapshotMatches(m_memoryObjectStorage[index]->GetSnapshot(), m_mappings[index].Plan,
                                         m_memorySummary.ResourceDomainGeneration, 1U) ||
            (m_memoryObjectStorage[index]->GetChargeOwner().GetValue() != m_resourceDomainIdentifier.GetValue()))
        {
            return MappingOwnerError::PostconditionRejected;
        }
        priorSequence = receipt.MappingSequence;
    }

    const MappingOwnerError guardError = ValidateFinalGuardVacancy();
    if (guardError != MappingOwnerError::None)
    {
        return guardError;
    }

    m_summary.FinalMappingCount = expectedFinalCount;
    m_summary.FirstMappingSequence = m_mappings[0U].Receipt.MappingSequence;
    m_summary.LastMappingSequence = m_mappings[m_summary.RangeCount - 1U].Receipt.MappingSequence;
    return MappingOwnerError::None;
}

Ia64InitialUserImageMappingOwner::InstallResult
Ia64InitialUserImageMappingOwner::Fail(const MappingOwnerError error) noexcept
{
    m_failure.Error = error;
    m_hasFailure = true;
    if ((m_state != Ia64InitialUserImageMappingOwnerState::Fresh) && (m_rootAddressSpace != nullptr))
    {
        m_finalRootSnapshot = m_rootAddressSpace->GetSnapshot();
        if (m_finalRootSnapshot.State == Kernel::Memory::AddressSpaceState::Faulted)
        {
            m_failure.RootMayBeFaulted = true;
        }
    }
    return InstallResult::Failure(m_failure);
}

Ia64InitialUserImageMappingOwner::InstallResult Ia64InitialUserImageMappingOwner::Install(
    Ia64InitialUserImageMemoryOwner& memoryOwner, Memory::Ia64BspRuntimeMemory& bspRuntimeMemory,
    Kernel::Object::ObjectRegistry& registry, const Memory::Ia64BspRuntimeUserDomain domain) noexcept
{
    if (m_state != Ia64InitialUserImageMappingOwnerState::Fresh)
    {
        Failure repeated{};
        repeated.Error = MappingOwnerError::AlreadyAttempted;
        repeated.RangeIndex = Ia64InitialUserImageNoSegment;
        return InstallResult::Failure(repeated);
    }
    m_domain = domain;

    ResetPreflightStorage();
    MappingOwnerError error = CaptureAndValidateOwners(memoryOwner, bspRuntimeMemory, registry);
    if (error == MappingOwnerError::None)
    {
        error = ValidateRanges(memoryOwner, registry);
    }
    if (error == MappingOwnerError::None)
    {
        error = ValidateAggregateFrame(registry);
    }
    if (error == MappingOwnerError::None)
    {
        error = ValidatePairwiseRanges();
    }
    if (error == MappingOwnerError::None)
    {
        error = ValidateExistingMappings();
    }
    if (error == MappingOwnerError::None)
    {
        error = RevalidateAuthoritativeState(memoryOwner, bspRuntimeMemory, registry);
    }
    if (error != MappingOwnerError::None)
    {
        return Fail(error);
    }

    m_state = Ia64InitialUserImageMappingOwnerState::Failed;
    error = InstallMappings();
    if (error == MappingOwnerError::None)
    {
        error = VerifyPostconditions(memoryOwner, bspRuntimeMemory);
    }
    if (error != MappingOwnerError::None)
    {
        return Fail(error);
    }

    m_failure = Ia64InitialUserImageMappingFailure{};
    m_failure.RangeIndex = Ia64InitialUserImageNoSegment;
    m_hasFailure = false;
    m_state = Ia64InitialUserImageMappingOwnerState::Ready;
    return InstallResult::Success(m_summary);
}

bool Ia64InitialUserImageMappingOwner::ValidateModuleSlice(const Boot::BootInfo& info) const noexcept
{
    if (m_state != Ia64InitialUserImageMappingOwnerState::Ready ||
        m_domain != Memory::Ia64BspRuntimeUserDomain::Pager || m_moduleAttempted || !Boot::ValidateBootModules(info))
    {
        return false;
    }
    const Foundation::UInt64 start{info.Files.PhysicalStart + info.Files.ModuleOffset};
    const Foundation::UInt64 bytes{info.Files.ModuleBytes};
    const Foundation::UInt8 bits{m_commonPhysicalAddressBits < RuntimeTranslationPhysicalAddressBits
                                     ? m_commonPhysicalAddressBits
                                     : RuntimeTranslationPhysicalAddressBits};
    if (start + bytes > (Foundation::UInt64{1U} << bits))
    {
        return false;
    }
    const Kernel::Memory::AddressSpaceSnapshot root = m_rootAddressSpace->GetSnapshot();
    if (!RootIsReadyAndNonresident(root, m_summary.AddressSpaceIdentity))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < root.MappingCount; ++index)
    {
        const auto record = m_rootAddressSpace->TryGetMapping(index);
        if (!record.HasValue())
        {
            return false;
        }
        const auto& mapping = record.GetValue();
        Foundation::UInt64 virtualEnd{0U};
        Foundation::UInt64 physicalEnd{0U};
        if (!TryGetPageRangeEnd(mapping.VirtualStart.GetValue(), mapping.Pages.GetValue(), virtualEnd) ||
            !TryGetPageRangeEnd(mapping.PhysicalStart.GetValue(), mapping.Pages.GetValue(), physicalEnd) ||
            RangesOverlap(start, start + bytes, mapping.PhysicalStart.GetValue(), physicalEnd) ||
            RangesOverlap(Abi::Ia64PagerModuleBase, Abi::Ia64PagerModuleBase + bytes, mapping.VirtualStart.GetValue(),
                          virtualEnd))
        {
            return false;
        }
    }
    return true;
}

bool Ia64InitialUserImageMappingOwner::MapModuleSlice() noexcept
{
    Kernel::Memory::PinnedFrameMappingPreparation preparation;
    const Kernel::Memory::PinnedFrameMappingRequest request{
        .VirtualStart = Kernel::Memory::VirtualAddress{Abi::Ia64PagerModuleBase},
        .Pages = Kernel::Memory::PageCount{m_moduleBytes / PageSizeBytes},
        .MemoryObjectOffsetPages = 0U,
        .FrameOffsetPages = 0U,
        .Permissions = Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}.With(
            Kernel::Memory::PagePermission::User),
        .Reserved = {},
    };
    if (preparation.Prepare(*m_registry, m_moduleMemoryId, m_moduleMemory, m_moduleFrameId, m_moduleFrame, request) !=
        Kernel::Memory::PinnedFrameMappingError::None)
    {
        return false;
    }
    const auto begin = m_rootAddressSpace->BeginPinnedFrameMapping(m_summary.AddressSpaceIdentity, preparation);
    if (!begin.HasValue() || m_rootAddressSpace->GetShootdownTargets() != 0U ||
        !m_rootAddressSpace->CompleteMapping(begin.GetValue().GetToken()).HasValue())
    {
        return false;
    }
    const auto record = m_rootAddressSpace->TryGetMapping(m_summary.FinalMappingCount);
    return record.HasValue() && record.GetValue().VirtualStart.GetValue() == Abi::Ia64PagerModuleBase &&
           record.GetValue().PhysicalStart.GetValue() == m_modulePhysicalStart &&
           record.GetValue().Pages.GetValue() == m_moduleBytes / PageSizeBytes &&
           record.GetValue().Permissions.GetBits() == request.Permissions.GetBits();
}

bool Ia64InitialUserImageMappingOwner::InstallBootModules(
    const Boot::BootInfo& info, Kernel::Object::RootCapabilityIssuer& issuer,
    Kernel::Resource::ResourceDomain& resources,
    const Kernel::Resource::ResourceDomainObjectId resourceIdentity) noexcept
{
    if (!ValidateModuleSlice(info) || !issuer.IsFromRegistry(*m_registry))
    {
        return false;
    }
    m_moduleAttempted = true;
    m_modulePhysicalStart = info.Files.PhysicalStart + info.Files.ModuleOffset;
    m_moduleBytes = info.Files.ModuleBytes;
    const Kernel::Memory::PageCount pages{m_moduleBytes / PageSizeBytes};
    const Kernel::Memory::PagePermissions permissions{
        Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}.With(
            Kernel::Memory::PagePermission::User)};
    if (m_moduleFrame.InitializeReadOnly(resources.GetIdentifier(),
                                         {Kernel::Memory::PhysicalAddress{m_modulePhysicalStart},
                                          pages,
                                          Kernel::Memory::MemoryCachePolicy::WriteBack,
                                          Kernel::Memory::FrameKind::Ram,
                                          {}}) != Kernel::Memory::FrameError::None ||
        m_moduleMemory
                .Initialize(*m_registry, resourceIdentity, resources,
                            {pages,
                             Kernel::Memory::MemoryCachePolicy::WriteBack,
                             permissions,
                             Kernel::Memory::MemoryCommitment::Pinned,
                             {}})
                .Error != Kernel::Memory::MemoryObjectError::None)
    {
        return false;
    }
    const auto frame = issuer.CreateObject(m_moduleFrame);
    const auto memory = issuer.CreateObject(m_moduleMemory);
    if (!frame.HasValue() || !memory.HasValue())
    {
        return false;
    }
    m_moduleFrameId = {*m_registry, frame.GetValue()};
    m_moduleMemoryId = {*m_registry, memory.GetValue()};
    return MapModuleSlice();
}

Foundation::Result<Ia64InitialUserImageMappingSummary, AccessError>
Ia64InitialUserImageMappingOwner::GetSummary() const noexcept
{
    using ResultType = Foundation::Result<Ia64InitialUserImageMappingSummary, AccessError>;
    if (m_state != Ia64InitialUserImageMappingOwnerState::Ready)
    {
        return ResultType::Failure(AccessError::NotReady);
    }
    return ResultType::Success(m_summary);
}

Foundation::Result<InstalledMapping, AccessError>
Ia64InitialUserImageMappingOwner::GetMapping(const Foundation::Size index) const noexcept
{
    using ResultType = Foundation::Result<InstalledMapping, AccessError>;
    if (m_state != Ia64InitialUserImageMappingOwnerState::Ready)
    {
        return ResultType::Failure(AccessError::NotReady);
    }
    if (index >= m_summary.RangeCount)
    {
        return ResultType::Failure(AccessError::IndexOutOfBounds);
    }
    return ResultType::Success(m_mappings[index]);
}

Foundation::Result<Failure, AccessError> Ia64InitialUserImageMappingOwner::GetFailure() const noexcept
{
    using ResultType = Foundation::Result<Failure, AccessError>;
    if (!m_hasFailure)
    {
        return ResultType::Failure(AccessError::NoFailure);
    }
    return ResultType::Success(m_failure);
}
} // namespace Asteria::Platform::Ia64::Executable
