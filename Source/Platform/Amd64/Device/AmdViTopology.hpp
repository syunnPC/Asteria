// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_TOPOLOGY_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_TOPOLOGY_HPP_INCLUDED

#include "Platform/Device/VtdTopology.hpp"

namespace Asteria::Platform::Amd64::Device
{
// The routing plan and refusal vocabulary AMD-Vi shares with VT-d.
using Platform::Device::VtdFunctionPolicy;
using Platform::Device::VtdFunctionRouting;
using Platform::Device::VtdTopologyError;
using Platform::Device::VtdTopologyPlan;
using Platform::Device::VtdTopologySelection;
using Platform::Device::IsBehindPciBridge;
using Platform::Device::VtdUnit;

/// Reuses the architecture's endpoint/reservation plan and refusal vocabulary.
/// Exactly one unique segment-0 unit is supported; alternate IVHD versions of
/// that unit are selected by the IVRS decoder. Aliased RIDs are never assignable
/// independently. IOAPIC/HPET/HID records create no PCI assignment authority.
[[nodiscard]] VtdTopologySelection SelectAmdViUnit(const Boot::Acpi::IvrsTopology& topology) noexcept;
[[nodiscard]] VtdTopologyError ResolveAmdViTopology(const Boot::Acpi::IvrsTopology& topology,
                                                    Hal::Device::IPciConfigurationSpace& configuration,
                                                    const Kernel::Device::PciFunctionAddress* functions,
                                                    Foundation::Size count, VtdTopologyPlan& output) noexcept;
/// Firmware contexts and device-table extent use requester IDs even without
/// a captured PCI function. These helpers never create assignment authority.
[[nodiscard]] Foundation::UInt8 AmdViReservedRegions(const Boot::Acpi::IvrsTopology& topology,
                                                     Foundation::UInt16 requester) noexcept;
[[nodiscard]] Foundation::Size AmdViDevicePages(const Boot::Acpi::IvrsTopology& topology,
                                                const Kernel::Device::PciFunctionAddress* functions,
                                                Foundation::Size count) noexcept;
} // namespace Asteria::Platform::Amd64::Device

#endif // ASTERIA_PLATFORM_AMD64_DEVICE_AMD_VI_TOPOLOGY_HPP_INCLUDED
