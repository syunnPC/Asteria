// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Device/Ia64PortIo.hpp"

#if defined(__ia64__)
extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadPhysicalPort(Asteria::Foundation::UInt64,
                                                                   Asteria::Foundation::UInt64) noexcept;
extern "C" void AsteriaIa64WritePhysicalPort(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64,
                                             Asteria::Foundation::UInt64) noexcept;
#endif

namespace Asteria::Platform::Ia64::Device
{
bool Ia64PortIo::Configure(const Foundation::UInt64 firmwareBase, const Foundation::UInt64 windowBytes) noexcept
{
    // The full 16-bit sparse namespace needs 64 MiB below the physical UC alias bit.
    constexpr Foundation::UInt64 window{0x0400'0000U};
    if ((m_base != 0U) || (firmwareBase == 0U) || ((firmwareBase & (window - 1U)) != 0U) || (windowBytes < window) ||
        (firmwareBase > (Foundation::UInt64{1U} << 63U) - window))
    {
        return false;
    }
    m_base = firmwareBase;
    return true;
}
Foundation::UInt32 Ia64PortIo::Read(const Foundation::UInt16 port, const Foundation::UInt8 width) noexcept
{
    if ((m_base == 0U) || ((width != 1U) && (width != 2U) && (width != 4U)) || ((port & (width - 1U)) != 0U))
    {
        if (width == 1U)
        {
            return 0xFFU;
        }
        if (width == 2U)
        {
            return 0xFFFFU;
        }
        return 0xFFFF'FFFFU;
    }
#if defined(__ia64__)
    return static_cast<Foundation::UInt32>(AsteriaIa64ReadPhysicalPort(SparseAddress(m_base, port), width));
#else
    const Foundation::UInt64 address = SparseAddress(m_base, port);
    if (width == 1U)
    {
        return *reinterpret_cast<const volatile Foundation::UInt8*>(address);
    }
    if (width == 2U)
    {
        return *reinterpret_cast<const volatile Foundation::UInt16*>(address);
    }
    return *reinterpret_cast<const volatile Foundation::UInt32*>(address);
#endif
}
void Ia64PortIo::Write(const Foundation::UInt16 port, const Foundation::UInt8 width,
                       const Foundation::UInt32 value) noexcept
{
    if ((m_base == 0U) || ((width != 1U) && (width != 2U) && (width != 4U)) || ((port & (width - 1U)) != 0U))
    {
        return;
    }
#if defined(__ia64__)
    AsteriaIa64WritePhysicalPort(SparseAddress(m_base, port), width, value);
#else
    const Foundation::UInt64 address = SparseAddress(m_base, port);
    if (width == 1U)
    {
        *reinterpret_cast<volatile Foundation::UInt8*>(address) = static_cast<Foundation::UInt8>(value);
    }
    else if (width == 2U)
    {
        *reinterpret_cast<volatile Foundation::UInt16*>(address) = static_cast<Foundation::UInt16>(value);
    }
    else
    {
        *reinterpret_cast<volatile Foundation::UInt32*>(address) = value;
    }
#endif
}
}
