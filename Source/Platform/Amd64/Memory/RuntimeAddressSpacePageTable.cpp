// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/RuntimeAddressSpacePageTable.hpp"

extern "C" Asteria::Foundation::UInt64
AsteriaAmd64LoadCr3AndReadBack(Asteria::Foundation::UInt64 rootPhysicalAddress) noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ReadCr4() noexcept;

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
constexpr Foundation::UInt64 PageOffsetMask{0xFFFU};
constexpr Foundation::UInt64 Cr4PageGlobalEnable{Foundation::UInt64{1U} << 7U};
constexpr Foundation::UInt64 Cr4LinearAddress57{Foundation::UInt64{1U} << 12U};
constexpr Foundation::UInt64 Cr4ProcessContextIdentifierEnable{Foundation::UInt64{1U} << 17U};
constexpr Foundation::UInt64 IncompatibleActivationCr4Mask{Cr4PageGlobalEnable | Cr4LinearAddress57 |
                                                           Cr4ProcessContextIdentifierEnable};

[[nodiscard]] constexpr bool CapabilitiesAreValid(const PagingCapabilities& capabilities) noexcept
{
    return (capabilities.PhysicalAddressBits >= 32U) && (capabilities.PhysicalAddressBits <= 52U) &&
           capabilities.FourLevelPagingEnabled && capabilities.ExecuteDisableEnabled &&
           capabilities.SupervisorWriteProtectEnabled && capabilities.GlobalPagesEnabled &&
           capabilities.PatLayoutConfigured && (capabilities.Reserved16 == 0U) && (capabilities.Reserved == 0U);
}

[[nodiscard]] bool StorageIsValid(const RuntimePageTableStorage& storage,
                                  const Foundation::UInt8 physicalAddressBits) noexcept
{
    if ((storage.Entries == nullptr) || (storage.PageCapacity == 0U) ||
        !IsRuntimePageTableCapacity(storage.PageCapacity) || (storage.UsedPageCount == 0U) ||
        (storage.UsedPageCount > storage.PageCapacity) || (storage.Reserved != 0U) ||
        (storage.PhysicalStart.GetValue() == 0U) || ((storage.PhysicalStart.GetValue() & PageOffsetMask) != 0U) ||
        ((reinterpret_cast<Foundation::UIntPointer>(storage.Entries) & PageOffsetMask) != 0U))
    {
        return false;
    }
    const Foundation::UInt64 physicalLimit = Foundation::UInt64{1U} << physicalAddressBits;
    const Foundation::UInt64 storageBytes = static_cast<Foundation::UInt64>(storage.PageCapacity) << 12U;
    return (storage.PhysicalStart.GetValue() < physicalLimit) &&
           (storageBytes <= (physicalLimit - storage.PhysicalStart.GetValue()));
}

[[nodiscard]] constexpr Hal::Memory::AddressSpacePageTableError MapError(const RuntimePageTableMapError error) noexcept
{
    switch (error)
    {
    case RuntimePageTableMapError::None:
        return Hal::Memory::AddressSpacePageTableError::None;

    case RuntimePageTableMapError::InvalidMappingPlan:
    case RuntimePageTableMapError::MappingTooLarge:
    case RuntimePageTableMapError::NonCanonicalVirtualAddress:
    case RuntimePageTableMapError::PhysicalAddressOutOfRange:
        return Hal::Memory::AddressSpacePageTableError::InvalidPlan;

    case RuntimePageTableMapError::MappingAliasesTableStorage:
    case RuntimePageTableMapError::MappingAlreadyPresent:
    case RuntimePageTableMapError::PhysicalWriteExecuteAlias:
        return Hal::Memory::AddressSpacePageTableError::MappingConflict;

    case RuntimePageTableMapError::InsufficientTableStorage:
        return Hal::Memory::AddressSpacePageTableError::CapacityExceeded;

    case RuntimePageTableMapError::InvalidStorage:
    case RuntimePageTableMapError::InvalidTableEntry:
    case RuntimePageTableMapError::MappingAbsent:
    case RuntimePageTableMapError::MappingMismatch:
        return Hal::Memory::AddressSpacePageTableError::CorruptState;

    case RuntimePageTableMapError::InvalidCapabilities:
    case RuntimePageTableMapError::FourLevelPagingRequired:
    case RuntimePageTableMapError::SupervisorWriteProtectRequired:
    case RuntimePageTableMapError::PageAttributeTableRequired:
    case RuntimePageTableMapError::ExecuteDisableRequired:
        return Hal::Memory::AddressSpacePageTableError::UnsupportedConfiguration;
    }
    return Hal::Memory::AddressSpacePageTableError::InternalFailure;
}
} // namespace

