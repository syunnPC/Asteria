// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_MAPPING_OWNER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_MAPPING_OWNER_HPP_INCLUDED

#include "Boot/BootModuleMapping.hpp"
#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/PinnedFrameMapping.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageMemoryOwner.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"

namespace Asteria::Platform::Ia64::Executable
{
enum class Ia64InitialUserImageMappingOwnerState : Foundation::UInt8
{
    Fresh,
    Failed,
    Ready,
};

enum class Ia64InitialUserImageMappingOwnerError : Foundation::UInt32
{
    None,
    AlreadyAttempted,
    MemoryOwnerNotReady,
    MemoryOwnerAccessRejected,
    InvalidMemorySummary,
    BspRuntimeMemoryNotReady,
    InitialComponentRootBorrowRejected,
    InvalidBspRuntime,
    InvalidInitialComponentRoot,
    RegistryBindingRejected,
    InvalidCommonAddressWidth,
    MappingCapacityExceeded,
    InvalidRange,
    InvalidAggregateFrame,
    InvalidMemoryObject,
    VirtualRangeOverlap,
    PhysicalRangeOverlap,
    InvalidExistingMapping,
    GuardedVirtualRangeOccupied,
    PhysicalSliceOccupied,
    AuthoritativeStateChanged,
    MappingPreparationRejected,
    AddressSpaceMappingRejected,
    NonemptyShootdown,
    MappingCompletionRejected,
    PostconditionRejected,
};

enum class Ia64InitialUserImageMappingAccessError : Foundation::UInt32
{
    None,
    NotReady,
    IndexOutOfBounds,
    NoFailure,
};

struct Ia64InitialUserImageInstalledMapping
{
    Ia64InitialUserImageMemoryRangePlan Plan;
    Kernel::Memory::AddressSpaceMappingReceipt Receipt;
};

struct Ia64InitialUserImageMappingSummary
{
    Kernel::Memory::AddressSpaceId AddressSpaceIdentity;
    Kernel::Memory::FrameId AggregateFrame;
    Foundation::Size InitialMappingCount;
    Foundation::Size FinalMappingCount;
    Foundation::Size RangeCount;
    Foundation::UInt64 FirstMappingSequence;
    Foundation::UInt64 LastMappingSequence;
};

struct Ia64InitialUserImageMappingFailure
{
    Ia64InitialUserImageMappingOwnerError Error;
    Ia64InitialUserImageMemoryAccessError MemoryAccess;
    Kernel::Memory::PinnedFrameMappingError MappingPreparation;
    Memory::Ia64BspRuntimeRootBorrowError RootBorrow;
    Kernel::Object::ObjectPinError Pin;
    Foundation::UInt8 Reserved0[3];
    Kernel::Memory::AddressSpaceFailure AddressSpace;
    Foundation::Size RangeIndex;
    bool RootMayBeFaulted;
    Foundation::UInt8 Reserved1[7];
};

/// Installs every materialized initial-user range into the BSP's exact private
/// initial-component root. This owner neither claims nor releases the physical
/// arena, Frame, or MemoryObjects retained by the memory owner.
///
/// Install is allocation-free and externally serialized on the non-preemptible
/// BSP before the private root is activated. The memory owner, BSP runtime,
/// registry, private root, embedded objects, and this owner remain alive and
/// immovable for the boot lifetime. The caller excludes concurrent mutation,
/// destruction, quiescence, root transitions, and mapping operations and holds
/// no collaborator lock while calling Install.
///
/// Every range and the complete root ledger are preflighted before the first
/// AddressSpace mutation. A preflight failure leaves this owner Fresh and may
/// be retried. Immediately before the first mapping preparation this owner
/// becomes Failed; every later failure is terminal and retains all committed
/// mappings and pins. A zero-resident root needs no invalidation dispatcher:
/// each zero-target transaction is completed directly with its returned token.
class Ia64InitialUserImageMappingOwner final
{
public:
    constexpr Ia64InitialUserImageMappingOwner() noexcept = default;
    ~Ia64InitialUserImageMappingOwner() noexcept = default;

