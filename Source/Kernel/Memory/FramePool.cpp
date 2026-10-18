// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/FramePool.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
[[nodiscard]] FramePoolError Convert(const PhysicalPageAllocatorError error) noexcept
{
    switch (error)
    {
    case PhysicalPageAllocatorError::None:
        return FramePoolError::None;
    case PhysicalPageAllocatorError::InvalidConfiguration:
        return FramePoolError::InvalidConfiguration;
    case PhysicalPageAllocatorError::AlreadyInitialized:
        return FramePoolError::AlreadyInitialized;
    case PhysicalPageAllocatorError::NotReady:
        return FramePoolError::NotReady;
    case PhysicalPageAllocatorError::InvalidRun:
        return FramePoolError::InvalidRun;
    case PhysicalPageAllocatorError::Exhausted:
        return FramePoolError::Exhausted;
    case PhysicalPageAllocatorError::NotAllocated:
    case PhysicalPageAllocatorError::Unverified:
        return FramePoolError::NotAllocated;
    case PhysicalPageAllocatorError::NotClaimed:
        return FramePoolError::NotScrubbing;
    }
    return FramePoolError::InvalidRun;
}
[[nodiscard]] PhysicalPageRun Run(const PhysicalAddress start, const Foundation::UInt64 pages) noexcept
{
    return PhysicalPageRun{.PhysicalStart = start, .Pages = pages, .DirtyMask = {}};
}
} // Namespace
FramePoolError FramePool::Initialize(const PhysicalAddress start, const Foundation::UInt64 pages) noexcept
{
    return Convert(m_allocator.Initialize(start, pages));
}
Foundation::Result<PhysicalAddress, FramePoolError>
FramePool::Allocate(const Foundation::UInt64 pages, const Foundation::UInt64 alignmentPages) noexcept
{
    using Result = Foundation::Result<PhysicalAddress, FramePoolError>;
    if ((pages == 0U) || (pages > MaximumRunPages) || (alignmentPages > MaximumAlignmentPages) ||
        ((alignmentPages & (alignmentPages - 1U)) != 0U))
    {
        return Result::Failure(FramePoolError::InvalidRun);
    }
    Foundation::UInt8 order{0U};
    while ((Foundation::UInt64{1U} << order) < alignmentPages)
    {
        ++order;
    }
    const auto run = m_allocator.Allocate(pages, order, 0U, true);
    return run.HasValue() ? Result::Success(run.GetValue().PhysicalStart) : Result::Failure(Convert(run.GetError()));
}
FramePoolError FramePool::Release(const PhysicalAddress start, const Foundation::UInt64 pages) noexcept
{
    return Convert(m_allocator.Release(Run(start, pages)));
}
FramePoolError FramePool::Return(const PhysicalAddress start, const Foundation::UInt64 pages) noexcept
{
    return Convert(m_allocator.ReturnVerified(Run(start, pages)));
}
Foundation::Result<FramePoolRun, FramePoolError> FramePool::BeginScrub(const Foundation::UInt64 maximumPages) noexcept
{
    using Result = Foundation::Result<FramePoolRun, FramePoolError>;
    if ((maximumPages == 0U) || (maximumPages > MaximumRunPages))
    {
        return Result::Failure(FramePoolError::InvalidRun);
    }
    // Batch the released dirty prefix so index
    // repair stays bounded.
    const auto claimed = m_allocator.ClaimDirtyPrefix(maximumPages);
    return claimed.HasValue() ? Result::Success({claimed.GetValue().PhysicalStart, claimed.GetValue().Pages})
                              : Result::Failure(Convert(claimed.GetError()));
}
FramePoolError FramePool::EndScrub(const PhysicalAddress start, const Foundation::UInt64 pages) noexcept
{
    return Convert(m_allocator.EndClaim(Run(start, pages), 4U));
}
FramePoolError FramePool::AbortScrub(const PhysicalAddress start, const Foundation::UInt64 pages) noexcept
{
    return Convert(m_allocator.EndClaim(Run(start, pages), 5U));
}
FramePoolSnapshot FramePool::GetSnapshot() const noexcept
{
    const auto snapshot = m_allocator.GetSnapshot();
    return FramePoolSnapshot{.PhysicalStart = snapshot.PhysicalStart,
                             .Pages = snapshot.ManagedPages,
                             .FreePages = snapshot.CleanPages,
                             .AllocatedPages = snapshot.AllocatedPages,
                             .DirtyPages = snapshot.DirtyPages,
                             .ScrubbingPages = snapshot.ClaimedPages + snapshot.RetiredPages};
}
} // Namespace Asteria::Kernel::Memory
