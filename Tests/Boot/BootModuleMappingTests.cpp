// SPDX-License-Identifier: Apache-2.0
#include "Boot/BootModuleMapping.hpp"

#include "Support/TestContext.hpp"

namespace
{
constinit Asteria::Boot::BootInfo g_info{};

void Reset() noexcept
{
    g_info.Files = {};
    g_info.MemoryRegionCount = 1U;
    g_info.MemoryRegions[0] = {0x100000U, 0U, 32U, 8U, 2U, Asteria::Boot::PhysicalMemoryKind::Loader};
    auto& region = g_info.Files;
    region.PhysicalStart = 0x100000U;
    region.PageCount = 32U;
    region.Files[0].Offset = 4096U;
    region.Files[0].SizeBytes = 4096U;
    region.Files[1].Offset = 8192U;
    region.Files[1].SizeBytes = 4096U;
    region.ModuleOffset = 12288U;
    region.ModuleBytes = 8192U;
    region.ModuleCount = 2U;
    region.Modules[0].File.Offset = 12288U + 80U;
    region.Modules[0].File.SizeBytes = 4096U;
    region.Modules[1].File.Offset = 12288U + 80U + 4096U;
    region.Modules[1].File.SizeBytes = 64U;
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"BootModuleMappingTests"};
    Reset();
    ASTERIA_EXPECT(context, "page-isolated LoaderData module slice", Asteria::Boot::ValidateBootModules(g_info));
    for (Asteria::Foundation::Size mutation{0U}; mutation < 12U; ++mutation)
    {
        Reset();
        auto& region = g_info.Files;
        switch (mutation)
        {
        case 0U:
            ++region.PhysicalStart;
            break;
        case 1U:
            ++region.ModuleOffset;
            break;
        case 2U:
            ++region.ModuleBytes;
            break;
        case 3U:
            region.ModuleCount = 9U;
            break;
        case 4U:
            region.PhysicalStart = ~Asteria::Foundation::UInt64{4095U};
            break;
        case 5U:
            region.Files[0].SizeBytes = region.ModuleOffset;
            break;
        case 6U:
            region.Modules[1].File.Offset = region.Modules[0].File.Offset;
            break;
        case 7U:
            region.ModuleBytes = Asteria::Abi::PagerModuleMaximumBytes + 4096U;
            break;
        case 8U:
            g_info.MemoryRegions[0].FirmwareType = 7U;
            break;
        case 9U:
            g_info.MemoryRegions[0].Attributes = 0x8000'0000'0000'0008ULL;
            break;
        case 10U:
            region.Modules[0].File.Offset = region.ModuleOffset;
            break;
        case 11U:
            region.Modules[1].File.SizeBytes = region.ModuleBytes;
            break;
        default:
            break;
        }
        ASTERIA_EXPECT(context, "unaligned, wrapping, overlapping or unowned slice refused",
                       !Asteria::Boot::ValidateBootModules(g_info));
    }
    return context.Finish();
}
