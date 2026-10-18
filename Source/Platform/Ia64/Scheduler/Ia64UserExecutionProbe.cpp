// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Scheduler/Ia64UserExecutionProbe.hpp"

#include "Runtime/Memory.hpp"

namespace Asteria::Platform::Ia64::Scheduler
{
namespace
{
constexpr Foundation::Size MappingCount{3U};
constexpr Foundation::Size CodeIndex{0U};
constexpr Foundation::Size MemoryStackIndex{1U};
constexpr Foundation::Size RseBackingStoreIndex{2U};
constexpr Foundation::UInt64 PageSizeBytes{Kernel::Memory::PhysicalPageSizeBytes};
constexpr Foundation::UInt64 PageOffsetMask{PageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UIntPointer MaximumUIntPointer{~Foundation::UIntPointer{0U}};
constexpr Foundation::UInt64 MaximumEncodablePhysicalAddressExclusive{Foundation::UInt64{1U} << 50U};
constexpr Foundation::UInt64 FirmwareWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 FirmwareRuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt32 ConventionalFirmwareMemoryType{7U};

constexpr Foundation::UInt64 MappingPageCounts[MappingCount]{
    Ia64UserExecutionProbeCodePageCount,
    Ia64UserExecutionProbeMemoryStackPageCount,
    Ia64UserExecutionProbeRseBackingStorePageCount,
};

constexpr Foundation::UInt64 MappingVirtualStarts[MappingCount]{
    Ia64UserExecutionProbeCodeVirtualStart,
    Ia64UserExecutionProbeMemoryStackVirtualStart,
    Ia64UserExecutionProbeRseBackingStoreVirtualStart,
};

constexpr Ia64UserExecutionProbeMapping MappingKinds[MappingCount]{
    Ia64UserExecutionProbeMapping::Code,
    Ia64UserExecutionProbeMapping::MemoryStack,
    Ia64UserExecutionProbeMapping::RseBackingStore,
};

[[nodiscard]] constexpr Kernel::Memory::PagePermissions CodePermissions() noexcept
{
    return Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}
        .With(Kernel::Memory::PagePermission::Execute)
        .With(Kernel::Memory::PagePermission::User);
}

[[nodiscard]] constexpr Kernel::Memory::PagePermissions WritablePermissions() noexcept
{
    return Kernel::Memory::PagePermissions{Kernel::Memory::PagePermission::Read}
        .With(Kernel::Memory::PagePermission::Write)
        .With(Kernel::Memory::PagePermission::User);
}

[[nodiscard]] constexpr Kernel::Memory::PagePermissions PermissionsFor(const Foundation::Size index) noexcept
{
    return index == CodeIndex ? CodePermissions() : WritablePermissions();
}

[[nodiscard]] constexpr Kernel::Memory::FaultAccess FaultAccessFor(const Foundation::Size index) noexcept
{
    return index == CodeIndex ? Kernel::Memory::FaultAccess::Execute : Kernel::Memory::FaultAccess::Write;
}

[[nodiscard]] constexpr bool TryGetEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                       Foundation::UInt64& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
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

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool AllocationAttributesAreValid(const Kernel::Memory::PhysicalPageRange& allocation) noexcept
{
    return ((allocation.Attributes & FirmwareWriteBackAttribute) != 0U) &&
           ((allocation.Attributes & FirmwareRuntimeAttribute) == 0U) &&
           (allocation.FirmwareType == ConventionalFirmwareMemoryType) && (allocation.Reserved == 0U);
}

[[nodiscard]] constexpr bool AllocationGeometryIsValid(const Kernel::Memory::PhysicalPageRange& allocation,
                                                       const Foundation::UInt64 expectedPages) noexcept
{
    Foundation::UInt64 end{0U};
    return (allocation.PhysicalStart != 0U) && ((allocation.PhysicalStart & PageOffsetMask) == 0U) &&
           (allocation.PageCount == expectedPages) &&
           TryGetEnd(allocation.PhysicalStart, expectedPages * PageSizeBytes, end) &&
           (end <= MaximumEncodablePhysicalAddressExclusive);
}

[[nodiscard]] bool AliasGeometryIsValid(const Ia64UserExecutionProbeWritableAlias& alias,
                                        const Foundation::UInt64 expectedSizeBytes, Foundation::UIntPointer& start,
                                        Foundation::UIntPointer& end) noexcept
{
    if ((alias.Data == nullptr) || (alias.SizeBytes != expectedSizeBytes))
    {
        return false;
    }
    start = reinterpret_cast<Foundation::UIntPointer>(alias.Data);
    return ((start & PageOffsetMask) == 0U) && TryGetPointerEnd(start, alias.SizeBytes, end);
}

[[nodiscard]] constexpr Ia64UserExecutionProbeFailure
MakeFailure(const Ia64UserExecutionProbeError error,
            const Ia64UserExecutionProbeMapping mapping = Ia64UserExecutionProbeMapping::None) noexcept
{
    return Ia64UserExecutionProbeFailure{
        .Error = error,
        .Mapping = mapping,
        .RootBorrow = Memory::Ia64BspRuntimeRootBorrowError::None,
        .Frame = Kernel::Memory::FrameError::None,
        .MemoryObject = {},
        .Registry = Kernel::Object::ObjectRegistryError::Ok,
        .Pin = Kernel::Object::ObjectPinError::Ok,
        .FaultToken = Kernel::Memory::FaultTokenError::None,
        .FaultMapping = Kernel::Memory::FaultFrameMappingError::None,
        .AddressSpace = {},
        .Shootdown = {},
        .Launch = Ia64UserModeLaunchError::None,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr Ia64UserModeVirtualRange MappingRange(const Foundation::Size index) noexcept
{
    return Ia64UserModeVirtualRange{
        .VirtualStart = MappingVirtualStarts[index],
        .SizeBytes = MappingPageCounts[index] * PageSizeBytes,
    };
}

[[nodiscard]] constexpr Ia64UserModeVirtualRange GuardedRange(const Foundation::Size index) noexcept
{
    const Ia64UserModeVirtualRange mapping = MappingRange(index);
    return Ia64UserModeVirtualRange{
        .VirtualStart = mapping.VirtualStart - PageSizeBytes,
        .SizeBytes = mapping.SizeBytes + (2U * PageSizeBytes),
    };
}

void PublishInstructionBytes(const Ia64UserExecutionProbeWritableAlias& alias) noexcept
{
#if defined(__ia64__)
    for (Foundation::Size offset{0U}; offset < alias.SizeBytes; offset += Ia64UserModeEntryBundleSizeBytes)
    {
        __asm__ volatile("fc.i %0" : : "r"(alias.Data + offset) : "memory");
    }
    __asm__ volatile(";; sync.i\n;; srlz.i\n;;" ::: "memory");
#else
    static_cast<void>(alias);
#endif
}

class RejectingRemoteShootdownTransport final : public Hal::Memory::ITlbShootdownTransport
{
public:
    [[nodiscard]] Hal::Memory::TlbShootdownTransportError
    Publish(const Kernel::Memory::TlbShootdownBatch&) noexcept override
    {
        return Hal::Memory::TlbShootdownTransportError::Unavailable;
    }
};

[[nodiscard]] bool MappingRecordMatches(const Kernel::Memory::AddressSpaceMappingRecord& record,
                                        const Foundation::Size index,
                                        const Kernel::Memory::PhysicalPageRange& allocation,
                                        const Kernel::Memory::MemoryObjectId memoryObject,
                                        const Kernel::Memory::FrameId frame) noexcept
{
    const Kernel::Memory::PagePermissions permissions = PermissionsFor(index);
    return record.MemoryObject.IsEqualTo(memoryObject) && !record.DeviceRegion.IsValid() &&
           record.Frame.IsEqualTo(frame) && (record.VirtualStart.GetValue() == MappingVirtualStarts[index]) &&
           (record.PhysicalStart.GetValue() == allocation.PhysicalStart) &&
           (record.Pages.GetValue() == MappingPageCounts[index]) && (record.MemoryObjectOffsetPages == 0U) &&
           (record.Permissions.GetBits() == permissions.GetBits()) &&
           (record.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           (record.Kind == Kernel::Memory::FrameKind::Ram) &&
           (record.Commitment == Kernel::Memory::MemoryCommitment::Pinned) && record.RuntimeMapping &&
           (record.Reserved[0U] == 0U) && (record.Reserved[1U] == 0U) && (record.Reserved[2U] == 0U);
}
} // namespace

Foundation::Result<Ia64UserExecutionProbeStageSummary, Ia64UserExecutionProbeStageError>
Ia64UserExecutionProbe::StageStorage(const Ia64UserExecutionProbeStorage& storage,
                                     const Ia64UserExecutionProbeCodeView codeImage) noexcept
{
    using ResultType = Foundation::Result<Ia64UserExecutionProbeStageSummary, Ia64UserExecutionProbeStageError>;

    if (m_state != Ia64UserExecutionProbeState::Fresh)
    {
        return ResultType::Failure(Ia64UserExecutionProbeStageError::AlreadyStaged);
    }

    const Ia64UserExecutionProbeStorageRegion* const regions[MappingCount]{
        &storage.Code,
        &storage.MemoryStack,
        &storage.RseBackingStore,
    };
    constexpr Ia64UserExecutionProbeStageError allocationErrors[MappingCount]{
        Ia64UserExecutionProbeStageError::InvalidCodeAllocation,
        Ia64UserExecutionProbeStageError::InvalidMemoryStackAllocation,
        Ia64UserExecutionProbeStageError::InvalidRseBackingStoreAllocation,
    };
    constexpr Ia64UserExecutionProbeStageError aliasErrors[MappingCount]{
        Ia64UserExecutionProbeStageError::InvalidCodeAlias,
        Ia64UserExecutionProbeStageError::InvalidMemoryStackAlias,
        Ia64UserExecutionProbeStageError::InvalidRseBackingStoreAlias,
    };
    Foundation::UInt64 physicalEnds[MappingCount]{};
    Foundation::UIntPointer aliasStarts[MappingCount]{};
    Foundation::UIntPointer aliasEnds[MappingCount]{};
    for (Foundation::Size index{0U}; index < MappingCount; ++index)
    {
        const auto& allocation = regions[index]->Allocation;
        const Foundation::UInt64 sizeBytes = MappingPageCounts[index] * PageSizeBytes;
        if (!AllocationGeometryIsValid(allocation, MappingPageCounts[index]))
        {
            return ResultType::Failure(allocationErrors[index]);
        }
        if (!AllocationAttributesAreValid(allocation))
        {
            return ResultType::Failure(Ia64UserExecutionProbeStageError::InvalidAllocationAttributes);
        }
        static_cast<void>(TryGetEnd(allocation.PhysicalStart, sizeBytes, physicalEnds[index]));
        if (!AliasGeometryIsValid(regions[index]->WritableAlias, sizeBytes, aliasStarts[index], aliasEnds[index]))
        {
            return ResultType::Failure(aliasErrors[index]);
        }
        if (regions[index]->WritableAlias.PhysicalStart != allocation.PhysicalStart)
        {
            return ResultType::Failure(Ia64UserExecutionProbeStageError::AliasPhysicalAddressMismatch);
        }
#if defined(__ia64__)
        if (aliasStarts[index] != static_cast<Foundation::UIntPointer>(allocation.PhysicalStart))
        {
            return ResultType::Failure(Ia64UserExecutionProbeStageError::AliasPhysicalAddressMismatch);
        }
#endif
    }

    for (Foundation::Size index{0U}; index < MappingCount; ++index)
    {
        for (Foundation::Size prior{0U}; prior < index; ++prior)
        {
            if (RangesOverlap(regions[index]->Allocation.PhysicalStart, physicalEnds[index],
                              regions[prior]->Allocation.PhysicalStart, physicalEnds[prior]))
            {
                return ResultType::Failure(Ia64UserExecutionProbeStageError::PhysicalAllocationsOverlap);
            }
            if (RangesOverlap(aliasStarts[index], aliasEnds[index], aliasStarts[prior], aliasEnds[prior]))
            {
                return ResultType::Failure(Ia64UserExecutionProbeStageError::WritableAliasesOverlap);
            }
        }
    }

    if ((codeImage.Data == nullptr) || (codeImage.SizeBytes < Ia64UserModeEntryBundleSizeBytes) ||
        (codeImage.SizeBytes > Ia64UserExecutionProbeCodeSizeBytes) ||
        ((codeImage.SizeBytes % Ia64UserModeEntryBundleSizeBytes) != 0U))
    {
        return ResultType::Failure(Ia64UserExecutionProbeStageError::InvalidCodeImage);
    }
    const Foundation::UIntPointer codeStart = reinterpret_cast<Foundation::UIntPointer>(codeImage.Data);
    Foundation::UIntPointer codeEnd{0U};
    if (!TryGetPointerEnd(codeStart, codeImage.SizeBytes, codeEnd))
    {
        return ResultType::Failure(Ia64UserExecutionProbeStageError::InvalidCodeImage);
    }

    const Foundation::UIntPointer ownerStart = reinterpret_cast<Foundation::UIntPointer>(this);
    Foundation::UIntPointer ownerEnd{0U};
    const Foundation::UIntPointer storageStart = reinterpret_cast<Foundation::UIntPointer>(&storage);
    Foundation::UIntPointer storageEnd{0U};
    const Foundation::UIntPointer codeViewStart = reinterpret_cast<Foundation::UIntPointer>(&codeImage);
    Foundation::UIntPointer codeViewEnd{0U};
    if (!TryGetPointerEnd(ownerStart, sizeof(*this), ownerEnd) ||
        !TryGetPointerEnd(storageStart, sizeof(storage), storageEnd) ||
        !TryGetPointerEnd(codeViewStart, sizeof(codeImage), codeViewEnd))
    {
        return ResultType::Failure(Ia64UserExecutionProbeStageError::WritableAliasAliasesControlStorage);
    }
    for (Foundation::Size index{0U}; index < MappingCount; ++index)
    {
        if (RangesOverlap(aliasStarts[index], aliasEnds[index], codeStart, codeEnd))
        {
            return ResultType::Failure(Ia64UserExecutionProbeStageError::CodeImageAliasesWritableStorage);
        }
        if (RangesOverlap(aliasStarts[index], aliasEnds[index], ownerStart, ownerEnd) ||
            RangesOverlap(aliasStarts[index], aliasEnds[index], storageStart, storageEnd) ||
            RangesOverlap(aliasStarts[index], aliasEnds[index], codeViewStart, codeViewEnd))
        {
            return ResultType::Failure(Ia64UserExecutionProbeStageError::WritableAliasAliasesControlStorage);
        }
    }

    for (Foundation::Size index{0U}; index < MappingCount; ++index)
    {
        static_cast<void>(memset(regions[index]->WritableAlias.Data, 0, regions[index]->WritableAlias.SizeBytes));
    }
    static_cast<void>(memcpy(storage.Code.WritableAlias.Data, codeImage.Data, codeImage.SizeBytes));
    PublishInstructionBytes(storage.Code.WritableAlias);
#if defined(__ia64__)
    __asm__ volatile("mf" ::: "memory");
#endif

    for (Foundation::Size index{0U}; index < MappingCount; ++index)
    {
        m_allocations[index] = regions[index]->Allocation;
    }
    m_codeImageSizeBytes = codeImage.SizeBytes;
    m_state = Ia64UserExecutionProbeState::Staged;
    return ResultType::Success(Ia64UserExecutionProbeStageSummary{
        .Code = m_allocations[CodeIndex],
        .MemoryStack = m_allocations[MemoryStackIndex],
        .RseBackingStore = m_allocations[RseBackingStoreIndex],
        .CodeImageSizeBytes = m_codeImageSizeBytes,
    });
}

Ia64UserExecutionProbeFailure
Ia64UserExecutionProbe::PrepareMappingObjects(Kernel::Object::ObjectRegistry& registry,
                                              Kernel::Object::RootCapabilityIssuer& issuer,
                                              const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
                                              Kernel::Resource::ResourceDomain& resourceDomain) noexcept
{
    const Kernel::Object::ResourceDomainId chargeOwner = resourceDomain.GetIdentifier();
    for (Foundation::Size index{0U}; index < MappingCount; ++index)
    {
        const Kernel::Memory::FrameDescriptor frameDescriptor{
            .PhysicalStart = Kernel::Memory::PhysicalAddress{m_allocations[index].PhysicalStart},
            .Pages = Kernel::Memory::PageCount{MappingPageCounts[index]},
            .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
            .Kind = Kernel::Memory::FrameKind::Ram,
            .Reserved = {},
        };
        const Kernel::Memory::FrameError frameError = m_frames[index].Initialize(chargeOwner, frameDescriptor);
        if (frameError != Kernel::Memory::FrameError::None)
        {
            Ia64UserExecutionProbeFailure failure =
                MakeFailure(Ia64UserExecutionProbeError::FrameInitializationRejected, MappingKinds[index]);
            failure.Frame = frameError;
            return failure;
        }

        const Kernel::Memory::MemoryObjectDescriptor objectDescriptor{
            .Pages = Kernel::Memory::PageCount{MappingPageCounts[index]},
            .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
            .MaximumPermissions = PermissionsFor(index),
            .Commitment = Kernel::Memory::MemoryCommitment::Pinned,
            .Reserved = {},
        };
        const Kernel::Memory::MemoryObjectFailure objectFailure =
            m_memoryObjects[index].Initialize(registry, resourceDomainIdentity, resourceDomain, objectDescriptor);
        if (objectFailure.Error != Kernel::Memory::MemoryObjectError::None)
        {
            Ia64UserExecutionProbeFailure failure =
                MakeFailure(Ia64UserExecutionProbeError::MemoryObjectInitializationRejected, MappingKinds[index]);
            failure.MemoryObject = objectFailure;
            return failure;
        }
    }

    for (Foundation::Size index{0U}; index < MappingCount; ++index)
    {
        const auto frameObject = issuer.CreateObject(m_frames[index]);
        if (!frameObject.HasValue())
        {
            Ia64UserExecutionProbeFailure failure =
                MakeFailure(Ia64UserExecutionProbeError::ObjectRegistrationRejected, MappingKinds[index]);
            failure.Registry = frameObject.GetError();
            return failure;
        }
        Kernel::Object::ObjectPin verification;
        const Kernel::Object::ObjectPinError pinError =
            registry.PinObject(frameObject.GetValue(), Kernel::Object::ObjectType::Frame, verification);
        if ((pinError != Kernel::Object::ObjectPinError::Ok) || (&verification.GetObject() != &m_frames[index]))
        {
            Ia64UserExecutionProbeFailure failure =
                MakeFailure(Ia64UserExecutionProbeError::InvalidRegistryBinding, MappingKinds[index]);
            failure.Pin = pinError;
            return failure;
        }
        m_frameIdentities[index] = Kernel::Memory::FrameId{registry, frameObject.GetValue()};
        verification.Reset();

        const auto memoryObject = issuer.CreateObject(m_memoryObjects[index]);
        if (!memoryObject.HasValue())
        {
            Ia64UserExecutionProbeFailure failure =
                MakeFailure(Ia64UserExecutionProbeError::ObjectRegistrationRejected, MappingKinds[index]);
            failure.Registry = memoryObject.GetError();
            return failure;
        }
        const Kernel::Object::ObjectPinError memoryPinError =
            registry.PinObject(memoryObject.GetValue(), Kernel::Object::ObjectType::MemoryObject, verification);
        if ((memoryPinError != Kernel::Object::ObjectPinError::Ok) ||
            (&verification.GetObject() != &m_memoryObjects[index]))
        {
            Ia64UserExecutionProbeFailure failure =
                MakeFailure(Ia64UserExecutionProbeError::InvalidRegistryBinding, MappingKinds[index]);
            failure.Pin = memoryPinError;
            return failure;
        }
        m_memoryObjectIdentities[index] = Kernel::Memory::MemoryObjectId{registry, memoryObject.GetValue()};
        verification.Reset();
    }
    return MakeFailure(Ia64UserExecutionProbeError::None);
}

Ia64UserExecutionProbeFailure
Ia64UserExecutionProbe::InstallMapping(const Foundation::Size index, Kernel::Object::ObjectRegistry& registry,
                                       const Kernel::Scheduler::ThreadId mappingThread,
                                       const Memory::Ia64BspRuntimeRootView root,
                                       Kernel::Memory::TlbShootdownDispatcher& shootdownDispatcher) noexcept
{
    Ia64UserExecutionProbeFailure failure = PrepareFaultMapping(index, registry, mappingThread, root);
    if (failure.Error != Ia64UserExecutionProbeError::None)
    {
        return failure;
    }
    failure = BeginFaultMapping(index, root);
    if (failure.Error != Ia64UserExecutionProbeError::None)
    {
        return failure;
    }
    failure = DispatchShootdown(index, root, shootdownDispatcher);
    if (failure.Error != Ia64UserExecutionProbeError::None)
    {
        return failure;
    }
    return CompleteFaultMapping(index, root);
}

Ia64UserExecutionProbeFailure
Ia64UserExecutionProbe::PrepareFaultMapping(const Foundation::Size index, Kernel::Object::ObjectRegistry& registry,
                                            const Kernel::Scheduler::ThreadId mappingThread,
                                            const Memory::Ia64BspRuntimeRootView root) noexcept
{
    const Kernel::Memory::FaultRecord fault{
        .VirtualAddress = MappingVirtualStarts[index],
        .InstructionPointer = Ia64UserExecutionProbeCodeVirtualStart,
        .ArchitectureSyndrome = 0U,
        .Access = FaultAccessFor(index),
        .Cause = Kernel::Memory::FaultCause::MissingMapping,
        .Privilege = Kernel::Memory::FaultPrivilege::User,
        .Reserved = {},
    };
    const Kernel::Memory::FaultTokenError tokenError = m_faultTokens[index].Initialize(
        registry, m_memoryObjects[index].GetChargeOwner(), mappingThread, root.AddressSpaceIdentity, fault);
    if (tokenError != Kernel::Memory::FaultTokenError::None)
    {
        Ia64UserExecutionProbeFailure failure =
            MakeFailure(Ia64UserExecutionProbeError::FaultTokenRejected, MappingKinds[index]);
        failure.FaultToken = tokenError;
        return failure;
    }

    const auto claim = m_faultTokens[index].ClaimResolution();
    if (!claim.HasValue())
    {
        Ia64UserExecutionProbeFailure failure =
            MakeFailure(Ia64UserExecutionProbeError::FaultClaimRejected, MappingKinds[index]);
        failure.FaultToken = claim.GetError();
        return failure;
    }
    const Kernel::Memory::FaultResolutionClaimToken claimToken = claim.GetValue().Token;
    m_faultClaims[index] = claimToken;
    const Kernel::Memory::FaultFrameMappingRequest request{
        .VirtualStart = Kernel::Memory::VirtualAddress{MappingVirtualStarts[index]},
        .Pages = Kernel::Memory::PageCount{MappingPageCounts[index]},
        .MemoryObjectOffsetPages = 0U,
        .FrameOffsetPages = 0U,
        .Permissions = PermissionsFor(index),
        .Reserved = {},
    };
    const Kernel::Memory::FaultFrameMappingError preparationError = m_mappingPreparations[index].Prepare(
        registry, m_faultTokens[index], claimToken, m_memoryObjectIdentities[index], m_memoryObjects[index],
        Kernel::Device::DeviceRegionId{}, nullptr, m_frameIdentities[index], m_frames[index], request);
    if (preparationError != Kernel::Memory::FaultFrameMappingError::None)
    {
        Ia64UserExecutionProbeFailure failure =
            MakeFailure(Ia64UserExecutionProbeError::FaultMappingPreparationRejected, MappingKinds[index]);
        failure.FaultMapping = preparationError;
        return failure;
    }

    return MakeFailure(Ia64UserExecutionProbeError::None);
}

Ia64UserExecutionProbeFailure
Ia64UserExecutionProbe::BeginFaultMapping(const Foundation::Size index,
                                          const Memory::Ia64BspRuntimeRootView root) noexcept
{
    const auto begin = root.AddressSpaceStorage->BeginFaultMapping(m_mappingPreparations[index]);
    if (!begin.HasValue())
    {
        Ia64UserExecutionProbeFailure failure =
            MakeFailure(Ia64UserExecutionProbeError::AddressSpaceMappingRejected, MappingKinds[index]);
        failure.AddressSpace = begin.GetError();
        return failure;
    }
    m_shootdownRequests[index] = begin.GetValue();
    return MakeFailure(Ia64UserExecutionProbeError::None);
}

Ia64UserExecutionProbeFailure
Ia64UserExecutionProbe::DispatchShootdown(const Foundation::Size index, const Memory::Ia64BspRuntimeRootView root,
                                          Kernel::Memory::TlbShootdownDispatcher& shootdownDispatcher) noexcept
{
    RejectingRemoteShootdownTransport transport;
    const auto dispatched =
        shootdownDispatcher.Dispatch(*root.AddressSpaceStorage, m_shootdownRequests[index], transport);
    if (!dispatched.HasValue())
    {
        Ia64UserExecutionProbeFailure failure =
            MakeFailure(Ia64UserExecutionProbeError::ShootdownRejected, MappingKinds[index]);
        failure.Shootdown = dispatched.GetError();
        return failure;
    }
    if ((dispatched.GetValue().QueuedProcessorMask != 0U) ||
        (dispatched.GetValue().AcknowledgedProcessorMask !=
         static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << root.Processor.GetValue())) ||
        dispatched.GetValue().RejectionObserved)
    {
        return MakeFailure(Ia64UserExecutionProbeError::ShootdownRejected, MappingKinds[index]);
    }

    return MakeFailure(Ia64UserExecutionProbeError::None);
}

Ia64UserExecutionProbeFailure
Ia64UserExecutionProbe::CompleteFaultMapping(const Foundation::Size index,
                                             const Memory::Ia64BspRuntimeRootView root) noexcept
{
    const auto completed = root.AddressSpaceStorage->CompleteFaultMapping(m_shootdownRequests[index].GetToken());
    if (!completed.HasValue())
    {
        Ia64UserExecutionProbeFailure failure =
            MakeFailure(Ia64UserExecutionProbeError::MappingCompletionRejected, MappingKinds[index]);
        failure.AddressSpace = completed.GetError();
        return failure;
    }
    m_mappingReceipts[index] = completed.GetValue();

    const Kernel::Memory::FaultTokenError commitError =
        m_faultTokens[index].CommitResolution(m_faultClaims[index], Kernel::Memory::FaultResolutionKind::Map);
    if (commitError != Kernel::Memory::FaultTokenError::None)
    {
        Ia64UserExecutionProbeFailure failure =
            MakeFailure(Ia64UserExecutionProbeError::FaultCommitRejected, MappingKinds[index]);
        failure.FaultToken = commitError;
        return failure;
    }
    return MakeFailure(Ia64UserExecutionProbeError::None);
}

Ia64UserExecutionProbeFailure Ia64UserExecutionProbe::BorrowAndValidateRoot(
    Memory::Ia64BspRuntimeMemory& runtimeMemory, Kernel::Object::ObjectRegistry& registry,
    const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain, const Kernel::Scheduler::ThreadId mappingThread,
    Kernel::Memory::TlbShootdownDispatcher& shootdownDispatcher, Memory::Ia64BspRuntimeRootView& root) noexcept
{
    const auto borrowed = runtimeMemory.BorrowReadyInitialComponentRoot();
    if (!borrowed.HasValue())
    {
        Ia64UserExecutionProbeFailure failure = MakeFailure(Ia64UserExecutionProbeError::RootBorrowRejected);
        failure.RootBorrow = borrowed.GetError();
        return failure;
    }
    root = borrowed.GetValue();
    if ((root.AddressSpaceStorage == nullptr) || !root.AddressSpaceIdentity.IsValid() ||
        !root.AddressSpaceIdentity.IsFromRegistry(registry) || !root.Processor.IsValid())
    {
        return MakeFailure(Ia64UserExecutionProbeError::InvalidRoot);
    }
    if (!shootdownDispatcher.GetProcessor().IsValid() ||
        (shootdownDispatcher.GetProcessor().GetValue() != root.Processor.GetValue()))
    {
        return MakeFailure(Ia64UserExecutionProbeError::InvalidDispatcher);
    }
    if (!resourceDomainIdentity.IsValid() || !resourceDomainIdentity.IsFromRegistry(registry) ||
        !mappingThread.IsValid() || !mappingThread.IsFromRegistry(registry))
    {
        return MakeFailure(!resourceDomainIdentity.IsValid() || !resourceDomainIdentity.IsFromRegistry(registry)
                               ? Ia64UserExecutionProbeError::InvalidResourceDomain
                               : Ia64UserExecutionProbeError::InvalidMappingThread);
    }

    Kernel::Object::ObjectPin verification;
    Kernel::Object::ObjectPinError pinError = registry.PinObject(
        root.AddressSpaceIdentity.GetObjectId(), Kernel::Object::ObjectType::AddressSpace, verification);
    if ((pinError != Kernel::Object::ObjectPinError::Ok) || (&verification.GetObject() != root.AddressSpaceStorage))
    {
        Ia64UserExecutionProbeFailure failure = MakeFailure(Ia64UserExecutionProbeError::InvalidRegistryBinding);
        failure.Pin = pinError;
        return failure;
    }
    verification.Reset();
    pinError = registry.PinObject(resourceDomainIdentity.GetObjectId(), Kernel::Object::ObjectType::ResourceDomain,
                                  verification);
    if ((pinError != Kernel::Object::ObjectPinError::Ok) || (&verification.GetObject() != &resourceDomain) ||
        !resourceDomain.IsReady())
    {
        Ia64UserExecutionProbeFailure failure = MakeFailure(Ia64UserExecutionProbeError::InvalidResourceDomain);
        failure.Pin = pinError;
        return failure;
    }
    verification.Reset();
    pinError = registry.PinObject(mappingThread.GetObjectId(), Kernel::Object::ObjectType::Thread, verification);
    if (pinError != Kernel::Object::ObjectPinError::Ok)
    {
        Ia64UserExecutionProbeFailure failure = MakeFailure(Ia64UserExecutionProbeError::InvalidMappingThread);
        failure.Pin = pinError;
        return failure;
    }
    verification.Reset();
    return MakeFailure(Ia64UserExecutionProbeError::None);
}

Ia64UserExecutionProbeFailure
Ia64UserExecutionProbe::ValidateMappingVacancy(const Memory::Ia64BspRuntimeRootView root,
                                               Foundation::Size& initialMappingCount) const noexcept
{
    const Kernel::Memory::AddressSpaceSnapshot rootSnapshot = root.AddressSpaceStorage->GetSnapshot();
    const Foundation::UInt8 rootProcessorBit =
        static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << root.Processor.GetValue());
    if ((rootSnapshot.State != Kernel::Memory::AddressSpaceState::Ready) ||
        !rootSnapshot.Identity.IsEqualTo(root.AddressSpaceIdentity) ||
        (rootSnapshot.ResidentProcessorMask != rootProcessorBit) ||
        (rootSnapshot.MappingCount > (Kernel::Memory::MaximumAddressSpaceMappings - MappingCount)))
    {
        return MakeFailure(rootSnapshot.MappingCount > (Kernel::Memory::MaximumAddressSpaceMappings - MappingCount)
                               ? Ia64UserExecutionProbeError::MappingCapacityExceeded
                               : Ia64UserExecutionProbeError::InvalidRoot);
    }

    for (Foundation::Size existingIndex{0U}; existingIndex < rootSnapshot.MappingCount; ++existingIndex)
    {
        const auto existing = root.AddressSpaceStorage->TryGetMapping(existingIndex);
        if (!existing.HasValue())
        {
            return MakeFailure(Ia64UserExecutionProbeError::InvalidRoot);
        }
        const Kernel::Memory::AddressSpaceMappingRecord& record = existing.GetValue();
        Foundation::UInt64 existingVirtualEnd{0U};
        Foundation::UInt64 existingPhysicalEnd{0U};
        const Foundation::UInt64 existingSize = record.Pages.GetValue() * PageSizeBytes;
        if (!TryGetEnd(record.VirtualStart.GetValue(), existingSize, existingVirtualEnd) ||
            !TryGetEnd(record.PhysicalStart.GetValue(), existingSize, existingPhysicalEnd))
        {
            return MakeFailure(Ia64UserExecutionProbeError::InvalidRoot);
        }
        for (Foundation::Size index{0U}; index < MappingCount; ++index)
        {
            const Ia64UserModeVirtualRange guarded = GuardedRange(index);
            const Foundation::UInt64 guardedEnd = guarded.VirtualStart + guarded.SizeBytes;
            if (RangesOverlap(record.VirtualStart.GetValue(), existingVirtualEnd, guarded.VirtualStart, guardedEnd))
            {
                return MakeFailure(Ia64UserExecutionProbeError::GuardedVirtualRangeOccupied, MappingKinds[index]);
            }
            const Foundation::UInt64 allocationEnd =
                m_allocations[index].PhysicalStart + (MappingPageCounts[index] * PageSizeBytes);
            if (RangesOverlap(record.PhysicalStart.GetValue(), existingPhysicalEnd, m_allocations[index].PhysicalStart,
                              allocationEnd))
            {
                return MakeFailure(Ia64UserExecutionProbeError::PhysicalAllocationAlreadyOwned, MappingKinds[index]);
            }
        }
    }
    initialMappingCount = rootSnapshot.MappingCount;
    return MakeFailure(Ia64UserExecutionProbeError::None);
}

Ia64UserExecutionProbeFailure
Ia64UserExecutionProbe::BuildLaunchRecord(const Ia64UserModeVirtualRange supervisorMemoryStack,
                                          const Ia64UserModeVirtualRange supervisorRseBackingStore,
                                          const Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept
{
    m_supervisorContinuation.MemoryStack = supervisorMemoryStack;
    m_supervisorContinuation.RseBackingStore = supervisorRseBackingStore;
    m_launchRequest = Ia64UserModeLaunchRequest{
        .Code = MappingRange(CodeIndex),
        .MemoryStack = MappingRange(MemoryStackIndex),
        .RseBackingStore = MappingRange(RseBackingStoreIndex),
        .EntryBundleAddress = Ia64UserExecutionProbeCodeVirtualStart,
        .InitialGlobalPointer = 0U,
        .InitialArguments = {},
        .Requirements =
            Ia64UserModeLaunchRequirements{
                .CurrentProcessorStatusMask = Ia64UserModeCurrentProcessorStatusMask,
                .CurrentProcessorStatusValue = Ia64UserModeCurrentProcessorStatusValue,
                .UserProcessorStatus = Ia64UserModeInitialProcessorStatus,
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
    const auto launchValidation = Ia64UserModeLaunch::Validate(m_launchRequest);
    if (!launchValidation.HasValue())
    {
        Ia64UserExecutionProbeFailure failure = MakeFailure(Ia64UserExecutionProbeError::LaunchRecordRejected);
        failure.Launch = launchValidation.GetError();
        return failure;
    }
    return MakeFailure(Ia64UserExecutionProbeError::None);
}

Ia64UserExecutionProbeFailure
Ia64UserExecutionProbe::ValidateMappingPostcondition(const Foundation::Size index,
                                                     const Memory::Ia64BspRuntimeRootView root,
                                                     const Foundation::Size initialMappingCount) const noexcept
{
    if ((m_mappingReceipts[index].MappingCount != (initialMappingCount + index + 1U)) ||
        (m_mappingReceipts[index].VirtualStart.GetValue() != MappingVirtualStarts[index]) ||
        (m_mappingReceipts[index].Pages.GetValue() != MappingPageCounts[index]))
    {
        return MakeFailure(Ia64UserExecutionProbeError::PostconditionRejected, MappingKinds[index]);
    }
    const auto record = root.AddressSpaceStorage->TryGetMapping(initialMappingCount + index);
    if (!record.HasValue() ||
        !MappingRecordMatches(record.GetValue(), index, m_allocations[index], m_memoryObjectIdentities[index],
                              m_frameIdentities[index]) ||
        (m_frames[index].GetSnapshot().State != Kernel::Memory::FrameState::Ready) ||
        (m_memoryObjects[index].GetSnapshot().State != Kernel::Memory::MemoryObjectState::Ready) ||
        (m_memoryObjects[index].GetSnapshot().ActiveMappings != 1U))
    {
        return MakeFailure(Ia64UserExecutionProbeError::PostconditionRejected, MappingKinds[index]);
    }
    return MakeFailure(Ia64UserExecutionProbeError::None);
}

Ia64UserExecutionProbeFailure
Ia64UserExecutionProbe::ValidatePostconditions(const Memory::Ia64BspRuntimeRootView root,
                                               const Foundation::Size initialMappingCount) const noexcept
{
    const Kernel::Memory::AddressSpaceSnapshot completedSnapshot = root.AddressSpaceStorage->GetSnapshot();
    const Foundation::UInt8 rootProcessorBit =
        static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << root.Processor.GetValue());
    if ((completedSnapshot.State != Kernel::Memory::AddressSpaceState::Ready) ||
        (completedSnapshot.ResidentProcessorMask != rootProcessorBit) ||
        (completedSnapshot.MappingCount != (initialMappingCount + MappingCount)))
    {
        return MakeFailure(Ia64UserExecutionProbeError::PostconditionRejected);
    }
    for (Foundation::Size index{0U}; index < MappingCount; ++index)
    {
        const Ia64UserExecutionProbeFailure mapping = ValidateMappingPostcondition(index, root, initialMappingCount);
        if (mapping.Error != Ia64UserExecutionProbeError::None)
        {
            return mapping;
        }
    }
    const auto finalLaunchValidation = Ia64UserModeLaunch::Validate(m_launchRequest);
    if (!finalLaunchValidation.HasValue())
    {
        Ia64UserExecutionProbeFailure failure = MakeFailure(Ia64UserExecutionProbeError::PostconditionRejected);
        failure.Launch = finalLaunchValidation.GetError();
        return failure;
    }
    return MakeFailure(Ia64UserExecutionProbeError::None);
}

Foundation::Result<Ia64UserModeLaunchRequest, Ia64UserExecutionProbeFailure> Ia64UserExecutionProbe::PrepareLaunch(
    Memory::Ia64BspRuntimeMemory& runtimeMemory, Kernel::Object::ObjectRegistry& registry,
    Kernel::Object::RootCapabilityIssuer& issuer, const Kernel::Resource::ResourceDomainObjectId resourceDomainIdentity,
    Kernel::Resource::ResourceDomain& resourceDomain, const Kernel::Scheduler::ThreadId mappingThread,
    Kernel::Memory::TlbShootdownDispatcher& shootdownDispatcher, const Ia64UserModeVirtualRange supervisorMemoryStack,
    const Ia64UserModeVirtualRange supervisorRseBackingStore,
    const Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept
{
    using ResultType = Foundation::Result<Ia64UserModeLaunchRequest, Ia64UserExecutionProbeFailure>;

    if (m_state == Ia64UserExecutionProbeState::Fresh)
    {
        return ResultType::Failure(MakeFailure(Ia64UserExecutionProbeError::NotStaged));
    }
    if (m_state != Ia64UserExecutionProbeState::Staged)
    {
        return ResultType::Failure(MakeFailure(Ia64UserExecutionProbeError::AlreadyPrepared));
    }

    Memory::Ia64BspRuntimeRootView root{};
    Ia64UserExecutionProbeFailure failure = BorrowAndValidateRoot(
        runtimeMemory, registry, resourceDomainIdentity, resourceDomain, mappingThread, shootdownDispatcher, root);
    if (failure.Error != Ia64UserExecutionProbeError::None)
    {
        return ResultType::Failure(failure);
    }
    Foundation::Size initialMappingCount{0U};
    failure = ValidateMappingVacancy(root, initialMappingCount);
    if (failure.Error != Ia64UserExecutionProbeError::None)
    {
        return ResultType::Failure(failure);
    }
    failure = BuildLaunchRecord(supervisorMemoryStack, supervisorRseBackingStore,
                                implementedVirtualAddressMostSignificantBit);
    if (failure.Error != Ia64UserExecutionProbeError::None)
    {
        return ResultType::Failure(failure);
    }

    m_state = Ia64UserExecutionProbeState::Failed;
    failure = PrepareMappingObjects(registry, issuer, resourceDomainIdentity, resourceDomain);
    if (failure.Error != Ia64UserExecutionProbeError::None)
    {
        return ResultType::Failure(failure);
    }
    for (Foundation::Size index{0U}; index < MappingCount; ++index)
    {
        failure = InstallMapping(index, registry, mappingThread, root, shootdownDispatcher);
        if (failure.Error != Ia64UserExecutionProbeError::None)
        {
            return ResultType::Failure(failure);
        }
    }
    failure = ValidatePostconditions(root, initialMappingCount);
    if (failure.Error != Ia64UserExecutionProbeError::None)
    {
        return ResultType::Failure(failure);
    }

    m_state = Ia64UserExecutionProbeState::Ready;
    return ResultType::Success(m_launchRequest);
}

Foundation::Result<Ia64UserModeLaunchRequest, Ia64UserExecutionProbeError>
Ia64UserExecutionProbe::GetLaunchRequest() const noexcept
{
    using ResultType = Foundation::Result<Ia64UserModeLaunchRequest, Ia64UserExecutionProbeError>;
    if (m_state != Ia64UserExecutionProbeState::Ready)
    {
        return ResultType::Failure(Ia64UserExecutionProbeError::NotStaged);
    }
    return ResultType::Success(m_launchRequest);
}
} // namespace Asteria::Platform::Ia64::Scheduler
