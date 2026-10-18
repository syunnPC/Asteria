// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_PCI_BAR_PROBE_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_PCI_BAR_PROBE_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Hal/Device/IPciConfigurationWriter.hpp"
#include "Kernel/Device/PciTypes.hpp"

namespace Asteria::Kernel::Device
{
/// A 32-bit memory BAR's assigned base and decoded size.
struct PciMemoryBar
{
    Foundation::UInt64 BaseAddress;
    Foundation::UInt64 LengthBytes;
};

enum class PciBarProbeError : Foundation::UInt8
{
    None,
    InvalidBarIndex,
    ConfigurationReadFailed,
    ConfigurationWriteFailed,
    NotMemory32,
    Unassigned,
    InvalidSize,
};

/// Sizes the 32-bit memory BAR barIndex of the type-zero function at address the standard way,
/// where no firmware reported its extent: with memory decode turned off, it writes all ones to the
/// BAR, reads the size mask back, and restores the BAR and then the Command register, whatever
/// failed. Boot only: nothing else may access the function meanwhile.
[[nodiscard]] Foundation::Result<PciMemoryBar, PciBarProbeError>
ProbePciMemoryBar32(Hal::Device::IPciConfigurationSpace& reader, Hal::Device::IPciConfigurationWriter& writer,
                    PciFunctionAddress address, Foundation::UInt8 barIndex) noexcept;

/// As ProbePciMemoryBar32, accepting an assigned 64-bit memory BAR too.
/// Both halves are restored before decode is restored; bus mastering must
/// already be disabled by the boot's firmware handoff.
[[nodiscard]] Foundation::Result<PciMemoryBar, PciBarProbeError>
ProbePciMemoryBar(Hal::Device::IPciConfigurationSpace& reader, Hal::Device::IPciConfigurationWriter& writer,
                  PciFunctionAddress address, Foundation::UInt8 barIndex) noexcept;

static_assert(sizeof(PciMemoryBar) == 16U);
static_assert(sizeof(PciBarProbeError) == 1U);
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_PCI_BAR_PROBE_HPP_INCLUDED
