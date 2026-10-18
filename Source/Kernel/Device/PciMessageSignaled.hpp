// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_PCI_MESSAGE_SIGNALED_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_PCI_MESSAGE_SIGNALED_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Hal/Device/IPciConfigurationWriter.hpp"
#include "Kernel/Device/PciTypes.hpp"

namespace Asteria::Kernel::Device
{
/// Where a function's MSI capability (ID 05) lies and its layout: 64-bit
/// message addresses put the data at +12 rather than +8, and per-vector
/// masking adds a mask word after the data.
struct PciMessageCapability
{
    Foundation::UInt16 Offset;
    bool Wide;
    bool PerVectorMask;
    Foundation::UInt8 Reserved[4];
};

/// Walks the function's capability list, at most 48 entries, each
/// dword-aligned at 0x40 or above; false when it has no MSI capability.
[[nodiscard]] bool FindPciMessageCapability(Hal::Device::IPciConfigurationSpace& reader, PciFunctionAddress address,
                                            PciMessageCapability& capability) noexcept;

/// Programs one message: the address and data, every per-vector mask bit clear, then Command bit 10
/// (INTx Disable), and only then the MSI enable bit with one message, so the function never raises
/// INTx and an MSI at once. Normal context.
[[nodiscard]] bool EnablePciMessages(Hal::Device::IPciConfigurationSpace& reader,
                                     Hal::Device::IPciConfigurationWriter& writer, PciFunctionAddress address,
                                     const PciMessageCapability& capability, Foundation::UInt64 messageAddress,
                                     Foundation::UInt16 messageData) noexcept;

/// Clears the MSI enable bit and leaves INTx Disable set, so the function
/// raises nothing.
[[nodiscard]] bool DisablePciMessages(Hal::Device::IPciConfigurationSpace& reader,
                                      Hal::Device::IPciConfigurationWriter& writer, PciFunctionAddress address,
                                      const PciMessageCapability& capability) noexcept;

static_assert(sizeof(PciMessageCapability) == 8U);
} // Namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_PCI_MESSAGE_SIGNALED_HPP_INCLUDED
