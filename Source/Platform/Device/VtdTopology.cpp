// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/VtdTopology.hpp"

namespace Asteria::Platform::Device
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt32;
using Kernel::Device::PciFunctionAddress;
using Boot::Acpi::AcpiDmaDeviceScope;
using Hal::Device::PciConfigurationReadError;
constexpr UInt8 NoUnit{Boot::Acpi::MaximumAcpiDmaRemappingUnits};

struct ResolvedScope
{
    PciFunctionAddress Address;
    UInt8 Secondary;
    UInt8 Subordinate;
};

enum class ScopeResolution : UInt8
{
    Present,
    Absent,
    Broken,
};

[[nodiscard]] ScopeResolution ResolveScope(const AcpiDmaDeviceScope& scope, Hal::Device::IPciConfigurationSpace& ecam,
                                           ResolvedScope& output) noexcept
{
    if ((scope.PathEntries == 0U) || (scope.PathEntries > Boot::Acpi::MaximumAcpiDmaScopePath))
    {
        return ScopeResolution::Broken;
    }
    UInt8 bus{scope.StartBus};
    UInt8 maximumBus{255U};
    for (Size hop{0U}; hop < scope.PathEntries; ++hop)
    {
        const UInt8 device = (hop == 0U) ? scope.Device : scope.Path[(hop - 1U) * 2U];
        const UInt8 function = (hop == 0U) ? scope.Function : scope.Path[(hop * 2U) - 1U];
        const PciFunctionAddress address{0U, bus, device, function};
        UInt32 identity{0U};
        UInt32 header{0U};
        const bool last = (hop + 1U) == scope.PathEntries;
        if (!address.IsValid() || (ecam.Read32(address, 0U, identity) != PciConfigurationReadError::None))
        {
            return ScopeResolution::Broken;
        }
        if ((identity & 0xFFFFU) == 0xFFFFU)
        {
            return last ? ScopeResolution::Absent : ScopeResolution::Broken;
        }
        if (ecam.Read32(address, 0x0CU, header) != PciConfigurationReadError::None)
        {
            return ScopeResolution::Broken;
        }
        const bool bridge = ((header >> 16U) & 0x7FU) == 1U;
        if (last && (scope.Type == Boot::Acpi::AcpiDmaScopeEndpoint))
        {
            if (bridge)
            {
                return ScopeResolution::Broken;
            }
            output = {.Address = address, .Secondary = 0U, .Subordinate = 0U};
            return ScopeResolution::Present;
        }
        UInt32 buses{0U};
        if (!bridge || (ecam.Read32(address, 0x18U, buses) != PciConfigurationReadError::None))
        {
            return ScopeResolution::Broken;
        }
        const auto secondary = static_cast<UInt8>((buses >> 8U) & 0xFFU);
        const auto subordinate = static_cast<UInt8>((buses >> 16U) & 0xFFU);
        if (last && (secondary == 0U) && (subordinate == 0U))
        {
            // A bridge the firmware gave no bus range forwards to no bus
            // (the 7500 hub's empty secondary bridge): the scope names the
            // bridge alone. A path cannot descend through such a bridge.
            output = {.Address = address, .Secondary = 0U, .Subordinate = 0U};
            return ScopeResolution::Present;
        }
        if (((buses & 0xFFU) != bus) || (secondary <= bus) || (subordinate < secondary) || (subordinate > maximumBus))
        {
            return ScopeResolution::Broken;
        }
        if (last)
        {
            output = {.Address = address, .Secondary = secondary, .Subordinate = subordinate};
            return ScopeResolution::Present;
        }
        bus = secondary;
        maximumBus = subordinate;
    }
    return ScopeResolution::Broken;
}

[[nodiscard]] bool Names(const ResolvedScope& scope, const PciFunctionAddress address) noexcept
{
    return scope.Address.IsEqualTo(address) || ((scope.Secondary != 0U) && (address.GetBus() >= scope.Secondary) &&
                                                (address.GetBus() <= scope.Subordinate));
}

