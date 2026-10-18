// SPDX-License-Identifier: Apache-2.0
#include "Boot/Uefi/XhciBootReport.hpp"

#include <cstring>
#include <memory>

#include "Support/TestContext.hpp"

namespace
{
namespace U = Asteria::Boot::Uefi;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
UInt8 g_descriptor[48U]{};
UInt32 g_attributes{0U};
UInt32 g_frees{0U};
struct alignas(16) RegisterStorage
{
    UInt32 Words[1024U]{};
};
void StoreWord(const Asteria::Foundation::Size offset, const UInt64 value) noexcept
{
    for (Asteria::Foundation::Size byte{0U}; byte < 8U; ++byte)
    {
        g_descriptor[offset + byte] = static_cast<UInt8>((value >> (byte * 8U)) & 255U);
    }
}
U::Status ASTERIA_UEFI_API Attributes(U::PciIoProtocol*, UInt8 bar, UInt64* attributes, void** resources)
{
    ++g_attributes;
    *attributes = 0U;
    *resources = g_descriptor;
    return bar == 0U ? U::Success : U::NotFound;
}
U::Status ASTERIA_UEFI_API Free(void*)
{
    ++g_frees;
    return U::Success;
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"XhciBootReportTests"};
    U::PciIoProtocol protocol{};
    protocol.GetBarAttributes = &Attributes;
    U::BootServices services{};
    services.FreePoolFunction = &Free;
    Asteria::Inventory::PciFunction function{};
    function.Class = 12U;
    function.Subclass = 3U;
    function.Interface = 0x30U;
    function.Bars[0U] = 0x142E0004U;
    function.Bars[1U] = 0x30U;
    char text[4096U]{};
    Asteria::Inventory::ReportText report{text, sizeof(text) - 1U};
    UInt16 scratchpads{0U};
    ASTERIA_EXPECT(context, "disabled BAR is never dereferenced or probed",
                   U::ReportXhci(report, protocol, services, function, 0U, scratchpads) == 0U, g_attributes == 0U);
    ASTERIA_EXPECT(context, "unavailable configuration is never dereferenced",
                   U::ReportXhci(report, protocol, services, function, 0xFFFF'FFFFU, scratchpads) == 0U,
                   g_attributes == 0U);
    ASTERIA_EXPECT(context, "undecoded high BAR is never dereferenced",
                   U::ReportXhci(report, protocol, services, function, 2U, scratchpads) == 0U, g_attributes == 1U,
                   g_frees == 1U);

    auto storage = std::make_unique<RegisterStorage>();
    UInt32* const registers = storage->Words;
    registers[0U] = 0x01200080U;
    registers[1U] = 1U << 24U;
    registers[2U] = 2U << 21U;
    registers[0x88U / 4U] = 1U;
    // Test memory owner supplies the firmware identity mapping. BAR encoding
    // follows the allocated address, so verdicts do not depend on host ASLR.
    const UInt64 base = reinterpret_cast<UInt64>(registers);
    function.Bars[0U] = static_cast<UInt32>(base & 0xFFFF'FFF0U) | 4U;
    function.Bars[1U] = static_cast<UInt32>(base >> 32U);
    g_descriptor[0U] = 0x8AU;
    g_descriptor[1U] = 0x2BU;
    StoreWord(14U, base);
    StoreWord(38U, 4096U);
    UInt32 original[1024U]{};
    for (Asteria::Foundation::Size word{0U}; word < 1024U; ++word)
    {
        original[word] = registers[word];
    }
    ASTERIA_EXPECT(context, "decoded firmware aperture supplies the complete snapshot and scratch count",
                   U::ReportXhci(report, protocol, services, function, 6U, scratchpads) == 4096U, scratchpads == 64U,
                   g_attributes == 2U, g_frees == 2U, std::strstr(text, "CAPLENGTH=0x80 HCIVERSION=0x0120") != nullptr,
                   std::strstr(text, "XHCI legacy absent") != nullptr);
    ASTERIA_EXPECT(context, "snapshot leaves every register unchanged",
                   std::memcmp(original, registers, sizeof(original)) == 0);
    StoreWord(30U, 4096U);
    ASTERIA_EXPECT(context, "translated descriptor cannot authorize identity-mapped MMIO",
                   U::ReportXhci(report, protocol, services, function, 2U, scratchpads) == 0U, g_frees == 3U);
    return context.Finish();
}
