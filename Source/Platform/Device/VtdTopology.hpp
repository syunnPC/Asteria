// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_VTD_TOPOLOGY_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_VTD_TOPOLOGY_HPP_INCLUDED

#include "Boot/BootInfo.hpp"
#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Platform/Device/VtdUnit.hpp"

namespace Asteria::Platform::Device
{
enum class VtdTopologyError : Foundation::UInt8
{
    None,
    NoDmar,
    UnitCount,
    Segment,
    Flags,
    ReservedMemory,
    ScopeCapacity,
    ScopePath,
    ScopeOverlap,
    IncludeAllOverlap,
    RegisterRange,
    FunctionCapacity,
    UnservedReservedMemory,
    Hardware,
};

struct VtdTopologySelection
{
    Foundation::UInt64 RegisterBase;
    VtdTopologyError Error;
    Foundation::UInt8 Reserved[7];
};

enum class VtdFunctionPolicy : Foundation::UInt8
{
    Assignable,
    Display,
    Bridge,
    ReservedMemory,
    Unserved,
};

struct VtdFunctionRouting
{
    Kernel::Device::PciFunctionAddress Address;
    /// MaximumAcpiDmaRemappingUnits denotes no owning unit.
    Foundation::UInt8 Unit;
    VtdFunctionPolicy Policy;
    /// One bit per retained RMRR; a device receives only its own ranges.
    Foundation::UInt8 Regions;
    Foundation::UInt8 Reserved[3];
};

struct VtdTopologyPlan
{
    VtdFunctionRouting Functions[VtdUnit::MaximumRequesters];
    Foundation::Size Count;
    bool KeepDisabled[Boot::Acpi::MaximumAcpiDmaRemappingUnits];
    bool DisplayConflict[Boot::Acpi::MaximumAcpiDmaRemappingUnits];
    bool NoFunctions[Boot::Acpi::MaximumAcpiDmaRemappingUnits];
    Foundation::UInt32 IgnoredScopes;
};

/// Validates all bounded DRHD/RMRR records, including segments, flags,
/// register sizes and uniqueness. RegisterBase is the first unit's base.
/// addressBits (32-63) is the ISA adapter's limit for register sets and
/// RMRRs: AMD64 keeps its 48-bit physical window; IA-64 passes the smaller of
/// the DMAR host address width and the PAL physical address size. Any other
/// width refuses the topology (RegisterRange).
[[nodiscard]] VtdTopologySelection SelectVtdUnit(const Boot::Acpi::AcpiTopology& topology,
                                                 Foundation::UInt8 addressBits) noexcept;

/// Intel VT-d 8.3: Size[3:0] encodes 2^N 4 KiB pages; reserved bits
/// must be zero. Returns zero for an invalid size encoding.
[[nodiscard]] Foundation::UInt64 VtdRegisterBytes(const Boot::Acpi::AcpiDmaRemappingUnit& unit) noexcept;

/// BSP boot only, before translation or user assignment. Resolves endpoint
/// and bridge paths through ECAM (including bridge secondary/subordinate bus
/// numbers). Explicit scopes win over INCLUDE_PCI_ALL. Ambiguous scopes or
/// a broken intermediate bridge refuse the topology. An absent final hop is
/// skipped and counted; a final bridge without a bus range (secondary and
/// subordinate both zero) names the bridge alone. Even on refusal, Count/Regions identify firmware
/// reservations; ambiguous RMRR resolution conservatively reserves every
/// captured function. No hardware or authority is changed. Bounded, allocation-free.
/// addressBits is SelectVtdUnit's.
[[nodiscard]] VtdTopologyError ResolveVtdTopology(const Boot::Acpi::AcpiTopology& topology,
                                                  Hal::Device::IPciConfigurationSpace& ecam,
                                                  const Kernel::Device::PciFunctionAddress* functions,
                                                  Foundation::Size count, VtdTopologyPlan& output,
                                                  Foundation::UInt8 addressBits) noexcept;

/// Conventional PCI downstream of a PCIe-to-PCI bridge shares a requester.
/// Such endpoints cannot receive independent single-function DMA authority.
[[nodiscard]] bool IsBehindPciBridge(Hal::Device::IPciConfigurationSpace& configuration,
                                     Kernel::Device::PciFunctionAddress function,
                                     const Kernel::Device::PciFunctionAddress* functions,
                                     Foundation::Size count) noexcept;

/// Ordinary endpoints only: preserve bridge forwarding, SMM-owned DMA and
/// firmware display scanout. No configuration reads or writes.
[[nodiscard]] bool ShouldSweepVtdFunction(const VtdFunctionRouting& function, const VtdTopologyPlan& plan) noexcept;

/// A firmware range may span memory-map gaps; it must not overlap any RAM
/// the kernel can allocate, including loader/boot-services and reclaimable RAM.
[[nodiscard]] bool VtdFirmwareRangeIsSafe(const Boot::BootInfo& info, Foundation::UInt64 base,
                                          Foundation::UInt64 limit) noexcept;

/// Two kernel-owned physical ranges the UEFI map may not show as LoaderData:
/// AMD64's runtime pool and startup pages; IA-64's executing image copy and
/// run-time memory reservation (Ia64VtdInput).
struct VtdFirmwareMemoryOwnership
{
    Foundation::UInt64 PoolStart;
    Foundation::UInt64 PoolPages;
    Foundation::UInt64 StartupStart;
    Foundation::UInt64 StartupPages;
};

/// Firmware QI writes require complete UEFI-map coverage by BootServices,
/// RuntimeServices, Reserved or ACPI NVS memory, excluding all kernel-owned
/// pages (including LoaderData, retained files/modules and inventory).
/// Inclusive bounds; malformed maps or ownership ranges refuse the write.
/// BSP boot policy, bounded by MaximumMemoryRegions squared, allocation-free.
[[nodiscard]] bool VtdFirmwareQueueRangeIsWritable(const Boot::BootInfo& info, Foundation::UInt64 base,
                                                   Foundation::UInt64 limit,
                                                   const VtdFirmwareMemoryOwnership& ownership) noexcept;

/// Compatibility helper for single-unit, one-hop topology clients. The boot
/// uses ResolveVtdTopology for complete ECAM routing.
[[nodiscard]] Foundation::Size SelectVtdRequesters(const Boot::Acpi::AcpiTopology& topology,
                                                   const Kernel::Device::PciFunctionAddress* functions,
                                                   Foundation::Size count, Foundation::UInt16* output,
                                                   Foundation::Size capacity, Foundation::UInt8 addressBits) noexcept;
} // namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_VTD_TOPOLOGY_HPP_INCLUDED
