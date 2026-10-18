// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Boot/Amd64MetadataArena.hpp"

namespace Asteria::Platform::Amd64::Bootstrap
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;

constexpr UInt64 MaximumUInt64{~UInt64{0U}};
constexpr UInt64 WriteBackAttribute{UInt64{1U} << 3U};
constexpr UInt64 RuntimeAttribute{UInt64{1U} << 63U};
constexpr UInt64 ArenaAlignmentBytes{MetadataArenaAlignmentPages * RuntimePoolPageBytes};

// Whether [start, start + bytes) meets [otherStart, otherStart + otherBytes);
// an empty other range meets nothing.
[[nodiscard]] bool Overlaps(const UInt64 start, const UInt64 bytes, const UInt64 otherStart,
                            const UInt64 otherBytes) noexcept
{
    if ((otherBytes == 0U) || (otherStart > MaximumUInt64 - otherBytes))
    {
        return otherBytes != 0U;
    }
    return (start < otherStart + otherBytes) && (otherStart < start + bytes);
}

[[nodiscard]] bool OverlapsPages(const UInt64 start, const UInt64 bytes,
                                 const ::Asteria::Boot::BootReservation& other) noexcept
{
    return (other.PageCount > (MaximumUInt64 / RuntimePoolPageBytes)) ||
           Overlaps(start, bytes, other.PhysicalStart, other.PageCount * RuntimePoolPageBytes);
}
[[nodiscard]] bool AddIdentity(Kernel::Memory::InitialMappingPlan& seed, const UInt64 start, const UInt64 pages,
                               const Kernel::Memory::InitialMappingOwner owner) noexcept
{
    using Kernel::Memory::MappingPermission;
    return seed.AddMapping(Kernel::Memory::InitialMapping{
               Kernel::Memory::VirtualAddress{start}, Kernel::Memory::PhysicalAddress{start},
               Kernel::Memory::PageCount{pages},
               Kernel::Memory::MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write),
               Kernel::Memory::MemoryCachePolicy::WriteBack, owner, 0U}) ==
           Kernel::Memory::InitialMappingPlanError::None;
}

[[nodiscard]] UInt64 ArenaPage(const UInt64 arenaPhysicalStart, const Size page) noexcept
{
    return arenaPhysicalStart + (static_cast<UInt64>(page) * RuntimePoolPageBytes);
}
} // Namespace

bool AddMetadataArenaMappings(Kernel::Memory::InitialMappingPlan& seed, const UInt64 arenaPhysicalStart,
                              const UInt64 arenaPages) noexcept
{
    using Kernel::Memory::InitialMappingOwner;
    if ((arenaPages <= MetadataArenaSlabFirstPage) || (arenaPages > MetadataArenaMaximumPages))
    {
        return false;
    }
    if (!AddIdentity(seed, ArenaPage(arenaPhysicalStart, MetadataArenaRootArenaFirstPage),
                     MetadataArenaRootArenaCount * RootArenaPages, InitialMappingOwner::PageTable))
    {
        return false;
    }
    for (Size slot{ThreadStackSlots}; slot < Kernel::Scheduler::ThreadPool::Capacity; ++slot)
    {
        if (!AddIdentity(seed, ArenaPage(arenaPhysicalStart, MetadataArenaThreadStackPage(slot)), KernelStackPages,
                         InitialMappingOwner::NucleusStack))
        {
            return false;
        }
    }
    return AddIdentity(seed, ArenaPage(arenaPhysicalStart, MetadataArenaSlabFirstPage),
                       arenaPages - MetadataArenaSlabFirstPage, InitialMappingOwner::NucleusWritableData);
}

MetadataArenaReceiptError ValidateMetadataArenaReceipt(const ::Asteria::Boot::BootInfo& info) noexcept
{
    const ::Asteria::Boot::BootKernelInput& input = info.Kernel;
    const ::Asteria::Boot::BootReservation& arena = input.MetadataArena;
    if ((arena.PhysicalStart == 0U) || ((arena.PhysicalStart % ArenaAlignmentBytes) != 0U) || (arena.PageCount == 0U) ||
        ((arena.PageCount % MetadataArenaAlignmentPages) != 0U) || (arena.Reserved != 0U))
    {
        return MetadataArenaReceiptError::InvalidExtent;
    }
    if ((arena.PageCount < MetadataArenaBitmapFirstPage) || (arena.PageCount > MetadataArenaMaximumPages))
    {
        return MetadataArenaReceiptError::InvalidSize;
    }
    const UInt64 bytes = arena.PageCount * RuntimePoolPageBytes;
    // Identity seed mappings: the arena must stay below every fixed user
    // range, so no user mapping can alias it.
    if ((arena.PhysicalStart >= Abi::Amd64FixedUserRangesStart) ||
        (bytes > Abi::Amd64FixedUserRangesStart - arena.PhysicalStart))
    {
        return MetadataArenaReceiptError::UserRange;
    }
    // Provenance, as the IA-64 runtime receipt's: one LoaderData region of
    // the final map covers the whole arena, write-back and not runtime.
    Size covering{0U};
    bool valid{false};
    for (Size index{0U}; (index < info.MemoryRegionCount) && (index < ::Asteria::Boot::MaximumMemoryRegions); ++index)
    {
        const ::Asteria::Boot::PhysicalMemoryRegion& region = info.MemoryRegions[index];
        if ((region.PageCount > (MaximumUInt64 / RuntimePoolPageBytes)) ||
            (region.PhysicalStart > MaximumUInt64 - (region.PageCount * RuntimePoolPageBytes)))
        {
            return MetadataArenaReceiptError::InvalidProvenance;
        }
        const UInt64 regionEnd = region.PhysicalStart + (region.PageCount * RuntimePoolPageBytes);
        if (!Overlaps(arena.PhysicalStart, bytes, region.PhysicalStart, regionEnd - region.PhysicalStart))
        {
            continue;
        }
        ++covering;
        valid = (region.PhysicalStart <= arena.PhysicalStart) && (regionEnd >= arena.PhysicalStart + bytes) &&
                (region.Kind == ::Asteria::Boot::PhysicalMemoryKind::Loader) && (region.FirmwareType == 2U) &&
                ((region.Attributes & WriteBackAttribute) != 0U) && ((region.Attributes & RuntimeAttribute) == 0U);
    }
    if ((covering != 1U) || !valid)
    {
        return MetadataArenaReceiptError::InvalidProvenance;
    }
    const ::Asteria::Boot::BootImagePlacement& image = input.Image;
    if (Overlaps(arena.PhysicalStart, bytes, image.PhysicalStart, image.SizeBytes) ||
        OverlapsPages(arena.PhysicalStart, bytes, input.Bootstrap) ||
        OverlapsPages(arena.PhysicalStart, bytes, input.RuntimePool) ||
        OverlapsPages(arena.PhysicalStart, bytes, input.Startup) ||
        OverlapsPages(arena.PhysicalStart, bytes, input.Ia64Runtime) ||
        OverlapsPages(arena.PhysicalStart, bytes,
                      ::Asteria::Boot::BootReservation{info.Files.PhysicalStart, info.Files.PageCount, 0U, 0U, 0U}) ||
        OverlapsPages(arena.PhysicalStart, bytes,
                      ::Asteria::Boot::BootReservation{info.Inventory.PhysicalStart, info.Inventory.Pages, 0U, 0U, 0U}))
    {
        return MetadataArenaReceiptError::Overlap;
    }
    return MetadataArenaReceiptError::None;
}
} // Namespace Asteria::Platform::Amd64::Bootstrap
