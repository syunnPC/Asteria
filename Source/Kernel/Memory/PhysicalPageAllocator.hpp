// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_PHYSICAL_PAGE_ALLOCATOR_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_PHYSICAL_PAGE_ALLOCATOR_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"
#include "Kernel/Memory/PhysicalPageImportEvidence.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Memory
{
enum class PhysicalPageAllocatorError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    NotReady,
    InvalidRun,
    Exhausted,
    NotAllocated,
    NotClaimed,
    Unverified,
};

struct PhysicalPageRun
{
    static constexpr Foundation::Size DirtyWords{8U};
    PhysicalAddress PhysicalStart;
    Foundation::UInt64 Pages;
    /// One bit per page still requiring zeroing; clear only after Zero=None.
    Foundation::UInt64 DirtyMask[DirtyWords];
};

struct PhysicalPageAllocatorSnapshot
{
    PhysicalAddress PhysicalStart;
    Foundation::UInt64 ManagedPages;
    Foundation::UInt64 AllocatablePages;
    Foundation::UInt64 CleanPages;
    Foundation::UInt64 DirtyPages;
    Foundation::UInt64 AllocatedPages;
    Foundation::UInt64 ClaimedPages;
    Foundation::UInt64 RetiredPages;
    Foundation::UInt64 FreeLowBlocks;
};

