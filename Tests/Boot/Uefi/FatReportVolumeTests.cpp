// SPDX-License-Identifier: Apache-2.0
#include "Boot/Uefi/FatReportVolume.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace Uefi = Asteria::Boot::Uefi;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
constexpr Size SectorBytes{512U};
constexpr Size Sectors{131072U};
constexpr Size Reserved{32U};
constexpr Size FatSectors{1024U};
constexpr Size DataStart{Reserved + FatSectors * 2U};
struct Media
{
    UInt32 MediaId{1U};
    Uefi::Boolean Removable{0U};
    Uefi::Boolean Present{1U};
    Uefi::Boolean LogicalPartition{1U};
    Uefi::Boolean ReadOnly{0U};
    Uefi::Boolean WriteCaching{0U};
    UInt32 BlockSize{512U};
    UInt32 IoAlign{4096U};
    UInt64 LastBlock{Sectors - 1U};
};
struct Block;
using Read = Uefi::Status(ASTERIA_UEFI_API*)(Block*, UInt32, UInt64, Size, void*);
using Write = Uefi::Status(ASTERIA_UEFI_API*)(Block*, UInt32, UInt64, Size, const void*);
using Flush = Uefi::Status(ASTERIA_UEFI_API*)(Block*);
struct Block
{
    UInt64 Revision;
    Media* Medium;
    void* Reset;
    Read ReadFunction;
    Write WriteFunction;
    Flush FlushFunction;
};
std::vector<UInt8> g_disk{};
constinit Media g_media{};
constinit Block g_block{};
constinit Uefi::BootServices g_services{};
void* g_pages{nullptr};
Size g_writeCalls{0U};
Size g_failWrite{0U};
Size g_allocatedPages{0U};
Size g_releasedPages{0U};
bool g_failFlush{false};

