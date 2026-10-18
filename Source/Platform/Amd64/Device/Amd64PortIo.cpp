// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Device/Amd64PortIo.hpp"

namespace Asteria::Platform::Amd64::Device
{
Foundation::UInt32 Amd64PortIo::Read(const Foundation::UInt16 port, const Foundation::UInt8 width) noexcept
{
    if (width == 1U)
    {
        Foundation::UInt8 value{0U};
        __asm__ __volatile__("inb %1,%0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }
    if (width == 2U)
    {
        Foundation::UInt16 value{0U};
        __asm__ __volatile__("inw %1,%0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }
    return Read32(port);
}

void Amd64PortIo::Write(const Foundation::UInt16 port, const Foundation::UInt8 width,
                        const Foundation::UInt32 value) noexcept
{
    if (width == 1U)
    {
        __asm__ __volatile__("outb %0,%1" : : "a"(static_cast<Foundation::UInt8>(value)), "Nd"(port) : "memory");
    }
    else if (width == 2U)
    {
        __asm__ __volatile__("outw %0,%1" : : "a"(static_cast<Foundation::UInt16>(value)), "Nd"(port) : "memory");
    }
    else
    {
        Write32(port, value);
    }
}

Foundation::UInt32 Amd64PortIo::Read32(const Foundation::UInt16 port) noexcept
{
    Foundation::UInt32 value{0U};
    __asm__ __volatile__("inl %1,%0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

void Amd64PortIo::Write32(const Foundation::UInt16 port, const Foundation::UInt32 value) noexcept
{
    __asm__ __volatile__("outl %0,%1" : : "a"(value), "Nd"(port) : "memory");
}
} // namespace Asteria::Platform::Amd64::Device