/// Ascending admission of at most 64 padded segments. RAM is never allocator metadata. All runtime
/// searches descend bounded summary trees; rebuilding an index visits at most 512 pages,
/// independent of the amount of managed memory.
struct PhysicalPageImportRange
{
    PhysicalAddress Start;
    Foundation::UInt64 Pages;
};
struct PhysicalPageImportResult
{
    Foundation::UInt64 ManagedPages;
    Foundation::UInt64 UnmanagedPages;
    Foundation::UInt64 MetadataBytes;
    Foundation::UInt64 LowWatermarkBlocks;
    Foundation::Size Segments;
};
class PhysicalPageAllocator final
{
public:
    // Compatibility initialization only; the importer has no page-count cap.
    static constexpr Foundation::Size MaximumManagedPages{1024U};
    static constexpr Foundation::Size MaximumSegments{64U};
    static constexpr Foundation::Size MaximumOrder{9U};
    static constexpr Foundation::Size MaximumRunPages{1U << MaximumOrder};
    static constexpr Foundation::Size MaximumScrubOrder{4U};
    static constexpr Foundation::Size MaximumScrubPages{1U << MaximumScrubOrder};
    static constexpr Foundation::Size DirtyWords{PhysicalPageRun::DirtyWords};
    static constexpr Foundation::UInt64 MergeHolePages{(64U << 20U) / MappingPageSizeBytes};
    // Includes the clean-prefix index, ownership/claim checks
    // and padding. Platforms reserve this conservative bound per span GiB.
    static constexpr Foundation::UInt64 MetadataBytesPerGiB{384U << 10U};
    constexpr PhysicalPageAllocator() noexcept = default;
    PhysicalPageAllocator(const PhysicalPageAllocator&) = delete;
    PhysicalPageAllocator& operator=(const PhysicalPageAllocator&) = delete;
    [[nodiscard]] PhysicalPageAllocatorError Initialize(PhysicalAddress start, Foundation::UInt64 pages,
                                                        bool dirty = false) noexcept;
    /// Sorted, disjoint page ranges; no range includes page zero. Caller has
    /// subtracted every receipt and DMA exclusion before this one-shot import.
    [[nodiscard]] Foundation::Result<PhysicalPageImportResult, PhysicalPageAllocatorError>
    Import(const PhysicalPageImportRange* ranges, Foundation::Size count, void* metadata,
           Foundation::Size metadataBytes, Foundation::UInt64 lowOverride = 0U,
           Foundation::UInt64 minimumLowBlocks = 1U) noexcept;
    [[nodiscard]] static constexpr Foundation::Size MetadataBytes(Foundation::Size pages) noexcept;
    void RecordImport(PhysicalPageImportEvidence& evidence) const noexcept
    {
        evidence.ManagedPages = m_snapshot.ManagedPages;
        evidence.UnmanagedPages = m_import.UnmanagedPages;
        evidence.MetadataBytes = m_import.MetadataBytes;
        evidence.LowWatermarkBlocks = m_watermark;
        evidence.Segments = m_segmentCount;
        for (Foundation::Size i{0U}; i < m_segmentCount; ++i)
        {
            evidence.SegmentStart[i] = m_segments[i].Base * MappingPageSizeBytes;
            evidence.SegmentPages[i] = m_segments[i].Pages;
        }
    }
    [[nodiscard]] bool Contains(PhysicalAddress start, Foundation::UInt64 pages) const noexcept;
    [[nodiscard]] Foundation::Result<PhysicalPageRun, PhysicalPageAllocatorError>
    Allocate(Foundation::UInt64 pages, Foundation::UInt8 alignmentOrder = 0U, Foundation::UInt8 limitBits = 0U,
             bool cleanOnly = false) noexcept;
    [[nodiscard]] PhysicalPageAllocatorError Release(const PhysicalPageRun& run) noexcept;
    [[nodiscard]] PhysicalPageAllocatorError ReturnVerified(const PhysicalPageRun& run) noexcept;
    [[nodiscard]] Foundation::Result<PhysicalPageRun, PhysicalPageAllocatorError>
    ClaimDirty(Foundation::UInt8 maximumOrder = MaximumScrubOrder) noexcept;
    [[nodiscard]] PhysicalPageAllocatorError FinishScrub(const PhysicalPageRun& run) noexcept;
    [[nodiscard]] PhysicalPageAllocatorError AbortScrub(const PhysicalPageRun& run) noexcept;
    [[nodiscard]] PhysicalPageAllocatorError Retire(const PhysicalPageRun& run) noexcept;
    [[nodiscard]] PhysicalPageAllocatorSnapshot GetSnapshot() const noexcept;

private:
    friend class FramePool;
    [[nodiscard]] Foundation::Result<PhysicalPageRun, PhysicalPageAllocatorError>
    ClaimDirtyPrefix(Foundation::UInt64 maximumPages) noexcept;
    static constexpr Foundation::Size Orders{MaximumOrder + 1U};
    static constexpr Foundation::Size StateBitmaps{8U};
    static constexpr Foundation::Size Pairs{Orders * (Orders + 1U) / 2U};
    static constexpr Foundation::Size MaximumLevels{9U};
    struct BitTree
    {
        Foundation::UInt64* Data{nullptr};
        Foundation::Size Bits{0U};
        [[nodiscard]] bool Get(Foundation::Size bit) const noexcept;
        void Set(Foundation::Size bit, bool value) noexcept;
        [[nodiscard]] Foundation::Size Find(Foundation::Size first, bool reverse) const noexcept;
    };
    struct MaxTree
    {
        Foundation::UInt16* Data{nullptr};
        Foundation::Size Leaves{0U};
        void Set(Foundation::Size leaf, Foundation::UInt16 value) noexcept;
        [[nodiscard]] Foundation::Size Find(Foundation::UInt16 minimum, bool reverse) const noexcept;
    };
    struct Segment
    {
        Foundation::UInt64 Base{0U};
        Foundation::Size Pages{0U};
        BitTree Free[Orders]{};
        BitTree DirtyChunks{};
        BitTree CollectionChunks{};
        MaxTree Clean[Pairs]{};
        Foundation::UInt64 Counts[Orders]{};
        Foundation::UInt64* Managed{nullptr};
        Foundation::UInt64* Dirty{nullptr};
        Foundation::UInt64* Allocated{nullptr};
        Foundation::UInt64* Claimed{nullptr};
        Foundation::UInt64* ClaimStarts{nullptr};
        Foundation::UInt64* ClaimEnds{nullptr};
        Foundation::UInt64* Retired{nullptr};
        // Collection visits released pages only. ClaimDirty also sees initially dirty pages.
        Foundation::UInt64* CollectionDirty{nullptr};
    };
    [[nodiscard]] static constexpr Foundation::Size TreeWords(Foundation::Size leaves) noexcept
    {
        Foundation::Size total{0U};
        for (Foundation::Size level{0U}; level < MaximumLevels; ++level)
        {
            total += leaves;
            if (leaves == 1U)
            {
                break;
            }
            leaves = (leaves + 63U) / 64U;
        }
        return total;
    }
    [[nodiscard]] static constexpr Foundation::Size Pair(Foundation::Size k, Foundation::Size j) noexcept
    {
        return j * (j + 1U) / 2U + k;
    }
    void Prepare(Segment& segment, Foundation::UInt64 base, Foundation::Size pages,
                 Foundation::UInt8*& memory) noexcept;
    void SetFree(Segment& segment, Foundation::Size order, Foundation::Size block, bool free) noexcept;
    void FreeRange(Segment& segment, Foundation::Size first, Foundation::Size pages) noexcept;
    void Rebuild(Segment& segment, Foundation::Size chunk, bool cached = false) noexcept;
    void Changed(Segment& segment, Foundation::Size first, Foundation::Size pages, bool cached = false) noexcept;
    [[nodiscard]] Segment* FindRun(const PhysicalPageRun& run, Foundation::Size& first) noexcept;
    [[nodiscard]] PhysicalPageRun MakeRun(const Segment& segment, Foundation::Size first, Foundation::Size pages,
                                          bool cached = false) const noexcept;
    [[nodiscard]] bool IsClean(const Segment& segment, Foundation::Size first, Foundation::Size pages) const noexcept;
    [[nodiscard]] PhysicalPageAllocatorError Return(const PhysicalPageRun& run, bool verified) noexcept;
    [[nodiscard]] PhysicalPageAllocatorError EndClaim(const PhysicalPageRun& run, Foundation::UInt8 action) noexcept;
    [[nodiscard]] Foundation::UInt64 LowBlocks() const noexcept;
    [[nodiscard]] static bool Bit(const Foundation::UInt64* bits, Foundation::Size page) noexcept;
    static void Set(Foundation::UInt64* bits, Foundation::Size page, bool value) noexcept;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Segment m_segments[MaximumSegments]{};
    Foundation::Size m_segmentCount{0U};
    PhysicalPageAllocatorSnapshot m_snapshot{};
    PhysicalPageImportResult m_import{};
    Foundation::UInt64 m_watermark{0U};
    // Owner scratch, protected by the leaf lock; never on a target stack.
    Foundation::UInt16 m_prefix[2U * MaximumRunPages]{};
    Foundation::UInt64 m_chunkDirty[DirtyWords]{};
    Foundation::UInt64 m_choiceDirty[DirtyWords]{};
    PhysicalPageRun m_choice{};
    alignas(8) Foundation::UInt8 m_legacy[4096U]{};
    bool m_directional{false};
    bool m_ready{false};
};
constexpr Foundation::Size PhysicalPageAllocator::MetadataBytes(const Foundation::Size pages) noexcept
{
    Foundation::Size bytes{StateBitmaps * ((pages + 63U) / 64U) * sizeof(Foundation::UInt64)};
    for (Foundation::Size order{0U}; order < Orders; ++order)
    {
        bytes += TreeWords(((pages >> order) + 63U) / 64U) * sizeof(Foundation::UInt64);
    }
    const Foundation::Size chunks{pages / MaximumRunPages};
    bytes += 2U * TreeWords((chunks + 63U) / 64U) * sizeof(Foundation::UInt64);
    const Foundation::Size maximaBytes{TreeWords(chunks) * sizeof(Foundation::UInt16)};
    bytes += Pairs * ((maximaBytes + 7U) & ~Foundation::Size{7U});
    return bytes;
}
static_assert(PhysicalPageAllocator::MetadataBytes(1536U) <= 2560U);
static_assert(PhysicalPageAllocator::MetadataBytes(1U << 18U) <= PhysicalPageAllocator::MetadataBytesPerGiB);

static_assert(PhysicalPageAllocator::DirtyWords == 8U);
static_assert(PhysicalPageAllocator::DirtyWords * 64U == PhysicalPageAllocator::MaximumRunPages);
static_assert(sizeof(PhysicalPageRun) == 80U);
static_assert(PhysicalPageAllocator::MaximumScrubPages == 16U);
} // Namespace Asteria::Kernel::Memory
#endif
