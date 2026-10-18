// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>

#include "Boot/Acpi/AcpiTableParser.hpp"
#include "Boot/Acpi/AcpiTopology.hpp"
#include "Platform/Device/VtdTopology.hpp"
#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::Acpi::AcpiIoControllerKind;
using Boot::Acpi::AcpiPciConfigurationRegion;
using Boot::Acpi::AcpiProcessorControllerKind;
using Boot::Acpi::AcpiRootTableKind;
using Boot::Acpi::AcpiTableParseError;
using Boot::Acpi::AcpiTableParser;
using Boot::Acpi::AcpiTargetArchitecture;
using Boot::Acpi::AcpiTopology;

constexpr Foundation::Size PageBytes{4096U};
// The memory-map index of the fixture page's region.
constexpr Foundation::Size TableRegionIndex{1U};
constexpr Foundation::Size RsdpOffset{0U};
constexpr Foundation::Size XsdtOffset{64U};
constexpr Foundation::Size RsdtOffset{128U};
constexpr Foundation::Size MadtOffset{256U};
constexpr Foundation::Size MadtEntriesOffset{MadtOffset + 44U};
constexpr Foundation::Size McfgOffset{512U};
constexpr Foundation::Size McfgAllocationsOffset{McfgOffset + 44U};
constexpr Foundation::UInt64 PciConfigurationBase{0x8000'0000ULL};
constexpr Foundation::Size DmarOffset{768U};
constexpr Foundation::UInt64 DmarUnitBase{0xFED9'0000ULL};

void ClearBytes(Foundation::UInt8* const bytes, const Foundation::Size byteCount) noexcept
{
    for (Foundation::Size index{0U}; index < byteCount; ++index)
    {
        bytes[index] = 0U;
    }
}

void Store16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    bytes[0U] = static_cast<Foundation::UInt8>(value);
    bytes[1U] = static_cast<Foundation::UInt8>(value >> 8U);
}

void Store32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void Store64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void StoreSignature(Foundation::UInt8* const bytes, const char* const signature,
                    const Foundation::Size byteCount) noexcept
{
    for (Foundation::Size index{0U}; index < byteCount; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(signature[index]);
    }
}

void RepairChecksum(Foundation::UInt8* const bytes, const Foundation::Size byteCount,
                    const Foundation::Size checksumOffset) noexcept
{
    bytes[checksumOffset] = 0U;
    Foundation::UInt8 sum{0U};
    for (Foundation::Size index{0U}; index < byteCount; ++index)
    {
        sum = static_cast<Foundation::UInt8>(sum + bytes[index]);
    }
    bytes[checksumOffset] = static_cast<Foundation::UInt8>(0U - sum);
}

[[nodiscard]] bool TopologiesEqual(const AcpiTopology& left, const AcpiTopology& right) noexcept
{
    const auto* const leftBytes = reinterpret_cast<const Foundation::UInt8*>(&left);
    const auto* const rightBytes = reinterpret_cast<const Foundation::UInt8*>(&right);
    for (Foundation::Size index{0U}; index < sizeof(AcpiTopology); ++index)
    {
        if (leftBytes[index] != rightBytes[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] AcpiTopology MakeSentinel() noexcept
{
    AcpiTopology topology{};
    auto* const bytes = reinterpret_cast<Foundation::UInt8*>(&topology);
    for (Foundation::Size index{0U}; index < sizeof(topology); ++index)
    {
        bytes[index] = 0xA5U;
    }
    return topology;
}

struct LocalApicEntry
{
    Foundation::UInt8 AcpiUid;
    Foundation::UInt8 ApicId;
    Foundation::UInt32 Flags;
};

constexpr Foundation::UInt32 Enabled{1U};
constexpr Foundation::UInt32 OnlineCapable{2U};

class AcpiFixture final
{
public:
    AcpiFixture() noexcept
    {
        void* const memory =
            mmap(nullptr, PageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (memory != MAP_FAILED)
        {
            m_bytes = static_cast<Foundation::UInt8*>(memory);
        }
    }

    ~AcpiFixture() noexcept
    {
        if (m_bytes != nullptr)
        {
            static_cast<void>(munmap(m_bytes, PageBytes));
        }
    }

    AcpiFixture(const AcpiFixture&) = delete;
    AcpiFixture& operator=(const AcpiFixture&) = delete;

    [[nodiscard]] bool IsReady() const noexcept
    {
        return m_bytes != nullptr;
    }

    [[nodiscard]] const void* Rsdp() const noexcept
    {
        return m_bytes + RsdpOffset;
    }

    [[nodiscard]] Boot::BootInfo& BootInfo() noexcept
    {
        return m_bootInfo;
    }

    [[nodiscard]] Foundation::UInt8* Bytes() noexcept
    {
        return m_bytes;
    }

    void BuildAmd64(const bool useXsdt = true, const bool includeMcfg = true) noexcept
    {
        Reset();
        Foundation::Size offset{MadtEntriesOffset};

        Foundation::UInt8* entry = m_bytes + offset;
        entry[0U] = 0U;
        entry[1U] = 8U;
        entry[2U] = 0U;
        entry[3U] = 2U;
        Store32(entry + 4U, 1U);
        offset += 8U;

        entry = m_bytes + offset;
        entry[0U] = 9U;
        entry[1U] = 16U;
        Store32(entry + 4U, 0x101U);
        Store32(entry + 8U, 2U);
        Store32(entry + 12U, 1U);
        offset += 16U;

        entry = m_bytes + offset;
        entry[0U] = 1U;
        entry[1U] = 12U;
        entry[2U] = 3U;
        Store32(entry + 4U, 0xFEC0'0000U);
        Store32(entry + 8U, 0U);
        offset += 12U;

        entry = m_bytes + offset;
        entry[0U] = 2U;
        entry[1U] = 10U;
        entry[2U] = 0U;
        entry[3U] = 9U;
        Store32(entry + 4U, 20U);
        Store16(entry + 8U, 0x000FU);
        offset += 10U;

        entry = m_bytes + offset;
        entry[0U] = 5U;
        entry[1U] = 12U;
        Store64(entry + 4U, 0xFEE0'1000U);
        offset += 12U;

        FinalizeMadt(offset - MadtOffset);
        if (includeMcfg)
        {
            FinalizeMcfg();
        }
        FinalizeRoot(useXsdt, includeMcfg);
        FinalizeRsdp(useXsdt ? 2U : 0U, useXsdt);
    }

    // An AMD64 MADT with the given local APICs, in order, and one I/O APIC.
    void BuildAmd64Processors(const LocalApicEntry* const processors, const Foundation::Size count) noexcept
    {
        Reset();
        Foundation::Size offset{MadtEntriesOffset};
        for (Foundation::Size index{0U}; index < count; ++index)
        {
            Foundation::UInt8* const entry = m_bytes + offset;
            entry[0U] = 0U;
            entry[1U] = 8U;
            entry[2U] = processors[index].AcpiUid;
            entry[3U] = processors[index].ApicId;
            Store32(entry + 4U, processors[index].Flags);
            offset += 8U;
        }
        Foundation::UInt8* const ioApic = m_bytes + offset;
        ioApic[0U] = 1U;
        ioApic[1U] = 12U;
        ioApic[2U] = 3U;
        Store32(ioApic + 4U, 0xFEC0'0000U);
        Store32(ioApic + 8U, 0U);
        offset += 12U;
        FinalizeMadt(offset - MadtOffset);
        FinalizeRoot(true, false);
        FinalizeRsdp(2U, true);
    }

    void BuildIa64(const bool useLegacyLocalSapic = false, const bool includeMcfg = true) noexcept
    {
        Reset();
        Foundation::Size offset{MadtEntriesOffset};

        Foundation::UInt8* entry = m_bytes + offset;
        entry[0U] = 7U;
        entry[1U] = useLegacyLocalSapic ? 12U : 17U;
        entry[2U] = useLegacyLocalSapic ? 42U : 0U;
        entry[3U] = 4U;
        entry[4U] = 1U;
        Store32(entry + 8U, 1U);
        if (useLegacyLocalSapic)
        {
            offset += 12U;
        }
        else
        {
            Store32(entry + 12U, 42U);
            entry[16U] = 0U;
            offset += 17U;
        }

        entry = m_bytes + offset;
        entry[0U] = 1U;
        entry[1U] = 12U;
        entry[2U] = 8U;
        Store32(entry + 4U, 0xFEC0'0000U);
        Store32(entry + 8U, 32U);
        offset += 12U;

        entry = m_bytes + offset;
        entry[0U] = 6U;
        entry[1U] = 16U;
        entry[2U] = 8U;
        Store32(entry + 4U, 32U);
        Store64(entry + 8U, 0x0000'0000'FEC1'0000ULL);
        offset += 16U;

        entry = m_bytes + offset;
        entry[0U] = 5U;
        entry[1U] = 12U;
        Store64(entry + 4U, 0x0000'0000'FEE1'0000ULL);
        offset += 12U;

        FinalizeMadt(offset - MadtOffset);
        if (includeMcfg)
        {
            FinalizeMcfg();
        }
        FinalizeRoot(true, includeMcfg);
        FinalizeRsdp(2U, true);
    }

    void RepairMadt() noexcept
    {
        const Foundation::Size length = static_cast<Foundation::Size>(m_bytes[MadtOffset + 4U]) |
                                        (static_cast<Foundation::Size>(m_bytes[MadtOffset + 5U]) << 8U) |
                                        (static_cast<Foundation::Size>(m_bytes[MadtOffset + 6U]) << 16U) |
                                        (static_cast<Foundation::Size>(m_bytes[MadtOffset + 7U]) << 24U);
        RepairChecksum(m_bytes + MadtOffset, length, 9U);
    }

    void SetMcfgAllocation(const Foundation::Size index, const AcpiPciConfigurationRegion& configurationRegion) noexcept
    {
        Foundation::UInt8* const allocation = m_bytes + McfgAllocationsOffset + (index * 16U);
        Store64(allocation, configurationRegion.BaseAddress);
        Store16(allocation + 8U, configurationRegion.Segment);
        allocation[10U] = configurationRegion.FirstBus;
        allocation[11U] = configurationRegion.LastBus;
        Store32(allocation + 12U, configurationRegion.Reserved);
    }

    void SetMcfgAllocationCount(const Foundation::Size count) noexcept
    {
        Store32(m_bytes + McfgOffset + 4U, static_cast<Foundation::UInt32>(44U + (count * 16U)));
        RepairMcfg();
    }

    void RepairMcfg() noexcept
    {
        const Foundation::Size length = static_cast<Foundation::Size>(m_bytes[McfgOffset + 4U]) |
                                        (static_cast<Foundation::Size>(m_bytes[McfgOffset + 5U]) << 8U) |
                                        (static_cast<Foundation::Size>(m_bytes[McfgOffset + 6U]) << 16U) |
                                        (static_cast<Foundation::Size>(m_bytes[McfgOffset + 7U]) << 24U);
        RepairChecksum(m_bytes + McfgOffset, length, 9U);
    }

    /// A DMAR of units DRHDs (the last INCLUDE_PCI_ALL, as firmware lists
    /// them), each with scopes endpoint scopes on bus 2 (device s, function
    /// unit; the last with lastHops hops), and reservedRegions RMRRs, listed
    /// after the MADT and MCFG.
    void AddDmar(const Foundation::Size units, const Foundation::Size reservedRegions,
                 const Foundation::Size scopes = 0U, const Foundation::Size lastHops = 1U) noexcept
    {
        Foundation::UInt8* const dmar = m_bytes + DmarOffset;
        StoreSignature(dmar, "DMAR", 4U);
        dmar[8U] = 1U;
        dmar[36U] = 38U;
        dmar[37U] = 1U;
        Foundation::Size offset{48U};
        for (Foundation::Size unit{0U}; unit < units; ++unit)
        {
            Foundation::UInt8* const structure = dmar + offset;
            Store16(structure, 0U);
            structure[4U] = (unit + 1U == units) ? 1U : 0U;
            Store16(structure + 6U, static_cast<Foundation::UInt16>(unit));
            Store64(structure + 8U, DmarUnitBase + (unit * PageBytes));
            Foundation::Size length{16U};
            for (Foundation::Size index{0U}; index < scopes; ++index)
            {
                Foundation::UInt8* const scope = structure + length;
                const Foundation::Size hops = (index + 1U == scopes) ? lastHops : 1U;
                scope[0U] = 1U;
                scope[1U] = static_cast<Foundation::UInt8>(6U + (2U * hops));
                scope[5U] = 2U;
                for (Foundation::Size hop{0U}; hop < hops; ++hop)
                {
                    scope[6U + (2U * hop)] = static_cast<Foundation::UInt8>(index % 32U);
                    scope[7U + (2U * hop)] = static_cast<Foundation::UInt8>(unit);
                }
                length += scope[1U];
            }
            Store16(structure + 2U, static_cast<Foundation::UInt16>(length));
            offset += length;
        }
        for (Foundation::Size region{0U}; region < reservedRegions; ++region)
        {
            Foundation::UInt8* const structure = dmar + offset;
            Store16(structure, 1U);
            Store16(structure + 2U, 32U);
            structure[24U] = 1U;
            structure[25U] = 8U;
            structure[30U] = 2U;
            Store64(structure + 8U, 0x000E'0000U);
            Store64(structure + 16U, 0x000E'FFFFU);
            offset += 32U;
        }
        m_dmarLength = offset;
        RepairDmar();
        FinalizeRoot(true, true, true);
    }

    void AddSandyBridgeDmar(const bool overlap = false) noexcept
    {
        AddDmar(1U, 0U);
        Foundation::UInt8* const dmar = m_bytes + DmarOffset;
        Foundation::Size offset{48U};
        for (Foundation::Size unit{0U}; unit < 2U; ++unit)
        {
            Foundation::UInt8* const drhd = dmar + offset;
            Store16(drhd, 0U);
            Store16(drhd + 2U, ((unit == 0U) || overlap) ? 24U : 16U);
            drhd[4U] = overlap ? 0U : static_cast<Foundation::UInt8>(unit);
            Store64(drhd + 8U, DmarUnitBase + (unit * PageBytes));
            if ((unit == 0U) || overlap)
            {
                drhd[16U] = 1U;
                drhd[17U] = 8U;
                drhd[22U] = 2U;
            }
            offset += ((unit == 0U) || overlap) ? 24U : 16U;
        }
        constexpr Foundation::UInt8 devices[]{2U, 0x1AU, 0x1DU};
        for (Foundation::Size region{0U}; region < 3U; ++region)
        {
            Foundation::UInt8* const rmrr = dmar + offset;
            Store16(rmrr, 1U);
            Store16(rmrr + 2U, 32U);
            Store64(rmrr + 8U, (region == 0U) ? 0x7D00'0000U : 0x7C00'0000U);
            Store64(rmrr + 16U, (region == 0U) ? 0x811F'FFFFU : 0x7C00'1FFFU);
            rmrr[24U] = 1U;
            rmrr[25U] = 8U;
            rmrr[30U] = devices[region];
            offset += 32U;
        }
        // ATSR/RHSA/ANDD and a future structure are skipped by their length.
        constexpr Foundation::UInt16 types[]{2U, 3U, 4U, 0xFFFFU};
        constexpr Foundation::UInt16 lengths[]{8U, 20U, 12U, 6U};
        for (Foundation::Size index{0U}; index < 4U; ++index)
        {
            Store16(dmar + offset, types[index]);
            Store16(dmar + offset + 2U, lengths[index]);
            offset += lengths[index];
        }
        m_dmarLength = offset;
        RepairDmar();
    }

    void RepairDmar() noexcept
    {
        Store32(m_bytes + DmarOffset + 4U, static_cast<Foundation::UInt32>(m_dmarLength));
        RepairChecksum(m_bytes + DmarOffset, m_dmarLength, 9U);
    }

    void AddLargeRegisterDmar() noexcept
    {
        AddDmar(3U, 0U);
        Foundation::UInt8* const dmar = m_bytes + DmarOffset;
        ClearBytes(dmar + 48U, 144U);
        constexpr Foundation::UInt16 lengths[]{24U, 40U, 32U, 24U, 24U};
        Foundation::Size offset{48U};
        for (Foundation::Size index{0U}; index < 5U; ++index)
        {
            Foundation::UInt8* const structure = dmar + offset;
            Store16(structure, static_cast<Foundation::UInt16>(index < 3U ? 0U : index + 2U));
            Store16(structure + 2U, lengths[index]);
            const Foundation::Size header{index < 3U ? 16U : 8U};
            if (index < 3U)
            {
                structure[4U] = index == 2U ? 1U : 0U;
                structure[5U] = 4U;
                Store64(structure + 8U, 0xFC80'0000U + (index * 65536U));
            }
            for (Foundation::Size scope{header}; scope < lengths[index]; scope += 8U)
            {
                structure[scope] = index == 2U ? 3U : 1U;
                structure[scope + 1U] = 8U;
                structure[scope + 6U] = static_cast<Foundation::UInt8>(scope / 8U);
            }
            offset += lengths[index];
        }
        m_dmarLength = offset;
        RepairDmar();
    }

    void DuplicateMcfgRootEntry() noexcept
    {
        Foundation::UInt8* const root = m_bytes + XsdtOffset;
        const Foundation::UInt64 mcfgAddress =
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_bytes + McfgOffset));
        Store64(root + 52U, mcfgAddress);
        Store32(root + 4U, 60U);
        RepairChecksum(root, 60U, 9U);
    }

private:
    // The page's region lies between two conventional regions that cover
    // The rest of the address space, so the parser, which accepts tables anywhere outside reusable
    // memory, dereferences nothing beyond the page.
    void Reset() noexcept
    {
        ClearBytes(m_bytes, PageBytes);
        m_bootInfo = {};
        m_bootInfo.Signature = Boot::BootInfoSignature;
        m_bootInfo.Version = Boot::BootInfoVersion;
        m_bootInfo.SizeBytes = sizeof(Boot::BootInfo);
        m_bootInfo.MemoryRegionCount = 3U;
        m_bootInfo.FirmwareDescriptorVersion = 1U;
        m_bootInfo.FirmwareDescriptorSize = 40U;
        m_bootInfo.FirmwareMemoryMapSizeBytes = 3U * 40U;
        const Foundation::UInt64 page =
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_bytes));
        m_bootInfo.MemoryRegions[0U] = Boot::PhysicalMemoryRegion{
            .PhysicalStart = 0U,
            .VirtualStart = 0U,
            .PageCount = page / PageBytes,
            .Attributes = 0U,
            .FirmwareType = 7U,
            .Kind = Boot::PhysicalMemoryKind::Conventional,
        };
        m_bootInfo.MemoryRegions[TableRegionIndex] = Boot::PhysicalMemoryRegion{
            .PhysicalStart = page,
            .VirtualStart = 0U,
            .PageCount = 1U,
            .Attributes = 0U,
            .FirmwareType = 9U,
            .Kind = Boot::PhysicalMemoryKind::AcpiReclaimable,
        };
        m_bootInfo.MemoryRegions[2U] = Boot::PhysicalMemoryRegion{
            .PhysicalStart = page + PageBytes,
            .VirtualStart = 0U,
            .PageCount = (Foundation::UInt64{0U} - (page + PageBytes)) / PageBytes,
            .Attributes = 0U,
            .FirmwareType = 7U,
            .Kind = Boot::PhysicalMemoryKind::Conventional,
        };
    }

    void FinalizeMadt(const Foundation::Size length) noexcept
    {
        Foundation::UInt8* const madt = m_bytes + MadtOffset;
        StoreSignature(madt, "APIC", 4U);
        Store32(madt + 4U, static_cast<Foundation::UInt32>(length));
        madt[8U] = 6U;
        Store32(madt + 36U, 0xFEE0'0000U);
        Store32(madt + 40U, 1U);
        RepairChecksum(madt, length, 9U);
    }

    void FinalizeMcfg() noexcept
    {
        Foundation::UInt8* const mcfg = m_bytes + McfgOffset;
        StoreSignature(mcfg, "MCFG", 4U);
        Store32(mcfg + 4U, 60U);
        mcfg[8U] = 1U;
        SetMcfgAllocation(0U, AcpiPciConfigurationRegion{
                                  .BaseAddress = PciConfigurationBase,
                                  .Segment = 0U,
                                  .FirstBus = 0U,
                                  .LastBus = 255U,
                                  .Reserved = 0U,
                              });
        RepairMcfg();
    }

    void FinalizeRoot(const bool useXsdt, const bool includeMcfg, const bool includeDmar = false) noexcept
    {
        const Foundation::Size offset = useXsdt ? XsdtOffset : RsdtOffset;
        const Foundation::Size entryBytes = useXsdt ? 8U : 4U;
        const Foundation::Size entryCount = (includeMcfg ? 2U : 1U) + (includeDmar ? 1U : 0U);
        Foundation::UInt8* const root = m_bytes + offset;
        StoreSignature(root, useXsdt ? "XSDT" : "RSDT", 4U);
        Store32(root + 4U, static_cast<Foundation::UInt32>(36U + (entryBytes * entryCount)));
        root[8U] = 1U;
        const Foundation::UInt64 madtAddress =
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_bytes + MadtOffset));
        const Foundation::UInt64 mcfgAddress =
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_bytes + McfgOffset));
        if (useXsdt)
        {
            Store64(root + 36U, madtAddress);
            if (includeMcfg)
            {
                Store64(root + 44U, mcfgAddress);
            }
            if (includeDmar)
            {
                Store64(
                    root + 36U + ((entryCount - 1U) * 8U),
                    static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_bytes + DmarOffset)));
            }
        }
        else
        {
            Store32(root + 36U, static_cast<Foundation::UInt32>(madtAddress));
            if (includeMcfg)
            {
                Store32(root + 40U, static_cast<Foundation::UInt32>(mcfgAddress));
            }
        }
        RepairChecksum(root, 36U + (entryBytes * entryCount), 9U);
    }

    void FinalizeRsdp(const Foundation::UInt8 revision, const bool useXsdt) noexcept
    {
        Foundation::UInt8* const rsdp = m_bytes + RsdpOffset;
        StoreSignature(rsdp, "RSD PTR ", 8U);
        StoreSignature(rsdp + 9U, "ASTERI", 6U);
        rsdp[15U] = revision;
        const Foundation::UInt64 rsdtAddress =
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_bytes + RsdtOffset));
        Store32(rsdp + 16U, static_cast<Foundation::UInt32>(rsdtAddress));
        if (revision >= 2U)
        {
            Store32(rsdp + 20U, 36U);
            const Foundation::UInt64 xsdtAddress =
                static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(m_bytes + XsdtOffset));
            Store64(rsdp + 24U, useXsdt ? xsdtAddress : 0U);
        }
        RepairChecksum(rsdp, 20U, 8U);
        if (revision >= 2U)
        {
            RepairChecksum(rsdp, 36U, 32U);
        }
    }

    Foundation::UInt8* m_bytes{nullptr};
    Foundation::Size m_dmarLength{0U};
    Boot::BootInfo m_bootInfo{};
};

[[nodiscard]] bool McfgIsEmpty(const AcpiTopology& topology) noexcept
{
    if ((topology.McfgPhysicalAddress != 0U) || (topology.McfgRevision != 0U) ||
        (topology.PciConfigurationRegionCount != 0U))
    {
        return false;
    }
    for (const AcpiPciConfigurationRegion& configurationRegion : topology.PciConfigurationRegions)
    {
        if ((configurationRegion.BaseAddress != 0U) || (configurationRegion.Segment != 0U) ||
            (configurationRegion.FirstBus != 0U) || (configurationRegion.LastBus != 0U) ||
            (configurationRegion.Reserved != 0U))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ParseDiscardsMcfg(AcpiFixture& fixture) noexcept
{
    AcpiTopology output = MakeSentinel();
    const AcpiTableParseError error =
        AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output);
    return (error == AcpiTableParseError::None) && (output.ProcessorCount == 2U) && (output.IoControllerCount == 1U) &&
           McfgIsEmpty(output);
}

void TestAmd64Topology(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    ASTERIA_EXPECT(runner, "AMD64 ACPI fixture is mapped below 4 GiB", fixture.IsReady());
    if (!fixture.IsReady())
    {
        return;
    }
    fixture.BuildAmd64();
    AcpiTopology topology{};
    const AcpiTableParseError error =
        AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, topology);
    ASTERIA_EXPECT(runner, "AMD64 MADT is accepted", error == AcpiTableParseError::None);
    ASTERIA_EXPECT(runner, "XSDT and MADT metadata are normalized", topology.RootKind == AcpiRootTableKind::Xsdt,
                   topology.RsdpRevision == 2U, topology.MadtRevision == 6U, topology.MadtFlags == 1U,
                   topology.LocalControllerPhysicalAddress == 0xFEE0'1000U);
    ASTERIA_EXPECT(runner, "processor order and APIC identity are retained", topology.ProcessorCount == 2U,
                   topology.Processors[0U].AcpiUid == 0U, topology.Processors[0U].HardwareId == 2U,
                   topology.Processors[0U].Flags == 1U,
                   topology.Processors[0U].ControllerKind == AcpiProcessorControllerKind::LocalApic,
                   topology.Processors[1U].AcpiUid == 1U, topology.Processors[1U].HardwareId == 0x101U,
                   topology.Processors[1U].Flags == 2U,
                   topology.Processors[1U].ControllerKind == AcpiProcessorControllerKind::LocalX2Apic);
    ASTERIA_EXPECT(
        runner, "I/O APIC and ISA override are retained exactly", topology.IoControllerCount == 1U,
        topology.IoControllers[0U].ControllerId == 3U, topology.IoControllers[0U].PhysicalAddress == 0xFEC0'0000U,
        topology.IoControllers[0U].ControllerKind == AcpiIoControllerKind::IoApic,
        topology.InterruptOverrideCount == 1U, topology.InterruptOverrides[0U].Source == 9U,
        topology.InterruptOverrides[0U].GlobalSystemInterrupt == 20U, topology.InterruptOverrides[0U].Flags == 0x000FU);
    ASTERIA_EXPECT(
        runner, "MCFG metadata and its pointer-free allocation are retained",
        topology.McfgPhysicalAddress ==
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(fixture.Bytes() + McfgOffset)),
        topology.McfgRevision == 1U, topology.PciConfigurationRegionCount == 1U,
        topology.PciConfigurationRegions[0U].BaseAddress == PciConfigurationBase,
        topology.PciConfigurationRegions[0U].Segment == 0U, topology.PciConfigurationRegions[0U].FirstBus == 0U,
        topology.PciConfigurationRegions[0U].LastBus == 255U);

    const AcpiTableParseError inPlaceError = AcpiTableParser::Parse(
        fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, fixture.BootInfo().Acpi);
    ASTERIA_EXPECT(runner, "the exact BootInfo ACPI subobject is a supported output target",
                   inPlaceError == AcpiTableParseError::None, TopologiesEqual(fixture.BootInfo().Acpi, topology));
}

void TestIa64Topology(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    ASTERIA_EXPECT(runner, "IA-64 ACPI fixture is mapped below 4 GiB", fixture.IsReady());
    if (!fixture.IsReady())
    {
        return;
    }
    fixture.BuildIa64();
    AcpiTopology topology{};
    const AcpiTableParseError error =
        AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Ia64, topology);
    ASTERIA_EXPECT(runner, "IA-64 MADT is accepted", error == AcpiTableParseError::None);
    ASTERIA_EXPECT(runner, "local SAPIC identity and processor interrupt block are normalized",
                   topology.ProcessorCount == 1U, topology.Processors[0U].AcpiUid == 42U,
                   topology.Processors[0U].HardwareId == 4U, topology.Processors[0U].HardwareEid == 1U,
                   topology.Processors[0U].ControllerKind == AcpiProcessorControllerKind::LocalSapic,
                   topology.LocalControllerPhysicalAddress == 0x0000'0000'FEE1'0000ULL);
    ASTERIA_EXPECT(runner, "I/O SAPIC replaces its matching legacy I/O APIC record", topology.IoControllerCount == 1U,
                   topology.IoControllers[0U].ControllerId == 8U,
                   topology.IoControllers[0U].GlobalSystemInterruptBase == 32U,
                   topology.IoControllers[0U].PhysicalAddress == 0x0000'0000'FEC1'0000ULL,
                   topology.IoControllers[0U].ControllerKind == AcpiIoControllerKind::IoSapic);
    ASTERIA_EXPECT(runner, "IA-64 receives the normalized MCFG allocation in BootInfo", topology.McfgRevision == 1U,
                   topology.PciConfigurationRegionCount == 1U,
                   topology.PciConfigurationRegions[0U].BaseAddress == PciConfigurationBase);

    fixture.BuildIa64(true);
    AcpiTopology legacyTopology{};
    const AcpiTableParseError legacyError =
        AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Ia64, legacyTopology);
    ASTERIA_EXPECT(runner, "ACPI 2.0 Errata C local SAPIC entries derive UID from ProcessorId",
                   legacyError == AcpiTableParseError::None, legacyTopology.ProcessorCount == 1U,
                   legacyTopology.Processors[0U].AcpiUid == 42U, legacyTopology.Processors[0U].HardwareId == 4U,
                   legacyTopology.Processors[0U].HardwareEid == 1U, legacyTopology.Processors[0U].Flags == 1U,
                   legacyTopology.Processors[0U].ControllerKind == AcpiProcessorControllerKind::LocalSapic);
}

void TestRsdtFallback(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    ASTERIA_EXPECT(runner, "RSDT fixture is mapped below 4 GiB", fixture.IsReady());
    if (!fixture.IsReady())
    {
        return;
    }
    fixture.BuildAmd64(false, false);
    AcpiTopology topology{};
    const AcpiTableParseError error =
        AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, topology);
    ASTERIA_EXPECT(runner, "ACPI 1.0 RSDT fallback remains accepted without MCFG", error == AcpiTableParseError::None,
                   topology.RootKind == AcpiRootTableKind::Rsdt, topology.RsdpRevision == 0U,
                   topology.McfgPhysicalAddress == 0U, topology.PciConfigurationRegionCount == 0U);
}

