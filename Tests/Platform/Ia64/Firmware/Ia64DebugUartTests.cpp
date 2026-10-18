// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Firmware/Ia64DebugUart.hpp"

#include "Support/TestContext.hpp"

namespace
{
using Asteria::Boot::BootDebugPort;
using Asteria::Boot::BootDebugPortSource;
using Asteria::Boot::PhysicalMemoryRegion;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
namespace Firmware = Asteria::Platform::Ia64::Firmware;

constexpr UInt64 PortSpace{0x0FFF'FC00'0000ULL};
constexpr UInt64 PortSpaceBytes{0x0400'0000ULL};
constexpr UInt8 Memory{0U};
constexpr UInt8 Io{1U};

[[nodiscard]] BootDebugPort Port(const UInt8 space, const UInt64 address, const UInt8 stride) noexcept
{
    return {address, 0U, space, 1U, stride, static_cast<UInt8>(BootDebugPortSource::Spcr)};
}

[[nodiscard]] bool Is(const Firmware::Ia64DebugUart& uart, const UInt64 transmit, const UInt64 lineStatus) noexcept
{
    return (uart.TransmitAddress == transmit) && (uart.LineStatusAddress == lineStatus);
}

void TestResolution(Asteria::TestSupport::TestContext& context) noexcept
{
    const Firmware::Ia64PortSpaceWindow window{PortSpace, PortSpaceBytes};
    // SDM rev. 2.3 Vol. 2 Part I 10.7: ports 0x3F8 and 0x3FD in 4-port pages.
    ASTERIA_EXPECT(
        context, "COM1 resolves to its sparse I/O port space addresses",
        Is(Firmware::ResolveIa64DebugUart(Port(Io, 0x3F8U, 1U), window), PortSpace + 0xFE3F8U, PortSpace + 0xFF3FDU));
    ASTERIA_EXPECT(
        context, "a memory UART's registers are its stride apart",
        Is(Firmware::ResolveIa64DebugUart(Port(Memory, 0x47'F000'1000U, 1U), {}), 0x47'F000'1000U, 0x47'F000'1005U),
        Is(Firmware::ResolveIa64DebugUart(Port(Memory, 0xFE00'1000U, 4U), {}), 0xFE00'1000U, 0xFE00'1014U),
        Is(Firmware::ResolveIa64DebugUart(Firmware::Ia64PlatformDefaultDebugPort, {}),
           Firmware::Ia64PlatformDefaultDebugUart.TransmitAddress,
           Firmware::Ia64PlatformDefaultDebugUart.LineStatusAddress));
    ASTERIA_EXPECT(
        context, "an I/O port needs a whole aligned port space window",
        Is(Firmware::ResolveIa64DebugUart(Port(Io, 0x3F8U, 1U), {}), 0U, 0U),
        Is(Firmware::ResolveIa64DebugUart(Port(Io, 0x3F8U, 1U), {PortSpace, PortSpaceBytes - 4096U}), 0U, 0U),
        Is(Firmware::ResolveIa64DebugUart(Port(Io, 0x3F8U, 1U), {PortSpace + 4096U, PortSpaceBytes}), 0U, 0U),
        Is(Firmware::ResolveIa64DebugUart(Port(Io, 0x3F8U, 1U), {(UInt64{1U} << 63U), PortSpaceBytes}), 0U, 0U));
    ASTERIA_EXPECT(context, "no port, or an invalid record, names no UART",
                   Is(Firmware::ResolveIa64DebugUart({}, window), 0U, 0U),
                   Is(Firmware::ResolveIa64DebugUart(Port(Io, 0xFFFCU, 1U), window), 0U, 0U),
                   Is(Firmware::ResolveIa64DebugUart(Port(Memory, 0x1000U, 3U), window), 0U, 0U));
}

void TestWindow(Asteria::TestSupport::TestContext& context) noexcept
{
    PhysicalMemoryRegion regions[3U]{
        {0x0U, 0x0U, 0x8000U, 0x8U, 7U, Asteria::Boot::PhysicalMemoryKind::Conventional},
        {PortSpace, 0x0U, PortSpaceBytes / 4096U, 0x1U, Firmware::Ia64PortSpaceFirmwareType,
         Asteria::Boot::PhysicalMemoryKind::MemoryMappedIo},
        {0xFED0'0000U, 0x0U, 1U, 0x1U, 11U, Asteria::Boot::PhysicalMemoryKind::MemoryMappedIo},
    };
    const Firmware::Ia64PortSpaceWindow found = Firmware::FindIa64PortSpaceWindow(regions, 3U);
    ASTERIA_EXPECT(context, "the one EfiMemoryMappedIOPortSpace region is the window", found.Base == PortSpace,
                   found.Bytes == PortSpaceBytes);
    regions[2U].FirmwareType = Firmware::Ia64PortSpaceFirmwareType;
    const Firmware::Ia64PortSpaceWindow ambiguous = Firmware::FindIa64PortSpaceWindow(regions, 3U);
    const Firmware::Ia64PortSpaceWindow none = Firmware::FindIa64PortSpaceWindow(regions, 1U);
    ASTERIA_EXPECT(context, "two windows, or none, give no window", ambiguous.Base == 0U, ambiguous.Bytes == 0U,
                   none.Base == 0U, none.Bytes == 0U, Firmware::FindIa64PortSpaceWindow(nullptr, 3U).Base == 0U);
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"Ia64DebugUart"};
    TestResolution(context);
    TestWindow(context);
    return context.Finish();
}
