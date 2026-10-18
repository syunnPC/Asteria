// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FRAME_POOL_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FRAME_POOL_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"
#include "Kernel/Memory/PhysicalPageAllocator.hpp"

namespace Asteria::Kernel::Memory
{
enum class FramePoolError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    NotReady,
    /// A run is empty, longer than MaximumRunPages, or outside the pool.
    InvalidRun,
    Exhausted,
    /// The run is not exactly pages this pool handed out and has not taken back.
    NotAllocated,
    /// The run is not exactly pages BeginScrub claimed.
    NotScrubbing,
};

/// A run of pool pages.
struct FramePoolRun
{
    PhysicalAddress PhysicalStart;
    Foundation::UInt64 Pages;
};

struct FramePoolSnapshot
{
    PhysicalAddress PhysicalStart;
    Foundation::UInt64 Pages;
    Foundation::UInt64 FreePages;
    Foundation::UInt64 AllocatedPages;
    Foundation::UInt64 DirtyPages;
    Foundation::UInt64 ScrubbingPages;
};

/// Compatibility facade over the physical page allocator. Initialize
/// retains the zeroed 1,024-page hosted setup; native boot imports dirty segments directly into
/// GetAllocator without a separate pool receipt. Legacy Allocate requires clean pages. RAM Frame
/// creation uses the allocator directly and zeros its dirty mask outside both locks. BeginScrub
/// claims the lowest contiguous released
/// dirty prefix, freed clean by EndScrub or dirty by AbortScrub. Initial imported dirtiness waits
/// for creation or an explicit ClaimDirty call. A claimed run can be neither released nor
/// allocated. The pool has no lock and never touches the pages; its allocator's leaf lock is never
/// held across zeroing.
class FramePool final
{
public:
    static constexpr Foundation::Size MaximumPages{PhysicalPageAllocator::MaximumManagedPages};
    /// One run is at most 512 pages. Native scrubbers divide it into at most 16-page chunks. The
    /// legacy syscall and component segments retain their own bounds.
    static constexpr Foundation::Size MaximumRunPages{PhysicalPageAllocator::MaximumRunPages};
    /// Alignment is at most one maximum run.
    static constexpr Foundation::UInt64 MaximumAlignmentPages{PhysicalPageAllocator::MaximumRunPages};

    constexpr FramePool() noexcept = default;

    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;
    FramePool(FramePool&&) = delete;
    FramePool& operator=(FramePool&&) = delete;

    /// Takes pages zeroed pages from physicalStart on; once.
    [[nodiscard]] FramePoolError Initialize(PhysicalAddress physicalStart, Foundation::UInt64 pages) noexcept;

    /// A run of pages whose physical start is a multiple of alignmentPages
    /// pages (a power of two up to MaximumAlignmentPages; zero or one for a
    /// page); InvalidRun for another alignment.
    [[nodiscard]] Foundation::Result<PhysicalAddress, FramePoolError>
    Allocate(Foundation::UInt64 pages, Foundation::UInt64 alignmentPages = 1U) noexcept;

    /// Returns an allocated run; its pages become dirty.
    [[nodiscard]] FramePoolError Release(PhysicalAddress physicalStart, Foundation::UInt64 pages) noexcept;

    /// Returns an allocated run nothing has written (a creation that rolled
    /// back before it published the run); its pages become free again.
    [[nodiscard]] FramePoolError Return(PhysicalAddress physicalStart, Foundation::UInt64 pages) noexcept;

    /// Claims the lowest dirty run, at most maximumPages (1 to
    /// MaximumRunPages) long, for the caller to zero; Exhausted when no page
    /// is dirty.
    [[nodiscard]] Foundation::Result<FramePoolRun, FramePoolError> BeginScrub(Foundation::UInt64 maximumPages) noexcept;

    /// Frees exactly a run BeginScrub claimed, once the caller zeroed it.
    [[nodiscard]] FramePoolError EndScrub(PhysicalAddress physicalStart, Foundation::UInt64 pages) noexcept;

    /// Gives exactly a run BeginScrub claimed back to the dirty set unzeroed.
    [[nodiscard]] FramePoolError AbortScrub(PhysicalAddress physicalStart, Foundation::UInt64 pages) noexcept;

    [[nodiscard]] FramePoolSnapshot GetSnapshot() const noexcept;

    /// The legacy pool is a facade over the allocator.
    [[nodiscard]] PhysicalPageAllocator& GetAllocator() noexcept
    {
        return m_allocator;
    }
    [[nodiscard]] const PhysicalPageAllocator& GetAllocator() const noexcept
    {
        return m_allocator;
    }

private:
    PhysicalPageAllocator m_allocator{};
};

static_assert(sizeof(FramePoolError) == 1U);
static_assert(sizeof(FramePoolSnapshot) == 48U);
static_assert(sizeof(FramePoolRun) == 16U);
static_assert((FramePool::MaximumPages % 64U) == 0U);
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_FRAME_POOL_HPP_INCLUDED