void Put(Size offset, UInt64 value, Size bytes)
{
    for (Size i{0U}; i < bytes; ++i)
    {
        g_disk[offset + i] = static_cast<UInt8>(value & 255U);
        value >>= 8U;
    }
}
UInt32 Get(Size offset, Size bytes)
{
    UInt32 value{0U};
    for (Size i{0U}; i < bytes; ++i)
    {
        value |= UInt32{g_disk[offset + i]} << (i * 8U);
    }
    return value;
}
void Fat(UInt32 cluster, UInt32 value)
{
    for (Size copy{0U}; copy < 2U; ++copy)
    {
        Put((Reserved + copy * FatSectors) * SectorBytes + cluster * 4U, value, 4U);
    }
}
void Entry(Size cluster, Size index, const char* name, UInt8 attributes, UInt32 first)
{
    const Size offset{(DataStart + cluster - 2U) * SectorBytes + index * 32U};
    std::memcpy(g_disk.data() + offset, name, 11U);
    g_disk[offset + 11U] = attributes;
    Put(offset + 20U, first >> 16U, 2U);
    Put(offset + 26U, first & 0xFFFFU, 2U);
}
void Reset()
{
    g_disk.assign(Sectors * SectorBytes, 0U);
    Put(11U, SectorBytes, 2U);
    Put(13U, 1U, 1U);
    Put(14U, Reserved, 2U);
    Put(16U, 2U, 1U);
    Put(32U, Sectors, 4U);
    Put(36U, FatSectors, 4U);
    Put(44U, 2U, 4U);
    Put(510U, 0xAA55U, 2U);
    Fat(0U, 0x0FFF'FFF8U);
    for (UInt32 i{1U}; i <= 5U; ++i)
    {
        Fat(i, 0x0FFF'FFFFU);
    }
    Entry(2U, 0U, "EFI        ", 0x10U, 3U);
    Entry(3U, 0U, "ASTERIA    ", 0x10U, 4U);
    Entry(4U, 0U, "ACPI       ", 0x10U, 5U);
    g_writeCalls = 0U;
    g_failWrite = 0U;
    g_failFlush = false;
    g_media = {};
}
Uefi::Status ASTERIA_UEFI_API Allocate(Uefi::AllocateType, Uefi::MemoryType, Size count, UInt64* address)
{
    g_pages = std::aligned_alloc(4096U, count * 4096U);
    *address = reinterpret_cast<UInt64>(g_pages);
    g_allocatedPages += count;
    return g_pages == nullptr ? Uefi::NotFound : Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Free(UInt64 address, Size count)
{
    std::free(reinterpret_cast<void*>(address));
    g_releasedPages += count;
    g_pages = nullptr;
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API Handle(Uefi::Handle, Uefi::Guid*, void** interface)
{
    *interface = &g_block;
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API ReadSectors(Block*, UInt32 id, UInt64 sector, Size bytes, void* output)
{
    if (id != g_media.MediaId || sector >= Sectors || bytes != SectorBytes ||
        reinterpret_cast<UInt64>(output) % 4096U != 0U)
    {
        return Uefi::NotFound;
    }
    std::memcpy(output, g_disk.data() + sector * SectorBytes, bytes);
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API WriteSectors(Block*, UInt32 id, UInt64 sector, Size bytes, const void* input)
{
    ++g_writeCalls;
    if (g_writeCalls == g_failWrite || id != g_media.MediaId || sector >= Sectors || bytes != SectorBytes ||
        reinterpret_cast<UInt64>(input) % 4096U != 0U)
    {
        return Uefi::NotFound;
    }
    std::memcpy(g_disk.data() + sector * SectorBytes, input, bytes);
    return Uefi::Success;
}
Uefi::Status ASTERIA_UEFI_API FlushDisk(Block*)
{
    return g_failFlush ? Uefi::NotFound : Uefi::Success;
}
std::string ReadFile(Size directory, Size index)
{
    const Size entry{(DataStart + directory - 2U) * SectorBytes + index * 32U};
    UInt32 cluster{(Get(entry + 20U, 2U) << 16U) | Get(entry + 26U, 2U)};
    Size remaining{Get(entry + 28U, 4U)};
    std::string result{};
    while (remaining != 0U)
    {
        const Size bytes{remaining < SectorBytes ? remaining : SectorBytes};
        result.append(reinterpret_cast<const char*>(g_disk.data() + (DataStart + cluster - 2U) * SectorBytes), bytes);
        remaining -= bytes;
        cluster = Get(Reserved * SectorBytes + cluster * 4U, 4U) & 0x0FFF'FFFFU;
    }
    return result;
}
void Model(Asteria::TestSupport::TestContext& context)
{
    Reset();
    Uefi::FatReportVolume owner{g_services};
    Uefi::IAcpiReportVolume* const volume{owner.Open(nullptr)};
    ASTERIA_EXPECT(context, "clean logical FAT32 mounted", volume != nullptr);
    if (volume == nullptr)
    {
        return;
    }
    const UInt64 seed{Asteria::TestSupport::GetSeedRange(701U, 1U).First};
    Asteria::TestSupport::Random random{seed};
    for (Size step{0U}; step < 64U; ++step)
    {
        const Size failures{context.GetFailureCount()};
        const Size length{random.Below(10000U)};
        const std::string expected(length, static_cast<char>('A' + step % 26U));
        ASTERIA_EXPECT(context, "create or replace own report", volume->Start("ACPI.TXT"));
        Size offset{0U};
        while (offset < length)
        {
            const Size remaining{length - offset};
            const Size chunk{remaining < 97U ? remaining : 97U};
            ASTERIA_EXPECT(context, "streamed chunks accepted", volume->Append(expected.data() + offset, chunk));
            offset += chunk;
        }
        ASTERIA_EXPECT(
            context, "exact byte length and mirrored FAT after publish", volume->Finish(), ReadFile(4U, 1U) == expected,
            std::memcmp(g_disk.data() + Reserved * SectorBytes, g_disk.data() + (Reserved + FatSectors) * SectorBytes,
                        FatSectors * SectorBytes) == 0,
            (Get(Reserved * SectorBytes + 4U, 4U) & 0x0C00'0000U) == 0x0C00'0000U);
        if (context.GetFailureCount() != failures)
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu operation=replace-report bytes=%zu\n",
                                           static_cast<unsigned long long>(seed), step, length));
            return;
        }
    }
    ASTERIA_EXPECT(context, "raw-table directory and NOAML existence", volume->Start("ACPI/DSDT.AML"),
                   volume->Append("own", 3U), volume->Finish(), ReadFile(5U, 0U) == "own",
                   volume->Exists("NOAML") == Uefi::FilePresence::Missing);
    ASTERIA_EXPECT(context, "report pages released", owner.Release(), g_pages == nullptr);
}
void Boundaries(Asteria::TestSupport::TestContext& context)
{
    for (Size fault{0U}; fault < 6U; ++fault)
    {
        Reset();
        if (fault == 0U)
        {
            Put(13U, 3U, 1U);
        }
        else if (fault == 1U)
        {
            Put(36U, 0xFFFF'FFFFU, 4U);
        }
        else if (fault == 2U)
        {
            Put((Reserved + FatSectors) * SectorBytes + 4U, 0U, 4U);
        }
        else if (fault == 3U)
        {
            // No matching EFI entry and a cyclic root must terminate.
            g_disk[DataStart * SectorBytes] = 'X';
            for (Size offset{32U}; offset < SectorBytes; offset += 32U)
            {
                g_disk[DataStart * SectorBytes + offset] = 'X';
            }
            Fat(2U, 2U);
        }
        Uefi::FatReportVolume owner{g_services};
        Uefi::IAcpiReportVolume* const volume{owner.Open(nullptr)};
        if (fault < 4U)
        {
            ASTERIA_EXPECT(context, "malformed geometry/mirrors/cycle rejected without writes", volume == nullptr,
                           g_writeCalls == 0U);
        }
        else
        {
            ASTERIA_EXPECT(context, "valid boundary volume", volume != nullptr, owner.Open(nullptr) == nullptr);
            if (fault == 4U)
            {
                g_disk[(DataStart + 2U) * SectorBytes + 64U] = 'X';
                ASTERIA_EXPECT(context, "insertion cannot resurrect stale entries beyond end marker",
                               !volume->Start("REPORT.TXT"), g_writeCalls == 0U);
            }
            else
            {
                Entry(4U, 1U, "NOAML      ", 0x20U, 0U);
                ASTERIA_EXPECT(context, "existing NOAML on raw ESP",
                               volume->Exists("NOAML") == Uefi::FilePresence::Present);
            }
        }
    }
}
void LongMarker(Asteria::TestSupport::TestContext& context)
{
    Reset();
    constexpr char alias[]{"NOFADT~1   "};
    Entry(4U, 2U, alias, 0x20U, 0U);
    UInt8 checksum{0U};
    for (Size byte{0U}; byte < 11U; ++byte)
    {
        checksum = static_cast<UInt8>(((checksum & 1U) << 7U) + (checksum >> 1U) + static_cast<UInt8>(alias[byte]));
    }
    const Size offset{(DataStart + 2U) * SectorBytes + 32U};
    Put(offset, 0x41U, 1U);
    Put(offset + 11U, 0x0FU, 1U);
    Put(offset + 13U, checksum, 1U);
    constexpr Size positions[]{1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};
    constexpr char marker[]{"NOFADT8042"};
    for (Size character{0U}; character < 13U; ++character)
    {
        Put(offset + positions[character],
            character < 10U ? static_cast<UInt8>(marker[character]) : (character == 10U ? 0U : 0xFFFFU), 2U);
    }
    Uefi::FatReportVolume owner{g_services};
    auto* const volume = owner.Open(nullptr);
    ASTERIA_EXPECT(context, "long diagnostic marker", volume != nullptr,
                   volume->Exists(marker) == Uefi::FilePresence::Present, g_writeCalls == 0U);
    Put(offset + 13U, checksum ^ 1U, 1U);
    ASTERIA_EXPECT(context, "long name must bind the following short entry",
                   volume->Exists(marker) == Uefi::FilePresence::Missing);
}
void Failures(Asteria::TestSupport::TestContext& context)
{
    for (Size fault{0U}; fault < 5U; ++fault)
    {
        Reset();
        if (fault == 0U)
        {
            g_media.ReadOnly = 1U;
        }
        else if (fault == 1U)
        {
            Fat(1U, 0x07FF'FFFFU);
        }
        Uefi::FatReportVolume owner{g_services};
        Uefi::IAcpiReportVolume* const volume{owner.Open(nullptr)};
        if (fault < 2U)
        {
            ASTERIA_EXPECT(context, "readonly and dirty media are never written", volume == nullptr,
                           g_writeCalls == 0U);
            continue;
        }
        ASTERIA_EXPECT(context, "start failure model", volume != nullptr, volume->Start("REPORT.TXT"),
                       volume->Append("old", 3U), volume->Finish());
        if (fault == 2U)
        {
            g_failWrite = g_writeCalls + 1U;
            ASTERIA_EXPECT(context, "metadata IO failure fences writes", !volume->Start("REPORT.TXT"));
        }
        else
        {
            ASTERIA_EXPECT(context, "replace staging starts", volume->Start("REPORT.TXT"), volume->Append("new", 3U));
            if (fault == 3U)
            {
                g_failWrite = g_writeCalls + 1U;
            }
            else
            {
                g_failFlush = true;
            }
            ASTERIA_EXPECT(context, "data/flush failure retains the old directory entry", !volume->Finish());
        }
        ASTERIA_EXPECT(context, "failed transaction leaves old file and fences the volume", ReadFile(4U, 1U) == "old",
                       !volume->Start("ACPI.TXT"));
    }
}
} // namespace
int main()
{
    Asteria::TestSupport::TestContext context{"Firmware FAT report volume"};
    g_block.Medium = &g_media;
    g_block.ReadFunction = ReadSectors;
    g_block.WriteFunction = WriteSectors;
    g_block.FlushFunction = FlushDisk;
    g_services.AllocatePagesFunction = Allocate;
    g_services.FreePagesFunction = Free;
    g_services.HandleProtocolFunction = Handle;
    Model(context);
    Failures(context);
    LongMarker(context);
    Boundaries(context);
    ASTERIA_EXPECT(context, "all page owners restored resources", g_allocatedPages == g_releasedPages);
    return context.Finish();
}