[[nodiscard]] bool IsPciScope(const AcpiDmaDeviceScope& scope) noexcept
{
    return (scope.Type == Boot::Acpi::AcpiDmaScopeEndpoint) || (scope.Type == Boot::Acpi::AcpiDmaScopeBridge);
}

[[nodiscard]] bool Overlaps(const ResolvedScope& left, const ResolvedScope& right) noexcept
{
    return Names(left, right.Address) || Names(right, left.Address) ||
           ((left.Secondary != 0U) && (right.Secondary != 0U) && (left.Secondary <= right.Subordinate) &&
            (right.Secondary <= left.Subordinate));
}

[[nodiscard]] VtdTopologyError ResolveUnits(const Boot::Acpi::AcpiTopology& topology,
                                            Hal::Device::IPciConfigurationSpace& ecam, VtdTopologyPlan& output) noexcept
{
    // Fixed boot scratch: caller serializes BSP preparation, never an ISR.
    // Kept off the kernel stack (32 resolved scopes are 256 bytes).
    static ResolvedScope resolved[Boot::Acpi::MaximumAcpiDmaDeviceScopes]{};
    for (Size index{0U}; index < topology.DmaDeviceScopeCount; ++index)
    {
        const auto& scope = topology.DmaDeviceScopes[index];
        if (!IsPciScope(scope))
        {
            // IOAPIC and HPET scopes concern interrupt remapping, which is off.
            if ((scope.Type != 3U) && (scope.Type != 4U))
            {
                return VtdTopologyError::ScopePath;
            }
            continue;
        }
        if (scope.Unit >= topology.DmaRemappingUnitCount)
        {
            return VtdTopologyError::ScopePath;
        }
        const auto resolution = ResolveScope(scope, ecam, resolved[index]);
        if (resolution == ScopeResolution::Broken)
        {
            return VtdTopologyError::ScopePath;
        }
        if (resolution == ScopeResolution::Absent)
        {
            resolved[index] = {};
            ++output.IgnoredScopes;
            continue;
        }
        for (Size prior{0U}; prior < index; ++prior)
        {
            if (IsPciScope(topology.DmaDeviceScopes[prior]) && resolved[prior].Address.IsValid() &&
                Overlaps(resolved[prior], resolved[index]))
            {
                return VtdTopologyError::ScopeOverlap;
            }
        }
        bool named{false};
        for (Size function{0U}; function < output.Count; ++function)
        {
            auto& routing = output.Functions[function];
            if (Names(resolved[index], routing.Address))
            {
                routing.Unit = scope.Unit;
                named = true;
            }
        }
        if (!named)
        {
            return VtdTopologyError::FunctionCapacity;
        }
    }
    return VtdTopologyError::None;
}

[[nodiscard]] VtdTopologyError ResolveRegions(const Boot::Acpi::AcpiTopology& topology,
                                              Hal::Device::IPciConfigurationSpace& ecam,
                                              VtdTopologyPlan& output) noexcept
{
    for (Size index{0U}; index < topology.ReservedMemoryScopeCount; ++index)
    {
        const auto& scope = topology.ReservedMemoryScopes[index];
        ResolvedScope resolved{};
        if (!IsPciScope(scope) || (scope.Unit >= topology.ReservedMemoryRegionCount))
        {
            return VtdTopologyError::ScopePath;
        }
        const auto resolution = ResolveScope(scope, ecam, resolved);
        if (resolution == ScopeResolution::Broken)
        {
            return VtdTopologyError::ScopePath;
        }
        if (resolution == ScopeResolution::Absent)
        {
            ++output.IgnoredScopes;
            continue;
        }
        bool found{false};
        for (Size function{0U}; function < output.Count; ++function)
        {
            auto& routing = output.Functions[function];
            if (!Names(resolved, routing.Address))
            {
                continue;
            }
            routing.Regions |= static_cast<UInt8>(UInt32{1U} << scope.Unit);
            found = true;
        }
        if (!found)
        {
            return VtdTopologyError::UnservedReservedMemory;
        }
    }
    for (Size region{0U}; region < topology.ReservedMemoryRegionCount; ++region)
    {
        bool found{false};
        for (Size index{0U}; index < topology.ReservedMemoryScopeCount; ++index)
        {
            found = found || (topology.ReservedMemoryScopes[index].Unit == region);
        }
        if (!found)
        {
            return VtdTopologyError::ReservedMemory;
        }
    }
    return VtdTopologyError::None;
}
} // namespace

