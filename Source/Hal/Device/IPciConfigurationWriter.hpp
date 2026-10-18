// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_DEVICE_I_PCI_CONFIGURATION_WRITER_HPP_INCLUDED
#define ASTERIA_HAL_DEVICE_I_PCI_CONFIGURATION_WRITER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Device/PciTypes.hpp"

namespace Asteria::Hal::Device
{
enum class PciConfigurationWriteError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidAddress,
    Unsupported,
    HardwareFailure,
};

/// Supplies normalized PCI configuration writes, which only the kernel issues: drivers never get
/// raw configuration access. A successful Write32 stores one configuration DWORD given in host
/// integer form. Implementations validate the BDF and the naturally aligned register offset before
/// touching hardware. Calls are internally serialized, bounded, allocation-free, callback-free, and
/// never enter the caller; they may spin and are forbidden from an ISR or same-CPU reentry.
class IPciConfigurationWriter
{
public:
    [[nodiscard]] virtual PciConfigurationWriteError Write32(Kernel::Device::PciFunctionAddress address,
                                                             Foundation::UInt16 registerOffset,
                                                             Foundation::UInt32 value) noexcept = 0;

protected:
    constexpr IPciConfigurationWriter() noexcept = default;
    ~IPciConfigurationWriter() noexcept = default;
};

static_assert(sizeof(PciConfigurationWriteError) == 1U);
} // Namespace Asteria::Hal::Device

#endif // ASTERIA_HAL_DEVICE_I_PCI_CONFIGURATION_WRITER_HPP_INCLUDED
