// SPDX-License-Identifier: Apache-2.0
#include "Boot/BootInfoValidation.hpp"

#include "Support/TestContext.hpp"

namespace
{
constinit Asteria::Boot::BootInfo g_info{};

void Reset() noexcept
{
    g_info = {};
    g_info.Signature = Asteria::Boot::BootInfoSignature;
    g_info.Version = Asteria::Boot::BootInfoVersion;
    g_info.SizeBytes = sizeof(g_info);
    g_info.MemoryRegionCount = 1U;
    g_info.MemoryRegions[0U] = {0x10'0000U, 0U, 4U, 8U, 2U, Asteria::Boot::PhysicalMemoryKind::Loader};
    g_info.Files.PhysicalStart = 0x10'0000U;
    g_info.Files.PageCount = 4U;
    g_info.Files.ManifestBytes = 32U;
    g_info.Files.Files[Asteria::Boot::BootInitFile] = {4096U, 16U, 0U, {}, {}};
    g_info.Files.Files[Asteria::Boot::BootUserFile] = {4112U, 16U, 0U, {}, {}};
    Asteria::Boot::SealBootInfo(g_info);
}

// Every one of the 64 exclusions in use.
void FillExclusions() noexcept
{
    g_info.DmaExclusions.Count = Asteria::Boot::BootDmaExclusionMaximumCount;
    g_info.DmaExclusions.DeclaredCount = Asteria::Boot::BootDmaExclusionMaximumCount;
    for (auto& range : g_info.DmaExclusions.Ranges)
    {
        range = Asteria::Boot::BootDmaExclusion{
            0x7F00'0000U, 0x7F00'FFFFU, 0U, Asteria::Boot::BootDmaExclusionRmrr, 0U, 0U};
    }
}
} // Namespace

int main()
{
    using Asteria::Boot::SealBootInfo;
    using Asteria::Boot::ValidateBootInfo;
    using Asteria::Boot::ValidateBootFileRegion;
    Asteria::TestSupport::TestContext context{"BootInfoValidationTests"};
    Reset();
    ASTERIA_EXPECT(context, "sealed envelope is accepted", ValidateBootInfo(g_info));
    constexpr Asteria::Foundation::Size offsets[]{
        0U,
        8U,
        12U,
        16U,
        40U,
        81'960U,
        87'976U,
        88'016U,
        88'048U,
        88'064U,
        __builtin_offsetof(Asteria::Boot::BootInfo, Crc32c),
        __builtin_offsetof(Asteria::Boot::BootInfo, Kernel),
        __builtin_offsetof(Asteria::Boot::BootInfo, InventoryRecord),
        __builtin_offsetof(Asteria::Boot::BootInfo, Power),
        sizeof(g_info) - 1U,
    };
    auto* const bytes = reinterpret_cast<Asteria::Foundation::UInt8*>(&g_info);
    for (const auto offset : offsets)
    {
        bytes[offset] ^= 1U;
        ASTERIA_EXPECT(context, "one-bit corruption in every handoff group is rejected", !ValidateBootInfo(g_info));
        bytes[offset] ^= 1U;
        ASTERIA_EXPECT(context, "validation changes no bytes", ValidateBootInfo(g_info));
    }
    g_info.Version += 1U;
    SealBootInfo(g_info);
    ASTERIA_EXPECT(context, "unknown version rejects even with a correct CRC", !ValidateBootInfo(g_info));
    Reset();
    g_info.SizeBytes -= 8U;
    SealBootInfo(g_info);
    ASTERIA_EXPECT(context, "short prefix rejects even with a correct CRC", !ValidateBootInfo(g_info));
    Reset();
    g_info.MemoryRegionCount = Asteria::Boot::MaximumMemoryRegions + 1U;
    SealBootInfo(g_info);
    ASTERIA_EXPECT(context, "unbounded region count rejects", !ValidateBootInfo(g_info));
    Reset();
    g_info.ReservedAbi = 1U;
    SealBootInfo(g_info);
    ASTERIA_EXPECT(context, "ABI reserved bits reject", !ValidateBootInfo(g_info));
    Reset();
    ASTERIA_EXPECT(context, "whole-file receipt is valid without following its addresses",
                   ValidateBootFileRegion(g_info));
    g_info.Files.Files[Asteria::Boot::BootUserFile].Offset = ~Asteria::Foundation::UInt64{0U};
    SealBootInfo(g_info);
    ASTERIA_EXPECT(context, "valid checksum does not authorize an out-of-region view", ValidateBootInfo(g_info),
                   !ValidateBootFileRegion(g_info));
    Reset();
    g_info.Files.PhysicalStart = ~Asteria::Foundation::UInt64{4095U};
    ASTERIA_EXPECT(context, "wrapping physical receipt is rejected", !ValidateBootFileRegion(g_info));
    Reset();
    g_info.Files.PageCount = ~Asteria::Foundation::UInt64{0U};
    ASTERIA_EXPECT(context, "wrapping page count is rejected", !ValidateBootFileRegion(g_info));
    Reset();
    g_info.Files.Files[Asteria::Boot::BootUserFile].Offset = 4096U;
    ASTERIA_EXPECT(context, "overlapping file receipts are rejected", !ValidateBootFileRegion(g_info));
    Reset();
    g_info.Files.Files[Asteria::Boot::BootInitFile].SizeBytes = 0U;
    ASTERIA_EXPECT(context, "required image cannot be absent", !ValidateBootFileRegion(g_info));
    Reset();
    g_info.MemoryRegions[0U].PageCount = 3U;
    ASTERIA_EXPECT(context, "entire allocation must lie in the final map", !ValidateBootFileRegion(g_info));
    Reset();
    g_info.MemoryRegions[0U].FirmwareType = 7U;
    ASTERIA_EXPECT(context, "conventional memory is not a file receipt", !ValidateBootFileRegion(g_info));
    Reset();
    g_info.MemoryRegions[0U].Attributes |= 0x8000'0000'0000'0000ULL;
    ASTERIA_EXPECT(context, "runtime memory is not a file receipt", !ValidateBootFileRegion(g_info));

    // BootInfo v18: the firmware DMA exclusion list, the lowmemory value and the arena receipt's
    // reserved word.
    using Asteria::Boot::BootDmaExclusion;
    using Asteria::Boot::BootDmaExclusionMaximumCount;
    using Asteria::Boot::IsCompleteBootDmaExclusions;
    const auto sealed = [](const auto& change) noexcept
    {
        Reset();
        change();
        SealBootInfo(g_info);
        return ValidateBootInfo(g_info);
    };
    ASTERIA_EXPECT(context, "a v22 record names the boot info version 22 at 106,568 bytes",
                   Asteria::Boot::BootInfoVersion == 22U, sizeof(g_info) == 106'568U);
    ASTERIA_EXPECT(context, "well-formed exclusion lists are accepted",
                   sealed(
                       []
                       {
                           g_info.DmaExclusions.Count = 2U;
                           g_info.DmaExclusions.DeclaredCount = 3U;
                           g_info.DmaExclusions.Ranges[0U] = BootDmaExclusion{
                               0x7F00'0000U, 0x7F00'FFFFU, 0U, Asteria::Boot::BootDmaExclusionRmrr, 0U, 0U};
                           g_info.DmaExclusions.Ranges[1U] = BootDmaExclusion{
                               0x7E00'0000U, 0x7E00'0FFFU, 1U, Asteria::Boot::BootDmaExclusionIvmd, 1U, 0U};
                       }),
                   sealed(FillExclusions),
                   sealed(
                       []
                       {
                           g_info.DmaExclusions.Ranges[0U].Base = 0U;
                           g_info.DmaExclusions.Count = 0U;
                       }));
    ASTERIA_EXPECT(context, "malformed exclusion lists are rejected even with a correct CRC",
                   !sealed(
                       []
                       {
                           g_info.DmaExclusions.Count = 1U;
                       }),
                   !sealed(
                       []
                       {
                           FillExclusions();
                           g_info.DmaExclusions.Count = BootDmaExclusionMaximumCount + 1U;
                           g_info.DmaExclusions.DeclaredCount = BootDmaExclusionMaximumCount + 1U;
                       }),
                   !sealed(
                       []
                       {
                           FillExclusions();
                           g_info.DmaExclusions.Ranges[5U].Base = g_info.DmaExclusions.Ranges[5U].Limit + 1U;
                       }),
                   !sealed(
                       []
                       {
                           FillExclusions();
                           g_info.DmaExclusions.Ranges[6U].Kind = 3U;
                       }),
                   !sealed(
                       []
                       {
                           FillExclusions();
                           g_info.DmaExclusions.Ranges[7U].Malformed = 2U;
                       }),
                   !sealed(
                       []
                       {
                           FillExclusions();
                           g_info.DmaExclusions.Ranges[8U].Reserved = 1U;
                       }),
                   !sealed(
                       []
                       {
                           g_info.DmaExclusions.Count = 1U;
                           g_info.DmaExclusions.DeclaredCount = 1U;
                           g_info.DmaExclusions.Ranges[0U] = BootDmaExclusion{
                               0x7F00'0000U, 0x7F00'FFFFU, 0U, Asteria::Boot::BootDmaExclusionRmrr, 0U, 0U};
                           g_info.DmaExclusions.Ranges[1U].Limit = 1U;
                       }));
    // A declared count above the list's storage is a well-formed record the
    // Kernel refuses: the list would be incomplete, so the boot stops.
    Reset();
    FillExclusions();
    g_info.DmaExclusions.DeclaredCount = BootDmaExclusionMaximumCount + 1U;
    SealBootInfo(g_info);
    const bool overflowValid = ValidateBootInfo(g_info);
    const bool overflowComplete = IsCompleteBootDmaExclusions(g_info.DmaExclusions);
    g_info.DmaExclusions.DeclaredCount = BootDmaExclusionMaximumCount;
    ASTERIA_EXPECT(context, "more than 64 declared exclusions stop the boot; 64 do not", overflowValid,
                   !overflowComplete, IsCompleteBootDmaExclusions(g_info.DmaExclusions));
    ASTERIA_EXPECT(context, "the lowmemory value is at most 2,048 blocks",
                   sealed(
                       []
                       {
                           g_info.LowMemoryBlocks = Asteria::Boot::BootLowMemoryMaximumBlocks;
                       }),
                   !sealed(
                       []
                       {
                           g_info.LowMemoryBlocks = Asteria::Boot::BootLowMemoryMaximumBlocks + 1U;
                       }),
                   Asteria::Boot::BootLowMemoryMaximumBlocks == 2048U);
    ASTERIA_EXPECT(context, "the arena receipt's reserved word is zero",
                   sealed(
                       []
                       {
                           g_info.Kernel.MetadataArena = {0x1C00'0000U, 2048U, 0U, 2U, 0U};
                       }),
                   !sealed(
                       []
                       {
                           g_info.Kernel.MetadataArena.Reserved = 1U;
                       }));
    return context.Finish();
}