Foundation::UInt64 VtdRegisterBytes(const Boot::Acpi::AcpiDmaRemappingUnit& unit) noexcept
{
    return (unit.Size & 0xF0U) == 0U ? (Foundation::UInt64{4096U} << unit.Size) : 0U;
}

VtdTopologySelection SelectVtdUnit(const Boot::Acpi::AcpiTopology& topology, const UInt8 addressBits) noexcept
{
    const auto refuse = [](const VtdTopologyError error) noexcept
    {
        return VtdTopologySelection{.RegisterBase = 0U, .Error = error, .Reserved = {}};
    };
    if (topology.DmarHostAddressWidth == 0U)
    {
        return refuse(VtdTopologyError::NoDmar);
    }
    if ((addressBits < 32U) || (addressBits > 63U))
    {
        return refuse(VtdTopologyError::RegisterRange);
    }
    const Foundation::UInt64 addressLimit{Foundation::UInt64{1U} << addressBits};
    if ((topology.DmaRemappingUnitCount == 0U) ||
        (topology.DmaRemappingUnitCount > Boot::Acpi::MaximumAcpiDmaRemappingUnits))
    {
        return refuse(VtdTopologyError::UnitCount);
    }
    if (topology.ReservedMemoryRegionCount > Boot::Acpi::MaximumAcpiReservedMemoryRegions)
    {
        return refuse(VtdTopologyError::ReservedMemory);
    }
    if ((topology.DmaDeviceScopeCount > Boot::Acpi::MaximumAcpiDmaDeviceScopes) ||
        (topology.ReservedMemoryScopeCount > Boot::Acpi::MaximumAcpiDmaDeviceScopes))
    {
        return refuse(VtdTopologyError::ScopeCapacity);
    }
    bool includeAll{false};
    for (Size index{0U}; index < topology.DmaRemappingUnitCount; ++index)
    {
        const auto& unit = topology.DmaRemappingUnits[index];
        if (unit.Segment != 0U)
        {
            return refuse(VtdTopologyError::Segment);
        }
        if ((unit.Flags & 0xFEU) != 0U)
        {
            return refuse(VtdTopologyError::Flags);
        }
        const Foundation::UInt64 bytes{VtdRegisterBytes(unit)};
        if ((bytes == 0U) || (unit.RegisterBaseAddress == 0U) || ((unit.RegisterBaseAddress & (bytes - 1U)) != 0U) ||
            (unit.RegisterBaseAddress > addressLimit - bytes))
        {
            return refuse(VtdTopologyError::RegisterRange);
        }
        for (Size prior{0U}; prior < index; ++prior)
        {
            const auto& previous = topology.DmaRemappingUnits[prior];
            if ((previous.RegisterBaseAddress < unit.RegisterBaseAddress + bytes) &&
                (unit.RegisterBaseAddress < previous.RegisterBaseAddress + VtdRegisterBytes(previous)))
            {
                return refuse(VtdTopologyError::RegisterRange);
            }
        }
        if ((unit.Flags & Boot::Acpi::AcpiDmaRemappingIncludeAllFlag) != 0U)
        {
            if (includeAll)
            {
                return refuse(VtdTopologyError::IncludeAllOverlap);
            }
            includeAll = true;
        }
    }
    for (Size index{0U}; index < topology.ReservedMemoryRegionCount; ++index)
    {
        const auto& region = topology.ReservedMemoryRegions[index];
        if (region.Segment != 0U)
        {
            return refuse(VtdTopologyError::Segment);
        }
        if ((region.Base > region.Limit) || ((region.Base & 0xFFFU) != 0U) || ((region.Limit & 0xFFFU) != 0xFFFU) ||
            (region.Limit >= addressLimit))
        {
            return refuse(VtdTopologyError::ReservedMemory);
        }
    }
    return {.RegisterBase = topology.DmaRemappingUnits[0U].RegisterBaseAddress,
            .Error = VtdTopologyError::None,
            .Reserved = {}};
}

