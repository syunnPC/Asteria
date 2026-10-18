// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/FirmwareDmaReservations.hpp"

namespace Asteria::Platform::Device
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
}

void FirmwareDmaReservations::AddRequesters(const VtdTopologyPlan& plan) noexcept
{
    for (Size index{0U}; (index < plan.Count) && (index < VtdUnit::MaximumRequesters); ++index)
    {
        const VtdFunctionRouting& routing = plan.Functions[index];
        if ((routing.Regions == 0U) || (m_requesterCount == MaximumRequesters))
        {
            continue;
        }
        m_requesters[m_requesterCount++] = Requester{
            .Identifier = (UInt32{routing.Address.GetSegment()} << 16U) | routing.Address.GetRequesterId(),
            .Mask = routing.Regions,
            .Reserved = {},
        };
    }
}

bool FirmwareDmaReservations::BuildFromDmar(const Boot::Acpi::AcpiTopology& topology,
                                            const VtdTopologyPlan& plan) noexcept
{
    if (m_built)
    {
        return false;
    }
    m_built = true;
    for (Size index{0U};
         (index < topology.ReservedMemoryRegionCount) && (index < Boot::Acpi::MaximumAcpiReservedMemoryRegions);
         ++index)
    {
        m_ranges[m_rangeCount++] = Range{.Base = topology.ReservedMemoryRegions[index].Base,
                                         .Limit = topology.ReservedMemoryRegions[index].Limit};
    }
    AddRequesters(plan);
    return true;
}

bool FirmwareDmaReservations::BuildFromIvrs(const Boot::Acpi::IvrsTopology& ivrs, const VtdTopologyPlan& plan) noexcept
{
    if (m_built)
    {
        return false;
    }
    m_built = true;
    for (Size index{0U}; (index < ivrs.MemoryCount) && (index < Boot::Acpi::MaximumIvrsMemory); ++index)
    {
        m_ranges[m_rangeCount++] = Range{.Base = ivrs.Memory[index].Base, .Limit = ivrs.Memory[index].Limit};
    }
    AddRequesters(plan);
    return true;
}

Size FirmwareDmaReservations::GetRangeCount() const noexcept
{
    return m_rangeCount;
}

Size FirmwareDmaReservations::GetRequesterCount() const noexcept
{
    return m_requesterCount;
}

bool FirmwareDmaReservations::Overlaps(const UInt32 requester, const UInt64 first, const UInt64 last) const noexcept
{
    for (Size index{0U}; index < m_requesterCount; ++index)
    {
        if (m_requesters[index].Identifier != requester)
        {
            continue;
        }
        for (Size range{0U}; range < m_rangeCount; ++range)
        {
            if (((m_requesters[index].Mask & (1U << range)) != 0U) && (first <= m_ranges[range].Limit) &&
                (m_ranges[range].Base <= last))
            {
                return true;
            }
        }
    }
    return false;
}
} // namespace Asteria::Platform::Device
