// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Firmware/FirmwarePortIo.hpp"

namespace Asteria::Platform::Amd64::Firmware
{
Foundation::UInt32 ReadFirmwarePort32(const Foundation::UInt16 port) noexcept
{
    Foundation::UInt32 value{0U};
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
} // namespace Asteria::Platform::Amd64::Firmware