bool IsBehindPciBridge(Hal::Device::IPciConfigurationSpace& configuration, const PciFunctionAddress function,
                       const PciFunctionAddress* const functions, const Size count) noexcept
{
    for (Size index{0U}; index < count; ++index)
    {
        UInt32 classCode{0U};
        if (configuration.Read32(functions[index], 8U, classCode) != PciConfigurationReadError::None)
        {
            return true;
        }
        // Base/subclass 06/04, PCI programming interface 00 is the
        // conventional PCI side; a pure PCIe root port has an Express
        // capability with port type 4 or 6, a PCIe-to-PCI bridge type 7.
        if ((classCode >> 16U) != 0x0604U)
        {
            continue;
        }
        UInt32 capabilities{0U};
        if (configuration.Read32(functions[index], 0x34U, capabilities) != PciConfigurationReadError::None)
        {
            return true;
        }
        bool conventional{true};
        Foundation::UInt8 offset = static_cast<Foundation::UInt8>(capabilities & 0xFCU);
        for (Size hop{0U}; (offset >= 0x40U) && (hop < 48U); ++hop)
        {
            UInt32 capability{0U};
            if (configuration.Read32(functions[index], offset, capability) != PciConfigurationReadError::None)
            {
                return true;
            }
            if ((capability & 0xFFU) == 0x10U)
            {
                const UInt32 portType = (capability >> 20U) & 15U;
                conventional = (portType == 7U) || (portType == 8U);
                break;
            }
            offset = static_cast<Foundation::UInt8>((capability >> 8U) & 0xFCU);
        }
        if (!conventional)
        {
            continue;
        }
        UInt32 buses{0U};
        if (configuration.Read32(functions[index], 0x18U, buses) != PciConfigurationReadError::None)
        {
            return true;
        }
        const UInt32 secondary = (buses >> 8U) & 0xFFU;
        const UInt32 subordinate = (buses >> 16U) & 0xFFU;
        if ((secondary != 0U) && (function.GetBus() >= secondary) && (function.GetBus() <= subordinate))
        {
            return true;
        }
    }
    return false;
}