void TestMcfgValidation(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    ASTERIA_EXPECT(runner, "MCFG fixture is mapped below 4 GiB", fixture.IsReady());
    if (!fixture.IsReady())
    {
        return;
    }
    const AcpiTopology sentinel = MakeSentinel();

    fixture.BuildAmd64();
    fixture.SetMcfgAllocation(0U, AcpiPciConfigurationRegion{
                                      .BaseAddress = PciConfigurationBase,
                                      .Segment = 0U,
                                      .FirstBus = 64U,
                                      .LastBus = 127U,
                                      .Reserved = 0U,
                                  });
    fixture.SetMcfgAllocation(1U, AcpiPciConfigurationRegion{
                                      .BaseAddress = PciConfigurationBase,
                                      .Segment = 0U,
                                      .FirstBus = 0U,
                                      .LastBus = 63U,
                                      .Reserved = 0U,
                                  });
    fixture.SetMcfgAllocationCount(2U);
    AcpiTopology topology{};
    const AcpiTableParseError sortedError =
        AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, topology);
    ASTERIA_EXPECT(
        runner, "MCFG allocations are published in deterministic segment/bus order",
        sortedError == AcpiTableParseError::None, topology.PciConfigurationRegionCount == 2U,
        topology.PciConfigurationRegions[0U].FirstBus == 0U, topology.PciConfigurationRegions[0U].LastBus == 63U,
        topology.PciConfigurationRegions[1U].FirstBus == 64U, topology.PciConfigurationRegions[1U].LastBus == 127U);

    fixture.BuildAmd64();
    fixture.SetMcfgAllocation(1U, AcpiPciConfigurationRegion{
                                      .BaseAddress = 0x9000'0000ULL,
                                      .Segment = 1U,
                                      .FirstBus = 0U,
                                      .LastBus = 255U,
                                      .Reserved = 0U,
                                  });
    fixture.SetMcfgAllocationCount(2U);
    const AcpiTableParseError multiSegmentError =
        AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, topology);
    ASTERIA_EXPECT(runner, "distinct segment groups may each expose the full bus-number range",
                   multiSegmentError == AcpiTableParseError::None, topology.PciConfigurationRegionCount == 2U,
                   topology.PciConfigurationRegions[0U].Segment == 0U,
                   topology.PciConfigurationRegions[1U].Segment == 1U);

    fixture.BuildAmd64();
    fixture.Bytes()[McfgOffset + 9U] ^= 1U;
    const bool independentMcfgIsDiscarded = ParseDiscardsMcfg(fixture);
    fixture.BootInfo().Acpi = sentinel;
    const AcpiTableParseError inPlaceMcfgError = AcpiTableParser::Parse(
        fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, fixture.BootInfo().Acpi);
    ASTERIA_EXPECT(runner, "MCFG checksum failure publishes MADT but no MCFG state", independentMcfgIsDiscarded,
                   inPlaceMcfgError == AcpiTableParseError::None, fixture.BootInfo().Acpi.ProcessorCount == 2U,
                   fixture.BootInfo().Acpi.IoControllerCount == 1U, McfgIsEmpty(fixture.BootInfo().Acpi));

    fixture.BuildAmd64();
    fixture.SetMcfgAllocation(0U, AcpiPciConfigurationRegion{
                                      .BaseAddress = PciConfigurationBase + 4096U,
                                      .Segment = 0U,
                                      .FirstBus = 0U,
                                      .LastBus = 0U,
                                      .Reserved = 0U,
                                  });
    fixture.RepairMcfg();
    ASTERIA_EXPECT(runner, "an unaligned ECAM base is omitted from the topology", ParseDiscardsMcfg(fixture));

    fixture.BuildAmd64();
    fixture.SetMcfgAllocation(0U, AcpiPciConfigurationRegion{
                                      .BaseAddress = PciConfigurationBase,
                                      .Segment = 0U,
                                      .FirstBus = 4U,
                                      .LastBus = 3U,
                                      .Reserved = 0U,
                                  });
    fixture.RepairMcfg();
    ASTERIA_EXPECT(runner, "a reversed MCFG bus range is omitted from the topology", ParseDiscardsMcfg(fixture));

    fixture.BuildAmd64();
    fixture.SetMcfgAllocation(0U, AcpiPciConfigurationRegion{
                                      .BaseAddress = PciConfigurationBase,
                                      .Segment = 0U,
                                      .FirstBus = 0U,
                                      .LastBus = 255U,
                                      .Reserved = 1U,
                                  });
    fixture.RepairMcfg();
    ASTERIA_EXPECT(runner, "nonzero MCFG allocation reserved bits discard the optional table",
                   ParseDiscardsMcfg(fixture));

    fixture.BuildAmd64();
    fixture.SetMcfgAllocationCount(9U);
    ASTERIA_EXPECT(runner, "an over-capacity MCFG is omitted from the topology", ParseDiscardsMcfg(fixture));

    fixture.BuildAmd64();
    fixture.SetMcfgAllocation(0U, AcpiPciConfigurationRegion{
                                      .BaseAddress = PciConfigurationBase,
                                      .Segment = 0U,
                                      .FirstBus = 0U,
                                      .LastBus = 63U,
                                      .Reserved = 0U,
                                  });
    fixture.SetMcfgAllocation(1U, AcpiPciConfigurationRegion{
                                      .BaseAddress = PciConfigurationBase,
                                      .Segment = 0U,
                                      .FirstBus = 32U,
                                      .LastBus = 63U,
                                      .Reserved = 0U,
                                  });
    fixture.SetMcfgAllocationCount(2U);
    ASTERIA_EXPECT(runner, "overlapping bus ranges within one segment discard the optional table",
                   ParseDiscardsMcfg(fixture));

    fixture.BuildAmd64();
    fixture.SetMcfgAllocation(0U, AcpiPciConfigurationRegion{
                                      .BaseAddress = PciConfigurationBase,
                                      .Segment = 0U,
                                      .FirstBus = 0U,
                                      .LastBus = 31U,
                                      .Reserved = 0U,
                                  });
    fixture.SetMcfgAllocation(1U, AcpiPciConfigurationRegion{
                                      .BaseAddress = PciConfigurationBase,
                                      .Segment = 1U,
                                      .FirstBus = 0U,
                                      .LastBus = 31U,
                                      .Reserved = 0U,
                                  });
    fixture.SetMcfgAllocationCount(2U);
    ASTERIA_EXPECT(runner, "physical ECAM aliasing across segments discards the optional table",
                   ParseDiscardsMcfg(fixture));

    fixture.BuildAmd64();
    fixture.DuplicateMcfgRootEntry();
    ASTERIA_EXPECT(runner, "duplicate MCFG roots publish MADT but no MCFG state", ParseDiscardsMcfg(fixture));

    fixture.BuildAmd64();
    Store32(fixture.Bytes() + McfgOffset + 4U, static_cast<Foundation::UInt32>(PageBytes - McfgOffset + 1U));
    ASTERIA_EXPECT(runner, "an out-of-range MCFG extent is omitted from the topology", ParseDiscardsMcfg(fixture));
}

