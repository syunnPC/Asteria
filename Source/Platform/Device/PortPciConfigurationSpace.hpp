// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_PORT_PCI_CONFIGURATION_SPACE_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_PORT_PCI_CONFIGURATION_SPACE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Hal/Device/IPciConfigurationWriter.hpp"
#include "Hal/Device/IPortIo32.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Platform::Device
{
/// PCI configuration mechanism #1 (the 0xCF8 address and 0xCFC data ports) at run time, on any
/// processor: segment 0 and the first 256 configuration bytes of each function, which hold the
/// Command register and the MSI capability the kernel programs. Extended configuration space is
/// Unsupported. One lock covers each address-then-data pair.
class PortPciConfigurationSpace final : public Hal::Device::IPciConfigurationSpace,
                                        public Hal::Device::IPciConfigurationWriter
{
public:
    static constexpr Foundation::UInt16 AddressPort{0x0CF8U};
    static constexpr Foundation::UInt16 DataPort{0x0CFCU};

    explicit constexpr PortPciConfigurationSpace(Hal::Device::IPortIo32& ports) noexcept : m_ports{ports}
    {
    }

    PortPciConfigurationSpace(const PortPciConfigurationSpace&) = delete;
    PortPciConfigurationSpace& operator=(const PortPciConfigurationSpace&) = delete;

    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(Kernel::Device::PciFunctionAddress address,
                                                                Foundation::UInt16 registerOffset,
                                                                Foundation::UInt32& output) noexcept override;
    [[nodiscard]] Hal::Device::PciConfigurationWriteError Write32(Kernel::Device::PciFunctionAddress address,
                                                                  Foundation::UInt16 registerOffset,
                                                                  Foundation::UInt32 value) noexcept override;

    /// The address-port value of a register, or zero for one this mechanism
    /// cannot reach.
    [[nodiscard]] static Foundation::UInt32 Encode(Kernel::Device::PciFunctionAddress address,
                                                   Foundation::UInt16 registerOffset) noexcept;

private:
    Hal::Device::IPortIo32& m_ports;
    Platform::Synchronization::SpinLock m_synchronization;
};
} // Namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_PORT_PCI_CONFIGURATION_SPACE_HPP_INCLUDED