VtdTopologyError ResolveVtdTopology(const Boot::Acpi::AcpiTopology& topology, Hal::Device::IPciConfigurationSpace& ecam,
                                    const PciFunctionAddress* const functions, const Size count,
                                    VtdTopologyPlan& output, const UInt8 addressBits) noexcept
{
    output.Count = 0U;
    output.IgnoredScopes = 0U;
    for (Size unit{0U}; unit < Boot::Acpi::MaximumAcpiDmaRemappingUnits; ++unit)
    {
        output.KeepDisabled[unit] = false;
        output.DisplayConflict[unit] = false;
        output.NoFunctions[unit] = false;
    }
    if ((count > VtdUnit::MaximumRequesters) || ((count != 0U) && (functions == nullptr)))
    {
        return VtdTopologyError::FunctionCapacity;
    }
    for (Size index{0U}; index < count; ++index)
    {
        if (!functions[index].IsValid() || (functions[index].GetSegment() != 0U))
        {
            return VtdTopologyError::Segment;
        }
        output.Functions[index] = {.Address = functions[index],
                                   .Unit = NoUnit,
                                   .Policy = VtdFunctionPolicy::Unserved,
                                   .Regions = 0U,
                                   .Reserved = {}};
    }
    output.Count = count;
    VtdTopologyError reservations{VtdTopologyError::ScopeCapacity};
    if ((topology.ReservedMemoryScopeCount <= Boot::Acpi::MaximumAcpiDmaDeviceScopes) &&
        (topology.ReservedMemoryRegionCount <= Boot::Acpi::MaximumAcpiReservedMemoryRegions))
    {
        reservations = ResolveRegions(topology, ecam, output);
    }
    if (reservations != VtdTopologyError::None)
    {
        // A malformed walk or truncation cannot authorize a firmware device.
        for (Size index{0U}; index < count; ++index)
        {
            output.Functions[index].Regions = 0xFFU;
        }
    }
    const VtdTopologyError selected = SelectVtdUnit(topology, addressBits).Error;
    if (selected != VtdTopologyError::None)
    {
        return selected;
    }
    if (reservations != VtdTopologyError::None)
    {
        return reservations;
    }
    UInt8 fallback{NoUnit};
    for (Size index{0U}; index < topology.DmaRemappingUnitCount; ++index)
    {
        if (topology.DmaRemappingUnits[index].Flags != 0U)
        {
            fallback = static_cast<UInt8>(index);
        }
    }
    const VtdTopologyError units = ResolveUnits(topology, ecam, output);
    if (units != VtdTopologyError::None)
    {
        return units;
    }
    bool nonDisplay[Boot::Acpi::MaximumAcpiDmaRemappingUnits]{};
    bool populated[Boot::Acpi::MaximumAcpiDmaRemappingUnits]{};
    for (Size index{0U}; index < count; ++index)
    {
        auto& routing = output.Functions[index];
        if (routing.Unit == NoUnit)
        {
            routing.Unit = fallback;
        }
        UInt32 classCode{0U};
        if (ecam.Read32(routing.Address, 8U, classCode) != PciConfigurationReadError::None)
        {
            return VtdTopologyError::ScopePath;
        }
        UInt32 header{0U};
        if (ecam.Read32(routing.Address, 0x0CU, header) != PciConfigurationReadError::None)
        {
            return VtdTopologyError::ScopePath;
        }
        const bool bridge = ((header >> 16U) & 0x7FU) == 1U;
        const bool display = (classCode >> 24U) == 3U;
        const bool shared = !bridge && !display && IsBehindPciBridge(ecam, routing.Address, functions, count);
        routing.Policy = bridge ? VtdFunctionPolicy::Bridge
                                : (display ? VtdFunctionPolicy::Display
                                           : (shared ? VtdFunctionPolicy::Unserved : VtdFunctionPolicy::Assignable));
        if (routing.Unit == NoUnit)
        {
            routing.Policy = VtdFunctionPolicy::Unserved;
            continue;
        }
        populated[routing.Unit] = true;
        output.KeepDisabled[routing.Unit] = output.KeepDisabled[routing.Unit] || display;
        nonDisplay[routing.Unit] = nonDisplay[routing.Unit] || !display;
    }
    for (Size index{0U}; index < topology.DmaRemappingUnitCount; ++index)
    {
        output.DisplayConflict[index] = output.KeepDisabled[index] && nonDisplay[index];
        output.NoFunctions[index] =
            !populated[index] &&
            ((topology.DmaRemappingUnits[index].Flags & Boot::Acpi::AcpiDmaRemappingIncludeAllFlag) == 0U);
        output.KeepDisabled[index] = (output.KeepDisabled[index] && !nonDisplay[index]) || output.NoFunctions[index];
    }
    for (Size index{0U}; index < count; ++index)
    {
        auto& routing = output.Functions[index];
        if ((routing.Regions != 0U) && (routing.Unit == NoUnit))
        {
            return VtdTopologyError::UnservedReservedMemory;
        }
        if ((routing.Regions != 0U) && (routing.Policy == VtdFunctionPolicy::Assignable))
        {
            routing.Policy = VtdFunctionPolicy::ReservedMemory;
        }
    }
    return VtdTopologyError::None;
}