// Whether the topology retains exactly the given APIC IDs, in order, and
// reports ignoredCount further usable processors.
[[nodiscard]] bool RetainsProcessors(const AcpiTopology& topology, const Foundation::UInt8* const apicIds,
                                     const Foundation::Size count, const Foundation::UInt32 ignoredCount) noexcept
{
    if ((topology.ProcessorCount != count) || (topology.IgnoredProcessorCount != ignoredCount) ||
        (topology.TrailingReserved != 0U))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (topology.Processors[index].HardwareId != apicIds[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ParseRetains(AcpiFixture& fixture, const LocalApicEntry* const processors,
                                const Foundation::Size processorCount, const Foundation::UInt8* const apicIds,
                                const Foundation::Size count, const Foundation::UInt32 ignoredCount) noexcept
{
    fixture.BuildAmd64Processors(processors, processorCount);
    AcpiTopology output = MakeSentinel();
    return (AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output) ==
            AcpiTableParseError::None) &&
           RetainsProcessors(output, apicIds, count, ignoredCount);
}

[[nodiscard]] bool DmarIsEmpty(const AcpiTopology& topology) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&topology);
    for (Foundation::Size index{__builtin_offsetof(AcpiTopology, DmarHostAddressWidth)};
         index < __builtin_offsetof(AcpiTopology, PmTimerAddress); ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] AcpiTableParseError ParseAmd64(AcpiFixture& fixture, AcpiTopology& output) noexcept
{
    output = MakeSentinel();
    return AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output);
}

