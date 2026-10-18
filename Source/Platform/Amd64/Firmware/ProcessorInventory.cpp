// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Firmware/ProcessorInventory.hpp"

extern "C" void AsteriaAmd64InventoryCpuId(Asteria::Foundation::UInt32 leaf,
                                           Asteria::Foundation::UInt32* words) noexcept;
extern "C" Asteria::Foundation::UInt32 AsteriaAmd64InventoryApicMode() noexcept;

namespace Asteria::Platform::Amd64::Firmware
{
namespace
{
using Foundation::UInt32;
void WordText(Inventory::ReportText& text, const UInt32 word) noexcept
{
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        const UInt32 value = (word >> (index * 8U)) & 255U;
        text.Character(value >= 32U && value <= 126U ? static_cast<char>(value) : '?');
    }
}
void Feature(Inventory::ReportText& text, const char* const name, const bool present) noexcept
{
    text.Character(' ');
    text.Text(name);
    text.Character('=');
    text.Character(present ? '1' : '0');
}
} // namespace

void ReportProcessor(Inventory::ReportText& text) noexcept
{
    UInt32 words[4U]{};
    AsteriaAmd64InventoryCpuId(0U, words);
    const UInt32 basic = words[0U];
    text.Text("CPU: vendor=");
    WordText(text, words[1U]);
    WordText(text, words[3U]);
    WordText(text, words[2U]);
    AsteriaAmd64InventoryCpuId(1U, words);
    const UInt32 signature = words[0U];
    const UInt32 family = (signature >> 8U) & 15U;
    text.Text(" family=");
    text.Decimal(family == 15U ? family + ((signature >> 20U) & 255U) : family);
    text.Text(" model=");
    text.Decimal(((signature >> 4U) & 15U) + ((family == 6U || family == 15U) ? ((signature >> 12U) & 0xF0U) : 0U));
    text.Text(" stepping=");
    text.Decimal(signature & 15U);
    text.Text("\r\nCPU features:");
    Feature(text, "PAT", (words[3U] & (1U << 16U)) != 0U);
    Feature(text, "x2APIC", (words[2U] & (1U << 21U)) != 0U);
    Feature(text, "RDRAND", (words[2U] & (1U << 30U)) != 0U);
    Feature(text, "XSAVE", (words[2U] & (1U << 26U)) != 0U);
    Feature(text, "OSXSAVE", (words[2U] & (1U << 27U)) != 0U);
    Feature(text, "AVX", (words[2U] & (1U << 28U)) != 0U);
    words[1U] = 0U;
    if (basic >= 7U)
    {
        AsteriaAmd64InventoryCpuId(7U, words);
    }
    Feature(text, "RDSEED", (words[1U] & (1U << 18U)) != 0U);
    AsteriaAmd64InventoryCpuId(0x8000'0000U, words);
    const UInt32 extended = words[0U];
    words[3U] = 0U;
    if (extended >= 0x8000'0001U)
    {
        AsteriaAmd64InventoryCpuId(0x8000'0001U, words);
    }
    Feature(text, "NX", (words[3U] & (1U << 20U)) != 0U);
    words[3U] = 0U;
    if (extended >= 0x8000'0007U)
    {
        AsteriaAmd64InventoryCpuId(0x8000'0007U, words);
    }
    Feature(text, "invariant-TSC", (words[3U] & (1U << 8U)) != 0U);
    text.Text("\r\nCPU brand: ");
    if (extended >= 0x8000'0004U)
    {
        for (UInt32 leaf{0x8000'0002U}; leaf <= 0x8000'0004U; ++leaf)
        {
            AsteriaAmd64InventoryCpuId(leaf, words);
            for (UInt32 word : words)
            {
                for (Foundation::Size index{0U}; index < 4U; ++index)
                {
                    const UInt32 value = (word >> (index * 8U)) & 255U;
                    if (value != 0U)
                    {
                        text.Character(value >= 32U && value <= 126U ? static_cast<char>(value) : '?');
                    }
                }
            }
        }
    }
    text.Text("\r\nAPIC mode at firmware handoff: ");
    switch (AsteriaAmd64InventoryApicMode())
    {
    case 0U:
        text.Text("disabled");
        break;
    case 1U:
        text.Text("xAPIC");
        break;
    case 2U:
        text.Text("x2APIC");
        break;
    default:
        text.Text("unavailable");
        break;
    }
    text.Text("\r\n");
}
} // namespace Asteria::Platform::Amd64::Firmware
