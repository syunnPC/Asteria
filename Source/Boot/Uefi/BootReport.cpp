// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/BootReport.hpp"

#include "Boot/Uefi/BootConsole.hpp"
#include "Boot/Uefi/GraphicsModeChoice.hpp"
#include "Boot/Uefi/BootFileLoader.hpp"
#include "Abi/I8042.hpp"

#include "Inventory/ReportText.hpp"
#include "Boot/Uefi/AcpiTableDump.hpp"
#include "Boot/Uefi/AmlBootReport.hpp"
#include "Boot/Uefi/FatBootReport.hpp"
#include "Boot/Uefi/PciIoProtocol.hpp"
#include "Boot/Uefi/PeripheralReport.hpp"
#include "Boot/Uefi/RootBridgeReport.hpp"
#include "Boot/Uefi/TopologyReport.hpp"
#include "Boot/Uefi/XhciBootReport.hpp"
#include "Platform/Firmware/AhciPciResourceCapture.hpp"
#if defined(ASTERIA_TARGET_AMD64)
#include "Platform/Amd64/Firmware/ProcessorInventory.hpp"
#endif

namespace Asteria::Boot::Uefi
{
namespace
{
using Inventory::ReportText;
using Inventory::CapturedDriver;
using Platform::Firmware::AhciPciResource;
using Platform::Firmware::AhciPciResourceCapture;

class MissingVolume final : public IAcpiReportVolume
{
public:
    FilePresence Exists(const char*) noexcept override
    {
        return FilePresence::Missing;
    }
    bool Start(const char*) noexcept override
    {
        return false;
    }
    bool Append(const char*, Foundation::Size) noexcept override
    {
        return false;
    }
    bool Finish() noexcept override
    {
        return false;
    }
};

constinit bool g_dumpAttempted{false};
constexpr Foundation::UInt64 Create{0x8000'0000'0000'0003ULL};
constexpr Guid LoadedImageGuid{0x5B1B31A1U, 0x9562U, 0x11D2U, {0x8EU, 0x3FU, 0U, 0xA0U, 0xC9U, 0x69U, 0x72U, 0x3BU}};
constexpr Guid FileSystemGuid{0x964E5B22U, 0x6459U, 0x11D2U, {0x8EU, 0x39U, 0U, 0xA0U, 0xC9U, 0x69U, 0x72U, 0x3BU}};
constexpr Foundation::Size ReportCapacity{65'536U};
// BSP boot-only scratch, never retained across firmware exit.
constinit char g_report[ReportCapacity]{};
struct alignas(4096) InventoryStorage
{
    Abi::DeviceInventory Record;
    Foundation::UInt8 Padding[Abi::DeviceInventoryPages * 4096U - sizeof(Abi::DeviceInventory)];
};
constinit InventoryStorage g_inventory{};
static_assert(sizeof(InventoryStorage) == Abi::DeviceInventoryPages * 4096U);
constinit AhciPciResource g_ahci{};
constinit AhciPciResource g_nic{};
constinit AhciPciResource g_edu{};
constinit Foundation::UInt64 g_frameBuffer{0U};

[[nodiscard]] bool Matches(const Inventory::PciFunction& function, const AhciPciResource& resource) noexcept
{
    return resource.AbarLengthBytes != 0U && function.Segment == resource.Segment && function.Bus == resource.Bus &&
           function.Device == resource.Device && function.Function == resource.Function;
}

void CaptureDrivers(BootServices& services) noexcept
{
    g_ahci = {};
    g_nic = {};
    g_edu = {};
    static_cast<void>(AhciPciResourceCapture::Capture(services, g_ahci));
#if defined(ASTERIA_TARGET_AMD64)
    static_cast<void>(AhciPciResourceCapture::CaptureByIdentifier(services, 0x10D3'8086U, 0U, g_nic));
#endif
    static_cast<void>(AhciPciResourceCapture::CaptureByIdentifier(services, 0x11E8'1234U, 0U, g_edu));
}

[[nodiscard]] Foundation::UInt64 ReadResourceWord(const Foundation::UInt8* bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool IsGopDevice(PciIoProtocol& protocol, BootServices& services,
                               const Inventory::PciFunction& record) noexcept
{
    if (record.Class != 3U || g_frameBuffer == 0U || protocol.GetBarAttributes == nullptr)
    {
        return false;
    }
    for (Foundation::UInt8 index{0U}; index < 6U; ++index)
    {
        void* resources{nullptr};
        Foundation::UInt64 attributes{0U};
        bool matched{false};
        if (protocol.GetBarAttributes(&protocol, index, &attributes, &resources) == Success && resources != nullptr)
        {
            const auto* bytes = static_cast<const Foundation::UInt8*>(resources);
            // UEFI GetBarAttributes returns ACPI QWORD address descriptors.
            if (bytes[0U] == 0x8AU && bytes[1U] == 0x2BU && bytes[2U] == 0U && bytes[3U] == 0U)
            {
                const Foundation::UInt64 base = ReadResourceWord(bytes + 14U);
                const Foundation::UInt64 length = ReadResourceWord(bytes + 38U);
                matched = g_frameBuffer >= base && g_frameBuffer - base < length;
            }
        }
        if (resources != nullptr)
        {
            static_cast<void>(services.FreePoolFunction(resources));
        }
        if (matched)
        {
            return true;
        }
    }
    return false;
}

void ReportFunction(ReportText& text, PciIoProtocol& protocol, BootServices& services) noexcept
{
    Foundation::Size segment{0U};
    Foundation::Size bus{0U};
    Foundation::Size device{0U};
    Foundation::Size function{0U};
    Foundation::UInt32 configuration[16U]{};
    if (protocol.GetLocation == nullptr || protocol.Pci.Read == nullptr ||
        protocol.GetLocation(&protocol, &segment, &bus, &device, &function) != Success || segment > 65535U ||
        bus > 255U || device > 31U || function > 7U ||
        protocol.Pci.Read(&protocol, PciIoWidth::Uint32, 0U, 16U, configuration) != Success)
    {
        text.Text("PCI unavailable: configuration/location read failed\r\n");
        return;
    }
    Inventory::PciFunction record{};
    record.Segment = static_cast<Foundation::UInt16>(segment);
    record.Bus = static_cast<Foundation::UInt8>(bus);
    record.Device = static_cast<Foundation::UInt8>(device);
    record.Function = static_cast<Foundation::UInt8>(function);
    record.Vendor = static_cast<Foundation::UInt16>(configuration[0U] & 0xFFFFU);
    record.Product = static_cast<Foundation::UInt16>(configuration[0U] >> 16U);
    record.Class = static_cast<Foundation::UInt8>(configuration[2U] >> 24U);
    record.Subclass = static_cast<Foundation::UInt8>((configuration[2U] >> 16U) & 255U);
    record.Interface = static_cast<Foundation::UInt8>((configuration[2U] >> 8U) & 255U);
    record.HeaderType = static_cast<Foundation::UInt8>((configuration[3U] >> 16U) & 255U);
    record.InterruptLine = static_cast<Foundation::UInt8>(configuration[15U] & 255U);
    record.InterruptPin = static_cast<Foundation::UInt8>((configuration[15U] >> 8U) & 255U);
    const Foundation::Size bars = (record.HeaderType & 127U) == 0U ? 6U : ((record.HeaderType & 127U) == 1U ? 2U : 0U);
    for (Foundation::Size index{0U}; index < bars; ++index)
    {
        record.Bars[index] = configuration[index + 4U];
    }
    CapturedDriver driver{CapturedDriver::None};
    if (IsGopDevice(protocol, services, record))
    {
        driver = CapturedDriver::Display;
    }
    else if (Matches(record, g_ahci))
    {
        driver = CapturedDriver::Ahci;
    }
#if defined(ASTERIA_TARGET_AMD64)
    else if (Matches(record, g_nic))
    {
        driver = CapturedDriver::E1000e;
    }
#endif
    else if (Matches(record, g_edu))
    {
        driver = CapturedDriver::Edu;
    }
    auto& inventory = g_inventory.Record;
    if (inventory.PciCount < Abi::MaximumInventoryPciFunctions)
    {
        inventory.Pci[inventory.PciCount++] = {record, driver, 0U, 0U, 0U, 0U, Abi::InventoryDma::Unknown, 0U, 0U};
    }
    else
    {
        inventory.Truncated = 1U;
    }
    Inventory::FormatPci(text, record, driver);
    Foundation::UInt16 scratchpads{0U};
    const Foundation::UInt64 xhciBytes = ReportXhci(text, protocol, services, record, configuration[1U], scratchpads);
    // An EHCI's or OHCI's firmware BAR extent, kept in the same field.
    const Foundation::UInt64 barBytes =
        (xhciBytes != 0U) ? xhciBytes : CaptureUsbHostBar(protocol, services, record, configuration[1U]);
    if ((inventory.PciCount != 0U) && (inventory.Truncated == 0U))
    {
        inventory.Pci[inventory.PciCount - 1U].XhciBarBytes = barBytes;
        inventory.Pci[inventory.PciCount - 1U].XhciScratchpads = scratchpads;
    }
}

void ReportPci(ReportText& text, BootServices& services) noexcept
{
    if (services.LocateHandleBufferFunction == nullptr || services.HandleProtocolFunction == nullptr ||
        services.FreePoolFunction == nullptr)
    {
        text.Text("PCI unavailable: firmware protocol enumeration absent\r\n");
        return;
    }
    CaptureDrivers(services);
    Guid guid{PciIoProtocolGuid};
    Foundation::Size count{0U};
    Handle* handles{nullptr};
    const Status status =
        services.LocateHandleBufferFunction(LocateSearchType::ByProtocol, &guid, nullptr, &count, &handles);
    if (status == Success && handles != nullptr)
    {
        for (Foundation::Size index{0U}; index < count && index < 4096U; ++index)
        {
            void* interface{nullptr};
            if (services.HandleProtocolFunction(handles[index], &guid, &interface) == Success && interface != nullptr)
            {
                ReportFunction(text, *static_cast<PciIoProtocol*>(interface), services);
            }
            else
            {
                text.Text("PCI unavailable: HandleProtocol failed\r\n");
            }
        }
        if (count > 4096U)
        {
            text.Text("PCI enumeration truncated at 4096 handles\r\n");
        }
    }
    else
    {
        text.Text("PCI unavailable: no PCI I/O handles\r\n");
    }
    if (handles != nullptr)
    {
        static_cast<void>(services.FreePoolFunction(handles));
    }
}

[[nodiscard]] bool TrustMatches(const BootTrustEntry& entry, const Inventory::PciFunction& function) noexcept
{
    const bool address = (entry.Selector & BootTrustSelectsAddress) == 0U ||
                         (entry.Segment == function.Segment && entry.Bus == function.Bus &&
                          entry.Device == function.Device && entry.Function == function.Function);
    const bool identifier = (entry.Selector & BootTrustSelectsIdentifier) == 0U ||
                            (entry.Vendor == function.Vendor && entry.Product == function.Product);
    return entry.Selector != 0U && address && identifier;
}

// The trusted-driver allowlist: each entry, its module's digest verdict and the PCI functions it
// matches. The firmware report precedes the IOMMU takeover, so it cannot say whether an entry
// applies: the kernel decides that, on the debug channel and in the device inventory.
[[gnu::noinline]] void ReportTrust(ReportText& text, const BootTrustPolicy& trust, const BootFileRegion& files) noexcept
{
    if (trust.Count == 0U)
    {
        return;
    }
    text.Text("DMA trust: ");
    text.Decimal(trust.Count);
    text.Text(" BOOT.CFG entries; a verified entry drives its function WITHOUT DMA isolation (TRUSTED) only if no"
              " usable IOMMU serves it\r\n");
    const auto& inventory = g_inventory.Record;
    for (Foundation::Size index{0U}; index < trust.Count && index < BootTrustMaximumEntries; ++index)
    {
        const BootTrustEntry& entry = trust.Entries[index];
        FormatBootTrust(text, entry, index + 1U, files);
        Foundation::Size matches{0U};
        for (Foundation::Size function{0U}; function < inventory.PciCount; ++function)
        {
            const auto& candidate = inventory.Pci[function].Function;
            if (TrustMatches(entry, candidate))
            {
                text.Text(matches == 0U ? " matches " : ", ");
                text.Hex(candidate.Segment, 4U);
                text.Character(':');
                text.Hex(candidate.Bus, 2U);
                text.Character(':');
                text.Hex(candidate.Device, 2U);
                text.Character('.');
                text.Hex(candidate.Function, 1U);
                ++matches;
            }
        }
        text.Text(matches == 0U ? " matches no PCI function\r\n" : "\r\n");
    }
}

[[nodiscard]] bool Save(FileSystem& system, BootServices& services, Foundation::Size& bytes) noexcept
{
    File* root{nullptr};
    if (system.Open == nullptr || system.Open(&system, &root) != Success || root == nullptr)
    {
        return false;
    }
    Foundation::UInt16 efiName[]{'E', 'F', 'I', 0U};
    Foundation::UInt16 directoryName[]{'A', 'S', 'T', 'E', 'R', 'I', 'A', 0U};
    Foundation::UInt16 reportName[]{'R', 'E', 'P', 'O', 'R', 'T', '.', 'T', 'X', 'T', 0U};
    File* efi{nullptr};
    File* directory{nullptr};
    bool saved{false};
    if ((root->Open(root, &efi, efiName, Create, 0x10U) == Success ||
         root->Open(root, &efi, efiName, 1U, 0U) == Success) &&
        efi != nullptr)
    {
        if ((efi->Open(efi, &directory, directoryName, Create, 0x10U) == Success ||
             efi->Open(efi, &directory, directoryName, 1U, 0U) == Success) &&
            directory != nullptr)
        {
            saved = SaveAcpiFile(*directory, reportName, reinterpret_cast<const Foundation::UInt8*>(g_report), bytes);
#if !defined(ASTERIA_TARGET_AMD64)
            // IA-64's read-only file protocol has a writable Block I/O fallback.
            // Select that destination before the single facts/evidence walk.
            if (!saved)
            {
                static_cast<void>(directory->Close(directory));
                static_cast<void>(efi->Close(efi));
                static_cast<void>(root->Close(root));
                return false;
            }
#endif
            // First publish REPORT.TXT, then dump, then append the outcome.
            g_dumpAttempted = true;
            ReportText outcome{g_report + bytes, ReportCapacity - bytes};
            DumpAcpiTables(*directory, GetReportSnapshot(), outcome);
            const bool appended = AppendAcpiFile(
                *directory, reportName, reinterpret_cast<const Foundation::UInt8*>(g_report + bytes), outcome.Bytes());
            bytes += outcome.Bytes();
            ReportText amlOutcome{g_report + bytes, ReportCapacity - bytes};
            FirmwareReportVolume volume{*directory};
            WriteAmlBootReport(volume, services, GetReportSnapshot().Acpi, amlOutcome);
            const bool amlAppended{AppendAcpiFile(*directory, reportName,
                                                  reinterpret_cast<const Foundation::UInt8*>(g_report + bytes),
                                                  amlOutcome.Bytes())};
            bytes += amlOutcome.Bytes();
            saved = amlAppended && saved;
            saved = appended && saved;
            saved = directory->Close(directory) == Success && saved;
        }
        saved = efi->Close(efi) == Success && saved;
    }
    saved = root->Close(root) == Success && saved;
    return saved;
}

[[gnu::noinline]] void ShowReport(SimpleTextOutputProtocol& console, const Foundation::Size bytes) noexcept
{
    Foundation::UInt16 chunk[97U]{};
    for (Foundation::Size offset{0U}; offset < bytes;)
    {
        Foundation::Size count{0U};
        while (count < 96U && offset < bytes)
        {
            chunk[count++] = static_cast<Foundation::UInt16>(g_report[offset++]);
        }
        chunk[count] = 0U;
        if (!WriteBootConsoleUnicode(console, chunk))
        {
            return;
        }
    }
}

// Whether REPORT.TXT reached the volume, on the console; out of line, so its
// UTF-16 lines stay out of WriteBootReport's frame.
[[gnu::noinline]] void ShowSaved(SimpleTextOutputProtocol& console, const bool saved) noexcept
{
    Foundation::UInt16 success[]{'R', 'E', 'P', 'O', 'R', 'T', '.',  'T',  'X', 'T',
                                 ' ', 's', 'a', 'v', 'e', 'd', '\r', '\n', 0U};
    Foundation::UInt16 failure[]{'R', 'E', 'P', 'O', 'R', 'T', '.', 'T', 'X', 'T', ' ', 'n',  'o',  't',
                                 ' ', 's', 'a', 'v', 'e', 'd', ':', ' ', 'v', 'o', 'l', 'u',  'm',  'e',
                                 ' ', 'u', 'n', 'a', 'v', 'a', 'i', 'l', 'a', 'b', 'l', 'e',  '/',  'w',
                                 'r', 'i', 't', 'e', ' ', 'f', 'a', 'i', 'l', 'e', 'd', '\r', '\n', 0U};
    static_cast<void>(WriteBootConsoleUnicode(console, saved ? success : failure));
}

[[nodiscard, gnu::noinline]] bool SaveReport(Handle image, BootServices& services, Foundation::Size& bytes) noexcept
{
    if (services.HandleProtocolFunction == nullptr)
    {
        return false;
    }
    Guid guid{LoadedImageGuid};
    void* interface{nullptr};
    if (services.HandleProtocolFunction(image, &guid, &interface) != Success || interface == nullptr)
    {
        return false;
    }
    const Handle device = static_cast<LoadedImageProtocol*>(interface)->DeviceHandle;
    guid = FileSystemGuid;
    interface = nullptr;
    bool saved{services.HandleProtocolFunction(device, &guid, &interface) == Success && interface != nullptr &&
               Save(*static_cast<FileSystem*>(interface), services, bytes)};
#if !defined(ASTERIA_TARGET_AMD64)
    // Only where the firmware's file system is read-only (the project's IA-64
    // firmware): writing a volume's sectors under a firmware driver that may
    // cache its FAT is a risk no real AMD64 machine's boot stick should take,
    // so there a failed write is only reported.
    if (!g_dumpAttempted)
    {
        saved = WriteFatBootReport(device, services, GetReportSnapshot(), g_report, ReportCapacity, bytes);
        g_dumpAttempted = saved;
    }
#endif
    return saved;
}
} // Namespace

BootInventoryLocation GetBootInventoryLocation() noexcept
{
    // Page-aligned static image storage remains reserved with the loaded image.
    return {reinterpret_cast<Foundation::UInt64>(&g_inventory), Abi::DeviceInventoryPages};
}

void WriteAmlBootReportWithoutVolume(BootServices& services, const Acpi::AcpiTopology& topology,
                                     Inventory::ReportText& outcome) noexcept
{
    MissingVolume volume{};
    WriteAmlBootReport(volume, services, topology, outcome);
}

void WriteBootReport(const Handle image, SystemTable& system, const BootDisplay& display, const BootFileRegion& files,
                     const BootTrustPolicy& trust) noexcept
{
    if (system.BootServicesTable == nullptr)
    {
        return;
    }
    ResetAmlBootEvidence();
    g_dumpAttempted = false;
    g_frameBuffer = display.FrameBufferBase;
    g_inventory.Record = {};
    g_inventory.Record.Version = Abi::DeviceInventoryVersion;
    ReportText text{g_report, ReportCapacity - 1024U};
    text.Text("Asteria firmware device report\r\nFirmware: ");
    if (system.FirmwareVendor != nullptr)
    {
        for (Foundation::Size index{0U}; index < 256U && system.FirmwareVendor[index] != 0U; ++index)
        {
            const Foundation::UInt16 value{system.FirmwareVendor[index]};
            text.Character(value >= 32U && value <= 126U ? static_cast<char>(value) : '?');
        }
    }
    text.Text(" revision=0x");
    text.Hex(system.FirmwareRevision, 8U);
    text.Text("\r\n");
    FormatGraphicsModeChoice(text);
    text.Text("GOP: ");
    text.Decimal(display.Width);
    text.Character('x');
    text.Decimal(display.Height);
    text.Text(" pitch=");
    text.Decimal(display.PixelsPerScanLine);
    text.Text(" format=");
    text.Decimal(display.Format);
    text.Text(" framebuffer=0x");
    text.Hex(display.FrameBufferBase, 16U);
    text.Text(" bytes=");
    text.Decimal(display.FrameBufferBytes);
    text.Text("\r\n");
#if defined(ASTERIA_TARGET_AMD64)
    Platform::Amd64::Firmware::ReportProcessor(text);
#else
    text.Text("CPU: IA-64 (AMD64 CPUID features not applicable)\r\n");
#endif
    FormatBootFiles(text, files);
    ReportTopology(text, system, g_inventory.Record);
    ReportPci(text, *system.BootServicesTable);
    ReportTrust(text, trust, files);
    ReportRootBridgeFunctions(text, *system.BootServicesTable, g_inventory.Record);
    ReportBootRouting(text, system, g_inventory.Record);
    ReportPeripherals(text, *system.BootServicesTable);
    Foundation::Size bytes{text.Bytes()};
    if (text.Truncated())
    {
        ReportText suffix{g_report + bytes, ReportCapacity - bytes};
        suffix.Text("\r\n[REPORT TRUNCATED]\r\n");
        bytes += suffix.Bytes();
    }
    const bool saved = SaveReport(image, *system.BootServicesTable, bytes);
    if (!g_dumpAttempted)
    {
        ReportText outcome{g_report + bytes, ReportCapacity - bytes};
        outcome.Text("ACPI dump FAILED: volume unavailable\r\n");
        bytes += outcome.Bytes();
    }
    if (!AmlBootEvidenceAttempted())
    {
        ReportText amlOutcome{g_report + bytes, ReportCapacity - bytes};
        WriteAmlBootReportWithoutVolume(*system.BootServicesTable, GetReportSnapshot().Acpi, amlOutcome);
        bytes += amlOutcome.Bytes();
    }
    if (system.ConsoleOut != nullptr && system.ConsoleOut->OutputString != nullptr)
    {
        ShowReport(*system.ConsoleOut, bytes);
        ShowSaved(*system.ConsoleOut, saved);
    }
}
} // Namespace Asteria::Boot::Uefi
