// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_ADDRESS_SPACE_OWNER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_ADDRESS_SPACE_OWNER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Ia64/Memory/Ia64ActiveRuntimeTranslationBindings.hpp"
#include "Platform/Ia64/Memory/Ia64RegionIdentifierAllocator.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeAddressSpacePageTable.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeProcessorCapabilities.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class Ia64RuntimeAddressSpaceOwnerState : Foundation::UInt8
{
    Fresh,
    Failed,
    Ready,
};

enum class Ia64RuntimeAddressSpaceOwnerError : Foundation::UInt8
{
    None,
    AlreadyPrepared,
    PreviousPreparationFailed,
    InvalidConfiguration,
    MappingPlanRejected,
    RegionIdentifierAllocationRejected,
    RegionIdentifierReceiptRejected,
    BackendInitializationRejected,
    AddressSpaceInitializationRejected,
    ObjectRegistrationRejected,
    IdentityBindingRejected,
    PostconditionRejected,
    NotReady,
};

struct Ia64RuntimeAddressSpaceOwnerSummary
{
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
    Foundation::Size MappingCount;
    Foundation::UInt32 FirstRegionIdentifier;
    Foundation::UInt32 LastRegionIdentifier;
    Foundation::UInt32 AllocatedAddressSpaceCount;
    Ia64RuntimeAddressSpaceOwnerState State;
    bool BackendClaimed;
    Foundation::UInt8 Reserved[2U];
};

struct Ia64RuntimeAddressSpaceOwnerFailure
{
    Ia64RuntimeAddressSpaceOwnerError Error;
    Ia64RegionIdentifierAllocatorError RegionIdentifierAllocator;
    Ia64RuntimeAddressSpacePageTableInitializationError Backend;
    Kernel::Memory::AddressSpaceError AddressSpace;
    Kernel::Object::ObjectRegistryError Registry;
    Kernel::Memory::AddressSpaceFailure IdentityBinding;
    InitialAddressSpacePreparationError MappingPlan;
};

struct Ia64RuntimeAddressSpaceOwnerView
{
    Kernel::Memory::AddressSpace* AddressSpaceStorage;
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
};

struct Ia64RuntimeAddressSpaceOwnerSnapshot
{
    Ia64RuntimeAddressSpaceOwnerSummary Summary;
    Ia64RuntimeAddressSpaceOwnerFailure Failure;
    Kernel::Memory::AddressSpaceSnapshot AddressSpace;
    Ia64RegionIdentifierAllocatorSnapshot RegionIdentifierAllocator;
    const Kernel::Memory::AddressSpace* AddressSpaceStorage;
    Ia64RuntimeAddressSpaceOwnerState State;
    Foundation::UInt8 Reserved[7U];
};

/// Fixed boot-lifetime owner of one private IA-64 runtime AddressSpace.
///
/// The integration owner supplies the system's authoritative processor
/// profile, RID allocator, active-binding table, and canonical Region-0 seed.
/// This object never initializes an allocator, activates a processor, edits an
/// RR directly, or calls the page-table backend's Map operation. Once the
/// allocator issues a receipt, every later failure is terminal and retains
/// all possibly committed state for the boot lifetime.
class Ia64RuntimeAddressSpaceOwner final
{
public:
    constexpr Ia64RuntimeAddressSpaceOwner() noexcept = default;
    ~Ia64RuntimeAddressSpaceOwner() noexcept = default;

    Ia64RuntimeAddressSpaceOwner(const Ia64RuntimeAddressSpaceOwner&) = delete;
    Ia64RuntimeAddressSpaceOwner& operator=(const Ia64RuntimeAddressSpaceOwner&) = delete;
    Ia64RuntimeAddressSpaceOwner(Ia64RuntimeAddressSpaceOwner&&) = delete;
    Ia64RuntimeAddressSpaceOwner& operator=(Ia64RuntimeAddressSpaceOwner&&) = delete;

    [[nodiscard]] Foundation::Result<Ia64RuntimeAddressSpaceOwnerSummary, Ia64RuntimeAddressSpaceOwnerFailure>
    Prepare(const Ia64RuntimeProcessorCapabilities& processorCapabilities,
            Ia64RegionIdentifierAllocator& regionIdentifierAllocator, const InitialAddressSpacePreparation& preparation,
            Ia64ActiveRuntimeTranslationBindings& activeBindings, Kernel::Object::ResourceDomainId chargeOwner,
            Kernel::Object::ObjectRegistry& registry, Kernel::Object::RootCapabilityIssuer& issuer) noexcept;

