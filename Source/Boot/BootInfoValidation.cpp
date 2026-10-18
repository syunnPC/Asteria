// SPDX-License-Identifier: Apache-2.0

#include "Boot/BootInfoValidation.hpp"

#include "Boot/BootModuleMapping.hpp"
#include "Foundation/Crc32c.hpp"

namespace Asteria::Boot
{
Foundation::UInt32 ComputeBootInfoCrc32c(const BootInfo& info) noexcept
{
    // Both target profiles specify little endian, eight-bit bytes and the
    // asserted BootInfo layout. Reading object representations is permitted.
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&info);
    constexpr Foundation::Size CrcOffset{__builtin_offsetof(BootInfo, Crc32c)};
    Foundation::UInt32 state = Foundation::Crc32c::Start();
    for (Foundation::Size index{0U}; index < sizeof(BootInfo); ++index)
    {
        const Foundation::UInt8 byte = (index >= CrcOffset && index < CrcOffset + 4U) ? 0U : bytes[index];
        state = Foundation::Crc32c::Update(state, byte);
    }
    return Foundation::Crc32c::Finish(state);
}

bool ValidateBootInfo(const BootInfo& info) noexcept
{
    return info.Signature == BootInfoSignature && info.Version == BootInfoVersion &&
           info.SizeBytes == sizeof(BootInfo) && info.Reserved == 0U && info.ReservedAbi == 0U &&
           info.MemoryRegionCount != 0U && info.MemoryRegionCount <= MaximumMemoryRegions &&
           info.Kernel.Reserved == 0U && info.Kernel.MetadataArena.Reserved == 0U &&
           IsValidBootDmaExclusions(info.DmaExclusions) && info.LowMemoryBlocks <= BootLowMemoryMaximumBlocks &&
           info.WatchdogSeconds <= Abi::WatchdogMaximumSeconds && info.SessionMode <= 1U && info.LoginEnabled <= 1U &&
           info.Crc32c == ComputeBootInfoCrc32c(info);
}

bool ValidateBootFileRegion(const BootInfo& info) noexcept
{
    constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};
    constexpr Foundation::UInt64 PageBytes{4096U};
    const BootFileRegion& region = info.Files;
    if (info.MemoryRegionCount == 0U || info.MemoryRegionCount > MaximumMemoryRegions || region.PhysicalStart == 0U ||
        (region.PhysicalStart & (PageBytes - 1U)) != 0U || region.PageCount == 0U ||
        region.PageCount > Maximum / PageBytes || region.PhysicalStart > Maximum - region.PageCount * PageBytes ||
        region.ManifestBytes == 0U || region.ManifestBytes > BootManifestMaximumBytes)
    {
        return false;
    }
    const Foundation::UInt64 bytes = region.PageCount * PageBytes;
    Foundation::UInt64 end{BootManifestMaximumBytes};
    for (Foundation::Size index{0U}; index < BootFileCount; ++index)
    {
        const BootFileRecord& file = region.Files[index];
        if (file.SizeBytes == 0U)
        {
            if (index != BootSpinnerFile)
            {
                return false;
            }
            continue;
        }
        if (file.SizeBytes > BootFileMaximumBytes || file.Offset < end || file.Offset > bytes ||
            file.SizeBytes > bytes - file.Offset)
        {
            return false;
        }
        end = file.Offset + file.SizeBytes;
    }
    if (region.ModuleCount > BootModuleMaximumCount || (region.ModuleCount != 0U && !ValidateBootModules(info)))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < info.MemoryRegionCount; ++index)
    {
        const PhysicalMemoryRegion& memory = info.MemoryRegions[index];
        if (memory.PhysicalStart <= region.PhysicalStart && memory.PageCount <= Maximum / PageBytes &&
            region.PhysicalStart - memory.PhysicalStart <= memory.PageCount * PageBytes &&
            bytes <= memory.PageCount * PageBytes - (region.PhysicalStart - memory.PhysicalStart))
        {
            return memory.Kind == PhysicalMemoryKind::Loader && memory.FirmwareType == 2U &&
                   (memory.Attributes & 8U) != 0U &&
                   (memory.Attributes & (0x8000'0000'0000'0000ULL | 0x1000U | 0x2000U)) == 0U;
        }
    }
    return false;
}

void SealBootInfo(BootInfo& info) noexcept
{
    info.Crc32c = ComputeBootInfoCrc32c(info);
}
} // namespace Asteria::Boot