// The optional DMAR: its units' register pages and flags, the host address
// width and the reserved-region count are retained; a malformed or duplicate
// DMAR publishes nothing of it and leaves the rest of the topology standing.
class DmarEcam final : public Hal::Device::IPciConfigurationSpace
{
public:
    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(const Kernel::Device::PciFunctionAddress address,
                                                                const Foundation::UInt16 offset,
                                                                Foundation::UInt32& output) noexcept override
    {
        output = (offset == 0U) ? ((address.GetDevice() == 2U) ? 0x1234'8086U : 0xFFFF'FFFFU)
                                : ((offset == 8U) ? 0x0300'0000U : 0U);
        return Hal::Device::PciConfigurationReadError::None;
    }
};

void TestSandyBridgeDmar(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    AcpiTopology output{};
    fixture.BuildAmd64();
    fixture.AddSandyBridgeDmar();
    ASTERIA_EXPECT(
        runner, "a laptop DMAR retains both DRHDs, stolen memory and both legacy USB buffers",
        ParseAmd64(fixture, output) == AcpiTableParseError::None, output.DmaRemappingUnitCount == 2U,
        output.DmaRemappingUnits[0U].RegisterBaseAddress == 0xFED9'0000U, output.DmaRemappingUnits[0U].Flags == 0U,
        output.DmaRemappingUnits[1U].Flags == 1U, output.DmaRemappingUnits[1U].RegisterBaseAddress == 0xFED9'1000U,
        output.DmaDeviceScopeCount == 1U, output.DmaDeviceScopes[0U].Device == 2U,
        output.ReservedMemoryRegionCount == 3U, output.ReservedMemoryScopeCount == 3U,
        output.ReservedMemoryRegions[0U].Base == 0x7D00'0000U, output.ReservedMemoryRegions[0U].Limit == 0x811F'FFFFU,
        output.ReservedMemoryRegions[1U].Base == 0x7C00'0000U, output.ReservedMemoryScopes[1U].Device == 0x1AU,
        output.ReservedMemoryScopes[2U].Device == 0x1DU, output.ReservedMemoryScopes[2U].Unit == 2U);
    fixture.BuildAmd64();
    fixture.AddSandyBridgeDmar();
    // FADT and the expanded DMAR share one handoff, including on real firmware.
    Foundation::UInt8* const fadt = fixture.Bytes() + 1024U;
    StoreSignature(fadt, "FACP", 4U);
    Store32(fadt + 4U, 116U);
    fadt[8U] = 3U;
    Store16(fadt + 109U, 2U);
    RepairChecksum(fadt, 116U, 9U);
    Foundation::UInt8* const root = fixture.Bytes() + XsdtOffset;
    Store64(root + 60U, static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(fadt)));
    Store32(root + 4U, 68U);
    RepairChecksum(root, 68U, 9U);
    ASTERIA_EXPECT(runner, "FADT keyboard authority survives the multi-unit DMAR handoff",
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, output.IapcBootArchitecture == 2U,
                   output.FadtRevision == 3U, output.DmaRemappingUnitCount == 2U,
                   output.ReservedMemoryRegionCount == 3U);
    fixture.AddSandyBridgeDmar(true);
    DmarEcam ecam;
    Platform::Device::VtdTopologyPlan plan{};
    const Kernel::Device::PciFunctionAddress gfx[]{Kernel::Device::PciFunctionAddress{0U, 0U, 2U, 0U}};
    ASTERIA_EXPECT(runner, "overlapping scopes in a DMAR byte image are retained then refused by ECAM routing",
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, output.DmaDeviceScopeCount == 2U,
                   Platform::Device::ResolveVtdTopology(output, ecam, gfx, 1U, plan, 48U) ==
                       Platform::Device::VtdTopologyError::ScopeOverlap);
    fixture.BuildAmd64();
    fixture.AddSandyBridgeDmar();
    // The first RMRR starts after 48 + 24 + 16 bytes.
    constexpr Foundation::Size rmrr{DmarOffset + 88U};
    // A short final RMRR still has a valid enclosing length, so it is skipped.
    Store16(fixture.Bytes() + DmarOffset + 224U, 1U);
    fixture.RepairDmar();
    ASTERIA_EXPECT(runner, "a short but safely tiled RMRR is skipped while the three valid RMRRs survive",
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, output.DmaRemappingUnitCount == 2U,
                   output.ReservedMemoryRegionCount == 3U, output.MalformedRmrrCount == 1U);
    fixture.BuildAmd64();
    fixture.AddSandyBridgeDmar();
    fixture.Bytes()[rmrr + 25U] = 7U;
    fixture.RepairDmar();
    ASTERIA_EXPECT(runner, "an odd RMRR scope length skips only that record and retains the USB ranges",
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, output.DmaRemappingUnitCount == 2U,
                   output.ReservedMemoryRegionCount == 2U, output.MalformedRmrrCount == 1U,
                   output.ReservedMemoryScopes[0U].Device == 0x1AU);
    fixture.BuildAmd64();
    fixture.AddSandyBridgeDmar();
    Store64(fixture.Bytes() + rmrr + 16U, 0x7CFF'FFFFU);
    fixture.RepairDmar();
    ASTERIA_EXPECT(runner, "an inverted RMRR range is skipped without discarding the remapping units",
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, output.DmaRemappingUnitCount == 2U,
                   output.ReservedMemoryRegionCount == 2U, output.MalformedRmrrCount == 1U);
    fixture.BuildAmd64();
    fixture.AddSandyBridgeDmar();
    Store16(fixture.Bytes() + rmrr + 2U, 23U);
    fixture.RepairDmar();
    ASTERIA_EXPECT(runner, "a short RMRR cannot read addresses outside its structure",
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, DmarIsEmpty(output));
}

// The firmware DMA exclusion list: every RMRR in table order, one the parser drops listed as
// malformed, an inverted one counted only, and the declared count past the list's 64.
void TestDmaExclusions(TestContext& runner) noexcept
{
    using Boot::BootDmaExclusionRmrr;
    using Boot::BootDmaExclusions;
    using Boot::IsCompleteBootDmaExclusions;
    using Boot::IsValidBootDmaExclusions;
    // The first RMRR of the laptop DMAR starts after 48 + 24 + 16 bytes.
    constexpr Foundation::Size rmrr{DmarOffset + 88U};
    AcpiFixture fixture;
    AcpiTopology output{};
    static BootDmaExclusions list{};
    fixture.BuildAmd64();
    bool parsed = ParseAmd64(fixture, output) == AcpiTableParseError::None;
    list.Count = 9U;
    AcpiTableParser::CollectDmaExclusions(fixture.BootInfo(), output, list);
    ASTERIA_EXPECT(runner, "without a DMAR or IVRS the list is empty", parsed, list.Count == 0U,
                   list.DeclaredCount == 0U, IsValidBootDmaExclusions(list));

    fixture.BuildAmd64();
    fixture.AddSandyBridgeDmar();
    parsed = ParseAmd64(fixture, output) == AcpiTableParseError::None;
    AcpiTableParser::CollectDmaExclusions(fixture.BootInfo(), output, list);
    ASTERIA_EXPECT(runner, "a laptop DMAR's three RMRRs are listed in table order", parsed, list.Count == 3U,
                   list.DeclaredCount == 3U, list.Ranges[0U].Base == 0x7D00'0000U,
                   list.Ranges[0U].Limit == 0x811F'FFFFU, list.Ranges[1U].Base == 0x7C00'0000U,
                   list.Ranges[2U].Limit == 0x7C00'1FFFU, list.Ranges[0U].Kind == BootDmaExclusionRmrr,
                   list.Ranges[0U].Malformed == 0U, list.Ranges[2U].Malformed == 0U, IsValidBootDmaExclusions(list),
                   IsCompleteBootDmaExclusions(list));

    fixture.BuildAmd64();
    fixture.AddSandyBridgeDmar();
    fixture.Bytes()[rmrr + 25U] = 7U;
    fixture.RepairDmar();
    parsed = ParseAmd64(fixture, output) == AcpiTableParseError::None;
    AcpiTableParser::CollectDmaExclusions(fixture.BootInfo(), output, list);
    ASTERIA_EXPECT(runner, "an RMRR the parser drops is still excluded, flagged malformed", parsed,
                   output.MalformedRmrrCount == 1U, list.Count == 3U, list.DeclaredCount == 3U,
                   list.Ranges[0U].Base == 0x7D00'0000U, list.Ranges[0U].Malformed == 1U,
                   list.Ranges[1U].Malformed == 0U, IsValidBootDmaExclusions(list));

    fixture.BuildAmd64();
    fixture.AddSandyBridgeDmar();
    Store64(fixture.Bytes() + rmrr + 16U, 0x7CFF'FFFFU);
    fixture.RepairDmar();
    parsed = ParseAmd64(fixture, output) == AcpiTableParseError::None;
    AcpiTableParser::CollectDmaExclusions(fixture.BootInfo(), output, list);
    ASTERIA_EXPECT(runner, "an inverted RMRR is counted but names no range", parsed, list.Count == 2U,
                   list.DeclaredCount == 3U, list.Ranges[0U].Base == 0x7C00'0000U, IsValidBootDmaExclusions(list));

    fixture.BuildAmd64();
    fixture.AddDmar(1U, Boot::BootDmaExclusionMaximumCount + 1U);
    parsed = ParseAmd64(fixture, output) == AcpiTableParseError::None;
    AcpiTableParser::CollectDmaExclusions(fixture.BootInfo(), output, list);
    const bool overflowComplete = IsCompleteBootDmaExclusions(list);
    fixture.BuildAmd64();
    fixture.AddDmar(1U, Boot::BootDmaExclusionMaximumCount);
    const bool fullParsed = ParseAmd64(fixture, output) == AcpiTableParseError::None;
    AcpiTableParser::CollectDmaExclusions(fixture.BootInfo(), output, list);
    ASTERIA_EXPECT(runner, "65 RMRRs fill the list and declare an incomplete one; 64 do not", parsed, fullParsed,
                   !overflowComplete, list.Count == Boot::BootDmaExclusionMaximumCount,
                   list.DeclaredCount == Boot::BootDmaExclusionMaximumCount, IsCompleteBootDmaExclusions(list),
                   IsValidBootDmaExclusions(list), list.Ranges[63U].Base == 0x000E'0000U);
}

void TestIvrsHandoff(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    fixture.BuildAmd64();
    fixture.AddDmar(1U, 0U);
    Foundation::UInt8* const ivrs = fixture.Bytes() + DmarOffset;
    ClearBytes(ivrs, 96U);
    StoreSignature(ivrs, "IVRS", 4U);
    Store32(ivrs + 4U, 96U);
    ivrs[8U] = 2U;
    ivrs[48U] = 0x10U;
    Store16(ivrs + 50U, 24U);
    Store64(ivrs + 56U, 0xFED80000U);
    ivrs[72U] = 0x10U;
    Store16(ivrs + 74U, 24U);
    Store64(ivrs + 80U, 0xFED84000U);
    Store16(ivrs + 88U, 1U);
    RepairChecksum(ivrs, 96U, 9U);
    AcpiTopology output{};
    ASTERIA_EXPECT(runner, "multiple and nonzero-segment IVHD units remain available to early handoff",
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, output.Ivrs.UnitCount == 2U,
                   output.IommuHandoffCount == 2U, output.IommuHandoffUnsafe == 0U,
                   output.IommuHandoffs[0U].RegisterBase == 0xFED80000U,
                   output.IommuHandoffs[1U].RegisterBase == 0xFED84000U);
    ++ivrs[40U];
    ASTERIA_EXPECT(runner, "a rejected IVRS checksum still retains bounded handoff bases and backend presence",
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, output.Ivrs.Malformed == 1U,
                   output.Ivrs.PhysicalAddress != 0U, output.IommuHandoffCount == 2U);
    Store16(ivrs + 74U, 2U);
    ASTERIA_EXPECT(runner, "an unsafe remainder retains earlier bases and stops before DMA",
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, output.IommuHandoffCount == 1U,
                   output.IommuHandoffUnsafe == 1U);
}

void TestDmar(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    if (!fixture.IsReady())
    {
        ASTERIA_EXPECT(runner, "the DMAR fixture is mapped below 4 GiB", false);
        return;
    }
    AcpiTopology output{};
    fixture.BuildAmd64();
    const AcpiTableParseError absent = ParseAmd64(fixture, output);
    ASTERIA_EXPECT(runner, "without a DMAR its fields are zero", absent == AcpiTableParseError::None,
                   DmarIsEmpty(output), output.ProcessorCount == 2U);

    fixture.BuildAmd64();
    fixture.AddDmar(1U, 0U);
    const AcpiTableParseError one = ParseAmd64(fixture, output);
    ASTERIA_EXPECT(
        runner, "a DMAR's one unit is retained with its flags and the host address width",
        one == AcpiTableParseError::None,
        output.DmarPhysicalAddress ==
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(fixture.Bytes() + DmarOffset)),
        output.DmarHostAddressWidth == 39U, output.DmarFlags == 1U, output.DmaRemappingUnitCount == 1U,
        output.ReservedMemoryRegionCount == 0U, output.DmaRemappingUnits[0U].RegisterBaseAddress == DmarUnitBase,
        output.DmaRemappingUnits[0U].Flags == Boot::Acpi::AcpiDmaRemappingIncludeAllFlag,
        output.DmaRemappingUnits[0U].Segment == 0U, output.DmaRemappingUnits[1U].RegisterBaseAddress == 0U,
        output.PciConfigurationRegionCount == 1U);

    fixture.BuildAmd64();
    fixture.AddDmar(5U, 2U);
    const AcpiTableParseError many = ParseAmd64(fixture, output);
    ASTERIA_EXPECT(runner, "every unit and reserved region is counted and the first four units retained",
                   many == AcpiTableParseError::None, output.DmaRemappingUnitCount == 5U,
                   output.ReservedMemoryRegionCount == 2U,
                   output.DmaRemappingUnits[3U].RegisterBaseAddress == (DmarUnitBase + (3U * PageBytes)),
                   output.DmaRemappingUnits[3U].Segment == 3U, output.DmaRemappingUnits[3U].Flags == 0U);

    fixture.BuildAmd64();
    fixture.AddDmar(2U, 0U, 3U, 2U);
    const AcpiTableParseError scoped = ParseAmd64(fixture, output);
    ASTERIA_EXPECT(runner, "each retained unit's device scopes are kept with their unit, bus and first hop",
                   scoped == AcpiTableParseError::None, output.DmaDeviceScopeCount == 6U,
                   output.DmaDeviceScopes[0U].Unit == 0U, output.DmaDeviceScopes[0U].Type == 1U,
                   output.DmaDeviceScopes[0U].StartBus == 2U, output.DmaDeviceScopes[0U].PathEntries == 1U,
                   output.DmaDeviceScopes[0U].Device == 0U, output.DmaDeviceScopes[2U].PathEntries == 2U,
                   output.DmaDeviceScopes[2U].Device == 2U, output.DmaDeviceScopes[2U].Path[0U] == 2U,
                   output.DmaDeviceScopes[2U].Path[1U] == 0U, output.DmaDeviceScopes[4U].Unit == 1U,
                   output.DmaDeviceScopes[4U].Device == 1U, output.DmaDeviceScopes[4U].Function == 1U,
                   output.DmaDeviceScopes[6U].Type == 0U);

    fixture.BuildAmd64();
    fixture.AddDmar(1U, 0U, 40U);
    const AcpiTableParseError crowded = ParseAmd64(fixture, output);
    ASTERIA_EXPECT(runner, "every scope is counted and the first 32 retained", crowded == AcpiTableParseError::None,
                   output.DmaDeviceScopeCount == 40U, output.DmaDeviceScopes[31U].Device == 31U);

    fixture.BuildAmd64();
    fixture.AddDmar(1U, 0U, 1U);
    fixture.Bytes()[DmarOffset + 48U + 16U + 1U] = 9U;
    fixture.RepairDmar();
    const AcpiTableParseError odd = ParseAmd64(fixture, output);
    ASTERIA_EXPECT(runner, "a scope with half a hop discards the DMAR", odd == AcpiTableParseError::None,
                   DmarIsEmpty(output));

    fixture.BuildAmd64();
    fixture.AddDmar(1U, 0U);
    fixture.Bytes()[DmarOffset + 48U + 8U] = 0x10U;
    fixture.RepairDmar();
    const AcpiTableParseError unaligned = ParseAmd64(fixture, output);
    ASTERIA_EXPECT(runner, "a unit's unaligned register base discards the DMAR", unaligned == AcpiTableParseError::None,
                   DmarIsEmpty(output), output.ProcessorCount == 2U);

    fixture.BuildAmd64();
    fixture.AddDmar(1U, 0U);
    Store16(fixture.Bytes() + DmarOffset + 48U + 2U, 17U);
    fixture.RepairDmar();
    const AcpiTableParseError overrun = ParseAmd64(fixture, output);
    const bool overrunEmpty = DmarIsEmpty(output);
    fixture.BuildAmd64();
    fixture.AddDmar(1U, 0U);
    Store16(fixture.Bytes() + DmarOffset + 48U + 2U, 2U);
    fixture.RepairDmar();
    const AcpiTableParseError tiny = ParseAmd64(fixture, output);
    const bool tinyEmpty = DmarIsEmpty(output);
    fixture.BuildAmd64();
    fixture.AddDmar(1U, 0U);
    ++fixture.Bytes()[DmarOffset + 40U];
    const AcpiTableParseError corrupt = ParseAmd64(fixture, output);
    const bool corruptEmpty = DmarIsEmpty(output);
    ASTERIA_EXPECT(runner, "a rejected DMAR retains its firmware-handoff register base", output.IommuHandoffCount == 1U,
                   output.IommuHandoffs[0U].Kind == 1U, output.IommuHandoffs[0U].RegisterBase == 0xFED90000U);
    ASTERIA_EXPECT(runner, "a structure past the table, a short structure or a bad checksum discards the DMAR",
                   overrun == AcpiTableParseError::None, overrunEmpty, tiny == AcpiTableParseError::None, tinyEmpty,
                   corrupt == AcpiTableParseError::None, corruptEmpty, output.PciConfigurationRegionCount == 1U);
}

void TestLargeRegisterDmar(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    if (!ASTERIA_EXPECT(runner, "large-register DMAR fixture", fixture.IsReady()))
    {
        return;
    }
    fixture.BuildAmd64();
    fixture.AddLargeRegisterDmar();
    AcpiTopology output{};
    ASTERIA_EXPECT(runner, "DRHD sizes retained; SATC and SIDP skipped by length",
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, output.DmaRemappingUnitCount == 3U,
                   output.DmaDeviceScopeCount == 6U, output.DmaRemappingUnits[0U].Size == 4U,
                   output.DmaRemappingUnits[2U].Size == 4U,
                   output.DmaRemappingUnits[2U].RegisterBaseAddress == 0xFC82'0000U,
                   Platform::Device::SelectVtdUnit(output, 48U).Error == Platform::Device::VtdTopologyError::None);
    // Read only this table at test time. No machine table is a repository fixture.
    const char* const path{std::getenv("ASTERIA_LARGE_REGISTER_DMAR")};
    if (path == nullptr || path[0U] == '\0')
    {
        static_cast<void>(std::printf("SKIP: set ASTERIA_LARGE_REGISTER_DMAR to a matching external DMAR.AML\n"));
        return;
    }
    std::FILE* const file{std::fopen(path, "rb")};
    if (!ASTERIA_EXPECT(runner, "external large-register DMAR read", file != nullptr))
    {
        return;
    }
    const Foundation::Size length{std::fread(fixture.Bytes() + DmarOffset, 1U, PageBytes - DmarOffset, file)};
    const bool complete{std::feof(file) != 0};
    static_cast<void>(std::fclose(file));
    ASTERIA_EXPECT(runner, "external large-register DMAR accepts all three adjacent units", complete, length >= 48U,
                   ParseAmd64(fixture, output) == AcpiTableParseError::None, output.DmaRemappingUnitCount == 3U,
                   output.DmaRemappingUnits[0U].Size == 4U,
                   output.DmaRemappingUnits[1U].RegisterBaseAddress == 0xFC81'0000U,
                   Platform::Device::SelectVtdUnit(output, 48U).Error == Platform::Device::VtdTopologyError::None);
}

void TestProcessorCapacity(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    ASTERIA_EXPECT(runner, "processor-capacity ACPI fixture is mapped below 4 GiB", fixture.IsReady());
    if (!fixture.IsReady())
    {
        return;
    }

    constexpr LocalApicEntry SixEnabled[]{
        {0U, 0U, Enabled}, {1U, 1U, Enabled}, {2U, 2U, Enabled},
        {3U, 3U, Enabled}, {4U, 4U, Enabled}, {5U, 5U, Enabled},
    };
    constexpr Foundation::UInt8 FirstFour[]{0U, 1U, 2U, 3U};
    ASTERIA_EXPECT(runner, "more than four enabled processors boot with the first four and count the rest",
                   ParseRetains(fixture, SixEnabled, 6U, FirstFour, 4U, 2U));

    fixture.BuildAmd64Processors(SixEnabled, 6U);
    AcpiTopology bspOutput{};
    constexpr Foundation::UInt8 WithBsp[]{0U, 1U, 2U, 5U};
    ASTERIA_EXPECT(runner, "a BSP beyond the first four enabled entries is retained in firmware order",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, bspOutput,
                                          5U) == AcpiTableParseError::None,
                   RetainsProcessors(bspOutput, WithBsp, 4U, 2U));

    fixture.BuildAmd64Processors(SixEnabled, 6U);
    auto* const entries = fixture.Bytes() + MadtEntriesOffset;
    ClearBytes(entries, 108U);
    for (Foundation::Size cpu{0U}; cpu < 6U; ++cpu)
    {
        auto* const entry = entries + cpu * 16U;
        entry[0U] = 9U;
        entry[1U] = 16U;
        Store32(entry + 4U, static_cast<Foundation::UInt32>(0x100U + cpu));
        Store32(entry + 8U, Enabled);
        Store32(entry + 12U, static_cast<Foundation::UInt32>(cpu));
    }
    // AMD64 permits a topology with no IOAPIC; only the CPU entries are needed.
    Store32(fixture.Bytes() + MadtOffset + 4U, 44U + 6U * 16U);
    fixture.RepairMadt();
    ASTERIA_EXPECT(runner, "BSP reservation uses the full 32-bit x2APIC ID",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, bspOutput,
                                          0x105U) == AcpiTableParseError::None,
                   bspOutput.ProcessorCount == 4U, bspOutput.IgnoredProcessorCount == 2U,
                   bspOutput.Processors[0U].HardwareId == 0x100U, bspOutput.Processors[1U].HardwareId == 0x101U,
                   bspOutput.Processors[2U].HardwareId == 0x102U, bspOutput.Processors[3U].HardwareId == 0x105U);

    // Online-capable processors listed first give way to later enabled ones.
    constexpr LocalApicEntry OnlineCapableFirst[]{
        {0U, 10U, OnlineCapable}, {1U, 11U, OnlineCapable}, {2U, 2U, Enabled}, {3U, 13U, OnlineCapable},
        {4U, 4U, Enabled},        {5U, 5U, Enabled},        {6U, 6U, Enabled},
    };
    constexpr Foundation::UInt8 EnabledOnly[]{2U, 4U, 5U, 6U};
    ASTERIA_EXPECT(runner, "the first four enabled processors displace online-capable ones",
                   ParseRetains(fixture, OnlineCapableFirst, 7U, EnabledOnly, 4U, 3U));

    constexpr LocalApicEntry OneEnabled[]{
        {0U, 0U, Enabled},        {1U, 11U, OnlineCapable}, {2U, 12U, OnlineCapable},
        {3U, 13U, OnlineCapable}, {4U, 14U, OnlineCapable},
    };
    constexpr Foundation::UInt8 OnlineCapableFill[]{0U, 11U, 12U, 13U};
    ASTERIA_EXPECT(runner, "online-capable processors fill the slots no enabled processor needs, in order",
                   ParseRetains(fixture, OneEnabled, 5U, OnlineCapableFill, 4U, 1U));

    constexpr LocalApicEntry EnabledAfterFull[]{
        {0U, 0U, Enabled}, {1U, 11U, OnlineCapable}, {2U, 2U, Enabled}, {3U, 13U, OnlineCapable}, {4U, 4U, Enabled},
    };
    constexpr Foundation::UInt8 LastOnlineCapableReplaced[]{0U, 11U, 2U, 4U};
    ASTERIA_EXPECT(runner, "an enabled processor beyond capacity replaces the last online-capable one",
                   ParseRetains(fixture, EnabledAfterFull, 5U, LastOnlineCapableReplaced, 4U, 1U));

    constexpr LocalApicEntry ExactlyFour[]{
        {0U, 0U, Enabled},
        {1U, 1U, Enabled},
        {2U, 2U, Enabled},
        {3U, 3U, Enabled},
    };
    ASTERIA_EXPECT(runner, "a machine within capacity ignores nothing",
                   ParseRetains(fixture, ExactlyFour, 4U, FirstFour, 4U, 0U));

    // An ignored processor still has to be well formed and distinct from the
    // retained ones; the output is unchanged on rejection.
    const AcpiTopology sentinel = MakeSentinel();
    constexpr LocalApicEntry IgnoredDuplicate[]{
        {0U, 0U, Enabled}, {1U, 1U, Enabled}, {2U, 2U, Enabled}, {3U, 3U, Enabled}, {4U, 1U, Enabled},
    };
    fixture.BuildAmd64Processors(IgnoredDuplicate, 5U);
    AcpiTopology output = sentinel;
    ASTERIA_EXPECT(runner, "an ignored processor duplicating a retained APIC ID is rejected atomically",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output) ==
                       AcpiTableParseError::DuplicateProcessor,
                   TopologiesEqual(output, sentinel));
    constexpr LocalApicEntry IgnoredMalformed[]{
        {0U, 0U, Enabled}, {1U, 1U, Enabled}, {2U, 2U, Enabled}, {3U, 3U, Enabled}, {4U, 4U, Enabled | OnlineCapable},
    };
    fixture.BuildAmd64Processors(IgnoredMalformed, 5U);
    output = sentinel;
    ASTERIA_EXPECT(runner, "an ignored processor with contradictory flags is rejected atomically",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output) ==
                       AcpiTableParseError::InvalidControllerEntry,
                   TopologiesEqual(output, sentinel));
}

