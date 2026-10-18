// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/InitialMappingPlan.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaximumPageCount{MaximumUInt64 >> 12U};
constexpr Foundation::UInt64 PageOffsetMask{MappingPageSizeBytes - 1U};

[[nodiscard]] bool IsCachePolicyValid(const MemoryCachePolicy policy) noexcept
{
    switch (policy)
    {
    case MemoryCachePolicy::WriteBack:
    case MemoryCachePolicy::Uncached:
    case MemoryCachePolicy::WriteCombining:
    case MemoryCachePolicy::WriteThrough:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool IsOwnerValid(const InitialMappingOwner owner) noexcept
{
    switch (owner)
    {
    case InitialMappingOwner::NucleusCode:
    case InitialMappingOwner::NucleusReadOnlyData:
    case InitialMappingOwner::NucleusWritableData:
    case InitialMappingOwner::NucleusStack:
    case InitialMappingOwner::PageTable:
    case InitialMappingOwner::BootHandoff:
    case InitialMappingOwner::FirmwareRuntimeCode:
    case InitialMappingOwner::FirmwareRuntimeData:
    case InitialMappingOwner::Device:
    case InitialMappingOwner::NucleusRegisterBackingStore:
    case InitialMappingOwner::NucleusInterruptVectorTable:
    case InitialMappingOwner::NucleusBootstrapRuntimeData:
    case InitialMappingOwner::NucleusDescriptorTables:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool HasValidOwnerPolicy(const InitialMapping& mapping) noexcept
{
    const bool writable = mapping.Permissions.Contains(MappingPermission::Write);
    const bool executable = mapping.Permissions.Contains(MappingPermission::Execute);
    switch (mapping.Owner)
    {
    case InitialMappingOwner::NucleusCode:
    case InitialMappingOwner::NucleusInterruptVectorTable:
        return executable && !writable && (mapping.CachePolicy == MemoryCachePolicy::WriteBack);
    case InitialMappingOwner::FirmwareRuntimeCode:
        return executable && !writable;
    case InitialMappingOwner::NucleusReadOnlyData:
    case InitialMappingOwner::BootHandoff:
        return !writable && !executable && (mapping.CachePolicy == MemoryCachePolicy::WriteBack);
    case InitialMappingOwner::NucleusWritableData:
    case InitialMappingOwner::NucleusStack:
    case InitialMappingOwner::PageTable:
    case InitialMappingOwner::NucleusRegisterBackingStore:
    case InitialMappingOwner::NucleusBootstrapRuntimeData:
    case InitialMappingOwner::NucleusDescriptorTables:
        return writable && !executable && (mapping.CachePolicy == MemoryCachePolicy::WriteBack);
    case InitialMappingOwner::FirmwareRuntimeData:
        return writable && !executable;
    case InitialMappingOwner::Device:
        return writable && !executable &&
               ((mapping.CachePolicy == MemoryCachePolicy::Uncached) ||
                (mapping.CachePolicy == MemoryCachePolicy::WriteCombining));
    default:
        return false;
    }
}

[[nodiscard]] bool TryGetEnd(const Foundation::UInt64 start, const Foundation::UInt64 pageCount,
                             Foundation::UInt64& end) noexcept
{
    if ((pageCount == 0U) || (pageCount > MaximumPageCount) || ((start & PageOffsetMask) != 0U))
    {
        return false;
    }
    const Foundation::UInt64 bytes = pageCount << 12U;
    if (start > (MaximumUInt64 - bytes))
    {
        return false;
    }
    end = start + bytes;
    return true;
}

[[nodiscard]] bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                 const Foundation::UInt64 secondStart, const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}
} // namespace

InitialMappingPlanError InitialMappingPlan::AddMapping(const InitialMapping& mapping) noexcept
{
    if (m_sealed)
    {
        return InitialMappingPlanError::AlreadySealed;
    }

    Foundation::UInt64 virtualEnd{0U};
    Foundation::UInt64 physicalEnd{0U};
    if ((mapping.Reserved != 0U) || !mapping.Permissions.HasOnlyKnownBits() ||
        !mapping.Permissions.Contains(MappingPermission::Read) || !IsCachePolicyValid(mapping.CachePolicy) ||
        !IsOwnerValid(mapping.Owner) ||
        !TryGetEnd(mapping.VirtualStart.GetValue(), mapping.Pages.GetValue(), virtualEnd) ||
        !TryGetEnd(mapping.PhysicalStart.GetValue(), mapping.Pages.GetValue(), physicalEnd))
    {
        return InitialMappingPlanError::InvalidMapping;
    }
    if (mapping.Permissions.Contains(MappingPermission::Write) &&
        mapping.Permissions.Contains(MappingPermission::Execute))
    {
        return InitialMappingPlanError::WriteExecuteMapping;
    }
    if (!HasValidOwnerPolicy(mapping))
    {
        return InitialMappingPlanError::OwnerPolicyMismatch;
    }

    for (Foundation::Size index{0U}; index < m_mappingCount; ++index)
    {
        const InitialMapping& current = m_mappings[index];
        const Foundation::UInt64 currentVirtualEnd =
            current.VirtualStart.GetValue() + (current.Pages.GetValue() << 12U);
        if (RangesOverlap(mapping.VirtualStart.GetValue(), virtualEnd, current.VirtualStart.GetValue(),
                          currentVirtualEnd))
        {
            return InitialMappingPlanError::VirtualOverlap;
        }

        const Foundation::UInt64 currentPhysicalEnd =
            current.PhysicalStart.GetValue() + (current.Pages.GetValue() << 12U);
        const bool physicalOverlap = RangesOverlap(mapping.PhysicalStart.GetValue(), physicalEnd,
                                                   current.PhysicalStart.GetValue(), currentPhysicalEnd);
        const bool writeExecuteAlias = (mapping.Permissions.Contains(MappingPermission::Write) &&
                                        current.Permissions.Contains(MappingPermission::Execute)) ||
                                       (mapping.Permissions.Contains(MappingPermission::Execute) &&
                                        current.Permissions.Contains(MappingPermission::Write));
        if (physicalOverlap && writeExecuteAlias)
        {
            return InitialMappingPlanError::PhysicalWriteExecuteAlias;
        }
        if (physicalOverlap && (mapping.Permissions.GetBits() != current.Permissions.GetBits()))
        {
            return InitialMappingPlanError::PhysicalPermissionAlias;
        }
        if (physicalOverlap && (mapping.CachePolicy != current.CachePolicy))
        {
            return InitialMappingPlanError::PhysicalCachePolicyAlias;
        }
    }

    if (m_mappingCount >= MaximumInitialMappings)
    {
        return InitialMappingPlanError::CapacityExceeded;
    }

    Foundation::Size destination{0U};
    while ((destination < m_mappingCount) &&
           (m_mappings[destination].VirtualStart.GetValue() < mapping.VirtualStart.GetValue()))
    {
        ++destination;
    }
    for (Foundation::Size index{m_mappingCount}; index > destination; --index)
    {
        m_mappings[index] = m_mappings[index - 1U];
    }
    m_mappings[destination] = mapping;
    ++m_mappingCount;
    return InitialMappingPlanError::None;
}

InitialMappingPlanError InitialMappingPlan::Seal() noexcept
{
    if (m_sealed)
    {
        return InitialMappingPlanError::AlreadySealed;
    }

    bool hasNucleusCode{false};
    bool hasNucleusStack{false};
    bool hasPageTable{false};
    for (Foundation::Size index{0U}; index < m_mappingCount; ++index)
    {
        switch (m_mappings[index].Owner)
        {
        case InitialMappingOwner::NucleusCode:
            hasNucleusCode = true;
            break;
        case InitialMappingOwner::NucleusBootstrapRuntimeData:
            hasNucleusStack = true;
            hasPageTable = true;
            break;
        case InitialMappingOwner::NucleusStack:
            hasNucleusStack = true;
            break;
        case InitialMappingOwner::PageTable:
            hasPageTable = true;
            break;
        default:
            break;
        }
    }
    if (!hasNucleusCode)
    {
        return InitialMappingPlanError::MissingNucleusCode;
    }
    if (!hasNucleusStack)
    {
        return InitialMappingPlanError::MissingNucleusStack;
    }
    if (!hasPageTable)
    {
        return InitialMappingPlanError::MissingPageTable;
    }

    m_sealed = true;
    return InitialMappingPlanError::None;
}

Foundation::Result<InitialMapping, InitialMappingPlanError>
InitialMappingPlan::GetMapping(const Foundation::Size index) const noexcept
{
    if (index >= m_mappingCount)
    {
        return Foundation::Result<InitialMapping, InitialMappingPlanError>::Failure(
            InitialMappingPlanError::IndexOutOfBounds);
    }
    return Foundation::Result<InitialMapping, InitialMappingPlanError>::Success(m_mappings[index]);
}

Foundation::Size InitialMappingPlan::GetMappingCount() const noexcept
{
    return m_mappingCount;
}

bool InitialMappingPlan::IsSealed() const noexcept
{
    return m_sealed;
}
} // namespace Asteria::Kernel::Memory
