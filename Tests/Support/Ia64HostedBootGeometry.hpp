// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_SUPPORT_IA64_HOSTED_BOOT_GEOMETRY_HPP_INCLUDED
#define ASTERIA_TESTS_SUPPORT_IA64_HOSTED_BOOT_GEOMETRY_HPP_INCLUDED

#include <new>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

namespace Asteria::TestSupport
{
/// Boot geometry for hosted IA-64 fixtures whose translation-register plan does not depend on where
/// the host linker and loader place the fixture's objects. - The loaded image is the hosted runtime
/// IVT as its Code section (the table is aligned to its 32 KiB size), one header page before it,
/// and three one-page read-only and two one-page writable sections after it. The image describes
/// host addresses only; nothing reads its other pages. - The run-time memory reservation is the
/// fixture's arena: a static Ia64HostedRuntimeMemoryArena, which fills one page of
/// Ia64HostedRuntimeMemoryPageBytes of its contents, so one locked DTR maps every owner in it, as
/// one DTR maps the boot's reservation. With the fixture's bootstrap reservation (seven DTRs, two
/// ITRs) the plan is then Ia64HostedDataTranslationRegisters DTRs and
/// Ia64HostedInstructionTranslationRegisters ITRs at every load address: the code takes two 16 KiB
/// pages, the read-only group an 8 KiB and a 4 KiB page, the writable group two 4 KiB pages and the
/// arena one page. Spanning the host bytes between the table and the arena instead, as the fixtures
/// once did, makes the count follow the binary's section layout and the load bias.
constexpr Foundation::UInt64 Ia64HostedPageBytes{4096U};
constexpr Foundation::UInt64 Ia64HostedImageTrailingPages{5U};
constexpr Foundation::UInt16 Ia64HostedDataTranslationRegisters{14U};
constexpr Foundation::UInt16 Ia64HostedInstructionTranslationRegisters{5U};

/// The smallest architected insertable page that holds bytes; zero for no
/// bytes or more than the largest architected page (256 MiB).
[[nodiscard]] constexpr Foundation::UInt64 Ia64HostedRuntimeMemoryPageBytes(const Foundation::UInt64 bytes) noexcept
{
    for (Foundation::UInt8 shift{12U}; shift <= 28U; ++shift)
    {
        const Foundation::UInt64 page = Foundation::UInt64{1U} << shift;
        if ((bytes != 0U) && ((Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask & page) != 0U) &&
            (page >= bytes))
        {
            return page;
        }
    }
    return 0U;
}

static_assert(Ia64HostedRuntimeMemoryPageBytes(0xED000U) == 0x10'0000U);
static_assert(Ia64HostedRuntimeMemoryPageBytes(0x10'0001U) == 0x40'0000U);
static_assert(Ia64HostedRuntimeMemoryPageBytes(0U) == 0U);
static_assert(Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableSizeBytes == 0x8000U);

/// Storage for a fixture's arena (TArena) as the whole run-time memory
/// Reservation: one page of Ia64HostedRuntimeMemoryPageBytes(sizeof(TArena)), aligned to its size.
/// No other host object can then share the page and its locked DTR, as nothing but the run-time
/// memory lies in the boot's reservation; otherwise the linker could place, say, a fixture's
/// user-image backing frames in the nucleus-mapped page. The storage is zero-initialized, so it
/// lies in.bss and costs the test binary no file bytes however large its alignment. Construct
/// builds the arena at the start of the page, as the boot builds Ia64BspRuntimeMemory in its
/// cleared reservation; fixtures bind their arena reference to it once, at namespace scope, before
/// any use.
template <typename TArena> class Ia64HostedRuntimeMemoryArena final
{
public:
    static constexpr Foundation::UInt64 PageBytes{Ia64HostedRuntimeMemoryPageBytes(sizeof(TArena))};

    constexpr Ia64HostedRuntimeMemoryArena() noexcept = default;
    Ia64HostedRuntimeMemoryArena(const Ia64HostedRuntimeMemoryArena&) = delete;
    Ia64HostedRuntimeMemoryArena& operator=(const Ia64HostedRuntimeMemoryArena&) = delete;
    Ia64HostedRuntimeMemoryArena(Ia64HostedRuntimeMemoryArena&&) = delete;
    Ia64HostedRuntimeMemoryArena& operator=(Ia64HostedRuntimeMemoryArena&&) = delete;

    [[nodiscard]] TArena& Construct() noexcept
    {
        return *::new (static_cast<void*>(m_storage)) TArena{};
    }

private:
    static_assert(PageBytes != 0U);

    alignas(Ia64HostedRuntimeMemoryPageBytes(sizeof(TArena))) Foundation::UInt8 m_storage[PageBytes]{};
};

struct Ia64HostedBootGeometry
{
    Platform::Ia64::Firmware::Ia64LoadedImageLayout Image;
    Kernel::Memory::PhysicalPageRange RuntimeMemoryPages;
    /// The image's LoaderData and the reservation's Conventional final-map
    /// regions, for the fixture's BootInfo.
    Boot::PhysicalMemoryRegion ImageRegion;
    Boot::PhysicalMemoryRegion RuntimeMemoryRegion;
};

[[nodiscard]] constexpr bool Ia64HostedRangesOverlap(const Foundation::UInt64 firstStart,
                                                     const Foundation::UInt64 firstBytes,
                                                     const Foundation::UInt64 secondStart,
                                                     const Foundation::UInt64 secondBytes) noexcept
{
    return (firstStart < (secondStart + secondBytes)) && (secondStart < (firstStart + firstBytes));
}

/// Whether two page ranges are identical, for fixtures that require their
/// bootstrap allocator to return exactly the arena's reservation, as the boot
/// allocates its reservation right after the bootstrap one.
[[nodiscard]] constexpr bool Ia64HostedPagesEqual(const Kernel::Memory::PhysicalPageRange& left,
                                                  const Kernel::Memory::PhysicalPageRange& right) noexcept
{
    return (left.PhysicalStart == right.PhysicalStart) && (left.PageCount == right.PageCount) &&
           (left.Attributes == right.Attributes) && (left.FirmwareType == right.FirmwareType) &&
           (left.Reserved == right.Reserved);
}

/// Table is the hosted runtime IVT's address; arena and arenaBytes are the arena a fixture built in
/// its Ia64HostedRuntimeMemoryArena (or another object aligned to
/// Ia64HostedRuntimeMemoryPageBytes(arenaBytes)); bootstrap is the fixture's bootstrap reservation
/// and attributes the write-back attribute word of its final map. Fails when the table or the arena
/// breaks its alignment or when the image, the arena's page and the bootstrap reservation overlap.
[[nodiscard]] inline bool MakeIa64HostedBootGeometry(const Foundation::UInt64 table, const Foundation::UInt64 arena,
                                                     const Foundation::UInt64 arenaBytes,
                                                     const Kernel::Memory::PhysicalPageRange& bootstrap,
                                                     Ia64HostedBootGeometry& geometry,
                                                     const Foundation::UInt64 attributes = 0x8U) noexcept
{
    using Platform::Ia64::Firmware::Ia64LoadedImageLayout;
    using Platform::Ia64::Firmware::Ia64LoadedImageSection;
    using Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose;
    constexpr Foundation::UInt64 TableBytes{Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableSizeBytes};
    constexpr Foundation::UInt64 RegionZeroLimit{Foundation::UInt64{1U} << 61U};
    const Foundation::UInt64 page = Ia64HostedRuntimeMemoryPageBytes(arenaBytes);
    const Foundation::UInt64 imageBase = table - Ia64HostedPageBytes;
    const Foundation::UInt64 imageBytes =
        Ia64HostedPageBytes + TableBytes + (Ia64HostedImageTrailingPages * Ia64HostedPageBytes);
    const Foundation::UInt64 bootstrapBytes = bootstrap.PageCount * Ia64HostedPageBytes;
    if ((page == 0U) || (table < Ia64HostedPageBytes) || ((table & (TableBytes - 1U)) != 0U) ||
        ((arena & (page - 1U)) != 0U) ||
        (table > (RegionZeroLimit - TableBytes - (Ia64HostedImageTrailingPages * Ia64HostedPageBytes))) ||
        (arena > (RegionZeroLimit - page)) || Ia64HostedRangesOverlap(imageBase, imageBytes, arena, page) ||
        Ia64HostedRangesOverlap(imageBase, imageBytes, bootstrap.PhysicalStart, bootstrapBytes) ||
        Ia64HostedRangesOverlap(arena, page, bootstrap.PhysicalStart, bootstrapBytes))
    {
        return false;
    }
    const Foundation::UInt64 readOnly = table + TableBytes;
    const Foundation::UInt64 writable = readOnly + (3U * Ia64HostedPageBytes);
    geometry.Image = Ia64LoadedImageLayout{
        .ImageBase = imageBase,
        .ImageSize = imageBytes,
        .Sections =
            {
                Ia64LoadedImageSection{
                    .VirtualStart = table,
                    .PageCount = TableBytes / Ia64HostedPageBytes,
                    .Purpose = Ia64LoadedImageSectionPurpose::Code,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = readOnly,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = readOnly + Ia64HostedPageBytes,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = readOnly + (2U * Ia64HostedPageBytes),
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = writable,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::WritableData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = writable + Ia64HostedPageBytes,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::WritableData,
                    .Reserved = 0U,
                },
            },
    };
    geometry.RuntimeMemoryPages = Kernel::Memory::PhysicalPageRange{
        .PhysicalStart = arena,
        .PageCount = page / Ia64HostedPageBytes,
        .Attributes = attributes,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
    geometry.ImageRegion = Boot::PhysicalMemoryRegion{
        .PhysicalStart = imageBase,
        .VirtualStart = 0U,
        .PageCount = imageBytes / Ia64HostedPageBytes,
        .Attributes = attributes,
        .FirmwareType = 2U,
        .Kind = Boot::PhysicalMemoryKind::Loader,
    };
    geometry.RuntimeMemoryRegion = Boot::PhysicalMemoryRegion{
        .PhysicalStart = arena,
        .VirtualStart = 0U,
        .PageCount = page / Ia64HostedPageBytes,
        .Attributes = attributes,
        .FirmwareType = 7U,
        .Kind = Boot::PhysicalMemoryKind::Conventional,
    };
    return true;
}

/// Sorts the first MemoryRegionCount regions of bootInfo by address, as the
/// final map lists them, and sets the map size from the descriptor size.
/// Fails on overlapping regions.
[[nodiscard]] inline bool SortIa64HostedBootRegions(Boot::BootInfo& bootInfo) noexcept
{
    const Foundation::Size count = bootInfo.MemoryRegionCount;
    if ((count == 0U) || (count > Boot::MaximumMemoryRegions))
    {
        return false;
    }
    for (Foundation::Size index{1U}; index < count; ++index)
    {
        const Boot::PhysicalMemoryRegion region = bootInfo.MemoryRegions[index];
        Foundation::Size position = index;
        while ((position != 0U) && (bootInfo.MemoryRegions[position - 1U].PhysicalStart > region.PhysicalStart))
        {
            bootInfo.MemoryRegions[position] = bootInfo.MemoryRegions[position - 1U];
            --position;
        }
        bootInfo.MemoryRegions[position] = region;
    }
    for (Foundation::Size index{1U}; index < count; ++index)
    {
        const Boot::PhysicalMemoryRegion& previous = bootInfo.MemoryRegions[index - 1U];
        if (Ia64HostedRangesOverlap(previous.PhysicalStart, previous.PageCount * Ia64HostedPageBytes,
                                    bootInfo.MemoryRegions[index].PhysicalStart,
                                    bootInfo.MemoryRegions[index].PageCount * Ia64HostedPageBytes))
        {
            return false;
        }
    }
    bootInfo.FirmwareMemoryMapSizeBytes = static_cast<Foundation::UInt64>(count) * bootInfo.FirmwareDescriptorSize;
    return true;
}
} // Namespace Asteria::TestSupport

#endif // ASTERIA_TESTS_SUPPORT_IA64_HOSTED_BOOT_GEOMETRY_HPP_INCLUDED
