// SPDX-License-Identifier: Apache-2.0

#include "Platform/Device/PortPciConfigurationSpace.hpp"

namespace Asteria::Platform::Device
{
namespace
{
constexpr Foundation::UInt32 EnableBit{0x8000'0000U};
constexpr Foundation::UInt16 MechanismLimit{0x100U};
} // namespace

Foundation::UInt32 PortPciConfigurationSpace::Encode(const Kernel::Device::PciFunctionAddress address,
                                                     const Foundation::UInt16 registerOffset) noexcept
{
    if (!address.IsValid() || (address.GetSegment() != 0U) || (registerOffset >= MechanismLimit) ||
        ((registerOffset & 3U) != 0U))
    {
        return 0U;
    }
    return EnableBit | (static_cast<Foundation::UInt32>(address.GetBus()) << 16U) |
           (static_cast<Foundation::UInt32>(address.GetDevice()) << 11U) |
           (static_cast<Foundation::UInt32>(address.GetFunction()) << 8U) | registerOffset;
}

Hal::Device::PciConfigurationReadError
PortPciConfigurationSpace::Read32(const Kernel::Device::PciFunctionAddress address,
                                  const Foundation::UInt16 registerOffset, Foundation::UInt32& output) noexcept
{
    if (!address.IsValid() || ((registerOffset & 3U) != 0U) || (registerOffset > 0xFFCU))
    {
        return Hal::Device::PciConfigurationReadError::InvalidAddress;
    }
    const Foundation::UInt32 encoded = Encode(address, registerOffset);
    if (encoded == 0U)
    {
        return Hal::Device::PciConfigurationReadError::Unsupported;
    }
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    m_ports.Write32(AddressPort, encoded);
    output = m_ports.Read32(DataPort);
    return Hal::Device::PciConfigurationReadError::None;
}

Hal::Device::PciConfigurationWriteError
PortPciConfigurationSpace::Write32(const Kernel::Device::PciFunctionAddress address,
                                   const Foundation::UInt16 registerOffset, const Foundation::UInt32 value) noexcept
{
    if (!address.IsValid() || ((registerOffset & 3U) != 0U) || (registerOffset > 0xFFCU))
    {
        return Hal::Device::PciConfigurationWriteError::InvalidAddress;
    }
    const Foundation::UInt32 encoded = Encode(address, registerOffset);
    if (encoded == 0U)
    {
        return Hal::Device::PciConfigurationWriteError::Unsupported;
    }
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    m_ports.Write32(AddressPort, encoded);
    m_ports.Write32(DataPort, value);
    return Hal::Device::PciConfigurationWriteError::None;
}
} // namespace Asteria::Platform::Device
