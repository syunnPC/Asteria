// SPDX-License-Identifier: Apache-2.0
#include <cstring>

#include "Inventory/ReportText.hpp"
#include "Support/TestContext.hpp"

int main()
{
    Asteria::TestSupport::TestContext context{"ReportTextTests"};
    using Asteria::Inventory::ReportText;
    using Asteria::Inventory::PciFunction;
    using Asteria::Inventory::CapturedDriver;
    using Asteria::Inventory::Support;
    char buffer[256U]{};
    ReportText text{buffer, sizeof(buffer) - 1U};
    text.Hex(0x12ABU, 8U);
    text.Character(' ');
    text.Decimal(18'446'744'073'709'551'615ULL);
    text.Text("\r\n");
    text.Character('\t');
    ASTERIA_EXPECT(context, "hexadecimal, maximum decimal and ASCII replacement",
                   std::strcmp(buffer, "000012ab 18446744073709551615\r\n?") == 0, !text.Truncated());
    char bounded[4U]{'a', 'b', 'c', 'd'};
    ReportText small{bounded, 3U};
    small.Text("12345");
    ASTERIA_EXPECT(context, "bounded buffer and explicit truncation", small.Bytes() == 3U, small.Truncated(),
                   bounded[3U] == 'd', bounded[2U] == '3');
    ReportText empty{nullptr, 10U};
    empty.Text("x");
    ASTERIA_EXPECT(context, "null buffer is safely empty", empty.Bytes() == 0U, empty.Truncated());
    PciFunction pci{};
    pci.Class = 1U;
    pci.Subclass = 6U;
    pci.Interface = 1U;
    ASTERIA_EXPECT(context, "AHCI class alone does not promise driver capture",
                   std::strcmp(Support(pci, CapturedDriver::None), "unsupported") == 0,
                   std::strcmp(Support(pci, CapturedDriver::Ahci), "ahci driver") == 0);
    ASTERIA_EXPECT(
        context, "captured NIC, edu and GOP support use their component names",
        std::strcmp(Support(pci, CapturedDriver::E1000e), "e1000e NIC driver") == 0,
        std::strcmp(Support(pci, CapturedDriver::Edu), "edu demo driver") == 0,
        std::strcmp(Support(pci, CapturedDriver::Display), "display broker") == 0,
        std::strcmp(Support(pci, CapturedDriver::EduPending), "edu demo driver (ECAM capture pending)") == 0,
        std::strcmp(Support(pci, CapturedDriver::E1000ePending), "e1000e NIC driver (ECAM capture pending)") == 0);
    pci.Class = 12U;
    pci.Subclass = 3U;
    for (unsigned interface{0U}; interface <= 0x30U; interface += 0x10U)
    {
        pci.Interface = static_cast<Asteria::Foundation::UInt8>(interface);
        ASTERIA_EXPECT(context, "an unpublished USB host controller names no driver",
                       std::strcmp(Support(pci, CapturedDriver::None), "USB host controller, no driver published") ==
                           0);
    }
    ASTERIA_EXPECT(context, "published xHCI and USB host controllers report their component owner",
                   std::strcmp(Support(pci, CapturedDriver::Xhci), "xhci driver") == 0,
                   std::strcmp(Support(pci, CapturedDriver::UsbHost), "usb host driver") == 0);
    pci.Interface = 0xFEU;
    ASTERIA_EXPECT(context, "unknown USB controller stays unsupported",
                   std::strcmp(Support(pci, CapturedDriver::None), "unsupported") == 0);
    pci.Class = 6U;
    ASTERIA_EXPECT(context, "bridge classification", std::strcmp(Support(pci, CapturedDriver::None), "bridge") == 0);
    pci = {};
    pci.Bus = 0U;
    pci.Device = 31U;
    pci.Function = 2U;
    pci.Vendor = 0x8086U;
    pci.Product = 0x2922U;
    pci.Class = 1U;
    pci.Subclass = 6U;
    pci.Interface = 1U;
    char line[256U]{};
    ReportText formatted{line, sizeof(line) - 1U};
    Asteria::Inventory::FormatPci(formatted, pci, CapturedDriver::Ahci);
    ASTERIA_EXPECT(
        context, "complete PCI line",
        std::strcmp(
            line,
            "PCI 0000:00:1f.2 8086:2922 class=01/06/01 SATA AHCI header=00 irq-pin/line=00/00 support=ahci driver\r\n") ==
            0);
    Asteria::Abi::DeviceInventory inventory{};
    inventory.Version = Asteria::Abi::DeviceInventoryVersion;
    inventory.PciCount = 1U;
    inventory.Pci[0U] = {pci, CapturedDriver::Ahci, 0U, 0U, 0U, 0U, Asteria::Abi::InventoryDma::Unknown, 0U, 0U};
    inventory.DiskCount = 1U;
    inventory.Disks[0U].Sectors = 42U;
    std::memcpy(inventory.Disks[0U].Model, "test disk", 9U);
    inventory.I8042 = 2U;
    inventory.Truncated = 1U;
    char rootText[2048U]{};
    ReportText root{rootText, sizeof(rootText) - 1U};
    Asteria::Inventory::FormatInventory(root, inventory);
    ASTERIA_EXPECT(
        context, "root inventory includes identifiers, disk and input plus overflow",
        std::strstr(rootText, "# devices: 0000:00:1f.2 8086:2922 class=01/06/01 SATA AHCI -> ahci driver\n") != nullptr,
        std::strstr(rootText, "# devices: disk AHCI port=0 sectors=42 model=\"test disk") != nullptr,
        std::strstr(rootText, "# devices: input 8042=present FADT-8042\n") != nullptr,
        std::strstr(rootText, "# devices: inventory truncated\n") != nullptr, !root.Truncated());
    inventory.PciCount = 2U;
    auto& xhci = inventory.Pci[1U];
    xhci.Function.Device = 0x14U;
    xhci.Function.Vendor = 0x8086U;
    xhci.Function.Product = 0xA87DU;
    xhci.Function.Class = 12U;
    xhci.Function.Subclass = 3U;
    xhci.Function.Interface = 0x30U;
    xhci.Driver = Asteria::Abi::InventoryDriver::Xhci;
    xhci.XhciIndex = 2U;
    char published[2048U]{};
    ReportText publication{published, sizeof(published) - 1U};
    Asteria::Inventory::FormatInventory(publication, inventory);
    ASTERIA_EXPECT(context, "per-function publication success reaches the screen inventory",
                   std::strstr(published, "# devices: 0000:00:14.0 8086:a87d class=0c/03/30 USB -> xhci driver"
                                          " publication=published step=0 err=0x00\n") != nullptr);
    xhci.Driver = Asteria::Abi::InventoryDriver::None;
    xhci.XhciIndex = 0U;
    xhci.PublicationStep = 2U;
    xhci.PublicationError = 13U;
    char skipped[2048U]{};
    ReportText failure{skipped, sizeof(skipped) - 1U};
    Asteria::Inventory::FormatInventory(failure, inventory);
    ASTERIA_EXPECT(context, "capture/publication failures retain their controller and error on screen",
                   std::strstr(skipped, "# devices: 0000:00:14.0 8086:a87d class=0c/03/30") != nullptr,
                   std::strstr(skipped, "publication=skipped step=2 err=0x0d\n") != nullptr);
    xhci.Driver = Asteria::Abi::InventoryDriver::Xhci;
    xhci.XhciIndex = 2U;
    xhci.PublicationStep = 0U;
    xhci.PublicationError = 0U;
    xhci.Dma = Asteria::Abi::InventoryDma::Trusted;
    xhci.TrustEntry = 1U;
    char trusted[4096U]{};
    ReportText trustText{trusted, sizeof(trusted) - 1U};
    Asteria::Inventory::FormatInventory(trustText, inventory);
    ASTERIA_EXPECT(
        context, "a trusted function says so on the screen inventory, and only it",
        std::strstr(trusted, "# devices: 0000:00:14.0 8086:a87d class=0c/03/30 USB -> xhci driver"
                             " (TRUSTED: no DMA isolation) publication=published step=0 err=0x00\n") != nullptr,
        std::strstr(trusted, "# devices: 0000:00:1f.2 8086:2922 class=01/06/01 SATA AHCI -> ahci driver\n") != nullptr);
    // A USB 1.1/2.0 host controller shows its outcome as an xHCI does: UHCI
    // published to the USB host driver, OHCI withheld, EHCI reserved for
    // firmware DMA; another class 0C function, SMBus, shows none.
    inventory.PciCount = 6U;
    for (Asteria::Foundation::Size index{2U}; index < 6U; ++index)
    {
        auto& usb = inventory.Pci[index].Function;
        usb = xhci.Function;
        usb.Device = 0x1DU;
        usb.Function = static_cast<Asteria::Foundation::UInt8>(index - 2U);
        usb.Product = 0x3A34U;
        usb.Interface = static_cast<Asteria::Foundation::UInt8>((index - 2U) * 0x10U);
    }
    inventory.Pci[2U].Driver = Asteria::Abi::InventoryDriver::UsbHost;
    inventory.Pci[2U].XhciIndex = 1U;
    inventory.Pci[3U].PublicationStep = 4U;
    inventory.Pci[4U].PublicationStep = 5U;
    inventory.Pci[5U].Function.Function = 3U;
    inventory.Pci[5U].Function.Subclass = 5U;
    inventory.Pci[5U].Function.Interface = 0U;
    char host[4096U]{};
    ReportText hostText{host, sizeof(host) - 1U};
    Asteria::Inventory::FormatInventory(hostText, inventory);
    ASTERIA_EXPECT(
        context, "USB 1.1/2.0 host controllers show their publication outcome on the screen inventory",
        std::strstr(host, "# devices: 0000:00:1d.0 8086:3a34 class=0c/03/00 USB -> usb host driver"
                          " publication=published step=0 err=0x00\n") != nullptr,
        std::strstr(host, "# devices: 0000:00:1d.1 8086:3a34 class=0c/03/10 USB -> USB host controller, no driver"
                          " published publication=skipped step=4 err=0x00\n") != nullptr,
        std::strstr(host, "# devices: 0000:00:1d.2 8086:3a34 class=0c/03/20 USB -> USB host controller, no driver"
                          " published publication=skipped step=5 err=0x00\n") != nullptr,
        std::strstr(host, "# devices: 0000:00:1d.3 8086:3a34 class=0c/05/00 serial bus -> unsupported\n") != nullptr,
        !hostText.Truncated());
    inventory.PciCount = Asteria::Abi::MaximumInventoryPciFunctions + 1U;
    char rejected[80U]{};
    ReportText invalid{rejected, sizeof(rejected) - 1U};
    Asteria::Inventory::FormatInventory(invalid, inventory);
    ASTERIA_EXPECT(context, "foreign record counts are rejected before array traversal",
                   std::strcmp(rejected, "# devices: invalid inventory record\n") == 0);
    return context.Finish();
}
