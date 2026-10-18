// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_BOOT_AMD64_METADATA_ARENA_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_BOOT_AMD64_METADATA_ARENA_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/AddressSpacePool.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Memory/FrameObjectPool.hpp"
#include "Kernel/Memory/MemoryObjectPool.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Scheduler/ThreadPool.hpp"
#include "Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp"
#include "Platform/Amd64/Executable/Amd64InitialUserImage.hpp"
#include "Platform/Amd64/Memory/InitialPageTablePreparation.hpp"
#include "Platform/Amd64/Memory/RuntimeAddressSpacePageTable.hpp"

namespace Asteria::Platform::Amd64::Bootstrap
{
/// What the boot runtime keeps of one created AddressSpace's root: the seed preparation over its
/// page-table arena, the page table that claims it, the plan, the AddressSpace and whether the pool
/// holds it. Sixteen of them, one per AddressSpace pool slot, outgrow the kernel image's writable
/// range, so they live in the metadata arena.
struct Amd64RuntimeRootSlot
{
    Memory::InitialPageTablePreparation Preparation;
    Memory::RuntimeAddressSpacePageTable PageTable;
    const Kernel::Memory::InitialMappingPlan* Plan;
    Kernel::Memory::AddressSpace Space;
    bool Held;
};

/// The slabs of the AMD64 metadata arena, each from a page boundary after the arena's kernel
/// stacks: the ObjectRegistry's entries, the Frame and MemoryObject pools' slots, then the created
/// roots' slots. The allocator bitmaps follow; the arena ends where the loader's allocation does.
[[nodiscard]] constexpr Foundation::Size MetadataArenaPagesOf(const Foundation::UInt64 bytes) noexcept
{
    return static_cast<Foundation::Size>((bytes + RuntimePoolPageBytes - 1U) / RuntimePoolPageBytes);
}

inline constexpr Foundation::UInt64 MetadataArenaRegistryBytes{Kernel::Object::ObjectRegistry::MaximumObjects *
                                                               Kernel::Object::ObjectRegistry::EntryBytes};
inline constexpr Foundation::Size MetadataArenaRegistryFirstPage{MetadataArenaSlabFirstPage};
inline constexpr Foundation::Size MetadataArenaFrameSlabFirstPage{MetadataArenaRegistryFirstPage +
                                                                  MetadataArenaPagesOf(MetadataArenaRegistryBytes)};
inline constexpr Foundation::Size MetadataArenaMemoryObjectSlabFirstPage{
    MetadataArenaFrameSlabFirstPage + MetadataArenaPagesOf(Kernel::Memory::FrameObjectPool::SlabBytes)};
inline constexpr Foundation::Size MetadataArenaRootSlotFirstPage{
    MetadataArenaMemoryObjectSlabFirstPage + MetadataArenaPagesOf(Kernel::Memory::MemoryObjectPool::SlabBytes)};
inline constexpr Foundation::UInt64 MetadataArenaRootSlotBytes{Kernel::Memory::AddressSpacePool::Capacity *
                                                               sizeof(Amd64RuntimeRootSlot)};
inline constexpr Foundation::Size MetadataArenaBitmapFirstPage{MetadataArenaRootSlotFirstPage +
                                                               MetadataArenaPagesOf(MetadataArenaRootSlotBytes)};

/// Bitmap/clean-prefix metadata per GiB of span. The conservative query bound includes ownership
/// and claim validation; Status records its overhead relative to the original half-byte estimate.
inline constexpr Foundation::UInt64 MetadataArenaBitmapBytesPerGiB{
    Kernel::Memory::PhysicalPageAllocator::MetadataBytesPerGiB};
/// The span the query answers for at most: what the cap leaves for bitmaps
/// after the slabs covers.
inline constexpr Foundation::UInt64 MetadataArenaMaximumSpanGiB{
    ((MetadataArenaMaximumPages - MetadataArenaBitmapFirstPage) * RuntimePoolPageBytes) /
    MetadataArenaBitmapBytesPerGiB};

static_assert(MetadataArenaRootArenaCount + ReservedArenaCount == Kernel::Memory::AddressSpacePool::Capacity);
static_assert(MetadataArenaThreadStackSlots + ThreadStackSlots == Kernel::Scheduler::ThreadPool::Capacity);
static_assert((RuntimePoolPageBytes % Kernel::Object::ObjectRegistry::EntryAlignment) == 0U);
static_assert((RuntimePoolPageBytes % Kernel::Memory::FrameObjectPool::SlabAlignment) == 0U);
static_assert((RuntimePoolPageBytes % Kernel::Memory::MemoryObjectPool::SlabAlignment) == 0U);
static_assert((RuntimePoolPageBytes % alignof(Amd64RuntimeRootSlot)) == 0U);
// The slabs and a 2 MiB rounding fit under the cap, whatever span the loader
// asks for; the bitmaps get what the cap leaves.
static_assert(MetadataArenaBitmapFirstPage <= MetadataArenaMaximumPages - MetadataArenaAlignmentPages);
// The arena lies below 1 GiB and below every fixed user range.
static_assert(Abi::Amd64FixedUserRangesStart == 0x4000'0000U);
static_assert(Executable::InitialUserImageStart >= Abi::Amd64FixedUserRangesStart);

/// The query's answer (KernelQuery::MetadataArenaPages) for a managed span of
/// SpanGiB GiB: the slabs and the bitmaps of that span, rounded up to 2 MiB and capped at
/// MetadataArenaMaximumPages. Pure arithmetic: it reads and writes no kernel state.
[[nodiscard]] constexpr Foundation::UInt64 GetMetadataArenaPages(const Foundation::UInt64 spanGiB) noexcept
{
    const Foundation::UInt64 span = (spanGiB < MetadataArenaMaximumSpanGiB) ? spanGiB : MetadataArenaMaximumSpanGiB;
    const Foundation::UInt64 pages =
        MetadataArenaBitmapFirstPage + MetadataArenaPagesOf(span * MetadataArenaBitmapBytesPerGiB);
    const Foundation::UInt64 rounded =
        ((pages + MetadataArenaAlignmentPages - 1U) / MetadataArenaAlignmentPages) * MetadataArenaAlignmentPages;
    return (rounded < MetadataArenaMaximumPages) ? rounded : MetadataArenaMaximumPages;
}

static_assert(GetMetadataArenaPages(0U) >= MetadataArenaBitmapFirstPage);
static_assert((GetMetadataArenaPages(0U) % MetadataArenaAlignmentPages) == 0U);
static_assert(GetMetadataArenaPages(~Foundation::UInt64{0U}) == MetadataArenaMaximumPages);

/// The seed mappings of the arena at arenaPhysicalStart of arenaPages pages,
/// identity and supervisor RW/NX in every root, appended to
/// seed: the created roots' page-table arenas, each created-thread stack
/// beyond the runtime pool's with its guard page left unmapped, and the slabs
/// and bitmaps to the arena's end. The boot runtime's seed and the host test
/// of the table budget (Amd64MetadataArenaTests) take the same list.
[[nodiscard]] bool AddMetadataArenaMappings(Kernel::Memory::InitialMappingPlan& seed,
                                            Foundation::UInt64 arenaPhysicalStart,
                                            Foundation::UInt64 arenaPages) noexcept;

/// Why a metadata-arena receipt was refused.
enum class MetadataArenaReceiptError : Foundation::UInt8
{
    None,
    /// Zero, misaligned to 2 MiB, not a whole number of 2 MiB blocks, or a
    /// nonzero reserved word.
    InvalidExtent,
    /// Smaller than the slabs need, or larger than the cap.
    InvalidSize,
    /// No single write-back, non-runtime LoaderData region of the final map
    /// covers it.
    InvalidProvenance,
    /// It reaches a fixed user range.
    UserRange,
    /// It overlaps another receipt: the image, a reservation, the boot files
    /// or the inventory.
    Overlap,
};

/// Checks the arena receipt of info's kernel input against info's final map
/// and its other receipts. Reads only info.
[[nodiscard]] MetadataArenaReceiptError ValidateMetadataArenaReceipt(const ::Asteria::Boot::BootInfo& info) noexcept;

static_assert(sizeof(MetadataArenaReceiptError) == 1U);
} // Namespace Asteria::Platform::Amd64::Bootstrap

#endif // ASTERIA_PLATFORM_AMD64_BOOT_AMD64_METADATA_ARENA_HPP_INCLUDED