void TestDuplicateX2ApicQuirk(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    if (!fixture.IsReady())
    {
        ASTERIA_EXPECT(runner, "quirk fixture mapped", false);
        return;
    }
    for (Foundation::UInt32 reverse{0U}; reverse < 2U; ++reverse)
    {
        fixture.BuildAmd64();
        auto* const entries = fixture.Bytes() + MadtEntriesOffset;
        ClearBytes(entries, 24U);
        auto* const legacy = entries + (reverse == 0U ? 0U : 16U);
        auto* const extended = entries + (reverse == 0U ? 8U : 0U);
        legacy[0U] = 0U;
        legacy[1U] = 8U;
        legacy[2U] = 7U;
        legacy[3U] = 2U;
        Store32(legacy + 4U, Enabled);
        extended[0U] = 9U;
        extended[1U] = 16U;
        Store32(extended + 4U, 2U);
        Store32(extended + 8U, Enabled);
        Store32(extended + 12U, 7U);
        fixture.RepairMadt();
        AcpiTopology output{};
        ASTERIA_EXPECT(runner, "low x2APIC duplicate prefers legacy entry in either order",
                       AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output,
                                              2U) == AcpiTableParseError::None,
                       output.ProcessorCount == 1U, output.IgnoredProcessorCount == 0U,
                       output.Processors[0U].ControllerKind == AcpiProcessorControllerKind::LocalApic);
        legacy[3U] = 255U;
        Store32(extended + 4U, 255U);
        fixture.RepairMadt();
        ASTERIA_EXPECT(runner, "the low-ID duplicate quirk excludes ID 255",
                       AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64,
                                              output) == AcpiTableParseError::DuplicateProcessor);
    }
}

