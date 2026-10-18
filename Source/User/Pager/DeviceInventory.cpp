// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/DeviceInventory.hpp"

#include "Abi/DeviceInventory.hpp"
#include "Abi/SystemCallAbi.hpp"
#include "Inventory/ReportText.hpp"

namespace Asteria::User::Pager
{
namespace
{
constinit char g_text[65'536U]{};
constinit Abi::InventoryPciFunction g_xhci[Abi::PagerXhciControllers]{};
constinit Foundation::UInt16 g_scratchpads[Abi::PagerXhciControllers]{};
constinit bool g_captured[Abi::PagerXhciControllers]{};
constinit Foundation::UInt8 g_usbHostInterface[Abi::PagerUsbHostControllers]{0xFFU, 0xFFU, 0xFFU, 0xFFU};
static_assert(Abi::PagerUsbHostControllers == 4U);
constexpr Foundation::UInt8 ReadPermissions{9U};
}

Foundation::UInt64 XhciDmaPageQuota(const Foundation::Size controller) noexcept
{
    if ((controller >= Abi::PagerXhciControllers) || !g_captured[controller])
    {
        return Abi::XhciDmaPages;
    }
    const Foundation::UInt64 count = g_scratchpads[controller];
    // Leave room for the driver to report its explicit scratchpad-budget
    // failure before attempting any DMA page allocation.
    return 68U + (count <= Abi::XhciScratchpadBudget ? count : 0U);
}

namespace
{
void AppendXhciAddress(Inventory::ReportText& text, const Foundation::Size controller) noexcept
{
    const auto& address = g_xhci[controller];
    text.Hex(address.Segment, 4U);
    text.Character(':');
    text.Hex(address.Bus, 2U);
    text.Character(':');
    text.Hex(address.Device, 2U);
    text.Character('.');
    text.Hex(address.Function, 1U);
}
} // namespace

void AppendInputDriver(Inventory::ReportText& text, const Foundation::Size device) noexcept
{
    // The USB host group's driver names no single device.
    if ((device < Abi::PagerDeviceXhci) || (device - Abi::PagerDeviceXhci >= Abi::PagerXhciControllers))
    {
        text.Text("usb");
        return;
    }
    text.Text("xhci ");
    AppendXhciAddress(text, device - Abi::PagerDeviceXhci);
}

Foundation::UInt8 UsbHostInterface(const Foundation::Size slot) noexcept
{
    return (slot < Abi::PagerUsbHostControllers) ? g_usbHostInterface[slot] : 0xFFU;
}

void ReportXhciFailure(ScenarioContext& context, const Foundation::Size device, const Foundation::UInt64 code) noexcept
{
    char line[160U]{};
    Inventory::ReportText text{line, sizeof(line)};
    const bool xhci = (device >= Abi::PagerDeviceXhci) && (device - Abi::PagerDeviceXhci < Abi::PagerXhciControllers);
    text.Text(xhci ? "# init: xhci failed, continuing " : "# init: usb failed, continuing");
    if (xhci)
    {
        AppendXhciAddress(text, device - Abi::PagerDeviceXhci);
    }
    if (code == Abi::KilledExitCode)
    {
        // Its supervisor killed it: no driver step to name.
        text.Text(": killed\n");
    }
    else
    {
        text.Text(" step=");
        text.Decimal((code >> 32U) & 255U);
        text.Text(" err=0x");
        text.Hex((code >> 40U) & 255U, 2U);
        text.Text(" code=0x");
        text.Hex(code, 16U);
        text.Character('\n');
    }
    // A driver may end while the terminal holds a partial edited line.
    WriteBetweenLines(context, line, text.Bytes());
}

void ReadDeviceInventory(ScenarioContext& context) noexcept
{
    const auto& bootstrap = *context.Bootstrap;
    if (bootstrap.InventoryFrame.Selector == 0U || bootstrap.InventoryPages != Abi::DeviceInventoryPages ||
        context.WindowStart == 0U || bootstrap.WindowPages < Abi::DeviceInventoryPages)
    {
        constexpr char Missing[]{"# devices: inventory unavailable\n"};
        WriteLine(context, Missing, sizeof(Missing) - 1U);
        return;
    }
    System::CapabilityHandle memory{};
    Abi::SystemCallError error{Abi::SystemCallError::None};
    if (!context.Calls->CreateMemory(context.Management, Abi::ObjectTypeMemoryObject, bootstrap.InventoryPages,
                                     ReadPermissions, memory, error))
    {
        constexpr char Failure[]{"# devices: inventory unavailable, mapping allocation failed\n"};
        WriteLine(context, Failure, sizeof(Failure) - 1U);
        return;
    }
    const ComponentMapping mapping{bootstrap.InventoryFrame, memory, context.WindowStart, bootstrap.InventoryPages,
                                   false};
    if (context.Calls->Map(context.OwnSpace, mapping, context.WindowStart, ReadPermissions, error))
    {
        // The syscall mapped an immutable, page-aligned ABI record into this
        // process. Bounds and version are validated by the formatter.
        const auto& record = *reinterpret_cast<const Abi::DeviceInventory*>(context.WindowStart);
        if ((record.Version == Abi::DeviceInventoryVersion) && (record.PciCount <= Abi::MaximumInventoryPciFunctions))
        {
            for (Foundation::Size index{0U}; index < record.PciCount; ++index)
            {
                const auto& entry = record.Pci[index];
                if ((entry.Driver == Abi::InventoryDriver::UsbHost) && (entry.XhciIndex != 0U) &&
                    (entry.XhciIndex <= Abi::PagerUsbHostControllers))
                {
                    g_usbHostInterface[entry.XhciIndex - 1U] = entry.Function.Interface;
                }
                if ((entry.Driver == Abi::InventoryDriver::Xhci) && (entry.XhciIndex != 0U) &&
                    (entry.XhciIndex <= Abi::PagerXhciControllers))
                {
                    g_xhci[entry.XhciIndex - 1U] = entry.Function;
                    g_scratchpads[entry.XhciIndex - 1U] = entry.XhciScratchpads;
                    g_captured[entry.XhciIndex - 1U] = entry.XhciBarBytes != 0U;
                }
            }
        }
        Inventory::ReportText text{g_text, sizeof(g_text)};
        Inventory::FormatInventory(text, record);
        context.DeviceText = g_text;
        context.DeviceTextBytes = text.Bytes();
        // WriteLine accepts one bounded line, as the display protocol does.
        Foundation::Size start{0U};
        for (Foundation::Size index{0U}; index < text.Bytes(); ++index)
        {
            if (g_text[index] == '\n')
            {
                WriteLine(context, g_text + start, index - start + 1U);
                start = index + 1U;
            }
        }
        static_cast<void>(context.Client->UnmapMemory(context.OwnSpace, context.WindowStart, bootstrap.InventoryPages));
    }
    else
    {
        constexpr char Failure[]{"# devices: inventory unavailable, read-only map refused\n"};
        WriteLine(context, Failure, sizeof(Failure) - 1U);
    }
    static_cast<void>(context.Calls->Destroy(memory, Abi::ObjectTypeMemoryObject, error));
}
} // namespace Asteria::User::Pager
