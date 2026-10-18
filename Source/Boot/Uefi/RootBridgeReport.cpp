// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/RootBridgeReport.hpp"

#include "Boot/Uefi/PciIoProtocol.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
constexpr Guid RootGuid{0x2F707EBBU, 0x4A1AU, 0x11D4U, {0x9AU, 0x38U, 0U, 0x90U, 0x27U, 0x3FU, 0xC1U, 0x4DU}};
struct RootBridge;
using ReadConfiguration = Status(ASTERIA_UEFI_API*)(RootBridge*, PciIoWidth, Foundation::UInt64, Foundation::Size,
                                                    void*);
using Configuration = Status(ASTERIA_UEFI_API*)(RootBridge*, void**);
struct RootAccess
{
    ReadConfiguration Read;
    void* Write;
};
struct RootBridge
{
    Handle Parent;
    void* PollMemory;
    void* PollIo;
    RootAccess Memory;
    RootAccess Io;
    RootAccess Pci;
    void* CopyMemory;
    void* Map;
    void* Unmap;
    void* AllocateBuffer;
    void* FreeBuffer;
    void* Flush;
    void* GetAttributes;
    void* SetAttributes;
    Configuration GetConfiguration;
    Foundation::UInt32 Segment;
};
static_assert(__builtin_offsetof(RootBridge, Pci) == 56U);
static_assert(__builtin_offsetof(RootBridge, GetConfiguration) == 136U);
static_assert(__builtin_offsetof(RootBridge, Segment) == 144U);