void TestCalibrationTimers(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    ASTERIA_EXPECT(runner, "counter fixture mapped", fixture.IsReady());
    if (!fixture.IsReady())
    {
        return;
    }
    for (Foundation::UInt32 variant{0U}; variant < 15U; ++variant)
    {
        fixture.BuildAmd64(true, false);
        Foundation::UInt8* const fadt = fixture.Bytes() + 1024U;
        Foundation::UInt8* const hpet = fixture.Bytes() + 1280U;
        ClearBytes(fadt, 220U);
        ClearBytes(hpet, 56U);
        StoreSignature(fadt, "FACP", 4U);
        Store32(fadt + 4U, variant == 8U ? 116U : 220U);
        fadt[8U] = variant == 8U ? 1U : 3U;
        Store32(fadt + 76U, 0x408U);
        fadt[91U] = 4U;
        Store32(fadt + 112U, variant == 1U ? 1U << 8U : (variant == 7U ? 1U << 20U : 0U));
        fadt[208U] = variant == 2U ? 0U : 1U;
        fadt[209U] = 32U;
        fadt[211U] = 3U;
        Store64(fadt + 212U, variant == 2U ? 0xFED0'1000U : 0x508U);
        if (variant == 3U)
        {
            fadt[210U] = 1U;
        }
        if (variant == 4U)
        {
            fadt[208U] = 2U;
        }
        if (variant == 5U)
        {
            Store64(fadt + 212U, 0x10000U);
        }
        if (variant == 6U)
        {
            fadt[211U] = 2U;
        }
        if (variant == 13U)
        {
            fadt[208U] = 0U;
            Store64(fadt + 212U, 1ULL << 47U);
        }
        RepairChecksum(fadt, variant == 8U ? 116U : 220U, 9U);
        if (variant == 9U)
        {
            fadt[9U] ^= 1U;
        }
        StoreSignature(hpet, "HPET", 4U);
        Store32(hpet + 4U, 56U);
        hpet[8U] = 1U;
        Store64(hpet + 44U, 0xFED0'0000U);
        if (variant == 10U)
        {
            hpet[40U] = 1U;
        }
        if (variant == 11U)
        {
            Store64(hpet + 44U, 0xFED0'0001U);
        }
        if (variant == 14U)
        {
            Store64(hpet + 44U, 1ULL << 47U);
        }
        RepairChecksum(hpet, 56U, 9U);
        Foundation::UInt8* const root = fixture.Bytes() + XsdtOffset;
        Store64(root + 44U, static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(fadt)));
        Store64(root + 52U, static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(hpet)));
        Store64(root + 60U, static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(fadt)));
        Store64(root + 68U, static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(hpet)));
        Store32(root + 4U, variant == 12U ? 76U : 60U);
        RepairChecksum(root, variant == 12U ? 76U : 60U, 9U);
        AcpiTopology topology{};
        const Foundation::UInt64 expected =
            variant == 2U ? 0xFED0'1000U
                          : ((variant >= 3U && variant <= 6U) || variant == 8U || variant == 13U
                                 ? 0x408U
                                 : (variant == 7U || variant == 9U || variant == 12U ? 0U : 0x508U));
        ASTERIA_EXPECT(runner, "extended PM preferred, malformed GAS falls back, reduced/duplicate rejected",
                       ParseAmd64(fixture, topology) == AcpiTableParseError::None, topology.PmTimerAddress == expected,
                       expected == 0U || topology.PmTimerSpace == (variant == 2U ? 0U : 1U),
                       topology.PmTimerBits ==
                           (variant == 1U ? 32U : (variant == 7U || variant == 9U || variant == 12U ? 0U : 24U)),
                       topology.HpetAddress ==
                           ((variant >= 10U && variant <= 12U) || variant == 14U ? 0U : 0xFED0'0000U));
    }
}

void TestOptionalLegacyDevices(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    ASTERIA_EXPECT(runner, "optional-device fixture is mapped", fixture.IsReady());
    if (!fixture.IsReady())
    {
        return;
    }
    constexpr Foundation::Size fadtOffset{1024U};
    for (Foundation::UInt32 variant{0U}; variant < 7U; ++variant)
    {
        fixture.BuildAmd64(true, false);
        Foundation::UInt8* const fadt = fixture.Bytes() + fadtOffset;
        StoreSignature(fadt, "FACP", 4U);
        const Foundation::Size length = (variant == 3U) ? 110U : 116U;
        Store32(fadt + 4U, static_cast<Foundation::UInt32>(length));
        fadt[8U] = (variant == 2U) ? 1U : 3U;
        Store16(fadt + 109U, (variant == 1U) ? 0U : 2U);
        RepairChecksum(fadt, length, 9U);
        if (variant == 4U)
        {
            fadt[9U] ^= 1U;
        }
        Foundation::UInt8* const root = fixture.Bytes() + XsdtOffset;
        const Foundation::UInt64 address =
            static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(fadt));
        Store64(root + 44U, (variant == 5U) ? address + PageBytes : address);
        const Foundation::Size rootLength = (variant == 6U) ? 60U : 52U;
        Store64(root + 52U, address);
        Store32(root + 4U, static_cast<Foundation::UInt32>(rootLength));
        RepairChecksum(root, rootLength, 9U);
        AcpiTopology topology{};
        ASTERIA_EXPECT(runner, "only a unique valid ACPI 2.0 FADT publishes the 8042 flag",
                       ParseAmd64(fixture, topology) == AcpiTableParseError::None,
                       topology.IapcBootArchitecture == ((variant == 0U) ? 2U : 0U),
                       topology.FadtRevision == ((variant <= 1U) ? 3U : 0U));
    }
    constexpr Foundation::Size ioOffset{MadtEntriesOffset + 24U};
    constexpr Foundation::Size overrideOffset{ioOffset + 12U};
    for (Foundation::UInt32 variant{0U}; variant < 6U; ++variant)
    {
        fixture.BuildAmd64();
        if (variant == 0U)
        {
            fixture.Bytes()[ioOffset] = 0xFEU;
        }
        else if (variant <= 2U)
        {
            Store32(fixture.Bytes() + ioOffset + 4U, (variant == 1U) ? 0U : 0xFEC0'0001U);
        }
        else if (variant == 3U)
        {
            fixture.Bytes()[overrideOffset + 2U] = 1U;
        }
        else
        {
            Store16(fixture.Bytes() + overrideOffset + 8U, (variant == 4U) ? 2U : 8U);
        }
        fixture.RepairMadt();
        AcpiTopology topology{};
        ASTERIA_EXPECT(runner, "missing IOAPICs and malformed overrides do not reject AMD64 boot",
                       ParseAmd64(fixture, topology) == AcpiTableParseError::None,
                       topology.IoControllerCount == ((variant < 3U) ? 0U : 1U),
                       topology.InterruptOverrideCount == ((variant < 3U) ? 1U : 0U),
                       topology.IapcBootArchitecture == 0U);
    }
}

void TestFailuresAreAtomic(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    ASTERIA_EXPECT(runner, "negative ACPI fixture is mapped below 4 GiB", fixture.IsReady());
    if (!fixture.IsReady())
    {
        return;
    }
    const AcpiTopology sentinel = MakeSentinel();

    fixture.BuildAmd64();
    fixture.Bytes()[8U] ^= 1U;
    AcpiTopology output = sentinel;
    ASTERIA_EXPECT(runner, "RSDP checksum failure leaves output unchanged",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output) ==
                       AcpiTableParseError::InvalidRsdp,
                   TopologiesEqual(output, sentinel));

    fixture.BuildAmd64();
    fixture.Bytes()[XsdtOffset + 9U] ^= 1U;
    output = sentinel;
    ASTERIA_EXPECT(runner, "root checksum failure leaves output unchanged",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output) ==
                       AcpiTableParseError::InvalidRootTable,
                   TopologiesEqual(output, sentinel));

    fixture.BuildAmd64();
    fixture.Bytes()[MadtEntriesOffset + 1U] = 1U;
    fixture.RepairMadt();
    output = sentinel;
    ASTERIA_EXPECT(runner, "short MADT entry is rejected before publication",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output) ==
                       AcpiTableParseError::InvalidMadt,
                   TopologiesEqual(output, sentinel));

    fixture.BuildAmd64();
    Store32(fixture.Bytes() + MadtEntriesOffset + 4U, 3U);
    fixture.RepairMadt();
    output = sentinel;
    ASTERIA_EXPECT(runner, "contradictory processor flags are rejected before publication",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output) ==
                       AcpiTableParseError::InvalidControllerEntry,
                   TopologiesEqual(output, sentinel));

    bool invalidLegacyLocalSapicRejected{true};
    for (Foundation::UInt8 length{13U}; length <= 15U; ++length)
    {
        fixture.BuildIa64();
        fixture.Bytes()[MadtEntriesOffset + 1U] = length;
        fixture.RepairMadt();
        output = sentinel;
        invalidLegacyLocalSapicRejected =
            invalidLegacyLocalSapicRejected &&
            (AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Ia64, output) ==
             AcpiTableParseError::InvalidControllerEntry) &&
            TopologiesEqual(output, sentinel);
    }
    fixture.BuildIa64(true);
    fixture.Bytes()[MadtEntriesOffset + 5U] = 1U;
    fixture.RepairMadt();
    output = sentinel;
    invalidLegacyLocalSapicRejected =
        invalidLegacyLocalSapicRejected &&
        (AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Ia64, output) ==
         AcpiTableParseError::InvalidControllerEntry) &&
        TopologiesEqual(output, sentinel);
    fixture.BuildIa64(true);
    Store32(fixture.Bytes() + MadtEntriesOffset + 8U, 4U);
    fixture.RepairMadt();
    output = sentinel;
    ASTERIA_EXPECT(runner, "malformed legacy local SAPIC entries are rejected atomically",
                   invalidLegacyLocalSapicRejected,
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Ia64, output) ==
                       AcpiTableParseError::InvalidControllerEntry,
                   TopologiesEqual(output, sentinel));

    fixture.BuildAmd64();
    Foundation::UInt8* const duplicateIoController = fixture.Bytes() + MadtEntriesOffset + 8U + 16U + 12U + 10U;
    duplicateIoController[0U] = 1U;
    duplicateIoController[1U] = 12U;
    duplicateIoController[2U] = 4U;
    duplicateIoController[3U] = 0U;
    Store32(duplicateIoController + 4U, 0xFEC0'0000U);
    Store32(duplicateIoController + 8U, 32U);
    fixture.RepairMadt();
    output = sentinel;
    ASTERIA_EXPECT(runner, "an unusable duplicate I/O controller is ignored on AMD64",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output) ==
                       AcpiTableParseError::None,
                   output.IoControllerCount == 1U);

    fixture.BuildAmd64();
    output = sentinel;
    ASTERIA_EXPECT(runner, "an APIC MADT cannot be consumed as an IA-64 SAPIC model",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Ia64, output) ==
                       AcpiTableParseError::UnsupportedInterruptModel,
                   TopologiesEqual(output, sentinel));

    fixture.BuildAmd64();
    fixture.BootInfo().MemoryRegions[TableRegionIndex].Kind = Boot::PhysicalMemoryKind::Conventional;
    output = sentinel;
    ASTERIA_EXPECT(runner, "tables in reusable memory are not dereferenced",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output) ==
                       AcpiTableParseError::TableInReusableMemory,
                   TopologiesEqual(output, sentinel));
}

