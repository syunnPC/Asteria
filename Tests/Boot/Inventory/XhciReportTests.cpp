// SPDX-License-Identifier: Apache-2.0
#include "Inventory/XhciReport.hpp"

#include <cstring>
#include <memory>

#include "Support/TestContext.hpp"

namespace
{
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
class Registers final : public Asteria::Inventory::IXhciReportRegisters
{
public:
    static constexpr UInt64 Bar{0x30'142E'0000ULL};
    [[nodiscard]] UInt32 Read32(const UInt32 offset) noexcept override
    {
        ++Reads;
        LastAddress = Bar + offset;
        if ((offset >= sizeof(Words)) || ((offset & 3U) != 0U))
        {
            Good = false;
            return 0xFFFF'FFFFU;
        }
        return Words[offset / 4U];
    }
    UInt32 Words[0x4000U / 4U]{};
    UInt32 Reads{0U};
    UInt64 LastAddress{0U};
    bool Good{true};
};
} // namespace
int main()
{
    Asteria::TestSupport::TestContext context{"XhciReportTests"};
    auto registers = std::make_unique<Registers>();
    registers->Words[0U] = 0x01200080U;
    registers->Words[1U] = (255U << 24U) | 16U;
    registers->Words[2U] = 1U << 21U;
    registers->Words[4U] = (0x40U << 16U) | 13U;
    registers->Words[5U] = 0x3000U;
    registers->Words[6U] = 0x2000U;
    registers->Words[7U] = 0x1234U;
    registers->Words[0x84U / 4U] = 0U;
    registers->Words[0x88U / 4U] = 1U;
    registers->Words[0x100U / 4U] = 0x01010000U | (4U << 8U) | 1U;
    registers->Words[0x104U / 4U] = 0xE011U;
    registers->Words[0x110U / 4U] = (2U << 24U) | (4U << 8U) | 2U;
    registers->Words[0x114U / 4U] = 0x20425355U;
    registers->Words[0x118U / 4U] = (128U << 8U) | 1U;
    registers->Words[0x120U / 4U] = (3U << 24U) | 2U;
    registers->Words[0x124U / 4U] = 0x20425355U;
    registers->Words[0x128U / 4U] = (127U << 8U) | 129U;
    registers->Words[(0x480U + 254U * 16U) / 4U] = 0x1203U;
    char text[4096U]{};
    Asteria::Inventory::ReportText report{text, sizeof(text) - 1U};
    Asteria::Inventory::FormatXhci(report, *registers, 0x4000U);
    ASTERIA_EXPECT(context, "raw capability/operational snapshot with full physical BAR address", registers->Good,
                   registers->LastAddress > 0xFFFF'FFFFU, !report.Truncated(),
                   std::strstr(text, "CAPLENGTH=0x80 HCIVERSION=0x0120 HCSPARAMS1=0xff000010") != nullptr,
                   std::strstr(text, "HCCPARAMS2=0x00001234 DBOFF=0x00003000 RTSOFF=0x00002000") != nullptr,
                   std::strstr(text, "PAGESIZE=0x00000001 USBSTS=0x00000000") != nullptr,
                   std::strstr(text, "USBLEGSUP=0x01010401 USBLEGCTLSTS=0x0000e011 BIOS=1 OS=1") != nullptr,
                   std::strstr(text, "revision=2.00 port-offset=1 port-count=128") != nullptr,
                   std::strstr(text, "revision=3.00 port-offset=129 port-count=127") != nullptr,
                   std::strstr(text, "port=255 PORTSC=0x00001203") != nullptr);
    // Unavailable ports must never be mistaken for 255 connected devices.
    registers->Words[0U] = 0xFFFF'FFFFU;
    registers->Reads = 0U;
    char unavailable[1024U]{};
    Asteria::Inventory::ReportText absent{unavailable, sizeof(unavailable) - 1U};
    Asteria::Inventory::FormatXhci(absent, *registers, 0x4000U);
    ASTERIA_EXPECT(context, "all-ones capability reads stop before derived MMIO", registers->Reads == 8U,
                   std::strstr(unavailable, "all-ones capability read") != nullptr);
    registers->Reads = 0U;
    Asteria::Inventory::ReportText shortBar{unavailable, sizeof(unavailable)};
    Asteria::Inventory::FormatXhci(shortBar, *registers, 28U);
    ASTERIA_EXPECT(context, "truncated aperture is never read", registers->Reads == 0U);
    registers->Words[0U] = 0x01200080U;
    for (UInt32 port{0U}; port < 255U; ++port)
    {
        registers->Words[(0x480U + port * 16U) / 4U] = 0xFFFF'FFFFU;
    }
    char ports[4096U]{};
    Asteria::Inventory::ReportText allOnes{ports, sizeof(ports) - 1U};
    Asteria::Inventory::FormatXhci(allOnes, *registers, 0x4000U);
    ASTERIA_EXPECT(context, "all-ones port reads do not advertise connections",
                   std::strstr(ports, "XHCI port=") == nullptr);
    for (UInt32 port{0U}; port < 255U; ++port)
    {
        registers->Words[(0x480U + port * 16U) / 4U] = 1U;
    }
    char bounded[4096U]{};
    Asteria::Inventory::ReportText connected{bounded, sizeof(bounded) - 1U};
    Asteria::Inventory::FormatXhci(connected, *registers, 0x4000U);
    ASTERIA_EXPECT(context, "connected-port output has an explicit independent bound",
                   std::strstr(bounded, "port=32 PORTSC=") != nullptr,
                   std::strstr(bounded, "port=33 PORTSC=") == nullptr,
                   std::strstr(bounded, "255 connected, first 32 shown") != nullptr);
    return context.Finish();
}