    Ia64InitialUserImageMappingOwner(const Ia64InitialUserImageMappingOwner&) = delete;
    Ia64InitialUserImageMappingOwner& operator=(const Ia64InitialUserImageMappingOwner&) = delete;
    Ia64InitialUserImageMappingOwner(Ia64InitialUserImageMappingOwner&&) = delete;
    Ia64InitialUserImageMappingOwner& operator=(Ia64InitialUserImageMappingOwner&&) = delete;

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageMappingSummary, Ia64InitialUserImageMappingFailure>
    Install(Ia64InitialUserImageMemoryOwner& memoryOwner, Memory::Ia64BspRuntimeMemory& bspRuntimeMemory,
            Kernel::Object::ObjectRegistry& registry,
            Memory::Ia64BspRuntimeUserDomain domain = Memory::Ia64BspRuntimeUserDomain::InitialComponent) noexcept;

    /// Pager only, before admission. Retains one read-only LoaderData slice;
    /// never copies module bytes or installs a translation register.
    [[nodiscard]] bool InstallBootModules(const Boot::BootInfo& info, Kernel::Object::RootCapabilityIssuer& issuer,
                                          Kernel::Resource::ResourceDomain& resources,
                                          Kernel::Resource::ResourceDomainObjectId resourceIdentity) noexcept;

    [[nodiscard]] constexpr Ia64InitialUserImageMappingOwnerState GetState() const noexcept
    {
        return m_state;
    }

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageMappingSummary, Ia64InitialUserImageMappingAccessError>
    GetSummary() const noexcept;

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageInstalledMapping, Ia64InitialUserImageMappingAccessError>
    GetMapping(Foundation::Size index) const noexcept;

    [[nodiscard]] Foundation::Result<Ia64InitialUserImageMappingFailure, Ia64InitialUserImageMappingAccessError>
    GetFailure() const noexcept;

private:
    using InstallResult = Foundation::Result<Ia64InitialUserImageMappingSummary, Ia64InitialUserImageMappingFailure>;

    [[nodiscard]] bool ValidateModuleSlice(const Boot::BootInfo& info) const noexcept;
    [[nodiscard]] bool MapModuleSlice() noexcept;
    Kernel::Memory::BootFrame m_moduleFrame{};
    Kernel::Memory::MemoryObject m_moduleMemory{};
    Kernel::Memory::FrameId m_moduleFrameId{};
    Kernel::Memory::MemoryObjectId m_moduleMemoryId{};
    Foundation::UInt64 m_modulePhysicalStart{0U};
    Foundation::UInt64 m_moduleBytes{0U};
    bool m_moduleAttempted{false};

    void ResetPreflightStorage() noexcept;

    [[nodiscard]] Ia64InitialUserImageMappingOwnerError
    CaptureAndValidateOwners(Ia64InitialUserImageMemoryOwner& memoryOwner,
                             Memory::Ia64BspRuntimeMemory& bspRuntimeMemory,
                             Kernel::Object::ObjectRegistry& registry) noexcept;
    [[nodiscard]] Ia64InitialUserImageMappingOwnerError
    ValidateAggregateFrame(Kernel::Object::ObjectRegistry& registry) noexcept;
    [[nodiscard]] Ia64InitialUserImageMappingOwnerError
    ValidateRanges(Ia64InitialUserImageMemoryOwner& memoryOwner, Kernel::Object::ObjectRegistry& registry) noexcept;
    [[nodiscard]] Ia64InitialUserImageMappingOwnerError ValidateRangeGeometry(Foundation::Size index) noexcept;
    [[nodiscard]] Ia64InitialUserImageMappingOwnerError ValidatePairwiseRanges() noexcept;
    [[nodiscard]] Ia64InitialUserImageMappingOwnerError ValidateExistingMappings() noexcept;
    [[nodiscard]] Ia64InitialUserImageMappingOwnerError
    RevalidateAuthoritativeState(Ia64InitialUserImageMemoryOwner& memoryOwner,
                                 Memory::Ia64BspRuntimeMemory& bspRuntimeMemory,
                                 Kernel::Object::ObjectRegistry& registry) noexcept;
    [[nodiscard, gnu::noinline]]
    Ia64InitialUserImageMappingOwnerError InstallMapping(Foundation::Size index) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageMappingOwnerError
    PrepareAndBeginMapping(Foundation::Size index, Kernel::Memory::AddressSpaceShootdownRequest& request) noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageMappingOwnerError CompleteAndValidateMapping(
        Foundation::Size index, const Kernel::Memory::AddressSpaceShootdownRequest& request) noexcept;
    [[nodiscard]] Ia64InitialUserImageMappingOwnerError InstallMappings() noexcept;
    [[nodiscard]] Ia64InitialUserImageMappingOwnerError ValidateFinalGuardVacancy() noexcept;
    [[nodiscard]] ASTERIA_NOINLINE_NOIPA Ia64InitialUserImageMappingOwnerError ValidateFinalOwnerBoundary(
        Ia64InitialUserImageMemoryOwner& memoryOwner, Memory::Ia64BspRuntimeMemory& bspRuntimeMemory) noexcept;
    [[nodiscard]] Ia64InitialUserImageMappingOwnerError
    VerifyPostconditions(Ia64InitialUserImageMemoryOwner& memoryOwner,
                         Memory::Ia64BspRuntimeMemory& bspRuntimeMemory) noexcept;

