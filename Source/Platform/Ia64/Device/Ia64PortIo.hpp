// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_DEVICE_IA64_PORT_IO_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_DEVICE_IA64_PORT_IO_HPP_INCLUDED

#include "Hal/Device/IPortIo.hpp"

namespace Asteria::Platform::Ia64::Device
{
class Ia64PortIo final : public Hal::Device::IPortIo
{
public:
    constexpr Ia64PortIo() noexcept = default;
    /// Boot-only, once: admit a nonzero 64 MiB aligned firmware port-space
    /// base and a complete 64 MiB window below physical-address bit 63.
    /// SDM rev. 2.3 Vol. 2 Part I 10.7; UEFI 2.11 7.2.3.
    [[nodiscard]] bool Configure(Foundation::UInt64 firmwareBase, Foundation::UInt64 windowBytes) noexcept;
    /// Whether Configure admitted a firmware port window.
    [[nodiscard]] bool IsConfigured() const noexcept
    {
        return m_base != 0U;
    }
    [[nodiscard]] static constexpr Foundation::UInt64 SparseAddress(const Foundation::UInt64 base,
                                                                    const Foundation::UInt16 port) noexcept
    {
        return base + ((static_cast<Foundation::UInt64>(port >> 2U) << 12U) | (port & 0xFFFU));
    }
    /// Exact-width aligned 1/2/4-byte transactions, through the physical UC
    /// alias with data translation disabled and mf.a acceptance after access.
    /// The native leaf preserves PSR/RSE state and uses no translation register.
    /// Unconfigured or invalid reads float; invalid writes do nothing.
    [[nodiscard]] Foundation::UInt32 Read(Foundation::UInt16 port, Foundation::UInt8 width) noexcept override;
    void Write(Foundation::UInt16 port, Foundation::UInt8 width, Foundation::UInt32 value) noexcept override;

private:
    Foundation::UInt64 m_base{0U};
};
} // namespace Asteria::Platform::Ia64::Device
#endif