RuntimeAddressSpacePageTableInitializationError
RuntimeAddressSpacePageTable::Initialize(const PagingCapabilities& capabilities,
                                         InitialPageTablePreparation& preparation,
                                         volatile Foundation::UInt64* const writableEntries) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != RuntimeAddressSpacePageTableState::Uninitialized)
    {
        return RuntimeAddressSpacePageTableInitializationError::AlreadyInitialized;
    }
    if (!CapabilitiesAreValid(capabilities))
    {
        return RuntimeAddressSpacePageTableInitializationError::InvalidCapabilities;
    }
    const InitialPageTablePreparationError ownershipError = preparation.ClaimRuntimeOwnership();
    if (ownershipError != InitialPageTablePreparationError::None)
    {
        return (ownershipError == InitialPageTablePreparationError::AlreadyPrepared)
                   ? RuntimeAddressSpacePageTableInitializationError::StorageAlreadyOwned
                   : RuntimeAddressSpacePageTableInitializationError::VerificationRejected;
    }
    const Kernel::Memory::InitialMappingPlan& seedPlan = preparation.m_mappingPlan;
    const InitialPageTablePreparationSummary& summary = preparation.m_summary;
    const RuntimePageTableStorage storage{
        .PhysicalStart = summary.Storage.RootPhysicalAddress,
        .Entries = writableEntries,
        .PageCapacity = summary.Storage.PageCapacity,
        .UsedPageCount = summary.Storage.UsedTablePages,
        .Reserved = 0U,
    };
    m_capabilities = capabilities;
    m_storage = storage;
    m_pendingSeedPlan = nullptr;
    m_owner = nullptr;
    m_state = RuntimeAddressSpacePageTableState::Faulted;
    if ((writableEntries == nullptr) ||
        (reinterpret_cast<Foundation::UIntPointer>(writableEntries) !=
         reinterpret_cast<Foundation::UIntPointer>(summary.Storage.Entries)) ||
        !StorageIsValid(storage, capabilities.PhysicalAddressBits))
    {
        return RuntimeAddressSpacePageTableInitializationError::InvalidStorage;
    }
    if (InitialPageTableVerifier::Verify(seedPlan, capabilities, summary) != InitialPageTableVerificationError::None)
    {
        return RuntimeAddressSpacePageTableInitializationError::VerificationRejected;
    }
    m_pendingSeedPlan = &seedPlan;
    m_state = RuntimeAddressSpacePageTableState::Ready;
    return RuntimeAddressSpacePageTableInitializationError::None;
}

bool RuntimeAddressSpacePageTable::IsReady() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_state == RuntimeAddressSpacePageTableState::Ready;
}

Hal::Memory::AddressSpacePageTableOwnershipError
RuntimeAddressSpacePageTable::Claim(const Kernel::Memory::AddressSpace& owner,
                                    const Kernel::Memory::InitialMappingPlan& seedPlan) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == RuntimeAddressSpacePageTableState::Claimed)
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::AlreadyOwned;
    }
    if (m_state != RuntimeAddressSpacePageTableState::Ready)
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::NotReady;
    }
    if (m_pendingSeedPlan != &seedPlan)
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::SeedMismatch;
    }
    m_pendingSeedPlan = nullptr;
    m_owner = &owner;
    m_state = RuntimeAddressSpacePageTableState::Claimed;
    return Hal::Memory::AddressSpacePageTableOwnershipError::None;
}

Hal::Memory::AddressSpacePageTableOwnershipError
RuntimeAddressSpacePageTable::Release(const Kernel::Memory::AddressSpace& owner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (((m_state != RuntimeAddressSpacePageTableState::Claimed) &&
         (m_state != RuntimeAddressSpacePageTableState::Faulted)) ||
        (m_owner != &owner))
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::InvalidOwner;
    }
    m_owner = nullptr;
    m_state = RuntimeAddressSpacePageTableState::Retired;
    return Hal::Memory::AddressSpacePageTableOwnershipError::None;
}

Hal::Memory::AddressSpacePageTableActivationError
RuntimeAddressSpacePageTable::Activate(const Kernel::Memory::AddressSpace& owner,
                                       const Kernel::Scheduler::CpuId processor) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != RuntimeAddressSpacePageTableState::Claimed)
    {
        return Hal::Memory::AddressSpacePageTableActivationError::NotReady;
    }
    if (m_owner != &owner)
    {
        return Hal::Memory::AddressSpacePageTableActivationError::InvalidOwner;
    }
    if (!processor.IsValid())
    {
        return Hal::Memory::AddressSpacePageTableActivationError::RejectedBeforeCommit;
    }
    // The AMD64 target profile requires four-level paging without PCIDs or
    // global translations. Read the live mode immediately before the native
    // commit so this aligned value is unambiguously a raw PML4 address and a
    // CR3 write completes the required local invalidation.
    const Foundation::UInt64 requestedRoot = m_storage.PhysicalStart.GetValue();
    const Foundation::UInt64 liveCr4 = AsteriaAmd64ReadCr4();
    if ((liveCr4 & IncompatibleActivationCr4Mask) != 0U)
    {
        return Hal::Memory::AddressSpacePageTableActivationError::UnsupportedConfiguration;
    }
    const Foundation::UInt64 observedRoot = AsteriaAmd64LoadCr3AndReadBack(requestedRoot);
    if (observedRoot != requestedRoot)
    {
        // The write is the native commit point. A mismatched readback cannot
        // prove that the previous root remained active.
        m_state = RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableActivationError::CommittedStateUnknown;
    }
    return Hal::Memory::AddressSpacePageTableActivationError::None;
}

