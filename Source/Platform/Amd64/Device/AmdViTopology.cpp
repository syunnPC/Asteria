// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Device/AmdViTopology.hpp"

namespace Asteria::Platform::Amd64::Device
{
namespace
{
[[nodiscard]] bool Matches(const Boot::Acpi::IvrsDevice& device, const Foundation::UInt16 requester) noexcept
{
    return (device.Type != 0x48U) && (requester >= device.First) && (requester <= device.Last);
}
} // namespace

Foundation::UInt8 AmdViReservedRegions(const Boot::Acpi::IvrsTopology& topology,
                                       const Foundation::UInt16 requester) noexcept
{
    Foundation::UInt8 regions{0U};
    if (requester == topology.IommuDevice)
    {
        return regions;
    }
    for (Foundation::Size index{0U}; (index < topology.MemoryCount) && (index < Boot::Acpi::MaximumIvrsMemory); ++index)
    {
        if ((requester >= topology.Memory[index].First) && (requester <= topology.Memory[index].Last))
        {
            regions |= static_cast<Foundation::UInt8>(Foundation::UInt64{1U} << index);
        }
    }
    return regions;
}

Foundation::Size AmdViDevicePages(const Boot::Acpi::IvrsTopology& topology,
                                  const Kernel::Device::PciFunctionAddress* const functions,
                                  const Foundation::Size count) noexcept
{
    Foundation::UInt16 highest = topology.IommuDevice;
    for (Foundation::Size index{0U}; (functions != nullptr) && (index < count); ++index)
    {
        if (functions[index].GetRequesterId() > highest)
        {
            highest = functions[index].GetRequesterId();
        }
    }
    for (Foundation::Size index{0U}; (index < topology.DeviceCount) && (index < Boot::Acpi::MaximumIvrsDevices);
         ++index)
    {
        const auto& entry = topology.Devices[index];
        if (entry.Last > highest)
        {
            highest = entry.Last;
        }
        if (entry.Alias > highest)
        {
            highest = entry.Alias;
        }
    }
    for (Foundation::Size index{0U}; (index < topology.MemoryCount) && (index < Boot::Acpi::MaximumIvrsMemory); ++index)
    {
        if (topology.Memory[index].Last > highest)
        {
            highest = topology.Memory[index].Last;
        }
    }
    return (static_cast<Foundation::Size>(highest >> 8U) + 1U) * 2U;
}

VtdTopologySelection SelectAmdViUnit(const Boot::Acpi::IvrsTopology& topology) noexcept
{
    VtdTopologyError error{VtdTopologyError::None};
    if (topology.PhysicalAddress == 0U)
    {
        error = VtdTopologyError::NoDmar;
    }
    else if (topology.UnitCount != 1U)
    {
        error = VtdTopologyError::UnitCount;
    }
    else if (topology.Segment != 0U)
    {
        error = VtdTopologyError::Segment;
    }
    else if ((topology.RegisterBase == 0U) || ((topology.RegisterBase & 0x3FFFU) != 0U) ||
             (topology.RegisterBase > 0x000F'FFFF'FFFF'C000ULL))
    {
        error = VtdTopologyError::RegisterRange;
    }
    else if ((topology.DeviceCount > Boot::Acpi::MaximumIvrsDevices) ||
             (topology.MemoryCount > Boot::Acpi::MaximumIvrsMemory))
    {
        error = VtdTopologyError::ScopeCapacity;
    }
    else if (topology.Malformed != 0U)
    {
        error = VtdTopologyError::ScopePath;
    }
    return {.RegisterBase = topology.RegisterBase, .Error = error, .Reserved = {}};
}

VtdTopologyError ResolveAmdViTopology(const Boot::Acpi::IvrsTopology& topology,
                                      Hal::Device::IPciConfigurationSpace& configuration,
                                      const Kernel::Device::PciFunctionAddress* const functions,
                                      const Foundation::Size count, VtdTopologyPlan& output) noexcept
{
    output = {};
    if ((functions == nullptr) || (count > VtdUnit::MaximumRequesters))
    {
        return VtdTopologyError::FunctionCapacity;
    }
    const VtdTopologyError selected = SelectAmdViUnit(topology).Error;
    output.Count = count;
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        auto& routing = output.Functions[index];
        routing.Address = functions[index];
        routing.Unit = 0U;
        routing.Policy = VtdFunctionPolicy::Unserved;
        const Foundation::UInt16 requester = functions[index].GetRequesterId();
        // Retain reservations even when unit selection fails. A truncated
        // memory list reserves all captured endpoints conservatively.
        for (Foundation::Size region{0U}; (region < topology.MemoryCount) && (region < Boot::Acpi::MaximumIvrsMemory);
             ++region)
        {
            const auto& memory = topology.Memory[region];
            if ((requester >= memory.First) && (requester <= memory.Last))
            {
                routing.Regions |= static_cast<Foundation::UInt8>(Foundation::UInt64{1U} << region);
            }
        }
        if (topology.MemoryCount > Boot::Acpi::MaximumIvrsMemory)
        {
            routing.Regions = 0xFFU;
        }
        // The unit's own PCI function never owns a translated DMA context.
        if (requester == topology.IommuDevice)
        {
            routing.Regions = 0U;
        }
        if ((functions[index].GetSegment() != 0U) || (selected != VtdTopologyError::None))
        {
            continue;
        }
        bool served{false};
        bool alias{false};
        for (Foundation::Size entry{0U}; entry < topology.DeviceCount; ++entry)
        {
            const auto& device = topology.Devices[entry];
            served = served || Matches(device, requester);
            if ((device.Type == 0x42U) || (device.Type == 0x43U))
            {
                alias = alias || Matches(device, requester) || (device.Alias == requester);
            }
        }
        if (!served && !alias && (requester != topology.IommuDevice))
        {
            // A host bridge (class 06h, subclass 00h) outside every IVHD
            // device entry is the root complex's own function, not a
            // requester this unit translates, so an IVMD range spanning its
            // ID (X870E: 00:00.0-0f:1f.7 beside an IVHD range from 00:00.3)
            // has nothing to reserve for it. Any other unserved function in
            // an IVMD range still refuses the unit below.
            Foundation::UInt32 hostClass{0U};
            if (configuration.Read32(functions[index], 8U, hostClass) != Hal::Device::PciConfigurationReadError::None)
            {
                return VtdTopologyError::Hardware;
            }
            if ((hostClass >> 16U) == 0x0600U)
            {
                routing.Regions = 0U;
            }
        }
        if (!served || (requester == topology.IommuDevice) || alias)
        {
            continue;
        }
        Foundation::UInt32 classCode{0U};
        if (configuration.Read32(functions[index], 8U, classCode) != Hal::Device::PciConfigurationReadError::None)
        {
            return VtdTopologyError::Hardware;
        }
        Foundation::UInt32 header{0U};
        if (configuration.Read32(functions[index], 0x0CU, header) != Hal::Device::PciConfigurationReadError::None)
        {
            return VtdTopologyError::Hardware;
        }
        const bool bridge = ((header >> 16U) & 0x7FU) == 1U;
        const Foundation::UInt32 baseClass = classCode >> 24U;
        const bool shared =
            !bridge && (baseClass != 3U) && IsBehindPciBridge(configuration, functions[index], functions, count);
        routing.Policy =
            shared ? VtdFunctionPolicy::Unserved
                   : (bridge ? VtdFunctionPolicy::Bridge
                             : ((baseClass == 3U) ? VtdFunctionPolicy::Display
                                                  : ((routing.Regions != 0U) ? VtdFunctionPolicy::ReservedMemory
                                                                             : VtdFunctionPolicy::Assignable)));
    }
    if (selected != VtdTopologyError::None)
    {
        return selected;
    }
    for (Foundation::Size region{0U}; region < topology.MemoryCount; ++region)
    {
        if (topology.Memory[region].Segment != 0U)
        {
            return VtdTopologyError::Segment;
        }
    }
    // A unity range whose requester aliases another function cannot be given
    // an independent kernel context safely with this single-function policy.
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if ((output.Functions[index].Regions != 0U) && (output.Functions[index].Policy == VtdFunctionPolicy::Unserved))
        {
            return VtdTopologyError::UnservedReservedMemory;
        }
    }
    return VtdTopologyError::None;
}
} // namespace Asteria::Platform::Amd64::Device
