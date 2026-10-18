// SPDX-License-Identifier: Apache-2.0
// ReadExfatBootSector on a boot sector built here as mkfs.exfat 1.3.2 lays out 64 MiB with 4 KiB
// clusters, and on one field at a time made wrong: every check the recogniser makes refuses its
// field's bad value and leaves the geometry unchanged. Needs no tool.

#include "User/Filesystem/Exfat/ExfatVolume.hpp"

#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Filesystem/Exfat/ExfatTestSupport.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Exfat::Put32;
using Exfat::Put64;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using User::Filesystem::ExfatGeometry;
using User::Filesystem::ExfatStatus;
using User::Filesystem::ReadExfatBootSector;

constexpr UInt64 DeviceSectors{131072U};

void BuildBootSector(UInt8 (&boot)[512U]) noexcept
{
    std::memset(boot, 0, sizeof(boot));
    boot[0U] = 0xEBU;
    boot[1U] = 0x76U;
    boot[2U] = 0x90U;
    std::memcpy(boot + 3U, "EXFAT   ", 8U);
    Put64(boot + 72U, DeviceSectors);
    Put32(boot + 80U, 2048U);
    Put32(boot + 84U, 126U);
    Put32(boot + 88U, 4096U);
    Put32(boot + 92U, 15872U);
    Put32(boot + 96U, 5U);
    Put32(boot + 100U, 0x1234'5678U);
    boot[104U] = 0x00U;
    boot[105U] = 0x01U;
    boot[108U] = 9U;
    boot[109U] = 3U;
    boot[110U] = 1U;
    boot[111U] = 0x80U;
    boot[510U] = 0x55U;
    boot[511U] = 0xAAU;
}

/// A field (offset, width in bytes, value) and the answer it must give.
struct Case
{
    const char* Description;
    Size Offset;
    Size Width;
    UInt64 Value;
    ExfatStatus Expected;
};

void TestRecognition(TestContext& context) noexcept
{
    UInt8 boot[512U]{};
    BuildBootSector(boot);
    ExfatGeometry geometry{};
    ASTERIA_EXPECT(context, "a boot sector as mkfs.exfat writes it decodes",
                   ReadExfatBootSector(boot, DeviceSectors, geometry) == ExfatStatus::Done,
                   geometry.VolumeSectors == DeviceSectors, geometry.FatStart == 2048U, geometry.FatSectors == 126U,
                   geometry.HeapStart == 4096U, geometry.Clusters == 15872U, geometry.RootCluster == 5U,
                   geometry.ClusterShift == 3U);

    static constexpr Case Cases[]{
        {"a FAT file system name", 3U, 1U, 'F', ExfatStatus::NotExfat},
        {"a lowercase name", 7U, 1U, 't', ExfatStatus::NotExfat},
        {"a nonzero byte 11 (a FAT parameter block)", 11U, 1U, 2U, ExfatStatus::Corrupt},
        {"a nonzero byte 63", 63U, 1U, 1U, ExfatStatus::Corrupt},
        {"no boot signature", 510U, 1U, 0U, ExfatStatus::Corrupt},
        {"no second boot signature byte", 511U, 1U, 0x55U, ExfatStatus::Corrupt},
        {"4096-byte sectors", 108U, 1U, 12U, ExfatStatus::Corrupt},
        {"clusters over 32 MiB", 109U, 1U, 17U, ExfatStatus::Corrupt},
        {"TexFAT's two FATs", 110U, 1U, 2U, ExfatStatus::Corrupt},
        {"no FAT", 110U, 1U, 0U, ExfatStatus::Corrupt},
        {"revision 2.0", 105U, 1U, 2U, ExfatStatus::Corrupt},
        {"a volume larger than the device", 72U, 8U, DeviceSectors + 1U, ExfatStatus::Corrupt},
        {"a FAT inside the boot regions", 80U, 4U, 23U, ExfatStatus::Corrupt},
        {"a FAT too short for its clusters", 84U, 4U, 124U, ExfatStatus::Corrupt},
        {"a heap overlapping the FAT", 88U, 4U, 2173U, ExfatStatus::Corrupt},
        {"no cluster", 92U, 4U, 0U, ExfatStatus::Corrupt},
        {"a heap past the volume", 92U, 4U, 15873U, ExfatStatus::Corrupt},
        {"a root before the heap", 96U, 4U, 1U, ExfatStatus::Corrupt},
        {"a root past the heap", 96U, 4U, 15874U, ExfatStatus::Corrupt},
    };
    for (const Case& test : Cases)
    {
        BuildBootSector(boot);
        for (Size at{0U}; at < test.Width; ++at)
        {
            boot[test.Offset + at] = static_cast<UInt8>(test.Value >> (8U * at));
        }
        geometry.Clusters = 0xC0FFEEU;
        ASTERIA_EXPECT(context, test.Description, ReadExfatBootSector(boot, DeviceSectors, geometry) == test.Expected,
                       geometry.Clusters == 0xC0FFEEU);
    }

    // The boundaries the checks allow.
    static constexpr Case Accepted[]{
        {"the FAT at sector 24", 80U, 4U, 24U, ExfatStatus::Done},
        {"a heap right after the FAT", 88U, 4U, 2174U, ExfatStatus::Done},
        {"the last cluster as the root", 96U, 4U, 15873U, ExfatStatus::Done},
        {"a jump instruction of any kind", 0U, 1U, 0U, ExfatStatus::Done},
        {"a minor revision", 104U, 1U, 7U, ExfatStatus::Done},
        {"VolumeFlags of any value", 106U, 1U, 0x07U, ExfatStatus::Done},
        {"a partition offset (bytes 64 to 71, after the zeros)", 64U, 8U, 0x0801U, ExfatStatus::Done},
    };
    for (const Case& test : Accepted)
    {
        BuildBootSector(boot);
        for (Size at{0U}; at < test.Width; ++at)
        {
            boot[test.Offset + at] = static_cast<UInt8>(test.Value >> (8U * at));
        }
        ASTERIA_EXPECT(context, test.Description, ReadExfatBootSector(boot, DeviceSectors, geometry) == test.Expected);
    }
    BuildBootSector(boot);
    boot[109U] = 16U;
    Put32(boot + 92U, 1U);
    Put32(boot + 96U, 2U);
    Put64(boot + 72U, 4096U + 65536U);
    ASTERIA_EXPECT(context, "a 32 MiB cluster that ends the volume decodes",
                   ReadExfatBootSector(boot, DeviceSectors, geometry) == ExfatStatus::Done,
                   geometry.ClusterShift == 16U);
    Put64(boot + 72U, 4096U + 65535U);
    ASTERIA_EXPECT(context, "a 32 MiB cluster one sector past the volume does not",
                   ReadExfatBootSector(boot, DeviceSectors, geometry) == ExfatStatus::Corrupt);

    // A 64 MiB cluster on a device of unknown size: refused by its shift
    // alone.
    Put64(boot + 72U, 4096U + 131072U);
    boot[109U] = 17U;
    ASTERIA_EXPECT(context, "clusters of 64 MiB do not decode",
                   ReadExfatBootSector(boot, ~UInt64{0U}, geometry) == ExfatStatus::Corrupt);

    // A FAT of 126 sectors numbers 16128 entries: clusters 0 to 16127.
    BuildBootSector(boot);
    boot[109U] = 0U;
    Put32(boot + 92U, 16126U);
    ASTERIA_EXPECT(context, "a FAT that holds exactly its clusters' entries decodes",
                   ReadExfatBootSector(boot, DeviceSectors, geometry) == ExfatStatus::Done);
    Put32(boot + 92U, 16127U);
    ASTERIA_EXPECT(context, "a FAT one entry short does not",
                   ReadExfatBootSector(boot, DeviceSectors, geometry) == ExfatStatus::Corrupt);

    // ClusterCount's bound, 0xFFFFFFF5, on a device of unknown size with a
    // FAT and a volume large enough for more.
    BuildBootSector(boot);
    boot[109U] = 0U;
    Put32(boot + 80U, 24U);
    Put32(boot + 84U, 0x0200'0000U);
    Put32(boot + 88U, 24U + 0x0200'0000U);
    Put64(boot + 72U, 24ULL + 0x0200'0000ULL + 0xFFFF'FFF6ULL);
    Put32(boot + 92U, 0xFFFF'FFF5U);
    ASTERIA_EXPECT(context, "the most clusters exFAT numbers decode",
                   ReadExfatBootSector(boot, ~UInt64{0U}, geometry) == ExfatStatus::Done,
                   geometry.Clusters == 0xFFFF'FFF5U);
    Put32(boot + 92U, 0xFFFF'FFF6U);
    ASTERIA_EXPECT(context, "one cluster more does not",
                   ReadExfatBootSector(boot, ~UInt64{0U}, geometry) == ExfatStatus::Corrupt);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext context{"ExfatBootSectorTests"};
    Asteria::Test::TestRecognition(context);
    return context.Finish();
}
