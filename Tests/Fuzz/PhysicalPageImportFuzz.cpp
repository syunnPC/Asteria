// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "Kernel/Memory/PhysicalPageImportPolicy.hpp"
#include "Kernel/Memory/PhysicalPageImporter.hpp"

extern "C" void AsteriaFuzzSeeds(void (*emit)(const std::uint8_t*, std::size_t))
{
    namespace Boot = Asteria::Boot;
    namespace Foundation = Asteria::Foundation;
    namespace Kernel = Asteria::Kernel;
    using Kernel::Memory::PhysicalPageImportExclusion;
    // Complete records make the hosted smoke campaign exercise admission,
    // rather than mutate inputs that remain shorter than the wire format.
    for (const Foundation::UInt32 type : {3U, 4U, 7U})
    {
        Boot::PhysicalMemoryRegion region{.PhysicalStart = 0x1'0000'0000ULL,
                                          .VirtualStart = 0U,
                                          .PageCount = 512U,
                                          .Attributes = 8U,
                                          .FirmwareType = type,
                                          .Kind = Boot::PhysicalMemoryKind::Conventional};
        PhysicalPageImportExclusion exclusions[4U]{
            {0x1000U, 4096U}, {0x2000U, 4096U}, {0x3000U, 4096U}, {0x4000U, 4096U}};
        Foundation::UInt64 requested{1U};
        std::uint8_t bytes[sizeof(region) + sizeof(exclusions) + sizeof(requested)]{};
        for (const Foundation::UInt64 attributes : {8ULL, 0ULL, 0x8000'0000'0000'0008ULL, 0x40008ULL})
        {
            region.Attributes = attributes;
            std::memcpy(bytes, &region, sizeof(region));
            std::memcpy(bytes + sizeof(region), exclusions, sizeof(exclusions));
            std::memcpy(bytes + sizeof(region) + sizeof(exclusions), &requested, sizeof(requested));
            emit(bytes, sizeof(bytes));
        }
    }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    namespace Boot = Asteria::Boot;
    namespace Foundation = Asteria::Foundation;
    namespace Kernel = Asteria::Kernel;
    Boot::PhysicalMemoryRegion region{};
    Kernel::Memory::PhysicalPageImportExclusion exclusions[4U]{};
    if (size < sizeof(region) + sizeof(exclusions) + 8U)
    {
        return 0;
    }
    std::memcpy(&region, data, sizeof(region));
    std::memcpy(exclusions, data + sizeof(region), sizeof(exclusions));
    Foundation::UInt64 requested{0U};
    std::memcpy(&requested, data + sizeof(region) + sizeof(exclusions), sizeof(requested));
    // Synthetic maps include well-formed and malformed type/attribute pairs,
    // page zero, holes, unaligned and overflowing exclusions, and high RAM.
    region.PhysicalStart &= ~Foundation::UInt64{4095U};
    region.PageCount %= 1025U;
    region.FirmwareType %= 16U;
    region.Kind = region.FirmwareType == 7U                                  ? Boot::PhysicalMemoryKind::Conventional
                  : (region.FirmwareType == 3U || region.FirmwareType == 4U) ? Boot::PhysicalMemoryKind::BootServices
                                                                             : Boot::PhysicalMemoryKind::Reserved;
    if ((requested & 2U) != 0U)
    {
        region.Kind = Boot::PhysicalMemoryKind::Reserved;
    }
    const Foundation::UInt64 page = (requested & 1U) != 0U
                                        ? region.PhysicalStart + ((requested >> 12U) % (region.PageCount + 1U)) * 4096U
                                        : requested & ~Foundation::UInt64{4095U};
    for (const bool services : {false, true})
    {
        const bool actual = Kernel::Memory::PhysicalPageImportPolicy::Allows(
            region, Kernel::Memory::PhysicalAddress{page}, services, exclusions, 4U);
        bool expected = ((region.FirmwareType == 7U && region.Kind == Boot::PhysicalMemoryKind::Conventional) ||
                         (services && (region.FirmwareType == 3U || region.FirmwareType == 4U) &&
                          region.Kind == Boot::PhysicalMemoryKind::BootServices)) &&
                        ((region.Attributes & 8U) != 0U) && ((region.Attributes & 0x8000'0000'0004'0000ULL) == 0U) &&
                        page != 0U && region.PageCount != 0U;
        const auto end =
            static_cast<__uint128_t>(region.PhysicalStart) + static_cast<__uint128_t>(region.PageCount) * 4096U;
        expected = expected && end <= ~Foundation::UInt64{0U} && page >= region.PhysicalStart &&
                   static_cast<__uint128_t>(page) + 4096U <= end;
        for (const auto excluded : exclusions)
        {
            const auto excludedEnd = static_cast<__uint128_t>(excluded.Start) + excluded.Bytes;
            expected = expected && excluded.Bytes != 0U && excludedEnd <= ~Foundation::UInt64{0U};
            if (static_cast<__uint128_t>(page) < excludedEnd && static_cast<__uint128_t>(page) + 4096U > excluded.Start)
            {
                expected = false;
            }
        }
        if (actual != expected)
        {
            std::abort();
        }
        bool valid{true};
        for (const auto excluded : exclusions)
        {
            valid = valid && excluded.Bytes != 0U && excluded.Bytes <= ~Foundation::UInt64{0U} - excluded.Start;
        }
        if (!valid)
        {
            continue;
        }
        // Exercise the actual interval importer and externally budgeted
        // allocator, not just the admission predicate. The hosted fuzz
        // adapter's storage is deliberately unrelated to synthetic RAM.
        Boot::BootInfo info{};
        info.MemoryRegionCount = 1U;
        info.MemoryRegions[0U] = region;
        const Foundation::UInt64 receiptOffset{((requested >> 32U) % (region.PageCount + 1U)) * 4096U};
        if (region.PhysicalStart > ~Foundation::UInt64{0U} - receiptOffset ||
            region.PhysicalStart + receiptOffset > ~Foundation::UInt64{0U} - 4096U)
        {
            continue;
        }
        info.Files.PhysicalStart = region.PhysicalStart + receiptOffset;
        info.Files.PageCount = 1U;
        const auto receiptEnd = static_cast<__uint128_t>(info.Files.PhysicalStart) + 4096U;
        const auto infoStart = reinterpret_cast<Foundation::UIntPointer>(&info);
        const auto infoEnd = static_cast<__uint128_t>(infoStart) + sizeof(info);
        const bool owns = actual &&
                          !(page<receiptEnd&& static_cast<__uint128_t>(page) + 4096U> info.Files.PhysicalStart) &&
                          !(page<infoEnd&& static_cast<__uint128_t>(page) + 4096U> infoStart);
        Kernel::Memory::PhysicalPageImporter importer;
        if (!importer.ExcludeBootInfo(info))
        {
            std::abort();
        }
        for (const auto excluded : exclusions)
        {
            if (!importer.Exclude(excluded.Start, excluded.Bytes))
            {
                std::abort();
            }
        }
        if (!importer.Build(info, services))
        {
            std::abort();
        }
        if (importer.Count() == 0U)
        {
            if (owns)
            {
                std::abort();
            }
            continue;
        }
        Kernel::Memory::PhysicalPageAllocator allocator;
        alignas(8) Foundation::UInt64 metadata[2048U]{};
        const auto result = allocator.Import(importer.Ranges(), importer.Count(), metadata, sizeof(metadata), 0U, 0U);
        if (!result.HasValue() || allocator.Contains(Kernel::Memory::PhysicalAddress{page}, 1U) != owns ||
            allocator.Contains(Kernel::Memory::PhysicalAddress{0U}, 1U) ||
            allocator.Contains(Kernel::Memory::PhysicalAddress{info.Files.PhysicalStart}, 1U))
        {
            std::abort();
        }
    }
    return 0;
}
