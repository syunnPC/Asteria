// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/PeripheralReport.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
using Inventory::ReportText;
constexpr Guid BlockGuid{0x964E5B21U, 0x6459U, 0x11D2U, {0x8EU, 0x39U, 0U, 0xA0U, 0xC9U, 0x69U, 0x72U, 0x3BU}};
constexpr Guid DiskGuid{0xD432A67FU, 0x14DCU, 0x484BU, {0xB3U, 0xBBU, 0x3FU, 0x02U, 0x91U, 0x84U, 0x93U, 0x27U}};
constexpr Guid UsbGuid{0x2B2F68D6U, 0x0CD2U, 0x44CFU, {0x8EU, 0x8BU, 0xBBU, 0xA2U, 0x0BU, 0x1BU, 0x5BU, 0x75U}};
constexpr Guid AtaInterface{0x5E948FE3U, 0x26D3U, 0x42B5U, {0xAFU, 0x17U, 0x61U, 2U, 0x87U, 0x18U, 0x8DU, 0xECU}};
constexpr Guid AhciInterface{0x9E498932U, 0x4ABCU, 0x45AFU, {0xA3U, 0x4DU, 2U, 0x47U, 0x78U, 0x7BU, 0xE7U, 0xC6U}};
constexpr Guid NvmeInterface{0x3AB14680U, 0x5D3FU, 0x4A4DU, {0xBCU, 0xDCU, 0xCCU, 0x38U, 0U, 0x18U, 0xC7U, 0xF7U}};
struct BlockMedia
{
    Foundation::UInt32 MediaId;
    Boolean Removable;
    Boolean Present;
    Boolean LogicalPartition;
    Boolean ReadOnly;
    Boolean WriteCaching;
    Foundation::UInt32 BlockSize;
    Foundation::UInt32 IoAlign;
    Foundation::UInt64 LastBlock;
};
struct BlockIo
{
    Foundation::UInt64 Revision;
    BlockMedia* Media;
};
struct DiskInfo;
using Identify = Status(ASTERIA_UEFI_API*)(DiskInfo*, void*, Foundation::UInt32*);
struct DiskInfo
{
    Guid Interface;
    void* Inquiry;
    Identify IdentifyDevice;
    void* SenseData;
    void* WhichIde;
};
struct UsbIo;
using UsbDescriptor = Status(ASTERIA_UEFI_API*)(UsbIo*, void*);
struct UsbIo
{
    void* ControlTransfer;
    void* BulkTransfer;
    void* AsyncInterruptTransfer;
    void* SyncInterruptTransfer;
    void* IsochronousTransfer;
    void* AsyncIsochronousTransfer;
    UsbDescriptor DeviceDescriptor;
    void* ConfigDescriptor;
    UsbDescriptor InterfaceDescriptor;
};
static_assert(__builtin_offsetof(BlockMedia, BlockSize) == 12U);
static_assert(__builtin_offsetof(BlockMedia, LastBlock) == 24U);
static_assert(__builtin_offsetof(DiskInfo, IdentifyDevice) == 24U);
static_assert(__builtin_offsetof(UsbIo, DeviceDescriptor) == 48U);
// IDENTIFY NVMe is 4096 bytes, ATA is 512. No large stack buffers.
alignas(8) constinit Foundation::UInt8 g_identify[4096U]{};

[[nodiscard]] bool Equal(const Guid& left, const Guid& right) noexcept
{
    if (left.Data1 != right.Data1 || left.Data2 != right.Data2 || left.Data3 != right.Data3)
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        if (left.Data4[index] != right.Data4[index])
        {
            return false;
        }
    }
    return true;
}

void Model(ReportText& text, BootServices& services, Handle handle) noexcept
{
    Guid guid{DiskGuid};
    void* interface{nullptr};
    if (services.HandleProtocolFunction(handle, &guid, &interface) != Success || interface == nullptr)
    {
        text.Text(" model=unavailable");
        return;
    }
    auto& disk = *static_cast<DiskInfo*>(interface);
    Foundation::UInt32 bytes{sizeof(g_identify)};
    if (disk.IdentifyDevice == nullptr || disk.IdentifyDevice(&disk, g_identify, &bytes) != Success)
    {
        text.Text(" model=unavailable");
        return;
    }
    const bool ata = Equal(disk.Interface, AtaInterface) || Equal(disk.Interface, AhciInterface);
    const bool nvme = Equal(disk.Interface, NvmeInterface);
    if ((!ata && !nvme) || (ata && bytes < 94U) || (nvme && bytes < 64U))
    {
        text.Text(" model=unavailable");
        return;
    }
    text.Text(" model=\"");
    const Foundation::Size start{ata ? 54U : 24U};
    for (Foundation::Size index{0U}; index < 40U; ++index)
    {
        const Foundation::UInt8 value{g_identify[start + (ata ? (index ^ 1U) : index)]};
        text.Character(value >= 32U && value <= 126U ? static_cast<char>(value) : '?');
    }
    text.Character('"');
}

