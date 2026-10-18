// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_DEVICE_I_PORT_IO_HPP_INCLUDED
#define ASTERIA_HAL_DEVICE_I_PORT_IO_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Hal::Device
{
/// Ordered byte, word and doubleword port operations. Caller validates width
/// (1, 2, 4), range and authority, and serializes on the BSP with IRQs disabled.
class IPortIo
{
public:
    [[nodiscard]] virtual Foundation::UInt32 Read(Foundation::UInt16 port, Foundation::UInt8 width) noexcept = 0;
    virtual void Write(Foundation::UInt16 port, Foundation::UInt8 width, Foundation::UInt32 value) noexcept = 0;

protected:
    constexpr IPortIo() noexcept = default;
    ~IPortIo() noexcept = default;
};
}
#endif
