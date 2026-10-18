// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_DEVICE_I_PORT_IO32_HPP_INCLUDED
#define ASTERIA_HAL_DEVICE_I_PORT_IO32_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Hal::Device
{
/// Doubleword I/O-port accesses of a processor that has an I/O space
/// (AMD64). Each call is one ordered port instruction; callers serialize.
class IPortIo32
{
public:
    [[nodiscard]] virtual Foundation::UInt32 Read32(Foundation::UInt16 port) noexcept = 0;
    virtual void Write32(Foundation::UInt16 port, Foundation::UInt32 value) noexcept = 0;

protected:
    constexpr IPortIo32() noexcept = default;
    ~IPortIo32() noexcept = default;
};
} // namespace Asteria::Hal::Device

#endif // ASTERIA_HAL_DEVICE_I_PORT_IO32_HPP_INCLUDED
