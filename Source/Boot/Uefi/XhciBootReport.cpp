// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/XhciBootReport.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
[[nodiscard]] Foundation::UInt64 Word(const Foundation::UInt8* bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= Foundation::UInt64{bytes[index]} << (index * 8U);
    }
    return value;
}
class ReportRegisters final : public Inventory::IXhciReportRegisters
{
public:
    explicit ReportRegisters(const Foundation::UInt64 base) noexcept
        // Firmware MMIO boundary: GetBarAttributes and MSE establish a decoded
        // physical aperture, identity mapped by UEFI until ExitBootServices.
        : m_registers{reinterpret_cast<volatile Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(base))}
    {
    }
    [[nodiscard]] Foundation::UInt32 Read32(const Foundation::UInt32 offset) noexcept override
    {
        return m_registers[offset / 4U];
    }

private:
    volatile Foundation::UInt32* m_registers;
};
}
// BAR 0's base, or zero when the function does not decode it as memory or
// the firmware offers no descriptor.
[[nodiscard]] Foundation::UInt64 MemoryBarZero(PciIoProtocol& protocol, BootServices& services,
                                               const Inventory::PciFunction& function,
                                               const Foundation::UInt32 command) noexcept
{
    const Foundation::UInt32 low = function.Bars[0U];
    const Foundation::UInt32 type = (low >> 1U) & 3U;
    if ((command == 0xFFFF'FFFFU) || ((command & 2U) == 0U) || ((low & 1U) != 0U) || ((type != 0U) && (type != 2U)) ||
        (low == 0xFFFF'FFFFU) || (protocol.GetBarAttributes == nullptr) || (services.FreePoolFunction == nullptr))
    {
        return 0U;
    }
    return Foundation::UInt64{low & ~15U} | (type == 2U ? Foundation::UInt64{function.Bars[1U]} << 32U : 0U);
}

// The extent of the memory BAR 0 at base as the firmware's bus-resource
// descriptor reports it (UEFI 2.10 section 14.4, GetBarAttributes), or zero.
[[nodiscard]] Foundation::UInt64 BarZeroExtent(PciIoProtocol& protocol, BootServices& services,
                                               const Foundation::UInt64 base) noexcept
{
    void* resources{nullptr};
    Foundation::UInt64 attributes{0U};
    Foundation::UInt64 length{0U};
    if ((protocol.GetBarAttributes(&protocol, 0U, &attributes, &resources) == Success) && (resources != nullptr))
    {
        const auto* bytes = static_cast<const Foundation::UInt8*>(resources);
        if ((bytes[0U] == 0x8AU) && (bytes[1U] == 0x2BU) && (bytes[2U] == 0U) && (bytes[3U] == 0U) &&
            (Word(bytes + 14U) == base) && (Word(bytes + 30U) == 0U))
        {
            length = Word(bytes + 38U);
        }
    }
    if (resources != nullptr)
    {
        static_cast<void>(services.FreePoolFunction(resources));
    }
    return length;
}

Foundation::UInt64 CaptureUsbHostBar(PciIoProtocol& protocol, BootServices& services,
                                     const Inventory::PciFunction& function, const Foundation::UInt32 command) noexcept
{
    if ((function.Class != 12U) || (function.Subclass != 3U) ||
        ((function.Interface != 0x10U) && (function.Interface != 0x20U)))
    {
        return 0U;
    }
    const Foundation::UInt64 base = MemoryBarZero(protocol, services, function, command);
    const Foundation::UInt64 length = (base == 0U) ? 0U : BarZeroExtent(protocol, services, base);
    return ((length < 16U) || (base > ~Foundation::UInt64{0U} - length)) ? 0U : length;
}

Foundation::UInt64 ReportXhci(Inventory::ReportText& text, PciIoProtocol& protocol, BootServices& services,
                              const Inventory::PciFunction& function, const Foundation::UInt32 command,
                              Foundation::UInt16& scratchpads) noexcept
{
    scratchpads = 0U;
    if ((function.Class != 12U) || (function.Subclass != 3U) || (function.Interface != 0x30U))
    {
        return 0U;
    }
    const Foundation::UInt64 base = MemoryBarZero(protocol, services, function, command);
    if (base == 0U)
    {
        text.Text("  XHCI skipped: BAR not memory-enabled/decoded\r\n");
        return 0U;
    }
    const Foundation::UInt64 length = BarZeroExtent(protocol, services, base);
    if (((base & 3U) != 0U) || (length < 32U) || (base > ~Foundation::UInt64{0U} - length))
    {
        text.Text("  XHCI skipped: BAR extent not decoded\r\n");
        return 0U;
    }
    text.Text("  XHCI BAR=0x");
    text.Hex(base, 16U);
    text.Text(" bytes=0x");
    text.Hex(length, 16U);
    text.Text("\r\n");
    ReportRegisters registers{base};
    Inventory::FormatXhci(text, registers, length);
    const Foundation::UInt32 hcs2 = registers.Read32(8U);
    scratchpads = static_cast<Foundation::UInt16>(((hcs2 >> 27U) & 31U) | (((hcs2 >> 21U) & 31U) << 5U));
    return length;
}
} // namespace Asteria::Boot::Uefi
