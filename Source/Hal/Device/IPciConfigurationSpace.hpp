// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_DEVICE_I_PCI_CONFIGURATION_SPACE_HPP_INCLUDED
#define ASTERIA_HAL_DEVICE_I_PCI_CONFIGURATION_SPACE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Device/PciTypes.hpp"

namespace Asteria::Hal::Device
{
enum class PciConfigurationReadError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidAddress,
    Unsupported,
    HardwareFailure,
};

/// Supplies normalized, read-only PCI configuration cycles.
///
/// A successful Read32 returns one little-endian configuration DWORD in host
/// integer form. A nonexistent function returns 0xFFFF'FFFF with success.
/// Failure leaves output unchanged. Implementations validate the BDF and the
/// naturally aligned 0-0xFFC register offset before touching hardware.
/// Calls are internally serialized, bounded, allocation-free, callback-free,
/// and never enter the caller. They may include platform ordering operations,
/// may spin, and are forbidden from an ISR or same-CPU reentry.
class IPciConfigurationSpace
{
public:
    [[nodiscard]] virtual PciConfigurationReadError Read32(Kernel::Device::PciFunctionAddress address,
                                                           Foundation::UInt16 registerOffset,
                                                           Foundation::UInt32& output) noexcept = 0;

protected:
    constexpr IPciConfigurationSpace() noexcept = default;
    ~IPciConfigurationSpace() noexcept = default;
};

static_assert(sizeof(PciConfigurationReadError) == 1U);
} // namespace Asteria::Hal::Device

#endif // ASTERIA_HAL_DEVICE_I_PCI_CONFIGURATION_SPACE_HPP_INCLUDED
