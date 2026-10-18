// SPDX-License-Identifier: Apache-2.0
#include "User/Filesystem/Partitions.hpp"
#include "Support/TestContext.hpp"
namespace
{
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
using Foundation::UInt64;
using Foundation::UInt32;
using Foundation::UInt8;
static UInt8 disk[64U][512U]{};
static User::Filesystem::Partitions table;
static Foundation::Size writes;
bool Read(void*, UInt64 sector, UInt32 count, UInt8* buffer) noexcept
{
    if (sector >= 64U || count != 1U)
    {
        return false;
    }
    for (Foundation::Size at{0U}; at < 512U; ++at)
    {
        buffer[at] = disk[sector][at];
    }
    return true;
}
bool Write(void*, UInt64, UInt32, const UInt8*) noexcept
{
    ++writes;
    return false;
}
void Put(UInt8* p, UInt64 value, Foundation::Size bytes) noexcept
{
    for (Foundation::Size n{0U}; n < bytes; ++n)
    {
        p[n] = static_cast<UInt8>(value >> (n * 8U));
    }
}
UInt32 Crc(const UInt8* p, Foundation::Size bytes) noexcept
{
    UInt32 crc{~0U};
    for (Foundation::Size n{0U}; n < bytes; ++n)
    {
        crc ^= p[n];
        for (Foundation::Size bit{0U}; bit < 8U; ++bit)
        {
            crc = (crc >> 1U) ^ (crc & 1U ? 0xEDB88320U : 0U);
        }
    }
    return ~crc;
}
void Test(TestSupport::TestContext& t) noexcept
{
    const User::Filesystem::FatBlockDevice device{nullptr, &Read, &Write, 64U};
    disk[0U][510U] = 0x55U;
    disk[0U][511U] = 0xAAU;
    auto* mbr = disk[0U] + 446U;
    mbr[4U] = 0xEFU;
    Put(mbr + 8U, 8U, 4U);
    Put(mbr + 12U, 32U, 4U);
    ASTERIA_EXPECT(t, "legacy MBR enumeration is read-only and bounds ranges",
                   table.Read(device) == User::Filesystem::FatStatus::Done, table.Count() == 1U,
                   table.Get(0U).First == 8U, table.Get(0U).Sectors == 32U, table.Get(0U).Esp, writes == 0U);
    Put(mbr + 12U, 64U, 4U);
    ASTERIA_EXPECT(t, "out-of-media MBR partitions are refused",
                   table.Read(device) == User::Filesystem::FatStatus::Corrupt);
    mbr[4U] = 0x0FU;
    Put(mbr + 8U, 8U, 4U);
    Put(mbr + 12U, 48U, 4U);
    auto* ebr = disk[8U];
    ebr[510U] = 0x55U;
    ebr[511U] = 0xAAU;
    auto* logical = ebr + 446U;
    logical[4U] = 0x0BU;
    Put(logical + 8U, 1U, 4U);
    Put(logical + 12U, 10U, 4U);
    auto* link = logical + 16U;
    link[4U] = 0x0FU;
    Put(link + 8U, 16U, 4U);
    disk[24U][510U] = 0x55U;
    disk[24U][511U] = 0xAAU;
    auto* second = disk[24U] + 446U;
    second[4U] = 0xEFU;
    Put(second + 8U, 1U, 4U);
    Put(second + 12U, 10U, 4U);
    ASTERIA_EXPECT(t, "EBR offsets are relative to their table and the extended container",
                   table.Read(device) == User::Filesystem::FatStatus::Done, table.Count() == 2U,
                   table.Get(0U).First == 9U, table.Get(1U).First == 25U, table.Get(1U).Esp);
    Put(second + 12U, 40U, 4U);
    ASTERIA_EXPECT(t, "logical partitions cannot leave their extended container",
                   table.Read(device) == User::Filesystem::FatStatus::Corrupt);
    Put(second + 12U, 10U, 4U);
    second[20U] = 0x0FU;
    Put(second + 24U, 16U, 4U);
    ASTERIA_EXPECT(t, "cyclic EBR links are refused within the enumeration bound",
                   table.Read(device) == User::Filesystem::FatStatus::Corrupt, writes == 0U);
    mbr[4U] = 0xEEU;
    Put(mbr + 8U, 1U, 4U);
    Put(mbr + 12U, 63U, 4U);
    auto* gpt = disk[1U];
    constexpr UInt8 magic[]{'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};
    for (Foundation::Size n{0U}; n < 8U; ++n)
    {
        gpt[n] = magic[n];
    }
    Put(gpt + 8U, 0x10000U, 4U);
    Put(gpt + 12U, 92U, 4U);
    Put(gpt + 24U, 1U, 8U);
    Put(gpt + 32U, 63U, 8U);
    Put(gpt + 40U, 3U, 8U);
    Put(gpt + 48U, 60U, 8U);
    Put(gpt + 72U, 2U, 8U);
    Put(gpt + 80U, 4U, 4U);
    Put(gpt + 84U, 128U, 4U);
    disk[2U][0U] = 1U;
    Put(disk[2U] + 32U, 8U, 8U);
    Put(disk[2U] + 40U, 39U, 8U);
    Put(gpt + 88U, Crc(disk[2U], 512U), 4U);
    Put(gpt + 16U, Crc(gpt, 92U), 4U);
    ASTERIA_EXPECT(t, "protective MBR enumerates GPT rather than reporting Corrupt",
                   table.Read(device) == User::Filesystem::FatStatus::Done, table.Get(0U).First == 8U,
                   table.Get(0U).Sectors == 32U, writes == 0U);
    Put(mbr + 12U, 62U, 4U);
    ASTERIA_EXPECT(t, "ProtectiveMbrExtent: a GPT cannot legitimize a malformed protective record",
                   table.Read(device) == User::Filesystem::FatStatus::Corrupt, writes == 0U);
    Put(mbr + 12U, 63U, 4U);
    disk[2U][10U] ^= 1U;
    ASTERIA_EXPECT(t, "GPT entry array CRC and header CRC are checked",
                   table.Read(device) == User::Filesystem::FatStatus::Corrupt);
    disk[2U][10U] ^= 1U;
    gpt[16U] ^= 1U;
    ASTERIA_EXPECT(t, "a corrupted GPT header is refused without writing",
                   table.Read(device) == User::Filesystem::FatStatus::Corrupt, writes == 0U);
}
} // namespace
int main()
{
    Asteria::TestSupport::TestContext t{"PartitionsTests"};
    Test(t);
    return t.Finish();
}
