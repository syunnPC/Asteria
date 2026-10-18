// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_METADATA_LAYOUT_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_METADATA_LAYOUT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/FrameObjectPool.hpp"
#include "Kernel/Memory/MemoryObjectPool.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Memory/InitialRseBackingStorePreparer.hpp"

namespace Asteria::Platform::Ia64::Memory
{
/// The slabs and the allocator bitmaps in the runtime page DTR0 maps, between the runtime owner and
/// the RSE store, as byte offsets from the page's start: the ObjectRegistry's entries, then the
/// Frame and MemoryObject pools' slots, each from a 4 KiB boundary, then the bitmap budget, a
/// separating page and the RSE in the page's final 32 KiB. DTR0 already maps all of it, so no
/// translation register is added: the boot still reports DTR=04 and ITR=02. No path with PSR.ic
/// clear touches the slabs.
[[nodiscard]] constexpr Foundation::UInt64 Ia64RuntimeMetadataPageAlign(const Foundation::UInt64 bytes) noexcept
{
    return (bytes + Kernel::Memory::PhysicalPageSizeBytes - 1U) & ~(Kernel::Memory::PhysicalPageSizeBytes - 1U);
}

inline constexpr Foundation::UInt64 Ia64RuntimeRegistryOffsetBytes{
    Ia64RuntimeMetadataPageAlign(BootstrapRuntimeObjectOffsetBytes + sizeof(Ia64BspRuntimeMemory))};
inline constexpr Foundation::UInt64 Ia64RuntimeFrameSlabOffsetBytes{
    Ia64RuntimeRegistryOffsetBytes + Ia64RuntimeMetadataPageAlign(Kernel::Object::ObjectRegistry::MaximumObjects *
                                                                  Kernel::Object::ObjectRegistry::EntryBytes)};
inline constexpr Foundation::UInt64 Ia64RuntimeMemoryObjectSlabOffsetBytes{
    Ia64RuntimeFrameSlabOffsetBytes + Ia64RuntimeMetadataPageAlign(Kernel::Memory::FrameObjectPool::SlabBytes)};
inline constexpr Foundation::UInt64 Ia64RuntimeBitmapOffsetBytes{
    Ia64RuntimeMemoryObjectSlabOffsetBytes + Ia64RuntimeMetadataPageAlign(Kernel::Memory::MemoryObjectPool::SlabBytes)};
inline constexpr Foundation::UInt64 Ia64RuntimeSlabBytes{Ia64RuntimeBitmapOffsetBytes - Ia64RuntimeRegistryOffsetBytes};
/// An unused page between the bitmap budget and the RSE store. Inside one
/// pinned page it is no hardware guard, only a separation.
inline constexpr Foundation::UInt64 Ia64RuntimeRseSeparationBytes{Kernel::Memory::PhysicalPageSizeBytes};
/// The bytes a runtime page must hold whatever the bitmaps get: the
/// bootstrap prefix, the owner, the slabs, the separation and the RSE. A
/// fallback page smaller than this is refused, as one that cannot hold the
/// owner was.
inline constexpr Foundation::UInt64 Ia64RuntimeRequiredBytes{
    Ia64RuntimeBitmapOffsetBytes + Ia64RuntimeRseSeparationBytes + InitialRseBackingStoreSizeBytes};
/// What the preferred 16 MiB page leaves for allocator bitmaps and the scalable clean-prefix
/// indexes; on a fallback page the metadata takes what remains of it.
inline constexpr Foundation::UInt64 Ia64RuntimeBitmapBudgetBytes{RuntimeMemoryReservationPreferredBytes -
                                                                 Ia64RuntimeRequiredBytes};

static_assert((Kernel::Memory::PhysicalPageSizeBytes % Kernel::Object::ObjectRegistry::EntryAlignment) == 0U);
static_assert((Kernel::Memory::PhysicalPageSizeBytes % Kernel::Memory::FrameObjectPool::SlabAlignment) == 0U);
static_assert((Kernel::Memory::PhysicalPageSizeBytes % Kernel::Memory::MemoryObjectPool::SlabAlignment) == 0U);
// The owner, the slabs, the bitmap budget, the separation and the RSE fill
// the preferred page exactly; the upper guard lies outside it.
static_assert(Ia64RuntimeRequiredBytes < RuntimeMemoryReservationPreferredBytes);
static_assert(Ia64RuntimeBitmapOffsetBytes + Ia64RuntimeBitmapBudgetBytes + Ia64RuntimeRseSeparationBytes +
                  InitialRseBackingStoreSizeBytes ==
              RuntimeMemoryReservationPreferredBytes);
// At least 8 GiB of managed span fit the conservative metadata budget.
static_assert(Ia64RuntimeBitmapBudgetBytes >= 8U * Kernel::Memory::PhysicalPageAllocator::MetadataBytesPerGiB);
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_RUNTIME_METADATA_LAYOUT_HPP_INCLUDED