// Real firmware places the RSDP and tables in ACPI memory, but also in
// reserved or runtime memory or in ranges the map omits (the legacy BIOS
// area): every placement the kernel will not reuse is accepted, every byte
// in reusable memory rejected.
void TestTablePlacement(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    if (!fixture.IsReady())
    {
        ASTERIA_EXPECT(runner, "the placement fixture maps its page", false);
        return;
    }
    struct Placement
    {
        Boot::PhysicalMemoryKind Kind;
        Foundation::UInt32 FirmwareType;
        bool Accepted;
    };
    constexpr Placement Placements[]{
        {Boot::PhysicalMemoryKind::Reserved, 0U, true},        {Boot::PhysicalMemoryKind::Loader, 2U, false},
        {Boot::PhysicalMemoryKind::BootServices, 4U, false},   {Boot::PhysicalMemoryKind::RuntimeServices, 6U, true},
        {Boot::PhysicalMemoryKind::Conventional, 7U, false},   {Boot::PhysicalMemoryKind::Unusable, 8U, true},
        {Boot::PhysicalMemoryKind::AcpiReclaimable, 9U, true}, {Boot::PhysicalMemoryKind::AcpiNonVolatile, 10U, true},
        {Boot::PhysicalMemoryKind::MemoryMappedIo, 11U, true}, {Boot::PhysicalMemoryKind::PalCode, 13U, true},
        {Boot::PhysicalMemoryKind::Persistent, 14U, false},    {Boot::PhysicalMemoryKind::Unaccepted, 15U, false},
    };
    const AcpiTopology sentinel = MakeSentinel();
    for (const Placement& placement : Placements)
    {
        fixture.BuildAmd64();
        Boot::PhysicalMemoryRegion& region = fixture.BootInfo().MemoryRegions[TableRegionIndex];
        region.Kind = placement.Kind;
        region.FirmwareType = placement.FirmwareType;
        AcpiTopology output = sentinel;
        const AcpiTableParseError error =
            AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output);
        ASTERIA_EXPECT(runner, "tables are accepted outside reusable memory and rejected in it",
                       placement.Accepted ? ((error == AcpiTableParseError::None) && (output.ProcessorCount == 2U))
                                          : ((error == AcpiTableParseError::TableInReusableMemory) &&
                                             TopologiesEqual(output, sentinel)));
    }

    // The page absent from the map: a gap between the conventional regions.
    fixture.BuildAmd64();
    Boot::BootInfo& unlisted = fixture.BootInfo();
    unlisted.MemoryRegions[TableRegionIndex] = unlisted.MemoryRegions[TableRegionIndex + 1U];
    unlisted.MemoryRegionCount = 2U;
    unlisted.FirmwareMemoryMapSizeBytes = 2U * 40U;
    AcpiTopology output = sentinel;
    ASTERIA_EXPECT(runner, "tables in memory the map does not list are accepted",
                   AcpiTableParser::Parse(unlisted, fixture.Rsdp(), AcpiTargetArchitecture::Amd64, output) ==
                       AcpiTableParseError::None,
                   output.ProcessorCount == 2U);

    // An RSDP whose last bytes run into the conventional region after the
    // page is rejected before any of it is read.
    fixture.BuildAmd64();
    output = sentinel;
    ASTERIA_EXPECT(runner, "a structure that crosses into reusable memory is rejected",
                   AcpiTableParser::Parse(fixture.BootInfo(), fixture.Bytes() + PageBytes - 8U,
                                          AcpiTargetArchitecture::Amd64,
                                          output) == AcpiTableParseError::TableInReusableMemory,
                   TopologiesEqual(output, sentinel));
}
void TestDefinitionBlocks(TestContext& runner)
{
    AcpiFixture fixture;
    ASTERIA_EXPECT(runner, "definition-block fixture mapped", fixture.IsReady());
    if (!fixture.IsReady())
    {
        return;
    }
    for (Foundation::UInt32 variant{0U}; variant < 20U; ++variant)
    {
        const Foundation::UInt32 scenario = variant % 10U;
        if (variant < 10U)
        {
            fixture.BuildAmd64(scenario != 9U, false);
        }
        else
        {
            fixture.BuildIa64(false, false);
        }
        auto* const bytes = fixture.Bytes();
        auto* const fadt = bytes + 1024U;
        auto* const legacy = bytes + 1280U;
        auto* const extended = bytes + 1344U;
        auto* const ssdt = bytes + 1536U;
        auto* const root = bytes + 2048U;
        const Foundation::Size fadtLength = scenario == 2U ? 144U : 148U;
        StoreSignature(fadt, "FACP", 4U);
        Store32(fadt + 4U, static_cast<Foundation::UInt32>(fadtLength));
        fadt[8U] = 6U;
        Store16(fadt + 109U, 0xAB02U);
        Store32(fadt + 40U, static_cast<Foundation::UInt32>(reinterpret_cast<Foundation::UIntPointer>(legacy)));
        Store64(fadt + 140U, scenario == 0U ? 0U : reinterpret_cast<Foundation::UIntPointer>(extended));
        RepairChecksum(fadt, fadtLength, 9U);
        for (auto* table : {legacy, extended, ssdt})
        {
            StoreSignature(table, table == ssdt ? "SSDT" : "DSDT", 4U);
            Store32(table + 4U, 40U);
            StoreSignature(table + 16U, table == legacy ? "LEGACY  " : "EXTENDED", 8U);
            table[8U] = 2U;
            RepairChecksum(table, 40U, 9U);
        }
        if (scenario == 3U)
        {
            extended[9U] ^= 1U;
            ssdt[9U] ^= 1U;
        }
        if (scenario == 4U)
        {
            Store32(extended + 4U, 35U);
            Store32(ssdt + 4U, 1'048'577U);
        }
        if (scenario == 5U)
        {
            Store64(fadt + 140U, reinterpret_cast<Foundation::UIntPointer>(bytes + PageBytes - 20U));
            RepairChecksum(fadt, fadtLength, 9U);
        }
        const Foundation::Size stride = scenario == 9U ? 4U : 8U;
        const Foundation::Size ssdtEntries = scenario == 6U ? Boot::Acpi::MaximumAcpiSsdts + 2U : 1U;
        const Foundation::Size rootLength = 36U + (2U + ssdtEntries) * stride;
        StoreSignature(root, stride == 8U ? "XSDT" : "RSDT", 4U);
        Store32(root + 4U, static_cast<Foundation::UInt32>(rootLength));
        root[8U] = 1U;
        for (Foundation::Size index{0U}; index < 2U + ssdtEntries; ++index)
        {
            const auto* table = index == 0U ? bytes + MadtOffset : (index == 1U ? fadt : ssdt);
            const Foundation::UInt64 address = reinterpret_cast<Foundation::UIntPointer>(table);
            if (stride == 8U)
            {
                Store64(root + 36U + index * stride, address);
            }
            else
            {
                Store32(root + 36U + index * stride, static_cast<Foundation::UInt32>(address));
            }
        }
        if (scenario == 7U)
        {
            Store64(root + 52U, reinterpret_cast<Foundation::UIntPointer>(bytes + PageBytes - 40U));
            StoreSignature(bytes + PageBytes - 40U, "SSDT", 4U);
            Store32(bytes + PageBytes - 36U, 80U);
        }
        if (scenario == 8U)
        {
            Store64(root + 52U, reinterpret_cast<Foundation::UIntPointer>(fadt));
        }
        RepairChecksum(root, rootLength, 9U);
        if (stride == 8U)
        {
            Store64(bytes + 24U, reinterpret_cast<Foundation::UIntPointer>(root));
            RepairChecksum(bytes, 36U, 32U);
        }
        else
        {
            bytes[15U] = 0U;
            Store32(bytes + 16U, static_cast<Foundation::UInt32>(reinterpret_cast<Foundation::UIntPointer>(root)));
            RepairChecksum(bytes, 20U, 8U);
        }
        AcpiTopology topology{};
        const bool absentDsdt = scenario == 4U || scenario == 5U || scenario == 8U;
        const auto* selected = scenario == 0U || scenario == 2U ? legacy : extended;
        const Foundation::UInt32 expectedSsdts =
            scenario == 4U || scenario == 7U || scenario == 8U
                ? 0U
                : (scenario == 6U ? static_cast<Foundation::UInt32>(Boot::Acpi::MaximumAcpiSsdts) : 1U);
        ASTERIA_EXPECT(
            runner, "definition blocks select extended pointers and bound optional failures",
            AcpiTableParser::Parse(fixture.BootInfo(), fixture.Rsdp(),
                                   variant < 10U ? AcpiTargetArchitecture::Amd64 : AcpiTargetArchitecture::Ia64,
                                   topology) == AcpiTableParseError::None,
            topology.Dsdt.Length == (absentDsdt ? 0U : 40U),
            topology.Dsdt.PhysicalAddress == (absentDsdt ? 0U : reinterpret_cast<Foundation::UIntPointer>(selected)),
            topology.SsdtCount == expectedSsdts, topology.SsdtOverflowCount == (scenario == 6U ? 2U : 0U),
            topology.RawFadtRevision == (scenario == 8U ? 0U : 6U),
            topology.RawIapcBootArchitecture == (scenario == 8U ? 0U : 0xAB02U));
        if (!absentDsdt)
        {
            ASTERIA_EXPECT(runner, "definition metadata preserves OEM ID revision and bad checksum",
                           topology.Dsdt.OemTableId[0U] == static_cast<char>(selected[16U]),
                           topology.Dsdt.Revision == 2U, topology.Dsdt.ChecksumValid == (scenario == 3U ? 0U : 1U));
        }
        if (expectedSsdts != 0U)
        {
            ASTERIA_EXPECT(runner, "SSDT checksum verdict is retained even on failure",
                           topology.Ssdts[0U].ChecksumValid == (scenario == 3U ? 0U : 1U));
        }
    }
}
void TestPowerRegisters(TestContext& runner) noexcept
{
    AcpiFixture fixture;
    if (!fixture.IsReady())
    {
        return;
    }
    for (Foundation::UInt32 variant{0U}; variant < 7U; ++variant)
    {
        fixture.BuildAmd64(true, false);
        auto* const fadt = fixture.Bytes() + 1024U;
        StoreSignature(fadt, "FACP", 4U);
        Store32(fadt + 4U, 244U);
        fadt[8U] = 6U;
        Store32(fadt + 64U, 0x400U);
        Store32(fadt + 68U, 0x404U);
        fadt[89U] = 2U;
        Store32(fadt + 48U, 0xB2U);
        fadt[52U] = 0xA1U;
        Store32(fadt + 112U, variant == 3U ? (1U << 20U) : (1U << 10U));
        fadt[116U] = 1U;
        fadt[117U] = 8U;
        fadt[119U] = 1U;
        Store64(fadt + 120U, 0xCF9U);
        fadt[128U] = 6U;
        if (variant == 1U || variant == 2U)
        {
            fadt[172U] = 0U;
            fadt[173U] = 16U;
            fadt[175U] = 2U;
            Store64(fadt + 176U, 0xFED01000U);
        }
        if (variant == 2U)
        {
            fadt[174U] = 1U;
        }
        if (variant == 6U)
        {
            fadt[117U] = 16U;
        }
        RepairChecksum(fadt, 244U, 9U);
        if (variant == 4U)
        {
            fadt[9U] ^= 1U;
        }
        auto* const root = fixture.Bytes() + XsdtOffset;
        const auto address = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(fadt));
        Store64(root + 44U, address);
        Store64(root + 52U, address);
        Store32(root + 4U, variant == 5U ? 60U : 52U);
        RepairChecksum(root, variant == 5U ? 60U : 52U, 9U);
        AcpiTopology topology{};
        Boot::Acpi::PowerRegisters power{};
        const auto result = AcpiTableParser::Parse(fixture.BootInfo(), fixture.Bytes(), AcpiTargetArchitecture::Amd64,
                                                   topology, 0xFFFFFFFFU, &power);
        ASTERIA_EXPECT(runner, "power tables are checksummed, unique, GAS-validated and extended-first",
                       result == AcpiTableParseError::None,
                       power.ControlA.Address ==
                           (variant == 1U ? 0xFED01000U : (variant == 0U || variant == 6U ? 0x400U : 0U)),
                       power.Reset.Address == (variant < 3U ? 0xCF9U : 0U),
                       power.SmiCommand == (variant < 3U || variant == 6U ? 0xB2U : 0U),
                       power.AcpiEnable == (variant < 3U || variant == 6U ? 0xA1U : 0U));
    }
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"AcpiTableParserTests"};
    Asteria::Test::TestDefinitionBlocks(runner);
    Asteria::Test::TestAmd64Topology(runner);
    Asteria::Test::TestIa64Topology(runner);
    Asteria::Test::TestRsdtFallback(runner);
    Asteria::Test::TestMcfgValidation(runner);
    Asteria::Test::TestDmar(runner);
    Asteria::Test::TestLargeRegisterDmar(runner);
    Asteria::Test::TestIvrsHandoff(runner);
    Asteria::Test::TestSandyBridgeDmar(runner);
    Asteria::Test::TestDmaExclusions(runner);
    Asteria::Test::TestProcessorCapacity(runner);
    Asteria::Test::TestDuplicateX2ApicQuirk(runner);
    Asteria::Test::TestOptionalLegacyDevices(runner);
    Asteria::Test::TestCalibrationTimers(runner);
    Asteria::Test::TestPowerRegisters(runner);
    Asteria::Test::TestFailuresAreAtomic(runner);
    Asteria::Test::TestTablePlacement(runner);
    return runner.Finish();
}
