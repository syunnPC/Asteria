// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_BOOT_AMD64_PROCESSOR_TOPOLOGY_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_BOOT_AMD64_PROCESSOR_TOPOLOGY_HPP_INCLUDED

#include "Boot/Acpi/AcpiTopology.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"

namespace Asteria::Platform::Amd64::Bootstrap
{
enum class Amd64ProcessorTopologyError : Foundation::UInt8
{
    None,
    /// A record with nonzero reserved bits or extended ID, a controller other
    /// than a local xAPIC or x2APIC, or flags other than exactly Enabled or
    /// exactly OnlineCapable.
    InvalidRecord,
    /// An enabled ID exceeds the chosen mode's destination width or is broadcast.
    UnaddressableProcessor,
    /// Two enabled records name the same APIC ID.
    DuplicateProcessor,
    /// No enabled record names the executing BSP.
    BspNotEnabled,
    /// More enabled processors than the runtime has slots for.
    TooManyProcessors,
};

/// The processors the AMD64 runtime starts, in logical order: the BSP as
/// logical CPU 0, then every other enabled processor in firmware (MADT) order.
struct Amd64ProcessorSelection
{
    Foundation::UInt32 HardwareIds[Kernel::Scheduler::MaximumProcessorCount];
    Foundation::Size Count;
};

/// The number of records flagged Enabled, which a valid selection admits.
[[nodiscard]] Foundation::Size
CountEnabledAmd64Processors(const ::Asteria::Boot::Acpi::AcpiTopology& topology) noexcept;

/// Validates every processor record of `topology` and selects the enabled
/// ones, the BSP (`bspApicId`, the executing processor's APIC ID) first. An
/// online-capable record that is not enabled is a processor that may be
/// added later: it is validated and omitted, as on IA-64, since this runtime
/// starts only the processors present at boot and has no hotplug. On failure
/// `selection` is unchanged. maximumApicId is 254 for xAPIC and 0xFFFFFFFE
/// for x2APIC; broadcast is always refused. Pure computation: no allocation, no hardware
/// access, callable before or after firmware exit.
[[nodiscard]] Amd64ProcessorTopologyError SelectAmd64Processors(const ::Asteria::Boot::Acpi::AcpiTopology& topology,
                                                                Foundation::UInt32 bspApicId,
                                                                Amd64ProcessorSelection& selection,
                                                                Foundation::UInt32 maximumApicId = 254U) noexcept;
} // namespace Asteria::Platform::Amd64::Bootstrap

#endif // ASTERIA_PLATFORM_AMD64_BOOT_AMD64_PROCESSOR_TOPOLOGY_HPP_INCLUDED
