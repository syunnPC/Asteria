// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Boot/BootInventory.hpp"

#include "Abi/DeviceInventory.hpp"
#include "Abi/I8042.hpp"
#include "Runtime/Memory.hpp"

namespace Asteria::Boot::Bootstrap
{
namespace
{
// The record BootInfo carried, then the trust block only the kernel writes
// (Abi::DeviceInventoryFrame).
struct alignas(4096) InventoryStorage
{
    Abi::DeviceInventoryFrame Frame;
    Foundation::UInt8 Padding[Abi::DeviceInventoryPages * 4096U - sizeof(Abi::DeviceInventoryFrame)];
};
constinit InventoryStorage g_inventory{};
constinit bool g_imported{false};
static_assert(sizeof(InventoryStorage) == Abi::DeviceInventoryPages * 4096U);

// Resolves every entry the loader left pending: the captured driver where
// the ECAM capture found that function, none otherwise.
void FinalizeCapture(const Abi::InventoryDriver pending, const Abi::InventoryDriver captured,
                     const Platform::Firmware::AhciPciResource* const resource) noexcept
{
    auto& inventory = g_inventory.Frame.Record;
    for (Foundation::Size index{0U}; index < inventory.PciCount; ++index)
    {
        auto& entry = inventory.Pci[index];
        if (entry.Driver == pending)
        {
            const auto& function = entry.Function;
            const bool matched = resource != nullptr && resource->AbarLengthBytes != 0U &&
                                 function.Segment == resource->Segment && function.Bus == resource->Bus &&
                                 function.Device == resource->Device && function.Function == resource->Function;
            entry.Driver = matched ? captured : Abi::InventoryDriver::None;
        }
    }
}
} // namespace

bool ImportBootInventory(BootInfo& info) noexcept
{
    if (!g_imported)
    {
        const auto* const source = &info.InventoryRecord;
        if (source->Version != Abi::DeviceInventoryVersion || source->PciCount > Abi::MaximumInventoryPciFunctions ||
            source->DiskCount > Abi::MaximumInventoryDisks || source->I8042 > 3U || source->Truncated > 1U)
        {
            return false;
        }
        static_cast<void>(memcpy(&g_inventory.Frame.Record, source, sizeof(g_inventory.Frame.Record)));
        g_imported = true;
    }
    info.Inventory = {reinterpret_cast<Foundation::UIntPointer>(&g_inventory), Abi::DeviceInventoryPages};
    return true;
}

void RecordBootInventoryDisk(const Foundation::UInt8 port, const Foundation::UInt64 sectors, const char* const model,
                             const Foundation::Size modelBytes) noexcept
{
    auto& record = g_inventory.Frame.Record;
    if (record.DiskCount >= Abi::MaximumInventoryDisks || model == nullptr)
    {
        record.Truncated = 1U;
        return;
    }
    auto& disk = record.Disks[record.DiskCount++];
    disk.Port = port;
    disk.Sectors = sectors;
    for (Foundation::Size index{0U}; index < sizeof(disk.Model); ++index)
    {
        disk.Model[index] = index < modelBytes ? model[index] : ' ';
    }
}

void RecordBootInventoryI8042(const Foundation::UInt64 declared) noexcept
{
    g_inventory.Frame.Record.I8042 = 1U;
    if ((declared & Abi::I8042FadtFlag) != 0U)
    {
        g_inventory.Frame.Record.I8042 = 2U;
    }
    else if ((declared & Abi::I8042KeyboardAml) != 0U)
    {
        g_inventory.Frame.Record.I8042 = 3U;
    }
}

void RecordBootInventoryXhci(const Platform::Firmware::AhciPciResource& resource,
                             const Foundation::UInt8 controller) noexcept
{
    auto& inventory = g_inventory.Frame.Record;
    for (Foundation::Size index{0U}; index < inventory.PciCount; ++index)
    {
        auto& entry = inventory.Pci[index];
        const auto& function = entry.Function;
        if (function.Segment == resource.Segment && function.Bus == resource.Bus &&
            function.Device == resource.Device && function.Function == resource.Function)
        {
            entry.Driver = Abi::InventoryDriver::Xhci;
            entry.XhciIndex = controller;
        }
    }
}

void RecordBootInventoryUsbHost(const Foundation::UInt16 segment, const Foundation::UInt8 bus,
                                const Foundation::UInt8 device, const Foundation::UInt8 function,
                                const Foundation::UInt8 index) noexcept
{
    auto& inventory = g_inventory.Frame.Record;
    for (Foundation::Size entryIndex{0U}; entryIndex < inventory.PciCount; ++entryIndex)
    {
        auto& entry = inventory.Pci[entryIndex];
        const auto& pci = entry.Function;
        if ((pci.Segment == segment) && (pci.Bus == bus) && (pci.Device == device) && (pci.Function == function))
        {
            entry.Driver = Abi::InventoryDriver::UsbHost;
            entry.XhciIndex = index;
        }
    }
}

bool GetBootInventoryXhciResource(const Foundation::UInt16 segment, const Foundation::UInt8 bus,
                                  const Foundation::UInt8 device, const Foundation::UInt8 function,
                                  Platform::Firmware::AhciPciResource& resource) noexcept
{
    const auto& inventory = g_inventory.Frame.Record;
    for (Foundation::Size index{0U}; index < inventory.PciCount; ++index)
    {
        const auto& entry = inventory.Pci[index];
        const auto& pci = entry.Function;
        if ((pci.Segment == segment) && (pci.Bus == bus) && (pci.Device == device) && (pci.Function == function) &&
            (entry.XhciBarBytes != 0U))
        {
            const Foundation::UInt64 base = Foundation::UInt64{pci.Bars[0U] & ~15U} |
                                            ((pci.Bars[0U] & 6U) == 4U ? Foundation::UInt64{pci.Bars[1U]} << 32U : 0U);
            resource = {base, entry.XhciBarBytes, segment, bus, device, function, 0U, {0U, 0U}};
            return true;
        }
    }
    return false;
}

void RecordBootInventoryXhciFailure(const Foundation::UInt16 segment, const Foundation::UInt8 bus,
                                    const Foundation::UInt8 device, const Foundation::UInt8 function,
                                    const Foundation::UInt8 step, const Foundation::UInt8 error) noexcept
{
    auto& inventory = g_inventory.Frame.Record;
    for (Foundation::Size index{0U}; index < inventory.PciCount; ++index)
    {
        auto& entry = inventory.Pci[index];
        const auto& pci = entry.Function;
        if ((pci.Segment == segment) && (pci.Bus == bus) && (pci.Device == device) && (pci.Function == function))
        {
            entry.PublicationStep = step;
            entry.PublicationError = error;
        }
    }
}

void RecordBootInventoryTrust(const BootTrustPolicy& policy, const BootFileRegion& files) noexcept
{
    auto& trust = g_inventory.Frame.Trust;
    const Foundation::Size count = (policy.Count < Abi::MaximumInventoryTrust)
                                       ? static_cast<Foundation::Size>(policy.Count)
                                       : Abi::MaximumInventoryTrust;
    trust.Count = static_cast<Foundation::UInt32>(count);
    for (Foundation::Size index{0U}; index < Abi::MaximumInventoryTrust; ++index)
    {
        auto& record = trust.Entries[index];
        record = {};
        if (index >= count)
        {
            continue;
        }
        const BootTrustEntry& entry = policy.Entries[index];
        record.Segment = entry.Segment;
        record.Bus = entry.Bus;
        record.Device = entry.Device;
        record.Function = entry.Function;
        record.Selector = entry.Selector;
        record.Vendor = entry.Vendor;
        record.Product = entry.Product;
        record.Driver = static_cast<Foundation::UInt8>(entry.Driver);
        record.Outcome = Abi::InventoryTrustOutcome::Pending;
        if (entry.Module < files.ModuleCount && entry.Module < BootModuleMaximumCount)
        {
            const char* const name = files.Modules[entry.Module].Name;
            for (Foundation::Size character{0U}; character + 1U < sizeof(record.Module) && name[character] != '\0';
                 ++character)
            {
                record.Module[character] = name[character];
            }
        }
        for (Foundation::Size byte{0U}; byte < sizeof(record.Digest); ++byte)
        {
            record.Digest[byte] = entry.Digest[byte];
        }
    }
}

void RecordBootInventoryTrustOutcome(const Foundation::Size number, const Abi::InventoryTrustOutcome outcome) noexcept
{
    auto& trust = g_inventory.Frame.Trust;
    if ((number != 0U) && (number <= trust.Count))
    {
        trust.Entries[number - 1U].Outcome = outcome;
    }
}

void RecordBootInventoryTrusted(const Foundation::UInt16 segment, const Foundation::UInt8 bus,
                                const Foundation::UInt8 device, const Foundation::UInt8 function,
                                const Foundation::UInt8 number) noexcept
{
    auto& inventory = g_inventory.Frame.Record;
    for (Foundation::Size index{0U}; index < inventory.PciCount; ++index)
    {
        auto& entry = inventory.Pci[index];
        const auto& pci = entry.Function;
        if ((pci.Segment == segment) && (pci.Bus == bus) && (pci.Device == device) && (pci.Function == function))
        {
            entry.Dma = Abi::InventoryDma::Trusted;
            entry.TrustEntry = number;
        }
    }
}

void FinalizeBootInventoryEduCapture(const Platform::Firmware::AhciPciResource* const resource) noexcept
{
    FinalizeCapture(Abi::InventoryDriver::EduPending, Abi::InventoryDriver::Edu, resource);
}

void FinalizeBootInventoryNicCapture(const Platform::Firmware::AhciPciResource* const resource) noexcept
{
    FinalizeCapture(Abi::InventoryDriver::E1000ePending, Abi::InventoryDriver::E1000e, resource);
}
} // namespace Asteria::Boot::Bootstrap