bool ShouldSweepVtdFunction(const VtdFunctionRouting& function, const VtdTopologyPlan& plan) noexcept
{
    return (function.Unit < Boot::Acpi::MaximumAcpiDmaRemappingUnits) && !plan.KeepDisabled[function.Unit] &&
           (function.Policy == VtdFunctionPolicy::Assignable) && (function.Regions == 0U);
}

bool VtdFirmwareRangeIsSafe(const Boot::BootInfo& info, const Foundation::UInt64 base,
                            const Foundation::UInt64 limit) noexcept
{
    using Boot::PhysicalMemoryKind;
    if ((base > limit) || (info.MemoryRegionCount > Boot::MaximumMemoryRegions))
    {
        return false;
    }
    for (Size index{0U}; index < info.MemoryRegionCount; ++index)
    {
        const auto& region = info.MemoryRegions[index];
        if (region.PageCount > ((~Foundation::UInt64{0U} - region.PhysicalStart) / 4096U))
        {
            return false;
        }
        const Foundation::UInt64 end = region.PhysicalStart + (region.PageCount * 4096U);
        if ((region.PhysicalStart > limit) || (end <= base))
        {
            continue;
        }
        switch (region.Kind)
        {
        case PhysicalMemoryKind::Loader:
        case PhysicalMemoryKind::BootServices:
        case PhysicalMemoryKind::Conventional:
        case PhysicalMemoryKind::AcpiReclaimable:
        case PhysicalMemoryKind::Persistent:
        case PhysicalMemoryKind::Unaccepted:
            return false;
        case PhysicalMemoryKind::Reserved:
        case PhysicalMemoryKind::RuntimeServices:
        case PhysicalMemoryKind::Unusable:
        case PhysicalMemoryKind::AcpiNonVolatile:
        case PhysicalMemoryKind::MemoryMappedIo:
        case PhysicalMemoryKind::PalCode:
            break;
        }
    }
    return true;
}

namespace
{
[[nodiscard]] bool FirmwareQueueKind(const Boot::PhysicalMemoryKind kind) noexcept
{
    switch (kind)
    {
    case Boot::PhysicalMemoryKind::BootServices:
    case Boot::PhysicalMemoryKind::RuntimeServices:
    case Boot::PhysicalMemoryKind::Reserved:
    case Boot::PhysicalMemoryKind::AcpiNonVolatile:
        return true;
    case Boot::PhysicalMemoryKind::Loader:
    case Boot::PhysicalMemoryKind::Conventional:
    case Boot::PhysicalMemoryKind::Unusable:
    case Boot::PhysicalMemoryKind::AcpiReclaimable:
    case Boot::PhysicalMemoryKind::MemoryMappedIo:
    case Boot::PhysicalMemoryKind::PalCode:
    case Boot::PhysicalMemoryKind::Persistent:
    case Boot::PhysicalMemoryKind::Unaccepted:
        return false;
    }
    return false;
}

[[nodiscard]] bool ExcludesOwnedPages(const Foundation::UInt64 base, const Foundation::UInt64 limit,
                                      const Foundation::UInt64 start, const Foundation::UInt64 pages) noexcept
{
    if (pages == 0U)
    {
        return true;
    }
    if ((start & 4095U) != 0U || pages > ((~Foundation::UInt64{0U} - start) / 4096U))
    {
        return false;
    }
    return start > limit || start + pages * 4096U <= base;
}
} // namespace

