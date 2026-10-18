// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/TopologyReport.hpp"

#include "Boot/Uefi/BootTransition.hpp"
#include "Boot/Uefi/PciIoProtocol.hpp"
#if defined(ASTERIA_TARGET_AMD64)
#include "Platform/Amd64/Device/AmdViTopology.hpp"
#include "Platform/Amd64/Device/Amd64VtdHardware.hpp"
#include "Platform/Amd64/Firmware/FirmwarePortIo.hpp"
#include "Platform/Device/VtdTopology.hpp"
#include "Inventory/EhciRmrrReport.hpp"
#endif

namespace Asteria::Boot::Uefi
{
namespace
{
using Inventory::ReportText;
alignas(8) constinit Foundation::UInt8 g_map[MaximumMemoryRegions * 64U]{};
// The raw map's size and descriptor stride, for the RMRR evidence's UEFI
// memory types (UEFI 2.10 section 7.2.3).
constinit Foundation::Size g_mapBytes{0U};
constinit Foundation::Size g_mapStride{0U};
constinit BootInfo g_snapshot{};
constexpr Guid AcpiGuid{0x8868E871U, 0xE4F1U, 0x11D3U, {0xBCU, 0x22U, 0U, 0x80U, 0xC7U, 0x3CU, 0x88U, 0x81U}};
constexpr Guid AcpiLegacyGuid{0xEB9D2D30U, 0x2D88U, 0x11D3U, {0x9AU, 0x16U, 0U, 0x90U, 0x27U, 0x3FU, 0xC1U, 0x4DU}};

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

class TableListing final : public Acpi::IAcpiTableVisitor
{
public:
    ReportText* Text{nullptr};
    void Visit(const Acpi::AcpiTableRecord& record, const Foundation::UInt8*) noexcept override;
};

void TableListing::Visit(const Acpi::AcpiTableRecord& record, const Foundation::UInt8*) noexcept
{
    auto& text = *Text;
    text.Text("ACPI ");
    for (const char value : record.Signature)
    {
        text.Character(value >= 32 && value <= 126 ? value : '?');
    }
    text.Text(" OEM-table=");
    for (const char value : record.OemTableId)
    {
        text.Character(value >= 32 && value <= 126 ? value : '?');
    }
    text.Text(" address=0x");
    text.Hex(record.PhysicalAddress, 16U);
    text.Text(" length=");
    text.Decimal(record.Length);
    text.Text(" revision=");
    text.Decimal(record.Revision);
    text.Text(" checksum=");
    text.Text(record.ChecksumValid != 0U ? "valid\r\n" : "BAD\r\n");
}

void AcpiReport(ReportText& text, SystemTable& system, Abi::DeviceInventory& inventory) noexcept
{
    const void* rsdp{nullptr};
    if (system.ConfigurationTables != nullptr)
    {
        for (Foundation::Size index{0U}; index < system.NumberOfTableEntries && index < 4096U; ++index)
        {
            const auto& table = system.ConfigurationTables[index];
            if (Equal(table.VendorGuid, AcpiGuid))
            {
                rsdp = table.VendorTable;
                break;
            }
            if (Equal(table.VendorGuid, AcpiLegacyGuid))
            {
                rsdp = table.VendorTable;
            }
        }
    }
#if defined(ASTERIA_TARGET_IA64)
    constexpr auto Architecture{Acpi::AcpiTargetArchitecture::Ia64};
#else
    constexpr auto Architecture{Acpi::AcpiTargetArchitecture::Amd64};
#endif
    const auto result = Acpi::AcpiTableParser::Parse(g_snapshot, rsdp, Architecture, g_snapshot.Acpi);
    if (result != Acpi::AcpiTableParseError::None)
    {
        text.Text("ACPI topology unavailable: parser error=");
        text.Decimal(static_cast<Foundation::UInt32>(result));
        text.Text("\r\n");
        return;
    }
    const auto& acpi = g_snapshot.Acpi;
    inventory.I8042 = acpi.FadtRevision == 0U ? 0U : ((acpi.IapcBootArchitecture & 2U) != 0U ? 2U : 1U);
    text.Text("ACPI RSDP revision=");
    text.Decimal(acpi.RsdpRevision);
    text.Text(" MADT processors=");
    text.Decimal(acpi.ProcessorCount + acpi.IgnoredProcessorCount);
    text.Text(" retained=");
    text.Decimal(acpi.ProcessorCount);
    text.Text(" controller-kinds=");
    for (Foundation::Size index{0U}; index < acpi.ProcessorCount; ++index)
    {
        switch (acpi.Processors[index].ControllerKind)
        {
        case Acpi::AcpiProcessorControllerKind::LocalApic:
            text.Text(" xAPIC");
            break;
        case Acpi::AcpiProcessorControllerKind::LocalX2Apic:
            text.Text(" x2APIC");
            break;
        case Acpi::AcpiProcessorControllerKind::LocalSapic:
            text.Text(" SAPIC");
            break;
        }
    }
    text.Text("\r\nDMAR units=");
    text.Decimal(acpi.DmaRemappingUnitCount);
    text.Text(" RMRR=");
    text.Decimal(acpi.ReservedMemoryRegionCount);
    text.Text(" malformed-RMRR=");
    text.Decimal(acpi.MalformedRmrrCount);
    const auto& ivrs = acpi.Ivrs;
    text.Text("\r\nAMD-Vi: units=");
    text.Decimal(ivrs.UnitCount);
    text.Text(" IVHD type=0x");
    text.Hex(ivrs.Type, 2U);
    text.Text(" IVMD=");
    text.Decimal(ivrs.MemoryCount);
    text.Text(" malformed=");
    text.Decimal(ivrs.Malformed);
    text.Text(" skipped-HID=");
    text.Decimal(ivrs.SkippedHid);
    text.Text("\r\n");
    for (Foundation::Size index{0U}; index < ivrs.MemoryCount && index < Acpi::MaximumIvrsMemory; ++index)
    {
        const auto& memory = ivrs.Memory[index];
        text.Text("IVMD type=0x");
        text.Hex(memory.Type, 2U);
        text.Text(" flags=0x");
        text.Hex(memory.Flags, 2U);
        text.Text(" segment=");
        text.Hex(memory.Segment, 4U);
        text.Text(" requesters=");
        text.Hex(memory.First, 4U);
        text.Character('-');
        text.Hex(memory.Last, 4U);
        text.Text(" base=0x");
        text.Hex(memory.Base, 16U);
        text.Text(" limit=0x");
        text.Hex(memory.Limit, 16U);
#if defined(ASTERIA_TARGET_AMD64)
        text.Text(" firmware-range-safe=");
        text.Decimal(Platform::Device::VtdFirmwareRangeIsSafe(g_snapshot, memory.Base, memory.Limit) ? 1U : 0U);
#endif
        text.Text("\r\n");
    }
    text.Text("Input: FADT revision=");
    text.Decimal(acpi.RawFadtRevision);
    text.Text(" IAPC_BOOT_ARCH=0x");
    text.Hex(acpi.RawIapcBootArchitecture, 4U);
    text.Text(" 8042=");
    text.Text(acpi.FadtRevision == 0U ? "unknown" : ((acpi.IapcBootArchitecture & 2U) != 0U ? "present" : "absent"));
    text.Text("\r\n");
    text.Text("SSDT retained=");
    text.Decimal(acpi.SsdtCount);
    text.Text(" overflow=");
    text.Decimal(acpi.SsdtOverflowCount);
    text.Text("\r\n");
    TableListing listing{};
    listing.Text = &text;
    Acpi::AcpiTableParser::VisitTables(g_snapshot, acpi, listing);
}
#if defined(ASTERIA_TARGET_AMD64)
class FirmwarePci final : public Hal::Device::IPciConfigurationSpace
{
public:
    BootServices* Services{nullptr};
    Handle* Handles{nullptr};
    Foundation::Size Count{0U};