    [[nodiscard]] Foundation::Result<Ia64RuntimeAddressSpaceOwnerView, Ia64RuntimeAddressSpaceOwnerError>
    BorrowReadyAddressSpace() noexcept;

    /// Names the translation-cache purge the owned backend runs after an
    /// unmap (Ia64RuntimeAddressSpacePageTable); once, after Prepare.
    [[nodiscard]] bool ConfigureTranslationCachePurge(Ia64LocalTranslationCachePurge& purge) noexcept;

    [[nodiscard]] Ia64RuntimeAddressSpaceOwnerSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] Ia64RuntimeAddressSpaceOwnerFailure Fail(Ia64RuntimeAddressSpaceOwnerError error) noexcept;
    [[nodiscard]] bool HasLiveRegistryBinding() const noexcept;
    [[nodiscard]] bool VerifyPostconditions(const Kernel::Memory::InitialMappingPlan& mappingPlan,
                                            const Ia64RegionIdentifierAllocatorSnapshot& allocatorBefore) noexcept;

    Ia64RegionIdentifierAllocationReceipt m_regionIdentifierReceipt{};
    Ia64RuntimeAddressSpacePageTable m_addressSpacePageTable;
    Kernel::Memory::AddressSpace m_addressSpace;
    Kernel::Object::ObjectPin m_addressSpacePin;
    Kernel::Object::ObjectRegistry* m_registry{nullptr};
    Ia64RegionIdentifierAllocator* m_regionIdentifierAllocator{nullptr};
    Ia64RuntimeAddressSpaceOwnerSummary m_summary{};
    Ia64RuntimeAddressSpaceOwnerFailure m_failure{};
    Ia64RuntimeAddressSpaceOwnerState m_state{Ia64RuntimeAddressSpaceOwnerState::Fresh};
};

static_assert(sizeof(Ia64RuntimeAddressSpaceOwnerState) == 1U);
static_assert(sizeof(Ia64RuntimeAddressSpaceOwnerError) == 1U);
static_assert(sizeof(Ia64RuntimeAddressSpaceOwnerSummary) == 40U);
static_assert(alignof(Ia64RuntimeAddressSpaceOwnerSummary) == 8U);
static_assert(__is_aggregate(Ia64RuntimeAddressSpaceOwnerSummary));
static_assert(__is_standard_layout(Ia64RuntimeAddressSpaceOwnerSummary));
static_assert(__is_trivially_copyable(Ia64RuntimeAddressSpaceOwnerSummary));
static_assert(sizeof(Ia64RuntimeAddressSpaceOwnerFailure) == 20U);
static_assert(alignof(Ia64RuntimeAddressSpaceOwnerFailure) == 4U);
static_assert(__is_aggregate(Ia64RuntimeAddressSpaceOwnerFailure));
static_assert(__is_standard_layout(Ia64RuntimeAddressSpaceOwnerFailure));
static_assert(__is_trivially_copyable(Ia64RuntimeAddressSpaceOwnerFailure));
static_assert(sizeof(Ia64RuntimeAddressSpaceOwnerView) == 24U);
static_assert(alignof(Ia64RuntimeAddressSpaceOwnerView) == 8U);
static_assert(__is_aggregate(Ia64RuntimeAddressSpaceOwnerView));
static_assert(__is_standard_layout(Ia64RuntimeAddressSpaceOwnerView));
static_assert(__is_trivially_copyable(Ia64RuntimeAddressSpaceOwnerView));
static_assert(sizeof(Ia64RuntimeAddressSpaceOwnerSnapshot) == 184U);
static_assert(alignof(Ia64RuntimeAddressSpaceOwnerSnapshot) == 8U);
static_assert(__is_aggregate(Ia64RuntimeAddressSpaceOwnerSnapshot));
static_assert(__is_standard_layout(Ia64RuntimeAddressSpaceOwnerSnapshot));
static_assert(__is_trivially_copyable(Ia64RuntimeAddressSpaceOwnerSnapshot));
static_assert(alignof(Ia64RuntimeAddressSpaceOwner) == 8U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_ADDRESS_SPACE_OWNER_HPP_INCLUDED