bool VtdFirmwareQueueRangeIsWritable(const Boot::BootInfo& info, const Foundation::UInt64 base,
                                     const Foundation::UInt64 limit,
                                     const VtdFirmwareMemoryOwnership& ownership) noexcept
{
    // Intel VT-d rev. 4.0 sections 6.5.2/6.5.2.8 define DMA reads of the
    // queue and a DWORD status write. UEFI 2.10 section 7.2.3 defines the
    // recorded memory types; exclusivity after ExitBootServices is Asteria
    // policy, not a claim that these types authorize arbitrary DMA.
    if (base > limit || limit == ~Foundation::UInt64{0U} || info.MemoryRegionCount > Boot::MaximumMemoryRegions ||
        !ExcludesOwnedPages(base, limit, ownership.PoolStart, ownership.PoolPages) ||
        !ExcludesOwnedPages(base, limit, ownership.StartupStart, ownership.StartupPages) ||
        !ExcludesOwnedPages(base, limit, info.Files.PhysicalStart, info.Files.PageCount) ||
        !ExcludesOwnedPages(base, limit, info.Inventory.PhysicalStart, info.Inventory.Pages))
    {
        return false;
    }
    for (Size index{0U}; index < info.MemoryRegionCount; ++index)
    {
        const auto& region = info.MemoryRegions[index];
        if ((region.PhysicalStart & 4095U) != 0U ||
            region.PageCount > ((~Foundation::UInt64{0U} - region.PhysicalStart) / 4096U))
        {
            return false;
        }
        if (region.PageCount != 0U && region.PhysicalStart <= limit &&
            region.PhysicalStart + region.PageCount * 4096U > base && !FirmwareQueueKind(region.Kind))
        {
            return false;
        }
    }
    Foundation::UInt64 covered{base};
    for (Size pass{0U}; pass < info.MemoryRegionCount; ++pass)
    {
        Foundation::UInt64 next{covered};
        for (Size index{0U}; index < info.MemoryRegionCount; ++index)
        {
            const auto& region = info.MemoryRegions[index];
            const Foundation::UInt64 end = region.PhysicalStart + region.PageCount * 4096U;
            if (FirmwareQueueKind(region.Kind) && region.PhysicalStart <= covered && end > next)
            {
                next = end;
            }
        }
        if (next > limit)
        {
            return true;
        }
        if (next == covered)
        {
            return false;
        }
        covered = next;
    }
    return false;
}

Foundation::Size SelectVtdRequesters(const Boot::Acpi::AcpiTopology& topology,
                                     const PciFunctionAddress* const functions, const Size count,
                                     Foundation::UInt16* const output, const Size capacity,
                                     const UInt8 addressBits) noexcept
{
    if ((SelectVtdUnit(topology, addressBits).Error != VtdTopologyError::None) ||
        (topology.DmaRemappingUnitCount != 1U) || (functions == nullptr) || (output == nullptr))
    {
        return 0U;
    }
    Size served{0U};
    for (Size index{0U}; (index < count) && (served < capacity); ++index)
    {
        const auto address = functions[index];
        if (!address.IsValid() || (address.GetSegment() != 0U))
        {
            continue;
        }
        bool named = topology.DmaRemappingUnits[0U].Flags != 0U;
        for (Size scope{0U}; !named && (scope < topology.DmaDeviceScopeCount); ++scope)
        {
            const auto& entry = topology.DmaDeviceScopes[scope];
            named = (entry.Unit == 0U) && (entry.Type == Boot::Acpi::AcpiDmaScopeEndpoint) &&
                    (entry.PathEntries == 1U) && (entry.StartBus == address.GetBus()) &&
                    (entry.Device == address.GetDevice()) && (entry.Function == address.GetFunction());
        }
        if (named)
        {
            output[served++] = address.GetRequesterId();
        }
    }
    return served;
}
} // namespace Asteria::Platform::Device