    Hal::Device::PciConfigurationReadError Read32(const Kernel::Device::PciFunctionAddress address,
                                                  const Foundation::UInt16 offset,
                                                  Foundation::UInt32& output) noexcept override
    {
        if (!address.IsValid() || offset > 0xFFCU || (offset & 3U) != 0U || Services == nullptr)
        {
            return Hal::Device::PciConfigurationReadError::InvalidAddress;
        }
        Guid guid{PciIoProtocolGuid};
        for (Foundation::Size index{0U}; index < Count && index < 4096U; ++index)
        {
            void* interface{nullptr};
            if (Services->HandleProtocolFunction(Handles[index], &guid, &interface) != Success || interface == nullptr)
            {
                continue;
            }
            auto& pci = *static_cast<PciIoProtocol*>(interface);
            Foundation::Size segment{0U};
            Foundation::Size bus{0U};
            Foundation::Size device{0U};
            Foundation::Size function{0U};
            if (pci.GetLocation == nullptr || pci.Pci.Read == nullptr ||
                pci.GetLocation(&pci, &segment, &bus, &device, &function) != Success)
            {
                continue;
            }
            if (segment == address.GetSegment() && bus == address.GetBus() && device == address.GetDevice() &&
                function == address.GetFunction())
            {
                Foundation::UInt32 value{0U};
                if (pci.Pci.Read(&pci, PciIoWidth::Uint32, offset, 1U, &value) != Success)
                {
                    return Hal::Device::PciConfigurationReadError::HardwareFailure;
                }
                output = value;
                return Hal::Device::PciConfigurationReadError::None;
            }
        }
        output = 0xFFFF'FFFFU;
        return Hal::Device::PciConfigurationReadError::None;
    }
};
constinit Platform::Device::VtdTopologyPlan g_plan{};
constinit Kernel::Device::PciFunctionAddress g_functions[Abi::MaximumInventoryPciFunctions]{};

constexpr Foundation::UInt32 AllOnes{0xFFFF'FFFFU};
constexpr Foundation::Size MaximumRmrrDescriptors{16U};
// BSP boot-only scratch: the UEFI descriptors that overlap one RMRR.
constinit Inventory::EhciEvidenceMemory g_rmrrMemory[MaximumRmrrDescriptors]{};

[[nodiscard]] Foundation::UInt64 RawWord(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= Foundation::UInt64{bytes[index]} << (index * 8U);
    }
    return value;
}

// One firmware-reserved EHCI: configuration through its PCI I/O protocol, registers through UEFI's identity
// mapping of BAR 0, read only. The formatter reads a register only while the function decodes
// memory and only inside the firmware's extent of the BAR.
class EhciEvidenceAccess final : public Inventory::IEhciEvidenceAccess
{
public:
    EhciEvidenceAccess(FirmwarePci& pci, const Kernel::Device::PciFunctionAddress address) noexcept
        : m_pci{pci}, m_address{address}
    {
        const Foundation::UInt32 low = ReadConfiguration(0x10U);
        const Foundation::UInt32 type = (low >> 1U) & 3U;
        if ((low != AllOnes) && ((low & 1U) == 0U) && ((type == 0U) || (type == 2U)))
        {
            m_base = Foundation::UInt64{low & ~15U} |
                     ((type == 2U) ? (Foundation::UInt64{ReadConfiguration(0x14U)} << 32U) : 0U);
        }
    }
    [[nodiscard]] Foundation::UInt32 ReadConfiguration(const Foundation::UInt16 offset) noexcept override
    {
        Foundation::UInt32 value{AllOnes};
        return (m_pci.Read32(m_address, offset, value) == Hal::Device::PciConfigurationReadError::None) ? value
                                                                                                        : AllOnes;
    }
    [[nodiscard]] Foundation::UInt32 ReadRegister(const Foundation::UInt32 offset) noexcept override
    {
        if (m_base == 0U)
        {
            return AllOnes;
        }
        // Firmware MMIO boundary: Memory Space Enable and the firmware's BAR
        // descriptor establish a decoded physical aperture, identity mapped
        // by UEFI until ExitBootServices.
        const auto* const registers =
            reinterpret_cast<volatile Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(m_base + offset));
        return *registers;
    }

private:
    FirmwarePci& m_pci;
    Kernel::Device::PciFunctionAddress m_address;
    Foundation::UInt64 m_base{0U};
};

// The PCH's LPC bridge (00:1f.0) and a firmware-time port read of SMI_EN.
class PchSmiAccess final : public Inventory::IPchSmiAccess
{
public:
    explicit PchSmiAccess(FirmwarePci& pci) noexcept : m_pci{pci}
    {
    }
    [[nodiscard]] Foundation::UInt32 ReadLpcConfiguration(const Foundation::UInt16 offset) noexcept override
    {
        Foundation::UInt32 value{AllOnes};
        return (m_pci.Read32(Kernel::Device::PciFunctionAddress{0U, 0U, 0x1FU, 0U}, offset, value) ==
                Hal::Device::PciConfigurationReadError::None)
                   ? value
                   : AllOnes;
    }
    [[nodiscard]] Foundation::UInt32 ReadPort32(const Foundation::UInt16 port) noexcept override
    {
        return Platform::Amd64::Firmware::ReadFirmwarePort32(port);
    }

private:
    FirmwarePci& m_pci;
};

// The raw UEFI descriptors that overlap [base, limit], in map order.
[[nodiscard]] Foundation::Size CollectRmrrMemory(const Foundation::UInt64 base, const Foundation::UInt64 limit) noexcept
{
    Foundation::Size count{0U};
    if (g_mapStride < 32U)
    {
        return 0U;
    }
    for (Foundation::Size offset{0U};
         (offset < g_mapBytes) && ((g_mapBytes - offset) >= 32U) && (count < MaximumRmrrDescriptors);
         offset += g_mapStride)
    {
        const Foundation::UInt8* const descriptor = g_map + offset;
        const Foundation::UInt64 start = RawWord(descriptor + 8U);
        const Foundation::UInt64 pages = RawWord(descriptor + 24U);
        if ((pages == 0U) || (start > limit) || (pages > ((~Foundation::UInt64{0U} - start) / 4096U)))
        {
            continue;
        }
        if ((start + (pages * 4096U)) <= base)
        {
            continue;
        }
        g_rmrrMemory[count++] = {start, pages, static_cast<Foundation::UInt32>(RawWord(descriptor) & 0xFFFF'FFFFU), 0U};
    }
    return count;
}

// The evidence section of every EHCI (class 0C/03/20) an RMRR reserves:
// nothing on a machine without one.
[[gnu::noinline]] void ReportReservedEhci(ReportText& text, FirmwarePci& pci,
                                          const Abi::DeviceInventory& inventory) noexcept
{
    bool reported{false};
    for (Foundation::Size index{0U}; index < g_plan.Count; ++index)
    {
        const auto& routing = g_plan.Functions[index];
        for (Foundation::Size entry{0U}; (routing.Regions != 0U) && (entry < inventory.PciCount); ++entry)
        {
            const auto& function = inventory.Pci[entry].Function;
            if ((function.Segment != routing.Address.GetSegment()) || (function.Bus != routing.Address.GetBus()) ||
                (function.Device != routing.Address.GetDevice()) ||
                (function.Function != routing.Address.GetFunction()) || (function.Class != 0x0CU) ||
                (function.Subclass != 0x03U) || (function.Interface != 0x20U))
            {
                continue;
            }
            EhciEvidenceAccess access{pci, routing.Address};
            Inventory::FormatEhciEvidence(
                text, access,
                Inventory::EhciEvidenceFunction{.Segment = function.Segment,
                                                .Bus = function.Bus,
                                                .Device = function.Device,
                                                .Function = function.Function,
                                                .Reserved = {},
                                                .Vendor = function.Vendor,
                                                .Product = function.Product,
                                                .Reserved2 = 0U,
                                                .BarBytes = inventory.Pci[entry].XhciBarBytes});
            for (Foundation::Size region{0U}; (region < g_snapshot.Acpi.ReservedMemoryRegionCount) &&
                                              (region < ::Asteria::Boot::Acpi::MaximumAcpiReservedMemoryRegions);
                 ++region)
            {
                if ((routing.Regions & (1U << region)) != 0U)
                {
                    const auto& range = g_snapshot.Acpi.ReservedMemoryRegions[region];
                    Inventory::FormatRmrrMemory(text, region, range.Base, range.Limit, g_rmrrMemory,
                                                CollectRmrrMemory(range.Base, range.Limit));
                }
            }
            reported = true;
        }
    }
    if (reported)
    {
        PchSmiAccess smi{pci};
        const auto& acpi = g_snapshot.Acpi;
        Inventory::FormatPchSmi(text, smi, (acpi.PmTimerSpace == 1U) ? acpi.PmTimerAddress : 0U);
    }
}
#endif
} // namespace

void ReportBootRouting(ReportText& text, SystemTable& system, const Abi::DeviceInventory& inventory) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    auto& services = *system.BootServicesTable;
    if (services.LocateHandleBufferFunction == nullptr || services.HandleProtocolFunction == nullptr ||
        services.FreePoolFunction == nullptr || inventory.PciCount > Abi::MaximumInventoryPciFunctions)
    {
        return;
    }
    FirmwarePci pci{};
    pci.Services = &services;
    Guid guid{PciIoProtocolGuid};
    if (services.LocateHandleBufferFunction(LocateSearchType::ByProtocol, &guid, nullptr, &pci.Count, &pci.Handles) ==
            Success &&
        pci.Handles != nullptr)
    {
        for (Foundation::Size index{0U}; index < inventory.PciCount; ++index)
        {
            const auto& function = inventory.Pci[index].Function;
            g_functions[index] = {function.Segment, function.Bus, function.Device, function.Function};
        }
        const auto result = Platform::Device::ResolveVtdTopology(g_snapshot.Acpi, pci, g_functions, inventory.PciCount,
                                                                 g_plan, Platform::Amd64::Device::Amd64VtdAddressBits);
        text.Text("VT-d: ");
        text.Decimal(g_snapshot.Acpi.DmaRemappingUnitCount);
        text.Text(" units, topology-status=");
        text.Decimal(static_cast<Foundation::UInt8>(result));
        text.Text("\r\n");
        if (result == Platform::Device::VtdTopologyError::None)
        {
            for (Foundation::Size index{0U}; index < g_snapshot.Acpi.DmaRemappingUnitCount; ++index)
            {
                if (g_plan.KeepDisabled[index])
                {
                    text.Text("VT-d unit ");
                    text.Decimal(index);
                    text.Text(!g_plan.NoFunctions[index] ? ": display-only unit off\r\n"
                                                         : ": no assignable functions, off\r\n");
                }
            }
        }
        for (Foundation::Size index{0U}; index < g_plan.Count; ++index)
        {
            if (g_plan.Functions[index].Regions != 0U)
            {
                text.Text("RMRR on ");
                text.Hex(g_plan.Functions[index].Address.GetBus(), 2U);
                text.Character(':');
                text.Hex(g_plan.Functions[index].Address.GetDevice(), 2U);
                text.Character('.');
                text.Hex(g_plan.Functions[index].Address.GetFunction(), 1U);
                text.Text(" mask=0x");
                text.Hex(g_plan.Functions[index].Regions, 2U);
                text.Text("\r\n");
            }
        }
        // Before the AMD-Vi resolution below reuses the plan.
        ReportReservedEhci(text, pci, inventory);
        // Match the boot's backend priority and use the same AMD-Vi routing
        // Policy, including alias and reserved-memory refusals. Reporting reads firmware
        // configuration only; it does not enable either unit.
        const auto& ivrs = g_snapshot.Acpi.Ivrs;
        const auto amdResult =
            Platform::Amd64::Device::ResolveAmdViTopology(ivrs, pci, g_functions, inventory.PciCount, g_plan);
        text.Text("AMD-Vi: selected=");
        text.Decimal(g_snapshot.Acpi.DmarPhysicalAddress == 0U && ivrs.PhysicalAddress != 0U ? 1U : 0U);
        text.Text(" topology-status=");
        text.Decimal(static_cast<Foundation::UInt8>(amdResult));
        text.Text("\r\n");
        for (Foundation::Size index{0U}; index < g_plan.Count; ++index)
        {
            const auto& function = g_plan.Functions[index];
            if (ivrs.PhysicalAddress != 0U)
            {
                text.Text("AMD-Vi function ");
                text.Hex(function.Address.GetBus(), 2U);
                text.Character(':');
                text.Hex(function.Address.GetDevice(), 2U);
                text.Character('.');
                text.Hex(function.Address.GetFunction(), 1U);
                text.Text(" policy=");
                text.Decimal(static_cast<Foundation::UInt8>(function.Policy));
                text.Text(" IVMD-mask=0x");
                text.Hex(function.Regions, 2U);
                text.Text("\r\n");
            }
        }
    }
    if (pci.Handles != nullptr)
    {
        static_cast<void>(services.FreePoolFunction(pci.Handles));
    }
#else
    static_cast<void>(text);
    static_cast<void>(system);
    static_cast<void>(inventory);
#endif
}

const BootInfo& GetReportSnapshot() noexcept
{
    return g_snapshot;
}

void ReportTopology(ReportText& text, SystemTable& system, Abi::DeviceInventory& inventory) noexcept
{
    g_snapshot = {};
    Foundation::Size bytes{sizeof(g_map)};
    Foundation::Size key{0U};
    Foundation::Size stride{0U};
    Foundation::UInt32 version{0U};
    const auto getMap = system.BootServicesTable->GetMemoryMapFunction;
    if (getMap == nullptr || getMap(&bytes, g_map, &key, &stride, &version) != Success ||
        !NormalizeMemoryMap(g_map, bytes, stride, version, g_snapshot))
    {
        text.Text("Memory/ACPI snapshot unavailable\r\n");
        return;
    }
    g_mapBytes = (bytes <= sizeof(g_map)) ? bytes : 0U;
    g_mapStride = stride;
    Foundation::UInt64 conventional{0U};
    Foundation::UInt64 highest{0U};
    for (Foundation::Size index{0U}; index < g_snapshot.MemoryRegionCount; ++index)
    {
        const auto& region = g_snapshot.MemoryRegions[index];
        const Foundation::UInt64 size{region.PageCount * 4096U};
        if (region.Kind == PhysicalMemoryKind::Conventional)
        {
            conventional += size;
        }
        if (size == 0U)
        {
            continue;
        }
        const Foundation::UInt64 end{region.PhysicalStart + size - 1U};
        if (end > highest)
        {
            highest = end;
        }
    }
    text.Text("Memory snapshot (before report I/O): conventional-bytes=");
    text.Decimal(conventional);
    text.Text(" descriptors=");
    text.Decimal(g_snapshot.MemoryRegionCount);
    text.Text(" highest-address=0x");
    text.Hex(highest, 16U);
    text.Text("\r\n");
    AcpiReport(text, system, inventory);
}
} // Namespace Asteria::Boot::Uefi