[[nodiscard]] Foundation::UInt64 ReadWord(const Foundation::UInt8* bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool Seen(const Abi::DeviceInventory& inventory, const Foundation::UInt32 segment,
                        const Foundation::UInt32 bus, const Foundation::UInt32 device,
                        const Foundation::UInt32 function) noexcept
{
    for (Foundation::Size index{0U}; index < inventory.PciCount; ++index)
    {
        const auto& entry = inventory.Pci[index].Function;
        if (entry.Segment == segment && entry.Bus == bus && entry.Device == device && entry.Function == function)
        {
            return true;
        }
    }
    return false;
}

void Record(Inventory::ReportText& text, Abi::DeviceInventory& inventory, const Foundation::UInt32 segment,
            const Foundation::UInt32 bus, const Foundation::UInt32 device, const Foundation::UInt32 function,
            const Foundation::UInt32* config) noexcept
{
    Abi::InventoryPciEntry entry{};
    auto& pci = entry.Function;
    pci.Segment = static_cast<Foundation::UInt16>(segment);
    pci.Bus = static_cast<Foundation::UInt8>(bus);
    pci.Device = static_cast<Foundation::UInt8>(device);
    pci.Function = static_cast<Foundation::UInt8>(function);
    pci.Vendor = static_cast<Foundation::UInt16>(config[0U] & 65535U);
    pci.Product = static_cast<Foundation::UInt16>(config[0U] >> 16U);
    pci.Class = static_cast<Foundation::UInt8>(config[2U] >> 24U);
    pci.Subclass = static_cast<Foundation::UInt8>((config[2U] >> 16U) & 255U);
    pci.Interface = static_cast<Foundation::UInt8>((config[2U] >> 8U) & 255U);
    pci.HeaderType = static_cast<Foundation::UInt8>((config[3U] >> 16U) & 255U);
    pci.InterruptLine = static_cast<Foundation::UInt8>(config[15U] & 255U);
    pci.InterruptPin = static_cast<Foundation::UInt8>((config[15U] >> 8U) & 255U);
    const Foundation::Size bars = (pci.HeaderType & 127U) == 0U ? 6U : ((pci.HeaderType & 127U) == 1U ? 2U : 0U);
    for (Foundation::Size index{0U}; index < bars; ++index)
    {
        pci.Bars[index] = config[index + 4U];
    }
    // No PCI I/O handle means firmware capture cannot provide a BAR extent.
    // The post-exit IA-64 ECAM capture fills the selected driver's decision.
    entry.Driver = Abi::InventoryDriver::None;
#if defined(ASTERIA_TARGET_IA64)
    if (pci.Vendor == 0x1234U && pci.Product == 0x11E8U && (pci.HeaderType & 127U) == 0U)
    {
        entry.Driver = Abi::InventoryDriver::EduPending;
    }
    if (pci.Vendor == 0x8086U && pci.Product == 0x10D3U && (pci.HeaderType & 127U) == 0U)
    {
        entry.Driver = Abi::InventoryDriver::E1000ePending;
    }
#endif
    if (inventory.PciCount < Abi::MaximumInventoryPciFunctions)
    {
        inventory.Pci[inventory.PciCount++] = entry;
    }
    else
    {
        inventory.Truncated = 1U;
    }
    Inventory::FormatPci(text, pci, entry.Driver);
    if ((pci.Class == 12U) && (pci.Subclass == 3U) && (pci.Interface == 0x30U))
    {
        text.Text("  XHCI skipped: no decoded firmware BAR extent\r\n");
    }
}

void Scan(Inventory::ReportText& text, RootBridge& root, Abi::DeviceInventory& inventory,
          const Foundation::UInt32 first, const Foundation::UInt32 last) noexcept
{
    for (Foundation::UInt32 bus{first}; bus <= last; ++bus)
    {
        for (Foundation::UInt32 device{0U}; device < 32U; ++device)
        {
            Foundation::UInt32 functions{1U};
            for (Foundation::UInt32 function{0U}; function < functions; ++function)
            {
                const Foundation::UInt64 address = (static_cast<Foundation::UInt64>(bus) << 24U) |
                                                   (static_cast<Foundation::UInt64>(device) << 16U) |
                                                   (static_cast<Foundation::UInt64>(function) << 8U);
                Foundation::UInt32 config[16U]{};
                if (root.Pci.Read(&root, PciIoWidth::Uint32, address, 16U, config) != Success ||
                    (config[0U] & 65535U) == 65535U || (config[0U] & 65535U) == 0U)
                {
                    continue;
                }
                if (function == 0U && (config[3U] & 0x0080'0000U) != 0U)
                {
                    functions = 8U;
                }
                if (!Seen(inventory, root.Segment, bus, device, function))
                {
                    Record(text, inventory, root.Segment, bus, device, function, config);
                }
            }
        }
    }
}

void Report(Inventory::ReportText& text, RootBridge& root, Abi::DeviceInventory& inventory) noexcept
{
    void* resources{nullptr};
    if (root.Segment > 65535U || root.Pci.Read == nullptr || root.GetConfiguration == nullptr ||
        root.GetConfiguration(&root, &resources) != Success || resources == nullptr)
    {
        return;
    }
    // Configuration returns a firmware-owned ACPI resource list; it is borrowed.
    const auto* bytes = static_cast<const Foundation::UInt8*>(resources);
    for (Foundation::Size index{0U}; index < 64U; ++index)
    {
        if (bytes[0U] == 0x79U)
        {
            return;
        }
        if (bytes[0U] != 0x8AU || bytes[1U] != 0x2BU || bytes[2U] != 0U)
        {
            text.Text("PCI root bridge resource list unavailable\r\n");
            return;
        }
        if (bytes[3U] == 2U)
        {
            const Foundation::UInt64 first = ReadWord(bytes + 14U);
            const Foundation::UInt64 last = ReadWord(bytes + 22U);
            if (first <= last && last <= 255U)
            {
                Scan(text, root, inventory, static_cast<Foundation::UInt32>(first),
                     static_cast<Foundation::UInt32>(last));
            }
        }
        bytes += 46U;
    }
}
}

void ReportRootBridgeFunctions(Inventory::ReportText& text, BootServices& services,
                               Abi::DeviceInventory& inventory) noexcept
{
    if (services.LocateHandleBufferFunction == nullptr || services.HandleProtocolFunction == nullptr ||
        services.FreePoolFunction == nullptr || inventory.Truncated != 0U)
    {
        return;
    }
    Guid guid{RootGuid};
    Handle* handles{nullptr};
    Foundation::Size count{0U};
    if (services.LocateHandleBufferFunction(LocateSearchType::ByProtocol, &guid, nullptr, &count, &handles) ==
            Success &&
        handles != nullptr)
    {
        for (Foundation::Size index{0U}; index < count && index < 64U; ++index)
        {
            void* interface{nullptr};
            if (services.HandleProtocolFunction(handles[index], &guid, &interface) == Success && interface != nullptr)
            {
                Report(text, *static_cast<RootBridge*>(interface), inventory);
            }
        }
    }
    if (handles != nullptr)
    {
        static_cast<void>(services.FreePoolFunction(handles));
    }
}
}
