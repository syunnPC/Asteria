// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_BOOTSTRAP_PAGE_ALLOCATOR_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_BOOTSTRAP_PAGE_ALLOCATOR_HPP_INCLUDED

#include "Boot/BootInfo.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Memory
{
constexpr Foundation::UInt64 PhysicalPageSizeBytes{4096U};
constexpr Foundation::Size MaximumFreePhysicalRanges{Boot::MaximumMemoryRegions};

enum class BootstrapPageAllocatorError : Foundation::UInt32
{
    None,
    AlreadyInitialized,
    InvalidBootInfo,
    NoUsableMemory,
    NotInitialized,
    InvalidRequest,
    InsufficientMemory,
    RangeCapacityExceeded,
    RangeIndexOutOfBounds,
};

struct PhysicalPageRange
{
    Foundation::UInt64 PhysicalStart;
    Foundation::UInt64 PageCount;
    Foundation::UInt64 Attributes;
    Foundation::UInt32 FirmwareType;
    Foundation::UInt32 Reserved;
};

/// Provides bounded physical-page allocation during single-CPU nucleus bootstrap.
///
/// Only memory classified as Conventional is admitted. Page zero is never admitted.
/// Allocation is deterministic: the lowest suitably aligned physical range wins.
/// The allocator owns only fixed metadata; it does not map, clear, or free pages.
/// Callers must prevent concurrent access and must retain this object for every
/// allocation whose accounting remains relevant.
class BootstrapPageAllocator final
{
public:
    constexpr BootstrapPageAllocator() noexcept
        : m_ranges{}, m_rangeCount{0U}, m_freePageCount{0U}, m_allocatedPageCount{0U}, m_initialized{false}
    {
    }

    BootstrapPageAllocator(const BootstrapPageAllocator&) = delete;
    BootstrapPageAllocator& operator=(const BootstrapPageAllocator&) = delete;
    BootstrapPageAllocator(BootstrapPageAllocator&&) = delete;
    BootstrapPageAllocator& operator=(BootstrapPageAllocator&&) = delete;

    /// Imports a complete, normalized boot memory map exactly once.
    /// On failure, the allocator remains uninitialized and owns no pages.
    [[nodiscard]] BootstrapPageAllocatorError Initialize(const Boot::BootInfo& bootInfo) noexcept;

    /// Allocates one physically contiguous range.
    /// Preconditions enforced as InvalidRequest:
    /// - pageCount is nonzero and representable.
    /// - alignmentPages is a nonzero, representable power of two.
    [[nodiscard]] Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>
    Allocate(Foundation::UInt64 pageCount, Foundation::UInt64 alignmentPages) noexcept;

    /// Allocate's lowest range that also ends at or below endLimit (an
    /// exclusive physical address), else InsufficientMemory; same
    /// preconditions.
    [[nodiscard]] Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>
    AllocateBelow(Foundation::UInt64 pageCount, Foundation::UInt64 alignmentPages,
                  Foundation::UInt64 endLimit) noexcept;

    [[nodiscard]] Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>
    GetFreeRange(Foundation::Size index) const noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] Foundation::Size GetFreeRangeCount() const noexcept;
    [[nodiscard]] Foundation::UInt64 GetFreePageCount() const noexcept;
    [[nodiscard]] Foundation::UInt64 GetAllocatedPageCount() const noexcept;

private:
    /// Both allocations; endPageLimit is an exclusive page number, or zero
    /// for no limit.
    [[nodiscard]] Foundation::Result<PhysicalPageRange, BootstrapPageAllocatorError>
    AllocateLimited(Foundation::UInt64 pageCount, Foundation::UInt64 alignmentPages,
                    Foundation::UInt64 endPageLimit) noexcept;

    PhysicalPageRange m_ranges[MaximumFreePhysicalRanges];
    Foundation::Size m_rangeCount;
    Foundation::UInt64 m_freePageCount;
    Foundation::UInt64 m_allocatedPageCount;
    bool m_initialized;
};

static_assert(sizeof(BootstrapPageAllocatorError) == 4U);
static_assert(sizeof(PhysicalPageRange) == 32U);
static_assert(alignof(PhysicalPageRange) == 8U);
} // namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_BOOTSTRAP_PAGE_ALLOCATOR_HPP_INCLUDED
