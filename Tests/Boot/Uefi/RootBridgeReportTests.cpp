// SPDX-License-Identifier: Apache-2.0
#include <cstring>

#include "Boot/Uefi/RootBridgeReport.hpp"
#include "Boot/Uefi/PciIoProtocol.hpp"
#include "Support/TestContext.hpp"

namespace
{
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Boot::Uefi::Status;
using Asteria::Boot::Uefi::Success;
using Asteria::Boot::Uefi::PciIoWidth;
using Asteria::Boot::Uefi::Handle;
struct Root;
using Access = Status(ASTERIA_UEFI_API*)(Root*, PciIoWidth, UInt64, Size, void*);
using Configuration = Status(ASTERIA_UEFI_API*)(Root*, void**);
// Test-owned EFI root-bridge prefix, including its otherwise unused slots.
struct Root
{
    void* Prefix[7U];
    Access Read;
    void* Unused[9U];
    Configuration Config;
    UInt32 Segment;
};
static_assert(__builtin_offsetof(Root, Read) == 56U);
static_assert(__builtin_offsetof(Root, Config) == 136U);
Asteria::Foundation::UInt8 g_resources[48U]{};
Root g_root{};
Handle g_handles[1U]{};
UInt32 g_reads{0U};
UInt32 g_frees{0U};
bool g_protocolPresent{true};
bool g_validReads{true};

Status ASTERIA_UEFI_API Read(Root*, PciIoWidth width, UInt64 address, Size count, void* output)
{
    ++g_reads;
    g_validReads = g_validReads && width == PciIoWidth::Uint32 && count == 16U && (address & 255U) == 0U;
    auto* words = static_cast<UInt32*>(output);
    for (Size index{0U}; index < count; ++index)
    {
        words[index] = 0xFFFF'FFFFU;
    }
    if (address == 0U || address == 256U)
    {
        words[0U] = address == 0U ? 0x11E8'1234U : 0x5678'ABCDU;
        words[2U] = 0x0200'0000U;
        words[3U] = address == 0U ? 0x0080'0000U : 0U;
        words[15U] = 0x0000'010BU;
    }
    return Success;
}
Status ASTERIA_UEFI_API Config(Root*, void** output)
{
    *output = g_resources;
    return Success;
}
Status ASTERIA_UEFI_API Locate(Asteria::Boot::Uefi::LocateSearchType, Asteria::Boot::Uefi::Guid*, void*, Size* count,
                               Handle** handles)
{
    *count = g_protocolPresent ? 1U : 0U;
    *handles = g_protocolPresent ? g_handles : nullptr;
    return g_protocolPresent ? Success : Asteria::Boot::Uefi::NotFound;
}
Status ASTERIA_UEFI_API Protocol(Handle, Asteria::Boot::Uefi::Guid*, void** output)
{
    *output = &g_root;
    return Success;
}
Status ASTERIA_UEFI_API Free(void*)
{
    ++g_frees;
    return Success;
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"RootBridgeReportTests"};
    g_resources[0U] = 0x8AU;
    g_resources[1U] = 0x2BU;
    g_resources[3U] = 2U;
    g_resources[38U] = 1U;
    g_resources[46U] = 0x79U;
    g_root.Read = &Read;
    g_root.Config = &Config;
    g_handles[0U] = &g_root;
    Asteria::Boot::Uefi::BootServices services{};
    services.LocateHandleBufferFunction = &Locate;
    services.HandleProtocolFunction = &Protocol;
    services.FreePoolFunction = &Free;
    Asteria::Abi::DeviceInventory inventory{};
    inventory.PciCount = 1U;
    char buffer[512U]{};
    Asteria::Inventory::ReportText report{buffer, sizeof(buffer) - 1U};
    Asteria::Boot::Uefi::ReportRootBridgeFunctions(report, services, inventory);
    ASTERIA_EXPECT(context, "root bridge adds the missing multifunction sibling without duplicating known PCI I/O",
                   inventory.PciCount == 2U, inventory.Pci[1U].Function.Function == 1U,
                   inventory.Pci[1U].Function.Vendor == 0xABCDU, inventory.Pci[1U].Function.Product == 0x5678U,
                   inventory.Pci[1U].Function.InterruptPin == 1U, inventory.Pci[1U].Function.InterruptLine == 11U,
                   std::strstr(buffer, "PCI 0000:00:00.1 abcd:5678 class=02/00/00") != nullptr,
                   std::strstr(buffer, "1234:11e8") == nullptr, g_reads == 39U, g_validReads, g_frees == 1U);
    g_protocolPresent = false;
    Asteria::Boot::Uefi::ReportRootBridgeFunctions(report, services, inventory);
    ASTERIA_EXPECT(context, "an absent root protocol is nonfatal and makes no config reads", inventory.PciCount == 2U,
                   g_reads == 39U, g_frees == 1U);
    g_protocolPresent = true;
    g_resources[1U] = 1U;
    Asteria::Boot::Uefi::ReportRootBridgeFunctions(report, services, inventory);
    ASTERIA_EXPECT(context, "malformed firmware resource descriptors are refused before scanning",
                   inventory.PciCount == 2U, g_reads == 39U, g_frees == 2U,
                   std::strstr(buffer, "PCI root bridge resource list unavailable") != nullptr);
    return context.Finish();
}
