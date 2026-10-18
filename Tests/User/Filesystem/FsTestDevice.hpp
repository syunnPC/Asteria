// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_FILESYSTEM_FS_TEST_DEVICE_HPP_INCLUDED
#define ASTERIA_TESTS_USER_FILESYSTEM_FS_TEST_DEVICE_HPP_INCLUDED

#include "User/Filesystem/FsVolume.hpp"

#include <cstring>

namespace Asteria::Test::Filesystem
{
namespace Fs = User::Filesystem;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;

/// Normal interface seams supply fault injection, without test-only production
/// paths. Limit is the number of writes allowed to land; subsequent writes
/// fail. LandedFailure returns false on the last allowed write after copying;
/// TornFailure copies half that write before returning false.
class RamDevice final
{
public:
    UInt8 Image[Fs::FsTotalSectors * Fs::FsSectorBytes]{};
    UInt32 Writes{0U};
    UInt32 Reads{0U};
    UInt32 Limit{~UInt32{0U}};
    UInt32 ReadLimit{~UInt32{0U}};
    bool LandedFailure{false};
    bool TornFailure{false};
    bool ProtectedWrite{false};
    UInt64 WriteSectors[32U]{};
    UInt32 WriteCounts[32U]{};

    void Reset() noexcept
    {
        std::memset(Image, 0, sizeof(Image));
        std::memset(Image + Fs::FsBootTokenSector * Fs::FsSectorBytes, 0xA5, Fs::FsSectorBytes);
        std::memset(Image + Fs::FsScratchSector * Fs::FsSectorBytes, 0x5A, Fs::FsSectorBytes);
        Arm();
    }

    void Arm(const UInt32 limit = ~UInt32{0U}, const bool landed = false, const bool torn = false) noexcept
    {
        Writes = 0U;
        Reads = 0U;
        Limit = limit;
        ReadLimit = ~UInt32{0U};
        LandedFailure = landed;
        TornFailure = torn;
        ProtectedWrite = false;
        std::memset(WriteSectors, 0, sizeof(WriteSectors));
        std::memset(WriteCounts, 0, sizeof(WriteCounts));
    }

    [[nodiscard]] Fs::FsBlockDevice Device() noexcept
    {
        return Fs::FsBlockDevice{.Context = this, .ReadSectors = Read, .WriteSectors = Write};
    }

    [[nodiscard]] bool SentinelsIntact() const noexcept
    {
        for (Size index{0U}; index < Fs::FsSectorBytes; ++index)
        {
            if ((Image[Fs::FsBootTokenSector * Fs::FsSectorBytes + index] != 0xA5U) ||
                (Image[Fs::FsScratchSector * Fs::FsSectorBytes + index] != 0x5AU))
            {
                return false;
            }
        }
        return !ProtectedWrite;
    }

private:
    [[nodiscard]] static bool InRange(const UInt64 sector, const UInt32 count) noexcept
    {
        return (count != 0U) && (sector < Fs::FsTotalSectors) && (count <= (Fs::FsTotalSectors - sector));
    }

    [[nodiscard]] static bool Read(void* context, const UInt64 sector, const UInt32 count, UInt8* buffer) noexcept
    {
        RamDevice& disk = *static_cast<RamDevice*>(context);
        if ((buffer == nullptr) || !InRange(sector, count) || (disk.Reads >= disk.ReadLimit))
        {
            return false;
        }
        ++disk.Reads;
        std::memcpy(buffer, disk.Image + sector * Fs::FsSectorBytes, count * Fs::FsSectorBytes);
        return true;
    }

    [[nodiscard]] static bool Write(void* context, const UInt64 sector, const UInt32 count,
                                    const UInt8* buffer) noexcept
    {
        RamDevice& disk = *static_cast<RamDevice*>(context);
        if ((buffer == nullptr) || !InRange(sector, count))
        {
            return false;
        }
        if ((sector < Fs::FsFirstCowBlock * Fs::FsSectorsPerBlock) &&
            !(((sector == Fs::FsSuperblockA) || (sector == Fs::FsSuperblockB)) && (count == 1U)))
        {
            disk.ProtectedWrite = true;
            return false;
        }
        if (disk.Writes >= disk.Limit)
        {
            return false;
        }
        if (disk.Writes < 32U)
        {
            disk.WriteSectors[disk.Writes] = sector;
            disk.WriteCounts[disk.Writes] = count;
        }
        ++disk.Writes;
        const bool last = disk.Writes == disk.Limit;
        const Size bytes = count * Fs::FsSectorBytes;
        std::memcpy(disk.Image + sector * Fs::FsSectorBytes, buffer, (last && disk.TornFailure) ? (bytes / 2U) : bytes);
        return !(last && (disk.LandedFailure || disk.TornFailure));
    }
};
} // namespace Asteria::Test::Filesystem

#endif // ASTERIA_TESTS_USER_FILESYSTEM_FS_TEST_DEVICE_HPP_INCLUDED
