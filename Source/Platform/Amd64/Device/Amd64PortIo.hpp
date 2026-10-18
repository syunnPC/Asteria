// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_PORT_IO_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_PORT_IO_HPP_INCLUDED

#include "Hal/Device/IPortIo32.hpp"
#include "Hal/Device/IPortIo.hpp"

namespace Asteria::Platform::Amd64::Device
{
/// The processor's `in` and `out` doubleword instructions.
class Amd64PortIo final : public Hal::Device::IPortIo32, public Hal::Device::IPortIo
{
public:
    constexpr Amd64PortIo() noexcept = default;

    [[nodiscard]] Foundation::UInt32 Read(Foundation::UInt16 port, Foundation::UInt8 width) noexcept override;
    void Write(Foundation::UInt16 port, Foundation::UInt8 width, Foundation::UInt32 value) noexcept override;

    [[nodiscard]] Foundation::UInt32 Read32(Foundation::UInt16 port) noexcept override;
    void Write32(Foundation::UInt16 port, Foundation::UInt32 value) noexcept override;
};
} // namespace Asteria::Platform::Amd64::Device

#endif // ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_PORT_IO_HPP_INCLUDED
