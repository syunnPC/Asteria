// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Device/Ia64PortIo.hpp"

#include "Support/TestContext.hpp"

int main()
{
    Asteria::TestSupport::TestContext context{"IA-64 firmware port window and sparse encoding"};
    using Asteria::Foundation::UInt64;
    using Asteria::Platform::Ia64::Device::Ia64PortIo;
    constexpr UInt64 base{0x1'0000'0000ULL};
    constexpr UInt64 window{0x400'0000U};
    for (Asteria::Foundation::UInt32 port{0U}; port < 65536U; ++port)
    {
        const UInt64 address = Ia64PortIo::SparseAddress(base, static_cast<Asteria::Foundation::UInt16>(port));
        ASTERIA_EXPECT(context, "SDM 10.7.1/2: sparse address preserves decode and bus byte lanes", address >= base,
                       address < base + window, ((address >> 12U) & 0x3FFFU) == (port >> 2U),
                       (address & 0xFFFU) == (port & 0xFFFU),
                       ((address >> 12U) & 0x3FFU) == ((address >> 2U) & 0x3FFU));
    }
    Ia64PortIo ports;
    ASTERIA_EXPECT(context,
                   "unavailable firmware ports float; reject zero, partial, misaligned and overflowing windows",
                   ports.Read(0x64U, 1U) == 0xFFU, !ports.Configure(0U, window), !ports.Configure(base, window - 1U),
                   !ports.Configure(base + 4U, window), !ports.Configure((UInt64{1U} << 63U) - 4U, window),
                   ports.Configure(base, window), !ports.Configure(base, window));
    // These invalid operations must return before dereferencing the synthetic base.
    ports.Write(0x61U, 2U, 0U);
    ports.Write(0x64U, 8U, 0U);
    ASTERIA_EXPECT(context, "no unaligned or oversized hardware transaction", ports.Read(0x61U, 2U) == 0xFFFFU,
                   ports.Read(0x64U, 8U) == 0xFFFF'FFFFU);
    return context.Finish();
}
