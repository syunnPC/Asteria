// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Scheduler/Ia64InitialUserImageLaunchOwner.hpp"

#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"

namespace Asteria::Platform::Ia64::Scheduler
{
namespace
{
using AccessError = Ia64InitialUserImageLaunchAccessError;
using Failure = Ia64InitialUserImageLaunchFailure;
using LaunchOwnerError = Ia64InitialUserImageLaunchOwnerError;
using MappingAccessError = Executable::Ia64InitialUserImageMappingAccessError;
using MappingOwnerState = Executable::Ia64InitialUserImageMappingOwnerState;
using MemoryAccessError = Executable::Ia64InitialUserImageMemoryAccessError;
using MemoryOwnerState = Executable::Ia64InitialUserImageMemoryOwnerState;
using Purpose = Executable::Ia64InitialUserImageMemoryPurpose;
using GuardPolicy = Executable::Ia64InitialUserImageGuardPolicy;
using InstalledMapping = Executable::Ia64InitialUserImageInstalledMapping;
using MemoryRangePlan = Executable::Ia64InitialUserImageMemoryRangePlan;
using MemoryRangeView = Executable::Ia64InitialUserImageMemoryRangeView;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PageSizeBytes{Kernel::Memory::MappingPageSizeBytes};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};

static_assert(PageSizeBytes == Ia64UserModePageSizeBytes);
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

[[nodiscard]] constexpr bool ContainsRange(const Foundation::UInt64 containerStart,
                                           const Foundation::UInt64 containerEnd, const Foundation::UInt64 valueStart,
                                           const Foundation::UInt64 valueSizeBytes) noexcept
{
    Foundation::UInt64 valueEnd{0U};
    return TryAdd(valueStart, valueSizeBytes, valueEnd) && (containerStart <= valueStart) && (valueEnd <= containerEnd);
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

[[nodiscard]] constexpr bool ImageInformationIsEqual(const Executable::Ia64P64ImageInformation& first,
                                                     const Executable::Ia64P64ImageInformation& second) noexcept
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

[[nodiscard]] constexpr bool
MemorySummariesAreEqual(const Executable::Ia64InitialUserImageMemorySummary& first,
                        const Executable::Ia64InitialUserImageMemorySummary& second) noexcept
{
    return ImageInformationIsEqual(first.Image, second.Image) &&
           PhysicalRangesAreEqual(first.AggregateArena, second.AggregateArena) &&
           first.AggregateFrame.IsEqualTo(second.AggregateFrame) &&
           first.ResourceDomainGeneration.IsEqualTo(second.ResourceDomainGeneration) &&
           (first.RangeCount == second.RangeCount);
}

[[nodiscard]] constexpr bool
MappingSummariesAreEqual(const Executable::Ia64InitialUserImageMappingSummary& first,
                         const Executable::Ia64InitialUserImageMappingSummary& second) noexcept
{
    return first.AddressSpaceIdentity.IsEqualTo(second.AddressSpaceIdentity) &&
           first.AggregateFrame.IsEqualTo(second.AggregateFrame) &&
           (first.InitialMappingCount == second.InitialMappingCount) &&
           (first.FinalMappingCount == second.FinalMappingCount) && (first.RangeCount == second.RangeCount) &&
           (first.FirstMappingSequence == second.FirstMappingSequence) &&
           (first.LastMappingSequence == second.LastMappingSequence);
}

[[nodiscard]] constexpr bool RangePlansAreEqual(const MemoryRangePlan& first, const MemoryRangePlan& second) noexcept
{
    return (first.Purpose == second.Purpose) && (first.GuardPolicy == second.GuardPolicy) &&
           (first.Permissions.GetBits() == second.Permissions.GetBits()) &&
           BytesAreZero(first.Reserved, sizeof(first.Reserved)) &&
           BytesAreZero(second.Reserved, sizeof(second.Reserved)) &&
           (first.OriginalSegmentIndex == second.OriginalSegmentIndex) && (first.VirtualStart == second.VirtualStart) &&
           (first.Pages.GetValue() == second.Pages.GetValue()) && (first.FrameOffsetPages == second.FrameOffsetPages) &&
           first.MemoryObject.IsEqualTo(second.MemoryObject);
}

[[nodiscard]] constexpr bool ReceiptsAreEqual(const Kernel::Memory::AddressSpaceMappingReceipt& first,
                                              const Kernel::Memory::AddressSpaceMappingReceipt& second) noexcept
{
    return (first.MappingSequence == second.MappingSequence) && (first.MappingCount == second.MappingCount) &&
           (first.VirtualStart.GetValue() == second.VirtualStart.GetValue()) &&
           (first.Pages.GetValue() == second.Pages.GetValue());
}

[[nodiscard]] constexpr bool InstalledMappingsAreEqual(const InstalledMapping& first,
                                                       const InstalledMapping& second) noexcept
{
    return RangePlansAreEqual(first.Plan, second.Plan) && ReceiptsAreEqual(first.Receipt, second.Receipt);
}

[[nodiscard]] constexpr bool LimitsAreEqual(const Kernel::Resource::ResourceDomainLimits& first,
                                            const Kernel::Resource::ResourceDomainLimits& second) noexcept
{
    return (first.CommittedMemoryPages == second.CommittedMemoryPages) &&
           (first.ResidentMemoryPages == second.ResidentMemoryPages) &&
           (first.KernelMetadataBytes == second.KernelMetadataBytes) &&
           (first.CapabilityHandles == second.CapabilityHandles) && (first.IpcQueueBytes == second.IpcQueueBytes) &&
           (first.Threads == second.Threads) && (first.ChildDomains == second.ChildDomains);
}

[[nodiscard]] constexpr bool UsageIsEqual(const Kernel::Resource::ResourceDomainUsage& first,
                                          const Kernel::Resource::ResourceDomainUsage& second) noexcept
{
    return (first.CommittedMemoryPages == second.CommittedMemoryPages) &&
           (first.ResidentMemoryPages == second.ResidentMemoryPages) &&
           (first.KernelMetadataBytes == second.KernelMetadataBytes) &&
           (first.CapabilityHandles == second.CapabilityHandles) && (first.IpcQueueBytes == second.IpcQueueBytes) &&
           (first.Threads == second.Threads) && (first.ChildDomains == second.ChildDomains);
}

[[nodiscard]] constexpr bool ResourceSnapshotsAreEqual(const Kernel::Resource::ResourceDomainSnapshot& first,
                                                       const Kernel::Resource::ResourceDomainSnapshot& second) noexcept
{
    return (first.Identity.GetValue() == second.Identity.GetValue()) && first.Parent.IsEqualTo(second.Parent) &&
           LimitsAreEqual(first.Limits, second.Limits) && UsageIsEqual(first.Usage, second.Usage) &&
           (first.Depth == second.Depth) && (first.State == second.State) &&
           BytesAreZero(first.Reserved, sizeof(first.Reserved)) &&
           BytesAreZero(second.Reserved, sizeof(second.Reserved));
}

[[nodiscard]] constexpr bool
ProtectionSnapshotsAreEqual(const Kernel::Protection::ProtectionDomainSnapshot& first,
                            const Kernel::Protection::ProtectionDomainSnapshot& second) noexcept
{
    return first.Identity.IsEqualTo(second.Identity) && first.AddressSpace.IsEqualTo(second.AddressSpace) &&
           (first.CapabilitySpace.GetValue() == second.CapabilitySpace.GetValue()) &&
           first.ResourceDomainIdentity.IsEqualTo(second.ResourceDomainIdentity) &&
           (first.ResourceDomain.GetValue() == second.ResourceDomain.GetValue()) && (first.Kind == second.Kind) &&
           (first.State == second.State) && BytesAreZero(first.Reserved, sizeof(first.Reserved)) &&
           BytesAreZero(second.Reserved, sizeof(second.Reserved));
}

[[nodiscard]] constexpr bool AddressSpaceSnapshotsAreEqual(const Kernel::Memory::AddressSpaceSnapshot& first,
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

[[nodiscard]] constexpr bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
                                                         const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (implementedMostSignificantBit == MaximumIa64UserModeImplementedAddressBit)
    {
        return true;
    }
    const Foundation::UInt8 unimplementedBitCount =
        static_cast<Foundation::UInt8>(MaximumIa64UserModeImplementedAddressBit - implementedMostSignificantBit);
    const Foundation::UInt64 mask = (Foundation::UInt64{1U} << unimplementedBitCount) - 1U;
    const Foundation::UInt64 actual = (address >> (implementedMostSignificantBit + 1U)) & mask;
    const Foundation::UInt64 expected =
        ((address & (Foundation::UInt64{1U} << implementedMostSignificantBit)) != 0U) ? mask : 0U;
    return actual == expected;
}

[[nodiscard]] constexpr bool IsImplementedUserRange(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                                    const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    return (start < end) && ((start >> 61U) == Ia64InitialUserVirtualRegion) &&
           (((end - 1U) >> 61U) == Ia64InitialUserVirtualRegion) &&
           IsVirtualAddressImplemented(start, implementedMostSignificantBit) &&
           IsVirtualAddressImplemented(end - 1U, implementedMostSignificantBit);
}

[[nodiscard]] constexpr Kernel::Memory::PagePermissions WritableUserPermissions() noexcept
{
    using Permission = Kernel::Memory::PagePermission;
    return Kernel::Memory::PagePermissions{Permission::Read}.With(Permission::Write).With(Permission::User);
}

[[nodiscard]] constexpr bool PermissionsAreSafe(const Kernel::Memory::PagePermissions permissions) noexcept
{
    using Permission = Kernel::Memory::PagePermission;
    return permissions.HasOnlyKnownBits() && permissions.Contains(Permission::Read) &&
           permissions.Contains(Permission::User) &&
           !(permissions.Contains(Permission::Write) && permissions.Contains(Permission::Execute));
}

[[nodiscard]] constexpr bool MappingRecordMatches(const Kernel::Memory::AddressSpaceMappingRecord& record,
                                                  const MemoryRangePlan& plan,
                                                  const Executable::Ia64InitialUserImageMemorySummary& summary) noexcept
{
    if ((plan.FrameOffsetPages > (MaximumUInt64 / PageSizeBytes)))
    {
        return false;
    }
    Foundation::UInt64 physicalStart{0U};
    if (!TryAdd(summary.AggregateArena.PhysicalStart, plan.FrameOffsetPages * PageSizeBytes, physicalStart))
    {
        return false;
    }
    return record.MemoryObject.IsEqualTo(plan.MemoryObject) && !record.DeviceRegion.IsValid() &&
           record.Frame.IsEqualTo(summary.AggregateFrame) && (record.VirtualStart.GetValue() == plan.VirtualStart) &&
           (record.PhysicalStart.GetValue() == physicalStart) && (record.Pages.GetValue() == plan.Pages.GetValue()) &&
           (record.MemoryObjectOffsetPages == 0U) && (record.Permissions.GetBits() == plan.Permissions.GetBits()) &&
           (record.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (record.Kind == Kernel::Memory::FrameKind::Ram) &&
           (record.Commitment == Kernel::Memory::MemoryCommitment::Pinned) && record.RuntimeMapping &&
           BytesAreZero(record.Reserved, sizeof(record.Reserved));
}

[[nodiscard]] constexpr bool
MemoryObjectSnapshotMatches(const Kernel::Memory::MemoryObjectSnapshot& snapshot, const MemoryRangePlan& plan,
                            const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                            const Kernel::Object::ResourceDomainId resourceDomain) noexcept
{
    return (snapshot.State == Kernel::Memory::MemoryObjectState::Ready) &&
           (snapshot.Descriptor.Pages.GetValue() == plan.Pages.GetValue()) &&
           (snapshot.Descriptor.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (snapshot.Descriptor.MaximumPermissions.GetBits() == plan.Permissions.GetBits()) &&
           (snapshot.Descriptor.Commitment == Kernel::Memory::MemoryCommitment::Pinned) &&
           BytesAreZero(snapshot.Descriptor.Reserved, sizeof(snapshot.Descriptor.Reserved)) &&
           snapshot.ResourceDomainIdentity.IsEqualTo(resourceDomainIdentity) &&
           (snapshot.ResourceDomain.GetValue() == resourceDomain.GetValue()) && (snapshot.ActiveMappings == 1U) &&
           BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

/// A grant names a valid object identity of a known type in `registry`, with
/// valid, nonempty rights that include Inspect and a nonzero lease deadline.
/// Pinning the target later proves that the object exists with that type.
[[nodiscard]] constexpr bool GrantIsWellFormed(const Ia64InitialUserImageBootstrapGrant& grant,
                                               const Kernel::Object::ObjectRegistry& registry) noexcept
{
    constexpr Kernel::Object::CapabilityRights inspectRights{Kernel::Object::CapabilityRight::Inspect};
    return (grant.Registry == &registry) && grant.Target.IsValid() &&
           Kernel::Object::IsKnownObjectType(grant.ExpectedType) && grant.Rights.IsValid() &&
           (grant.Rights.GetRawValue() != 0U) && grant.Rights.ContainsAll(inspectRights) &&
           (grant.LeaseDeadline.GetTicks() != 0U);
}

/// The retained pin holds the exact granted object, of the granted type,
/// charged to `chargeOwner`.
[[nodiscard]] bool TargetPinMatchesGrant(const Kernel::Object::ObjectPin& pin,
                                         const Ia64InitialUserImageBootstrapGrant& grant,
                                         const Kernel::Object::ResourceDomainId chargeOwner) noexcept
{
    return pin.IsValid() && (pin.GetObjectId().GetValue() == grant.Target.GetValue()) &&
           (pin.GetObjectType() == grant.ExpectedType) && (pin.GetObject().GetObjectType() == grant.ExpectedType) &&
           (pin.GetObject().GetChargeOwner().GetValue() == chargeOwner.GetValue());
}

/// A queried descriptor is exactly the capability minted from `grant` in
/// `revocationDomain`.
[[nodiscard]] constexpr bool DescriptorMatchesGrant(const Kernel::Object::CapabilityDescriptor& descriptor,
                                                    const Ia64InitialUserImageBootstrapGrant& grant,
                                                    const Kernel::Object::RevocationDomainId revocationDomain,
                                                    const Kernel::Object::ResourceDomainId chargeOwner) noexcept
{
    return (descriptor.Object.GetValue() == grant.Target.GetValue()) && (descriptor.Type == grant.ExpectedType) &&
           descriptor.Rights.IsEqualTo(grant.Rights) &&
           (descriptor.RevocationDomain.GetValue() == revocationDomain.GetValue()) &&
           (descriptor.LeaseDeadline.GetTicks() == grant.LeaseDeadline.GetTicks()) &&
           (descriptor.SealedObjectSelector == grant.SealedObjectSelector) &&
           (descriptor.ChargeOwner.GetValue() == chargeOwner.GetValue());
}
} // namespace

void Ia64InitialUserImageLaunchOwner::ResetPreflightStorage() noexcept
{
    m_bootstrapTargetPin.Reset();
    m_diagnosticTargetPin.Reset();
    for (Foundation::Size index{0U}; index < Executable::Ia64InitialUserImageMaximumMemoryObjectCount; ++index)
    {
        m_mappings[index] = InstalledMapping{};
        m_ranges[index] = MemoryRangeView{};
    }
    m_memorySummary = Executable::Ia64InitialUserImageMemorySummary{};
    m_mappingSummary = Executable::Ia64InitialUserImageMappingSummary{};
    m_protectionSnapshot = Kernel::Protection::ProtectionDomainSnapshot{};
    m_protectionBinding = Kernel::Protection::ProtectionDomainBinding{};
    m_resourceSnapshot = Kernel::Resource::ResourceDomainSnapshot{};
    m_addressSpaceSnapshot = Kernel::Memory::AddressSpaceSnapshot{};
    m_bootstrapGrant = Ia64InitialUserImageBootstrapGrant{};
    m_diagnosticGrant = Ia64InitialUserImageBootstrapGrant{};
    m_summary = Ia64InitialUserImageLaunchSummary{};
    m_summary.EntryMappingIndex = Executable::Ia64InitialUserImageNoSegment;
    m_summary.GlobalPointerMappingIndex = Executable::Ia64InitialUserImageNoSegment;
    m_summary.MemoryStackMappingIndex = Executable::Ia64InitialUserImageNoSegment;
    m_summary.RseBackingStoreMappingIndex = Executable::Ia64InitialUserImageNoSegment;
    m_failure = Failure{};
    m_failure.RangeIndex = Executable::Ia64InitialUserImageNoSegment;
    m_supervisorContinuation = Ia64UserModeSupervisorContinuation{};
    m_launchRequest = Ia64UserModeLaunchRequest{};
    m_registry = nullptr;
    m_capabilitySpace = nullptr;
    m_protectionDomain = nullptr;
    m_resourceDomain = nullptr;
    m_addressSpace = nullptr;
    m_initialRegistryObjectCount = 0U;
    m_initialCapabilityCount = 0U;
    m_hasFailure = false;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::CaptureOwners(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept
{
    if (memoryOwner.GetState() != MemoryOwnerState::Ready)
    {
        return LaunchOwnerError::MemoryOwnerNotReady;
    }
    if (mappingOwner.GetState() != MappingOwnerState::Ready)
    {
        return LaunchOwnerError::MappingOwnerNotReady;
    }

    const auto memorySummary = memoryOwner.GetSummary();
    if (!memorySummary.HasValue())
    {
        m_failure.MemoryAccess = memorySummary.GetError();
        return LaunchOwnerError::MemoryOwnerAccessRejected;
    }
    const auto mappingSummary = mappingOwner.GetSummary();
    if (!mappingSummary.HasValue())
    {
        m_failure.MappingAccess = mappingSummary.GetError();
        return LaunchOwnerError::MappingOwnerAccessRejected;
    }
    m_memorySummary = memorySummary.GetValue();
    m_mappingSummary = mappingSummary.GetValue();

    if ((m_memorySummary.RangeCount < 3U) ||
        (m_memorySummary.RangeCount > Executable::Ia64InitialUserImageMaximumMemoryObjectCount) ||
        (m_memorySummary.Image.Flags != Executable::Ia64P64ElfFlags) ||
        (m_memorySummary.Image.Machine != Executable::Ia64ElfMachine) ||
        (m_memorySummary.Image.ProgramHeaderCount == 0U) || (m_memorySummary.Image.EntryBundleAddress == 0U) ||
        (m_memorySummary.Image.GlobalPointer == 0U) || (m_memorySummary.AggregateArena.PhysicalStart == 0U) ||
        ((m_memorySummary.AggregateArena.PhysicalStart & PageOffsetMask) != 0U) ||
        (m_memorySummary.AggregateArena.PageCount == 0U) || !m_memorySummary.AggregateFrame.IsValid() ||
        !m_memorySummary.ResourceDomainGeneration.IsValid())
    {
        return LaunchOwnerError::InvalidMemorySummary;
    }

    if (!m_mappingSummary.AddressSpaceIdentity.IsValid() ||
        !m_mappingSummary.AggregateFrame.IsEqualTo(m_memorySummary.AggregateFrame) ||
        (m_mappingSummary.RangeCount != m_memorySummary.RangeCount) ||
        (m_mappingSummary.InitialMappingCount > m_mappingSummary.FinalMappingCount) ||
        (m_mappingSummary.RangeCount != (m_mappingSummary.FinalMappingCount - m_mappingSummary.InitialMappingCount)) ||
        (m_mappingSummary.FirstMappingSequence == 0U) ||
        (m_mappingSummary.RangeCount - 1U > (MaximumUInt64 - m_mappingSummary.FirstMappingSequence)) ||
        (m_mappingSummary.LastMappingSequence !=
         (m_mappingSummary.FirstMappingSequence + m_mappingSummary.RangeCount - 1U)))
    {
        return LaunchOwnerError::InvalidMappingSummary;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::CaptureRuntime(
    const Kernel::Memory::AddressSpaceId expectedAddressSpaceIdentity, Kernel::Object::ObjectRegistry& registry,
    Kernel::Object::RootCapabilityIssuer& issuer, Kernel::Object::RevocationRegistry& revocationRegistry,
    Kernel::Object::CapabilitySpace& capabilitySpace, const Ia64InitialUserImageBootstrapGrant& bootstrapGrant,
    const Ia64InitialUserImageBootstrapGrant& diagnosticGrant,
    const Kernel::Protection::ProtectionDomainId protectionDomainIdentity,
    Kernel::Protection::ProtectionDomain& protectionDomain,
    const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    if (!expectedAddressSpaceIdentity.IsValid() || !expectedAddressSpaceIdentity.IsFromRegistry(registry) ||
        !m_mappingSummary.AddressSpaceIdentity.IsEqualTo(expectedAddressSpaceIdentity))
    {
        return LaunchOwnerError::InvalidExpectedAddressSpace;
    }
    if (!issuer.IsFromRegistry(registry))
    {
        return LaunchOwnerError::IssuerRegistryMismatch;
    }
    if (!revocationRegistry.IsReady())
    {
        return LaunchOwnerError::RevocationRegistryRejected;
    }
    if (!capabilitySpace.IsReady() || !capabilitySpace.IsFromRegistry(registry) ||
        !capabilitySpace.IsFromRevocationRegistry(revocationRegistry) || !capabilitySpace.GetIdentifier().IsValid() ||
        (capabilitySpace.GetActiveCount() != 0U))
    {
        return LaunchOwnerError::CapabilitySpaceRejected;
    }
    if (!protectionDomainIdentity.IsValid() || !protectionDomainIdentity.IsFromRegistry(registry))
    {
        return LaunchOwnerError::ProtectionDomainRejected;
    }
    if (!resourceDomainIdentity.IsValid() || !resourceDomainIdentity.IsFromRegistry(registry) ||
        !m_memorySummary.ResourceDomainGeneration.IsEqualTo(resourceDomainIdentity))
    {
        return LaunchOwnerError::ResourceDomainRejected;
    }
    if (!GrantIsWellFormed(bootstrapGrant, registry))
    {
        return LaunchOwnerError::InvalidBootstrapGrant;
    }
    if (!GrantIsWellFormed(diagnosticGrant, registry) ||
        (diagnosticGrant.Target.GetValue() == bootstrapGrant.Target.GetValue()))
    {
        return LaunchOwnerError::InvalidDiagnosticGrant;
    }
    m_bootstrapGrant = bootstrapGrant;
    m_diagnosticGrant = diagnosticGrant;

    Kernel::Object::ObjectPin verification;
    m_failure.Pin = registry.PinObject(protectionDomainIdentity.GetObjectId(),
                                       Kernel::Object::ObjectType::ProtectionDomain, verification);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
        (&verification.GetObject() != static_cast<Kernel::Object::IKernelObject*>(&protectionDomain)))
    {
        return LaunchOwnerError::RegistryBindingRejected;
    }
    verification.Reset();
    m_failure.Pin = registry.PinObject(resourceDomainIdentity.GetObjectId(), Kernel::Object::ObjectType::ResourceDomain,
                                       verification);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
        (&verification.GetObject() != static_cast<Kernel::Object::IKernelObject*>(&resourceDomain)))
    {
        return LaunchOwnerError::RegistryBindingRejected;
    }
    verification.Reset();
    m_failure.Pin = registry.PinObject(expectedAddressSpaceIdentity.GetObjectId(),
                                       Kernel::Object::ObjectType::AddressSpace, verification);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid())
    {
        return LaunchOwnerError::RegistryBindingRejected;
    }
    m_addressSpace = static_cast<Kernel::Memory::AddressSpace*>(&verification.GetObject());
    verification.Reset();

    m_resourceSnapshot = resourceDomain.GetSnapshot();
    if ((m_resourceSnapshot.State != Kernel::Resource::ResourceDomainState::Ready) ||
        (m_resourceSnapshot.Identity.GetValue() != resourceDomain.GetIdentifier().GetValue()) ||
        !resourceDomain.IsReady())
    {
        return LaunchOwnerError::ResourceDomainRejected;
    }

    m_failure.Pin = registry.PinObject(m_bootstrapGrant.Target, m_bootstrapGrant.ExpectedType, m_bootstrapTargetPin);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) ||
        !TargetPinMatchesGrant(m_bootstrapTargetPin, m_bootstrapGrant, m_resourceSnapshot.Identity))
    {
        return LaunchOwnerError::BootstrapTargetRejected;
    }
    m_failure.Pin = registry.PinObject(m_diagnosticGrant.Target, m_diagnosticGrant.ExpectedType, m_diagnosticTargetPin);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) ||
        !TargetPinMatchesGrant(m_diagnosticTargetPin, m_diagnosticGrant, m_resourceSnapshot.Identity))
    {
        return LaunchOwnerError::DiagnosticTargetRejected;
    }

    m_protectionSnapshot = protectionDomain.GetSnapshot();
    const auto binding = protectionDomain.GetBinding();
    if (!binding.HasValue())
    {
        m_failure.ProtectionDomain = binding.GetError();
        return LaunchOwnerError::ProtectionDomainRejected;
    }
    m_protectionBinding = binding.GetValue();
    if ((m_protectionSnapshot.State != Kernel::Protection::ProtectionDomainState::Ready) ||
        (m_protectionSnapshot.Kind != Kernel::Protection::ProtectionDomainKind::UserCapable) ||
        !m_protectionSnapshot.Identity.IsEqualTo(protectionDomainIdentity) ||
        !m_protectionSnapshot.AddressSpace.IsEqualTo(expectedAddressSpaceIdentity) ||
        (m_protectionSnapshot.CapabilitySpace.GetValue() != capabilitySpace.GetIdentifier().GetValue()) ||
        !m_protectionSnapshot.ResourceDomainIdentity.IsEqualTo(resourceDomainIdentity) ||
        (m_protectionSnapshot.ResourceDomain.GetValue() != m_resourceSnapshot.Identity.GetValue()) ||
        !m_protectionBinding.Identity.IsEqualTo(protectionDomainIdentity) ||
        !m_protectionBinding.AddressSpace.IsEqualTo(expectedAddressSpaceIdentity) ||
        (m_protectionBinding.AddressSpaceStorage != m_addressSpace) ||
        (m_protectionBinding.Capabilities != &capabilitySpace) ||
        (m_protectionBinding.CapabilitySpace.GetValue() != capabilitySpace.GetIdentifier().GetValue()) ||
        !m_protectionBinding.ResourceDomainIdentity.IsEqualTo(resourceDomainIdentity) ||
        (m_protectionBinding.ResourceDomainStorage != &resourceDomain) ||
        (m_protectionBinding.ResourceDomain.GetValue() != m_resourceSnapshot.Identity.GetValue()))
    {
        return LaunchOwnerError::ProtectionDomainRejected;
    }

    m_addressSpaceSnapshot = m_addressSpace->GetSnapshot();
    if (!m_addressSpaceSnapshot.Identity.IsEqualTo(expectedAddressSpaceIdentity) ||
        (m_addressSpaceSnapshot.State != Kernel::Memory::AddressSpaceState::Ready) ||
        (m_addressSpaceSnapshot.MappingCount != m_mappingSummary.FinalMappingCount) ||
        (m_addressSpaceSnapshot.ResidentProcessorMask != 0U) || m_addressSpaceSnapshot.ActivatingProcessor.IsValid() ||
        m_addressSpaceSnapshot.TransitionProcessor.IsValid() || (m_addressSpaceSnapshot.TargetProcessorMask != 0U) ||
        (m_addressSpaceSnapshot.AcknowledgedProcessorMask != 0U) ||
        !BytesAreZero(m_addressSpaceSnapshot.Reserved, sizeof(m_addressSpaceSnapshot.Reserved)))
    {
        return LaunchOwnerError::AddressSpaceRejected;
    }

    m_registry = &registry;
    m_capabilitySpace = &capabilitySpace;
    m_protectionDomain = &protectionDomain;
    m_resourceDomain = &resourceDomain;
    m_initialRegistryObjectCount = registry.GetActiveCount();
    m_initialCapabilityCount = capabilitySpace.GetActiveCount();
    m_summary.AddressSpaceIdentity = expectedAddressSpaceIdentity;
    m_summary.ProtectionDomainIdentity = protectionDomainIdentity;
    m_summary.ResourceDomainIdentity = resourceDomainIdentity;
    m_summary.CapabilitySpaceIdentity = capabilitySpace.GetIdentifier();
    m_summary.RangeCount = m_mappingSummary.RangeCount;
    m_summary.FirstMappingSequence = m_mappingSummary.FirstMappingSequence;
    m_summary.LastMappingSequence = m_mappingSummary.LastMappingSequence;
    return LaunchOwnerError::None;
}

LaunchOwnerError
Ia64InitialUserImageLaunchOwner::ValidateMapping(Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
                                                 const Executable::Ia64InitialUserImageMappingOwner& mappingOwner,
                                                 const Foundation::Size index) noexcept
{
    m_failure.RangeIndex = index;
    const auto plan = memoryOwner.GetRangePlan(index);
    const auto range = memoryOwner.BorrowRange(index);
    const auto mapping = mappingOwner.GetMapping(index);
    if (!plan.HasValue() || !range.HasValue())
    {
        m_failure.MemoryAccess = !plan.HasValue() ? plan.GetError() : range.GetError();
        return LaunchOwnerError::MemoryOwnerAccessRejected;
    }
    if (!mapping.HasValue())
    {
        m_failure.MappingAccess = mapping.GetError();
        return LaunchOwnerError::MappingOwnerAccessRejected;
    }
    if (!RangePlansAreEqual(plan.GetValue(), range.GetValue().Plan) ||
        !RangePlansAreEqual(plan.GetValue(), mapping.GetValue().Plan))
    {
        return LaunchOwnerError::RangePlanMismatch;
    }
    m_ranges[index] = range.GetValue();
    m_mappings[index] = mapping.GetValue();
    const MemoryRangePlan& value = m_mappings[index].Plan;

    Foundation::UInt64 virtualEnd{0U};
    if (((value.VirtualStart & PageOffsetMask) != 0U) || !value.MemoryObject.IsValid() ||
        !value.MemoryObject.IsFromRegistry(*m_registry) ||
        !TryGetPageRangeEnd(value.VirtualStart, value.Pages.GetValue(), virtualEnd) ||
        !IsImplementedUserRange(value.VirtualStart, virtualEnd,
                                m_launchRequest.Requirements.ImplementedVirtualAddressMostSignificantBit) ||
        (value.FrameOffsetPages > m_memorySummary.AggregateArena.PageCount) ||
        (value.Pages.GetValue() > (m_memorySummary.AggregateArena.PageCount - value.FrameOffsetPages)) ||
        !BytesAreZero(value.Reserved, sizeof(value.Reserved)))
    {
        return LaunchOwnerError::InvalidRange;
    }
    if (!PermissionsAreSafe(value.Permissions))
    {
        return LaunchOwnerError::InvalidPermissions;
    }

    using Permission = Kernel::Memory::PagePermission;
    if (value.Purpose == Purpose::LoadSegment)
    {
        if ((value.GuardPolicy != GuardPolicy::None) ||
            (value.OriginalSegmentIndex == Executable::Ia64InitialUserImageNoSegment) ||
            (value.OriginalSegmentIndex >= m_memorySummary.Image.ProgramHeaderCount))
        {
            return LaunchOwnerError::InvalidRange;
        }
    }
    else if ((value.Purpose == Purpose::MemoryStack) || (value.Purpose == Purpose::RseBackingStore))
    {
        if ((value.GuardPolicy != GuardPolicy::OnePageBeforeAndAfter) ||
            (value.OriginalSegmentIndex != Executable::Ia64InitialUserImageNoSegment) ||
            (value.Permissions.GetBits() != WritableUserPermissions().GetBits()) ||
            value.Permissions.Contains(Permission::Execute))
        {
            return LaunchOwnerError::InvalidPermissions;
        }
    }
    else
    {
        return LaunchOwnerError::InvalidRange;
    }

    const Kernel::Memory::AddressSpaceMappingReceipt& receipt = m_mappings[index].Receipt;
    if ((receipt.MappingSequence != (m_mappingSummary.FirstMappingSequence + index)) ||
        (receipt.MappingCount != (m_mappingSummary.InitialMappingCount + index + 1U)) ||
        (receipt.VirtualStart.GetValue() != value.VirtualStart) || (receipt.Pages.GetValue() != value.Pages.GetValue()))
    {
        return LaunchOwnerError::InvalidMappingReceipt;
    }

    const auto record = m_addressSpace->TryGetMapping(m_mappingSummary.InitialMappingCount + index);
    if (!record.HasValue() || !MappingRecordMatches(record.GetValue(), value, m_memorySummary))
    {
        return LaunchOwnerError::InvalidMappingRecord;
    }
    if ((range.GetValue().FrameStorage == nullptr) || (range.GetValue().MemoryObjectStorage == nullptr))
    {
        return LaunchOwnerError::InvalidMappingRecord;
    }
    if ((index != 0U) && (range.GetValue().FrameStorage != m_ranges[0U].FrameStorage))
    {
        return LaunchOwnerError::InvalidMappingRecord;
    }
    return ValidateMappingStorage(index);
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::ValidateMappingStorage(const Foundation::Size index) noexcept
{
    const MemoryRangePlan& value = m_mappings[index].Plan;
    Executable::Ia64InitialUserImageMemoryRangeView& range = m_ranges[index];
    const Kernel::Memory::MemoryObjectSnapshot memoryObject = range.MemoryObjectStorage->GetSnapshot();
    if (!MemoryObjectSnapshotMatches(memoryObject, value, m_summary.ResourceDomainIdentity,
                                     m_resourceSnapshot.Identity) ||
        (range.MemoryObjectStorage->GetChargeOwner().GetValue() != m_resourceSnapshot.Identity.GetValue()))
    {
        return LaunchOwnerError::InvalidMappingRecord;
    }

    Kernel::Object::ObjectPin verification;
    m_failure.Pin =
        m_registry->PinObject(value.MemoryObject.GetObjectId(), Kernel::Object::ObjectType::MemoryObject, verification);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
        (&verification.GetObject() != static_cast<Kernel::Object::IKernelObject*>(range.MemoryObjectStorage)))
    {
        return LaunchOwnerError::RegistryBindingRejected;
    }
    verification.Reset();

    for (Foundation::Size previous{0U}; previous < index; ++previous)
    {
        if (value.MemoryObject.IsEqualTo(m_mappings[previous].Plan.MemoryObject) ||
            (range.MemoryObjectStorage == m_ranges[previous].MemoryObjectStorage))
        {
            return LaunchOwnerError::InvalidMappingRecord;
        }
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::ValidateMappings(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept
{
    Foundation::UInt64 expectedFrameOffset{0U};
    for (Foundation::Size index{0U}; index < m_mappingSummary.RangeCount; ++index)
    {
        const LaunchOwnerError error = ValidateMapping(memoryOwner, mappingOwner, index);
        if (error != LaunchOwnerError::None)
        {
            return error;
        }
        const MemoryRangePlan& plan = m_mappings[index].Plan;
        if (plan.FrameOffsetPages != expectedFrameOffset)
        {
            return LaunchOwnerError::InvalidRange;
        }
        if (expectedFrameOffset > (MaximumUInt64 - plan.Pages.GetValue()))
        {
            return LaunchOwnerError::InvalidRange;
        }
        expectedFrameOffset += plan.Pages.GetValue();
    }
    if (expectedFrameOffset != m_memorySummary.AggregateArena.PageCount)
    {
        return LaunchOwnerError::InvalidRange;
    }

    Kernel::Memory::Frame* const frame = m_ranges[0U].FrameStorage;
    if (frame == nullptr)
    {
        return LaunchOwnerError::InvalidMappingRecord;
    }
    Kernel::Object::ObjectPin verification;
    m_failure.Pin = m_registry->PinObject(m_memorySummary.AggregateFrame.GetObjectId(),
                                          Kernel::Object::ObjectType::Frame, verification);
    if ((m_failure.Pin != Kernel::Object::ObjectPinError::Ok) || !verification.IsValid() ||
        (&verification.GetObject() != static_cast<Kernel::Object::IKernelObject*>(frame)))
    {
        return LaunchOwnerError::RegistryBindingRejected;
    }
    verification.Reset();
    const Kernel::Memory::FrameSnapshot frameSnapshot = frame->GetSnapshot();
    if ((frameSnapshot.State != Kernel::Memory::FrameState::Ready) ||
        (frameSnapshot.Descriptor.PhysicalStart.GetValue() != m_memorySummary.AggregateArena.PhysicalStart) ||
        (frameSnapshot.Descriptor.Pages.GetValue() != m_memorySummary.AggregateArena.PageCount) ||
        (frameSnapshot.Descriptor.CachePolicy != Kernel::Memory::MemoryCachePolicy::WriteBack) ||
        (frameSnapshot.Descriptor.Kind != Kernel::Memory::FrameKind::Ram) ||
        !BytesAreZero(frameSnapshot.Descriptor.Reserved, sizeof(frameSnapshot.Descriptor.Reserved)) ||
        !BytesAreZero(frameSnapshot.Reserved, sizeof(frameSnapshot.Reserved)) ||
        (frame->GetChargeOwner().GetValue() != m_resourceSnapshot.Identity.GetValue()))
    {
        return LaunchOwnerError::InvalidMappingRecord;
    }
    m_failure.RangeIndex = Executable::Ia64InitialUserImageNoSegment;
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::SelectLaunchRanges() noexcept
{
    Foundation::Size entryCount{0U};
    Foundation::Size globalPointerCount{0U};
    Foundation::Size memoryStackCount{0U};
    Foundation::Size rseBackingStoreCount{0U};
    using Permission = Kernel::Memory::PagePermission;

    for (Foundation::Size index{0U}; index < m_mappingSummary.RangeCount; ++index)
    {
        const MemoryRangePlan& plan = m_mappings[index].Plan;
        Foundation::UInt64 end{0U};
        if (!TryGetPageRangeEnd(plan.VirtualStart, plan.Pages.GetValue(), end))
        {
            m_failure.RangeIndex = index;
            return LaunchOwnerError::InvalidRange;
        }
        if ((plan.Purpose == Purpose::LoadSegment) && plan.Permissions.Contains(Permission::Execute) &&
            ContainsRange(plan.VirtualStart, end, m_memorySummary.Image.EntryBundleAddress,
                          Ia64UserModeEntryBundleSizeBytes))
        {
            ++entryCount;
            m_summary.EntryMappingIndex = index;
        }
        if ((plan.Purpose == Purpose::LoadSegment) && plan.Permissions.Contains(Permission::Read) &&
            ContainsRange(plan.VirtualStart, end, m_memorySummary.Image.GlobalPointer,
                          Ia64UserModeGlobalPointerAlignmentBytes))
        {
            ++globalPointerCount;
            m_summary.GlobalPointerMappingIndex = index;
        }
        if (plan.Purpose == Purpose::MemoryStack)
        {
            ++memoryStackCount;
            m_summary.MemoryStackMappingIndex = index;
        }
        if (plan.Purpose == Purpose::RseBackingStore)
        {
            ++rseBackingStoreCount;
            m_summary.RseBackingStoreMappingIndex = index;
        }
    }

    if ((m_memorySummary.Image.EntryBundleAddress & (Ia64UserModeEntryBundleSizeBytes - 1U)) != 0U ||
        (entryCount != 1U))
    {
        return LaunchOwnerError::EntryMappingRejected;
    }
    if ((m_memorySummary.Image.GlobalPointer == 0U) ||
        ((m_memorySummary.Image.GlobalPointer & (Ia64UserModeGlobalPointerAlignmentBytes - 1U)) != 0U) ||
        (globalPointerCount != 1U))
    {
        return LaunchOwnerError::GlobalPointerMappingRejected;
    }
    if (memoryStackCount != 1U)
    {
        return LaunchOwnerError::MemoryStackMappingRejected;
    }
    if (rseBackingStoreCount != 1U)
    {
        return LaunchOwnerError::RseBackingStoreMappingRejected;
    }

    const MemoryRangePlan& memoryStack = m_mappings[m_summary.MemoryStackMappingIndex].Plan;
    if ((memoryStack.VirtualStart != Ia64InitialUserMemoryStackVirtualStart) ||
        (memoryStack.Pages.GetValue() != Ia64InitialUserMemoryStackPageCount))
    {
        return LaunchOwnerError::MemoryStackMappingRejected;
    }
    const MemoryRangePlan& rseBackingStore = m_mappings[m_summary.RseBackingStoreMappingIndex].Plan;
    if ((rseBackingStore.VirtualStart != Ia64InitialUserRseBackingStoreVirtualStart) ||
        (rseBackingStore.Pages.GetValue() != Ia64InitialUserRseBackingStorePageCount))
    {
        return LaunchOwnerError::RseBackingStoreMappingRejected;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::ValidateGuards() noexcept
{
    const Foundation::Size guardedIndices[]{
        m_summary.MemoryStackMappingIndex,
        m_summary.RseBackingStoreMappingIndex,
    };
    for (const Foundation::Size guardedIndex : guardedIndices)
    {
        const MemoryRangePlan& guarded = m_mappings[guardedIndex].Plan;
        Foundation::UInt64 mappingEnd{0U};
        if ((guarded.VirtualStart < PageSizeBytes) ||
            !TryGetPageRangeEnd(guarded.VirtualStart, guarded.Pages.GetValue(), mappingEnd) ||
            (mappingEnd > (MaximumUInt64 - PageSizeBytes)))
        {
            m_failure.RangeIndex = guardedIndex;
            return LaunchOwnerError::InvalidRange;
        }
        const Foundation::UInt64 guardStart = guarded.VirtualStart - PageSizeBytes;
        const Foundation::UInt64 guardEnd = mappingEnd + PageSizeBytes;
        for (Foundation::Size recordIndex{0U}; recordIndex < m_addressSpaceSnapshot.MappingCount; ++recordIndex)
        {
            const auto record = m_addressSpace->TryGetMapping(recordIndex);
            Foundation::UInt64 recordEnd{0U};
            if (!record.HasValue() || !TryGetPageRangeEnd(record.GetValue().VirtualStart.GetValue(),
                                                          record.GetValue().Pages.GetValue(), recordEnd))
            {
                m_failure.RangeIndex = guardedIndex;
                return LaunchOwnerError::InvalidMappingRecord;
            }
            if (RangesOverlap(record.GetValue().VirtualStart.GetValue(), recordEnd, guardStart, guarded.VirtualStart) ||
                RangesOverlap(record.GetValue().VirtualStart.GetValue(), recordEnd, mappingEnd, guardEnd))
            {
                m_failure.RangeIndex = guardedIndex;
                return LaunchOwnerError::GuardRangeOccupied;
            }
        }
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::BuildLaunchRequest(
    const Ia64UserModeVirtualRange supervisorMemoryStack, const Ia64UserModeVirtualRange supervisorRseBackingStore,
    const Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept
{
    if ((implementedVirtualAddressMostSignificantBit < MinimumIa64UserModeImplementedAddressBit) ||
        (implementedVirtualAddressMostSignificantBit > MaximumIa64UserModeImplementedAddressBit))
    {
        return LaunchOwnerError::InvalidImplementedAddressWidth;
    }

    const MemoryRangePlan& code = m_mappings[m_summary.EntryMappingIndex].Plan;
    const MemoryRangePlan& memoryStack = m_mappings[m_summary.MemoryStackMappingIndex].Plan;
    const MemoryRangePlan& rseBackingStore = m_mappings[m_summary.RseBackingStoreMappingIndex].Plan;
    m_supervisorContinuation = Ia64UserModeSupervisorContinuation{
        .Registers = {},
        .MemoryStack = supervisorMemoryStack,
        .RseBackingStore = supervisorRseBackingStore,
        .State = Ia64UserModeSupervisorContinuationState::Fresh,
        .Owner = Ia64UserModeSupervisorContinuationOwner::None,
    };
    m_launchRequest = Ia64UserModeLaunchRequest{
        .Code =
            Ia64UserModeVirtualRange{
                .VirtualStart = code.VirtualStart,
                .SizeBytes = code.Pages.GetValue() * PageSizeBytes,
            },
        .MemoryStack =
            Ia64UserModeVirtualRange{
                .VirtualStart = memoryStack.VirtualStart,
                .SizeBytes = memoryStack.Pages.GetValue() * PageSizeBytes,
            },
        .RseBackingStore =
            Ia64UserModeVirtualRange{
                .VirtualStart = rseBackingStore.VirtualStart,
                .SizeBytes = rseBackingStore.Pages.GetValue() * PageSizeBytes,
            },
        .EntryBundleAddress = m_memorySummary.Image.EntryBundleAddress,
        .InitialGlobalPointer = m_memorySummary.Image.GlobalPointer,
        .InitialArguments = {},
        .Requirements =
            Ia64UserModeLaunchRequirements{
                .CurrentProcessorStatusMask = Ia64UserModeCurrentProcessorStatusMask,
                .CurrentProcessorStatusValue = Ia64UserModeCurrentProcessorStatusValue,
                .UserProcessorStatus = Ia64UserModePreemptibleProcessorStatus,
                .ImplementedVirtualAddressMostSignificantBit = implementedVirtualAddressMostSignificantBit,
                .CurrentPrivilegeLevel = 0U,
                .CurrentVirtualMachineMode = 0U,
                .CurrentInstructionSet = 0U,
                .CurrentRegisterBank = 1U,
                .Reserved = {},
            },
        .SupervisorContinuation = &m_supervisorContinuation,
        .Reserved = 0U,
    };
    const auto validation = Ia64UserModeLaunch::Validate(m_launchRequest);
    if (!validation.HasValue())
    {
        m_failure.Launch = validation.GetError();
        return LaunchOwnerError::LaunchRequestRejected;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::RevalidatePreflight(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner, Kernel::Object::ObjectRegistry& registry,
    Kernel::Object::RootCapabilityIssuer& issuer, Kernel::Object::RevocationRegistry& revocationRegistry,
    Kernel::Object::CapabilitySpace& capabilitySpace, Kernel::Protection::ProtectionDomain& protectionDomain,
    Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    LaunchOwnerError error = RevalidateMemoryOwner(memoryOwner);
    if (error == LaunchOwnerError::None)
    {
        error = RevalidateMappingOwner(mappingOwner);
    }
    if (error == LaunchOwnerError::None)
    {
        error = RevalidateProtectionDomain(protectionDomain);
    }
    if (error == LaunchOwnerError::None)
    {
        error = RevalidateRuntimeState(registry, issuer, revocationRegistry, capabilitySpace, resourceDomain);
    }
    if (error == LaunchOwnerError::None)
    {
        error = RevalidateRanges(memoryOwner, mappingOwner);
    }
    return error;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::RevalidateMemoryOwner(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner) noexcept
{
    const auto memorySummary = memoryOwner.GetSummary();
    if ((memoryOwner.GetState() != MemoryOwnerState::Ready) || !memorySummary.HasValue() ||
        !MemorySummariesAreEqual(memorySummary.GetValue(), m_memorySummary))
    {
        if (!memorySummary.HasValue())
        {
            m_failure.MemoryAccess = memorySummary.GetError();
        }
        return LaunchOwnerError::AuthoritativeStateChanged;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::RevalidateMappingOwner(
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept
{
    const auto mappingSummary = mappingOwner.GetSummary();
    if ((mappingOwner.GetState() != MappingOwnerState::Ready) || !mappingSummary.HasValue() ||
        !MappingSummariesAreEqual(mappingSummary.GetValue(), m_mappingSummary))
    {
        if (!mappingSummary.HasValue())
        {
            m_failure.MappingAccess = mappingSummary.GetError();
        }
        return LaunchOwnerError::AuthoritativeStateChanged;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::RevalidateProtectionDomain(
    Kernel::Protection::ProtectionDomain& protectionDomain) noexcept
{
    const auto binding = protectionDomain.GetBinding();
    if ((&protectionDomain != m_protectionDomain) ||
        !ProtectionSnapshotsAreEqual(protectionDomain.GetSnapshot(), m_protectionSnapshot) || !binding.HasValue() ||
        (binding.GetValue().AddressSpaceStorage != m_addressSpace) ||
        (binding.GetValue().Capabilities != m_capabilitySpace) ||
        (binding.GetValue().ResourceDomainStorage != m_resourceDomain))
    {
        if (!binding.HasValue())
        {
            m_failure.ProtectionDomain = binding.GetError();
        }
        return LaunchOwnerError::AuthoritativeStateChanged;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::RevalidateRuntimeState(
    Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer,
    Kernel::Object::RevocationRegistry& revocationRegistry, Kernel::Object::CapabilitySpace& capabilitySpace,
    Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    if ((&registry != m_registry) || !issuer.IsFromRegistry(registry) || !revocationRegistry.IsReady() ||
        (&capabilitySpace != m_capabilitySpace) || !capabilitySpace.IsReady() ||
        !capabilitySpace.IsFromRegistry(registry) || !capabilitySpace.IsFromRevocationRegistry(revocationRegistry) ||
        (capabilitySpace.GetIdentifier().GetValue() != m_summary.CapabilitySpaceIdentity.GetValue()) ||
        (capabilitySpace.GetActiveCount() != m_initialCapabilityCount) || (&resourceDomain != m_resourceDomain) ||
        !ResourceSnapshotsAreEqual(resourceDomain.GetSnapshot(), m_resourceSnapshot) ||
        (m_bootstrapGrant.Registry != &registry) ||
        !TargetPinMatchesGrant(m_bootstrapTargetPin, m_bootstrapGrant, m_resourceSnapshot.Identity) ||
        (m_diagnosticGrant.Registry != &registry) ||
        !TargetPinMatchesGrant(m_diagnosticTargetPin, m_diagnosticGrant, m_resourceSnapshot.Identity) ||
        (registry.GetActiveCount() != m_initialRegistryObjectCount) ||
        !AddressSpaceSnapshotsAreEqual(m_addressSpace->GetSnapshot(), m_addressSpaceSnapshot))
    {
        return LaunchOwnerError::AuthoritativeStateChanged;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::RevalidateRanges(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept
{
    for (Foundation::Size index{0U}; index < m_mappingSummary.RangeCount; ++index)
    {
        const auto plan = memoryOwner.GetRangePlan(index);
        const auto range = memoryOwner.BorrowRange(index);
        const auto mapping = mappingOwner.GetMapping(index);
        if (!plan.HasValue() || !range.HasValue() || !mapping.HasValue() ||
            !RangePlansAreEqual(plan.GetValue(), m_mappings[index].Plan) ||
            !RangePlansAreEqual(range.GetValue().Plan, m_mappings[index].Plan) ||
            (range.GetValue().FrameStorage != m_ranges[index].FrameStorage) ||
            (range.GetValue().MemoryObjectStorage != m_ranges[index].MemoryObjectStorage) ||
            !InstalledMappingsAreEqual(mapping.GetValue(), m_mappings[index]))
        {
            m_failure.RangeIndex = index;
            return LaunchOwnerError::AuthoritativeStateChanged;
        }
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::CreateRevocationDomains(
    Kernel::Object::RevocationRegistry& revocationRegistry) noexcept
{
    const auto revocationDomain = revocationRegistry.CreateRoot();
    if (!revocationDomain.HasValue())
    {
        m_failure.Revocation = revocationDomain.GetError();
        return LaunchOwnerError::RevocationDomainCreationRejected;
    }
    m_summary.RevocationDomain = revocationDomain.GetValue();

    // A root of its own lets either capability be revoked without the other.
    // Both roots exist before the first mint, so a shortage of roots never
    // leaves the image with only one of its two capabilities.
    const auto diagnosticRevocationDomain = revocationRegistry.CreateRoot();
    if (!diagnosticRevocationDomain.HasValue())
    {
        m_failure.Revocation = diagnosticRevocationDomain.GetError();
        return LaunchOwnerError::DiagnosticRevocationDomainCreationRejected;
    }
    m_summary.DiagnosticRevocationDomain = diagnosticRevocationDomain.GetValue();
    return LaunchOwnerError::None;
}

LaunchOwnerError
Ia64InitialUserImageLaunchOwner::CreateCapability(Kernel::Object::RootCapabilityIssuer& issuer,
                                                  Kernel::Object::CapabilitySpace& capabilitySpace) noexcept
{
    const auto capability =
        issuer.Mint(capabilitySpace, m_bootstrapGrant.Target, m_bootstrapGrant.Rights, m_summary.RevocationDomain,
                    m_bootstrapGrant.LeaseDeadline, m_bootstrapGrant.SealedObjectSelector);
    if (!capability.HasValue())
    {
        m_failure.Capability = capability.GetError();
        return LaunchOwnerError::CapabilityMintRejected;
    }
    m_summary.BootstrapCapability = capability.GetValue();
    m_launchRequest.InitialArguments.Register4 = m_summary.BootstrapCapability.GetSelector();
    m_launchRequest.InitialArguments.Register5 = m_summary.BootstrapCapability.GetAuthenticator();

    const auto descriptor = capabilitySpace.Query(m_summary.BootstrapCapability);
    if (!descriptor.HasValue())
    {
        m_failure.Capability = descriptor.GetError();
        return LaunchOwnerError::CapabilityQueryRejected;
    }
    if ((capabilitySpace.GetActiveCount() != (m_initialCapabilityCount + 1U)) ||
        !m_summary.BootstrapCapability.IsValid() ||
        !DescriptorMatchesGrant(descriptor.GetValue(), m_bootstrapGrant, m_summary.RevocationDomain,
                                m_resourceSnapshot.Identity))
    {
        return LaunchOwnerError::CapabilityQueryRejected;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError
Ia64InitialUserImageLaunchOwner::CreateDiagnosticCapability(Kernel::Object::RootCapabilityIssuer& issuer,
                                                            Kernel::Object::CapabilitySpace& capabilitySpace) noexcept
{
    const auto capability = issuer.Mint(capabilitySpace, m_diagnosticGrant.Target, m_diagnosticGrant.Rights,
                                        m_summary.DiagnosticRevocationDomain, m_diagnosticGrant.LeaseDeadline,
                                        m_diagnosticGrant.SealedObjectSelector);
    if (!capability.HasValue())
    {
        m_failure.Capability = capability.GetError();
        return LaunchOwnerError::DiagnosticCapabilityMintRejected;
    }
    m_summary.DiagnosticCapability = capability.GetValue();
    m_launchRequest.InitialArguments.Register6 = m_summary.DiagnosticCapability.GetSelector();
    m_launchRequest.InitialArguments.Register7 = m_summary.DiagnosticCapability.GetAuthenticator();

    const auto descriptor = capabilitySpace.Query(m_summary.DiagnosticCapability);
    if (!descriptor.HasValue())
    {
        m_failure.Capability = descriptor.GetError();
        return LaunchOwnerError::DiagnosticCapabilityQueryRejected;
    }
    if ((capabilitySpace.GetActiveCount() != (m_initialCapabilityCount + 2U)) ||
        !m_summary.DiagnosticCapability.IsValid() ||
        !DescriptorMatchesGrant(descriptor.GetValue(), m_diagnosticGrant, m_summary.DiagnosticRevocationDomain,
                                m_resourceSnapshot.Identity))
    {
        return LaunchOwnerError::DiagnosticCapabilityQueryRejected;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::VerifyPostconditions(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner, Kernel::Object::ObjectRegistry& registry,
    Kernel::Object::RootCapabilityIssuer& issuer, Kernel::Object::RevocationRegistry& revocationRegistry,
    Kernel::Object::CapabilitySpace& capabilitySpace, Kernel::Protection::ProtectionDomain& protectionDomain,
    Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    LaunchOwnerError error = VerifyMemoryOwnerPostconditions(memoryOwner);
    if (error == LaunchOwnerError::None)
    {
        error = VerifyMappingOwnerPostconditions(mappingOwner);
    }
    if (error == LaunchOwnerError::None)
    {
        error = VerifyCapabilityPostconditions(revocationRegistry, capabilitySpace);
    }
    if (error == LaunchOwnerError::None)
    {
        error = VerifyRuntimePostconditions(registry, issuer, protectionDomain, resourceDomain);
    }
    if (error == LaunchOwnerError::None)
    {
        error = VerifyLaunchPostconditions();
    }
    return error;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::VerifyMemoryOwnerPostconditions(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner) noexcept
{
    const auto memorySummary = memoryOwner.GetSummary();
    if ((memoryOwner.GetState() != MemoryOwnerState::Ready) || !memorySummary.HasValue() ||
        !MemorySummariesAreEqual(memorySummary.GetValue(), m_memorySummary))
    {
        if (!memorySummary.HasValue())
        {
            m_failure.MemoryAccess = memorySummary.GetError();
        }
        return LaunchOwnerError::PostconditionRejected;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::VerifyMappingOwnerPostconditions(
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner) noexcept
{
    const auto mappingSummary = mappingOwner.GetSummary();
    if ((mappingOwner.GetState() != MappingOwnerState::Ready) || !mappingSummary.HasValue() ||
        !MappingSummariesAreEqual(mappingSummary.GetValue(), m_mappingSummary))
    {
        if (!mappingSummary.HasValue())
        {
            m_failure.MappingAccess = mappingSummary.GetError();
        }
        return LaunchOwnerError::PostconditionRejected;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::VerifyCapabilityPostconditions(
    Kernel::Object::RevocationRegistry& revocationRegistry, Kernel::Object::CapabilitySpace& capabilitySpace) noexcept
{
    const auto epoch = revocationRegistry.GetCurrentEpoch(m_summary.RevocationDomain);
    const auto descriptor = capabilitySpace.Query(m_summary.BootstrapCapability);
    const auto diagnosticEpoch = revocationRegistry.GetCurrentEpoch(m_summary.DiagnosticRevocationDomain);
    const auto diagnosticDescriptor = capabilitySpace.Query(m_summary.DiagnosticCapability);
    if (!epoch.HasValue() || (epoch.GetValue() != 1U) || !diagnosticEpoch.HasValue() ||
        (diagnosticEpoch.GetValue() != 1U) || (&capabilitySpace != m_capabilitySpace) ||
        !capabilitySpace.IsFromRevocationRegistry(revocationRegistry) ||
        (capabilitySpace.GetActiveCount() != (m_initialCapabilityCount + 2U)) || !descriptor.HasValue() ||
        !DescriptorMatchesGrant(descriptor.GetValue(), m_bootstrapGrant, m_summary.RevocationDomain,
                                m_resourceSnapshot.Identity) ||
        !diagnosticDescriptor.HasValue() ||
        !DescriptorMatchesGrant(diagnosticDescriptor.GetValue(), m_diagnosticGrant,
                                m_summary.DiagnosticRevocationDomain, m_resourceSnapshot.Identity))
    {
        if (!epoch.HasValue())
        {
            m_failure.Revocation = epoch.GetError();
        }
        else if (!diagnosticEpoch.HasValue())
        {
            m_failure.Revocation = diagnosticEpoch.GetError();
        }
        if (!descriptor.HasValue())
        {
            m_failure.Capability = descriptor.GetError();
        }
        else if (!diagnosticDescriptor.HasValue())
        {
            m_failure.Capability = diagnosticDescriptor.GetError();
        }
        return LaunchOwnerError::PostconditionRejected;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::VerifyRuntimePostconditions(
    Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer,
    Kernel::Protection::ProtectionDomain& protectionDomain, Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    const auto binding = protectionDomain.GetBinding();
    if ((&registry != m_registry) || !issuer.IsFromRegistry(registry) || (&protectionDomain != m_protectionDomain) ||
        !ProtectionSnapshotsAreEqual(protectionDomain.GetSnapshot(), m_protectionSnapshot) || !binding.HasValue() ||
        (binding.GetValue().AddressSpaceStorage != m_addressSpace) ||
        (binding.GetValue().Capabilities != m_capabilitySpace) ||
        (binding.GetValue().ResourceDomainStorage != m_resourceDomain) || (&resourceDomain != m_resourceDomain) ||
        !ResourceSnapshotsAreEqual(resourceDomain.GetSnapshot(), m_resourceSnapshot) ||
        (m_bootstrapGrant.Registry != &registry) ||
        !TargetPinMatchesGrant(m_bootstrapTargetPin, m_bootstrapGrant, m_resourceSnapshot.Identity) ||
        (m_diagnosticGrant.Registry != &registry) ||
        !TargetPinMatchesGrant(m_diagnosticTargetPin, m_diagnosticGrant, m_resourceSnapshot.Identity) ||
        (registry.GetActiveCount() != m_initialRegistryObjectCount) ||
        !AddressSpaceSnapshotsAreEqual(m_addressSpace->GetSnapshot(), m_addressSpaceSnapshot))
    {
        if (!binding.HasValue())
        {
            m_failure.ProtectionDomain = binding.GetError();
        }
        return LaunchOwnerError::PostconditionRejected;
    }
    return LaunchOwnerError::None;
}

LaunchOwnerError Ia64InitialUserImageLaunchOwner::VerifyLaunchPostconditions() noexcept
{
    const auto launch = Ia64UserModeLaunch::Validate(m_launchRequest);
    if (!launch.HasValue() || (m_launchRequest.SupervisorContinuation != &m_supervisorContinuation) ||
        (m_supervisorContinuation.State != Ia64UserModeSupervisorContinuationState::Fresh) ||
        (m_supervisorContinuation.Owner != Ia64UserModeSupervisorContinuationOwner::None) ||
        (m_launchRequest.InitialArguments.Register4 != m_summary.BootstrapCapability.GetSelector()) ||
        (m_launchRequest.InitialArguments.Register5 != m_summary.BootstrapCapability.GetAuthenticator()) ||
        (m_launchRequest.InitialArguments.Register6 != m_summary.DiagnosticCapability.GetSelector()) ||
        (m_launchRequest.InitialArguments.Register7 != m_summary.DiagnosticCapability.GetAuthenticator()))
    {
        if (!launch.HasValue())
        {
            m_failure.Launch = launch.GetError();
        }
        return LaunchOwnerError::PostconditionRejected;
    }
    return LaunchOwnerError::None;
}

Ia64InitialUserImageLaunchOwner::PrepareResult
Ia64InitialUserImageLaunchOwner::Fail(const LaunchOwnerError error) noexcept
{
    m_failure.Error = error;
    m_hasFailure = true;
    if (m_state == Ia64InitialUserImageLaunchOwnerState::Fresh)
    {
        m_bootstrapTargetPin.Reset();
        m_diagnosticTargetPin.Reset();
    }
    return PrepareResult::Failure(m_failure);
}

Ia64InitialUserImageLaunchOwner::PrepareResult Ia64InitialUserImageLaunchOwner::Prepare(
    Executable::Ia64InitialUserImageMemoryOwner& memoryOwner,
    const Executable::Ia64InitialUserImageMappingOwner& mappingOwner,
    const Kernel::Memory::AddressSpaceId expectedAddressSpaceIdentity, Kernel::Object::ObjectRegistry& registry,
    Kernel::Object::RootCapabilityIssuer& issuer, Kernel::Object::RevocationRegistry& revocationRegistry,
    Kernel::Object::CapabilitySpace& capabilitySpace, const Ia64InitialUserImageBootstrapGrant& bootstrapGrant,
    const Ia64InitialUserImageBootstrapGrant& diagnosticGrant,
    const Kernel::Protection::ProtectionDomainId protectionDomainIdentity,
    Kernel::Protection::ProtectionDomain& protectionDomain,
    const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain, const Ia64UserModeVirtualRange supervisorMemoryStack,
    const Ia64UserModeVirtualRange supervisorRseBackingStore,
    const Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept
{
    if (m_state != Ia64InitialUserImageLaunchOwnerState::Fresh)
    {
        Failure repeated{};
        repeated.Error = LaunchOwnerError::AlreadyAttempted;
        repeated.RangeIndex = Executable::Ia64InitialUserImageNoSegment;
        return PrepareResult::Failure(repeated);
    }

    ResetPreflightStorage();
    if ((implementedVirtualAddressMostSignificantBit < MinimumIa64UserModeImplementedAddressBit) ||
        (implementedVirtualAddressMostSignificantBit > MaximumIa64UserModeImplementedAddressBit))
    {
        return Fail(LaunchOwnerError::InvalidImplementedAddressWidth);
    }
    m_launchRequest.Requirements.ImplementedVirtualAddressMostSignificantBit =
        implementedVirtualAddressMostSignificantBit;
    LaunchOwnerError error = CaptureOwners(memoryOwner, mappingOwner);
    if (error == LaunchOwnerError::None)
    {
        error = CaptureRuntime(expectedAddressSpaceIdentity, registry, issuer, revocationRegistry, capabilitySpace,
                               bootstrapGrant, diagnosticGrant, protectionDomainIdentity, protectionDomain,
                               resourceDomainIdentity, resourceDomain);
    }
    if (error == LaunchOwnerError::None)
    {
        error = ValidateMappings(memoryOwner, mappingOwner);
    }
    if (error == LaunchOwnerError::None)
    {
        error = SelectLaunchRanges();
    }
    if (error == LaunchOwnerError::None)
    {
        error = ValidateGuards();
    }
    if (error == LaunchOwnerError::None)
    {
        error = BuildLaunchRequest(supervisorMemoryStack, supervisorRseBackingStore,
                                   implementedVirtualAddressMostSignificantBit);
    }
    if (error == LaunchOwnerError::None)
    {
        error = RevalidatePreflight(memoryOwner, mappingOwner, registry, issuer, revocationRegistry, capabilitySpace,
                                    protectionDomain, resourceDomain);
    }
    if (error != LaunchOwnerError::None)
    {
        return Fail(error);
    }

    m_state = Ia64InitialUserImageLaunchOwnerState::Failed;
    error = CreateRevocationDomains(revocationRegistry);
    if (error == LaunchOwnerError::None)
    {
        error = CreateCapability(issuer, capabilitySpace);
    }
    if (error == LaunchOwnerError::None)
    {
        error = CreateDiagnosticCapability(issuer, capabilitySpace);
    }
    if (error == LaunchOwnerError::None)
    {
        error = VerifyPostconditions(memoryOwner, mappingOwner, registry, issuer, revocationRegistry, capabilitySpace,
                                     protectionDomain, resourceDomain);
    }
    if (error != LaunchOwnerError::None)
    {
        return Fail(error);
    }

    m_failure = Failure{};
    m_failure.RangeIndex = Executable::Ia64InitialUserImageNoSegment;
    m_hasFailure = false;
    m_state = Ia64InitialUserImageLaunchOwnerState::Ready;
    return PrepareResult::Success(m_summary);
}

Foundation::Result<Ia64InitialUserImageLaunchSummary, AccessError>
Ia64InitialUserImageLaunchOwner::GetSummary() const noexcept
{
    using ResultType = Foundation::Result<Ia64InitialUserImageLaunchSummary, AccessError>;
    if (m_state != Ia64InitialUserImageLaunchOwnerState::Ready)
    {
        return ResultType::Failure(AccessError::NotReady);
    }
    return ResultType::Success(m_summary);
}

Foundation::Result<Ia64UserModeLaunchRequest, AccessError>
Ia64InitialUserImageLaunchOwner::GetLaunchRequest() const noexcept
{
    using ResultType = Foundation::Result<Ia64UserModeLaunchRequest, AccessError>;
    if (m_state != Ia64InitialUserImageLaunchOwnerState::Ready)
    {
        return ResultType::Failure(AccessError::NotReady);
    }
    return ResultType::Success(m_launchRequest);
}

Foundation::Result<Failure, AccessError> Ia64InitialUserImageLaunchOwner::GetFailure() const noexcept
{
    using ResultType = Foundation::Result<Failure, AccessError>;
    if (!m_hasFailure)
    {
        return ResultType::Failure(AccessError::NoFailure);
    }
    return ResultType::Success(m_failure);
}
} // namespace Asteria::Platform::Ia64::Scheduler