void Block(ReportText& text, BootServices& services, Handle handle, void* interface) noexcept
{
    const auto& block = *static_cast<BlockIo*>(interface);
    if (block.Media == nullptr)
    {
        text.Text("BLOCK media unavailable\r\n");
        return;
    }
    const auto& media = *block.Media;
    text.Text("BLOCK present=");
    text.Decimal(media.Present);
    text.Text(" block-size=");
    text.Decimal(media.BlockSize);
    text.Text(" blocks=");
    if (media.Present == 0U)
    {
        text.Text("0");
    }
    else if (media.LastBlock == 0xFFFF'FFFF'FFFF'FFFFULL)
    {
        text.Text("overflow");
    }
    else
    {
        text.Decimal(media.LastBlock + 1U);
    }
    text.Text(" removable=");
    text.Decimal(media.Removable);
    text.Text(" read-only=");
    text.Decimal(media.ReadOnly);
    text.Text(" partition=");
    text.Decimal(media.LogicalPartition);
    Model(text, services, handle);
    text.Text("\r\n");
}

void Usb(ReportText& text, void* interface) noexcept
{
    auto& usb = *static_cast<UsbIo*>(interface);
    Foundation::UInt8 device[18U]{};
    Foundation::UInt8 descriptor[9U]{};
    if (usb.DeviceDescriptor == nullptr || usb.InterfaceDescriptor == nullptr ||
        usb.DeviceDescriptor(&usb, device) != Success || usb.InterfaceDescriptor(&usb, descriptor) != Success)
    {
        text.Text("USB descriptors unavailable\r\n");
        return;
    }
    text.Text("USB ");
    text.Hex(static_cast<Foundation::UInt16>(device[8U] | (device[9U] << 8U)), 4U);
    text.Character(':');
    text.Hex(static_cast<Foundation::UInt16>(device[10U] | (device[11U] << 8U)), 4U);
    text.Text(" device-class=");
    text.Hex(device[4U], 2U);
    text.Character('/');
    text.Hex(device[5U], 2U);
    text.Character('/');
    text.Hex(device[6U], 2U);
    text.Text(" interface-class=");
    text.Hex(descriptor[5U], 2U);
    text.Character('/');
    text.Hex(descriptor[6U], 2U);
    text.Character('/');
    text.Hex(descriptor[7U], 2U);
    if (descriptor[5U] == 3U && descriptor[6U] == 1U)
    {
        if (descriptor[7U] == 1U)
        {
            text.Text(" HID boot keyboard");
        }
        else if (descriptor[7U] == 2U)
        {
            text.Text(" HID boot mouse");
        }
    }
    text.Text("\r\n");
}

void Enumerate(ReportText& text, BootServices& services, const bool blocks) noexcept
{
    Guid guid{blocks ? BlockGuid : UsbGuid};
    Foundation::Size count{0U};
    Handle* handles{nullptr};
    if (services.LocateHandleBufferFunction(LocateSearchType::ByProtocol, &guid, nullptr, &count, &handles) ==
            Success &&
        handles != nullptr)
    {
        for (Foundation::Size index{0U}; index < count && index < 4096U; ++index)
        {
            void* interface{nullptr};
            if (services.HandleProtocolFunction(handles[index], &guid, &interface) == Success && interface != nullptr)
            {
                if (blocks)
                {
                    Block(text, services, handles[index], interface);
                }
                else
                {
                    Usb(text, interface);
                }
            }
        }
        if (count > 4096U)
        {
            text.Text("Peripheral enumeration truncated at 4096 handles\r\n");
        }
    }
    else
    {
        text.Text(blocks ? "BLOCK no firmware handles\r\n" : "USB no firmware handles\r\n");
    }
    if (handles != nullptr)
    {
        static_cast<void>(services.FreePoolFunction(handles));
    }
}
} // namespace

void ReportPeripherals(ReportText& text, BootServices& services) noexcept
{
    if (services.LocateHandleBufferFunction == nullptr || services.HandleProtocolFunction == nullptr ||
        services.FreePoolFunction == nullptr)
    {
        text.Text("Peripheral enumeration unavailable\r\n");
        return;
    }
    Enumerate(text, services, true);
    Enumerate(text, services, false);
}
} // namespace Asteria::Boot::Uefi