    [[nodiscard]] InstallResult Fail(Ia64InitialUserImageMappingOwnerError error) noexcept;

    Ia64InitialUserImageInstalledMapping m_mappings[Ia64InitialUserImageMaximumMemoryObjectCount]{};
    Kernel::Memory::MemoryObject* m_memoryObjectStorage[Ia64InitialUserImageMaximumMemoryObjectCount]{};
    Ia64InitialUserImageMemorySummary m_memorySummary{};
    Memory::Ia64BspRuntimeMemorySnapshot m_bspSnapshot{};
    Memory::Ia64RuntimeAddressSpaceOwnerSnapshot m_initialComponentSnapshot{};
    Kernel::Memory::AddressSpaceSnapshot m_initialRootSnapshot{};
    Kernel::Memory::AddressSpaceSnapshot m_finalRootSnapshot{};
    Ia64InitialUserImageMappingSummary m_summary{};
    Ia64InitialUserImageMappingFailure m_failure{};
    Kernel::Object::ObjectRegistry* m_registry{nullptr};
    Kernel::Memory::AddressSpace* m_rootAddressSpace{nullptr};
    Kernel::Memory::Frame* m_aggregateFrameStorage{nullptr};
    Kernel::Memory::ProcessorAddressSpaceController* m_controllerStorage{nullptr};
    Kernel::Object::ResourceDomainId m_resourceDomainIdentifier{};
    Kernel::Scheduler::CpuId m_bspProcessor{};
    Foundation::UInt8 m_commonPhysicalAddressBits{0U};
    Foundation::UInt8 m_commonImplementedVirtualAddressMostSignificantBit{0U};
    bool m_hasFailure{false};
    Memory::Ia64BspRuntimeUserDomain m_domain{Memory::Ia64BspRuntimeUserDomain::InitialComponent};
    Ia64InitialUserImageMappingOwnerState m_state{Ia64InitialUserImageMappingOwnerState::Fresh};
};

static_assert(sizeof(Ia64InitialUserImageMappingOwnerState) == 1U);
static_assert(sizeof(Ia64InitialUserImageMappingOwnerError) == 4U);
static_assert(sizeof(Ia64InitialUserImageMappingAccessError) == 4U);
static_assert(__is_aggregate(Ia64InitialUserImageInstalledMapping));
static_assert(__is_standard_layout(Ia64InitialUserImageInstalledMapping));
static_assert(__is_trivially_copyable(Ia64InitialUserImageInstalledMapping));
static_assert(sizeof(Ia64InitialUserImageInstalledMapping) == 88U);
static_assert(alignof(Ia64InitialUserImageInstalledMapping) == 8U);
static_assert(__is_aggregate(Ia64InitialUserImageMappingSummary));
static_assert(__is_standard_layout(Ia64InitialUserImageMappingSummary));
static_assert(__is_trivially_copyable(Ia64InitialUserImageMappingSummary));
static_assert(sizeof(Ia64InitialUserImageMappingSummary) == 72U);
static_assert(alignof(Ia64InitialUserImageMappingSummary) == 8U);
static_assert(__is_aggregate(Ia64InitialUserImageMappingFailure));
static_assert(__is_standard_layout(Ia64InitialUserImageMappingFailure));
static_assert(__is_trivially_copyable(Ia64InitialUserImageMappingFailure));
static_assert(sizeof(Ia64InitialUserImageMappingFailure) == 40U);
static_assert(alignof(Ia64InitialUserImageMappingFailure) == 8U);
static_assert(alignof(Ia64InitialUserImageMappingOwner) == 16U);
} // namespace Asteria::Platform::Ia64::Executable

#endif // ASTERIA_PLATFORM_IA64_EXECUTABLE_IA64_INITIAL_USER_IMAGE_MAPPING_OWNER_HPP_INCLUDED
