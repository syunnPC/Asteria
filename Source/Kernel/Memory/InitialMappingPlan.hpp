// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_INITIAL_MAPPING_PLAN_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_INITIAL_MAPPING_PLAN_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"

namespace Asteria::Kernel::Memory
{
constexpr Foundation::Size MaximumInitialMappings{256U};

enum class InitialMappingOwner : Foundation::UInt8
{
    NucleusCode,
    NucleusReadOnlyData,
    NucleusWritableData,
    NucleusStack,
    PageTable,
    BootHandoff,
    FirmwareRuntimeCode,
    FirmwareRuntimeData,
    Device,
    NucleusRegisterBackingStore,
    NucleusInterruptVectorTable,
    NucleusDescriptorTables,
    /// One RW/NX reservation containing bootstrap stack, RSE and page-table storage.
    NucleusBootstrapRuntimeData,
};

struct InitialMapping
{
    VirtualAddress VirtualStart;
    PhysicalAddress PhysicalStart;
    PageCount Pages;
    MappingPermissions Permissions;
    MemoryCachePolicy CachePolicy;
    InitialMappingOwner Owner;
    Foundation::UInt32 Reserved;
};

enum class InitialMappingPlanError : Foundation::UInt32
{
    None,
    InvalidMapping,
    WriteExecuteMapping,
    OwnerPolicyMismatch,
    VirtualOverlap,
    PhysicalWriteExecuteAlias,
    PhysicalPermissionAlias,
    PhysicalCachePolicyAlias,
    CapacityExceeded,
    AlreadySealed,
    MissingNucleusCode,
    MissingNucleusStack,
    MissingPageTable,
    IndexOutOfBounds,
};

/// Builds a bounded, architecture-independent ownership plan for initial page tables.
/// Architecture ports must additionally validate native virtual-address shape and encode
/// the sealed plan without weakening its permissions or cache policy.
class InitialMappingPlan final
{
public:
    constexpr InitialMappingPlan() noexcept : m_mappings{}, m_mappingCount{0U}, m_sealed{false}
    {
    }

    InitialMappingPlan(const InitialMappingPlan&) = delete;
    InitialMappingPlan& operator=(const InitialMappingPlan&) = delete;
    InitialMappingPlan(InitialMappingPlan&&) = delete;
    InitialMappingPlan& operator=(InitialMappingPlan&&) = delete;

    /// Adds one page-aligned mapping and retains virtual-address order.
    /// Failure does not change the plan.
    [[nodiscard]] InitialMappingPlanError AddMapping(const InitialMapping& mapping) noexcept;

    /// Freezes the plan after proving that code, stack, and page-table ownership exist.
    [[nodiscard]] InitialMappingPlanError Seal() noexcept;

    [[nodiscard]] Foundation::Result<InitialMapping, InitialMappingPlanError>
    GetMapping(Foundation::Size index) const noexcept;

    [[nodiscard]] Foundation::Size GetMappingCount() const noexcept;
    [[nodiscard]] bool IsSealed() const noexcept;

private:
    InitialMapping m_mappings[MaximumInitialMappings];
    Foundation::Size m_mappingCount;
    bool m_sealed;
};

static_assert(sizeof(InitialMappingOwner) == 1U);
static_assert(sizeof(InitialMapping) == 32U);
static_assert(alignof(InitialMapping) == 8U);
static_assert(sizeof(InitialMappingPlanError) == 4U);
} // namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_INITIAL_MAPPING_PLAN_HPP_INCLUDED