Hal::Memory::AddressSpacePageTableError
RuntimeAddressSpacePageTable::Map(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
                                  Hal::Memory::AddressSpacePageTableMapResult& output) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state == RuntimeAddressSpacePageTableState::Uninitialized) ||
        (m_state == RuntimeAddressSpacePageTableState::Ready) ||
        (m_state == RuntimeAddressSpacePageTableState::Retired))
    {
        return Hal::Memory::AddressSpacePageTableError::NotReady;
    }
    if (m_state == RuntimeAddressSpacePageTableState::Faulted)
    {
        return Hal::Memory::AddressSpacePageTableError::CorruptState;
    }

    RuntimePageTableMapResult nativeResult{};
    const RuntimePageTableMapError nativeError =
        RuntimePageTableMapper::Map(mapping, m_capabilities, m_storage, nativeResult);
    if (nativeError != RuntimePageTableMapError::None)
    {
        const Hal::Memory::AddressSpacePageTableError error = MapError(nativeError);
        if (error == Hal::Memory::AddressSpacePageTableError::CorruptState)
        {
            m_state = RuntimeAddressSpacePageTableState::Faulted;
        }
        return error;
    }
    if ((nativeResult.UsedTablePages < m_storage.UsedPageCount) ||
        (nativeResult.UsedTablePages > m_storage.PageCapacity) ||
        (nativeResult.MappedPages.GetValue() != mapping.Pages.GetValue()) ||
        (nativeResult.InvalidationStart.GetValue() != mapping.VirtualStart.GetValue()) || (nativeResult.Reserved != 0U))
    {
        m_state = RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
    }

    m_storage.UsedPageCount = nativeResult.UsedTablePages;
    output = Hal::Memory::AddressSpacePageTableMapResult{
        .InvalidationStart = nativeResult.InvalidationStart,
        .InvalidationPages = nativeResult.MappedPages,
        .Reserved = 0U,
    };
    return Hal::Memory::AddressSpacePageTableError::None;
}

Hal::Memory::AddressSpacePageTableError
RuntimeAddressSpacePageTable::Unmap(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
                                    Hal::Memory::AddressSpacePageTableMapResult& output) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state == RuntimeAddressSpacePageTableState::Uninitialized) ||
        (m_state == RuntimeAddressSpacePageTableState::Ready) ||
        (m_state == RuntimeAddressSpacePageTableState::Retired))
    {
        return Hal::Memory::AddressSpacePageTableError::NotReady;
    }
    if (m_state == RuntimeAddressSpacePageTableState::Faulted)
    {
        return Hal::Memory::AddressSpacePageTableError::CorruptState;
    }

    RuntimePageTableMapResult nativeResult{};
    const RuntimePageTableMapError nativeError =
        RuntimePageTableMapper::Unmap(mapping, m_capabilities, m_storage, nativeResult);
    if (nativeError != RuntimePageTableMapError::None)
    {
        const Hal::Memory::AddressSpacePageTableError error = MapError(nativeError);
        if (error == Hal::Memory::AddressSpacePageTableError::CorruptState)
        {
            m_state = RuntimeAddressSpacePageTableState::Faulted;
        }
        return error;
    }
    if ((nativeResult.UsedTablePages != m_storage.UsedPageCount) ||
        (nativeResult.MappedPages.GetValue() != mapping.Pages.GetValue()) ||
        (nativeResult.InvalidationStart.GetValue() != mapping.VirtualStart.GetValue()) || (nativeResult.Reserved != 0U))
    {
        m_state = RuntimeAddressSpacePageTableState::Faulted;
        return Hal::Memory::AddressSpacePageTableError::CommittedStateUnknown;
    }
    output = Hal::Memory::AddressSpacePageTableMapResult{
        .InvalidationStart = nativeResult.InvalidationStart,
        .InvalidationPages = nativeResult.MappedPages,
        .Reserved = 0U,
    };
    return Hal::Memory::AddressSpacePageTableError::None;
}

RuntimeAddressSpacePageTableSnapshot RuntimeAddressSpacePageTable::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return RuntimeAddressSpacePageTableSnapshot{
        .RootPhysicalAddress = m_storage.PhysicalStart,
        .PageCapacity = m_storage.PageCapacity,
        .UsedTablePages = m_storage.UsedPageCount,
        .State = m_state,
        .Reserved = {},
    };
}
} // namespace Asteria::Platform::Amd64::Memory
