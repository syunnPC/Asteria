// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Kernel::Memory::PhysicalPageRange;

constexpr Foundation::UInt32 TestDescriptorSize{40U};
constexpr Foundation::UInt64 MaximumPhysicalPageCount{0x000F'FFFF'FFFF'FFFFULL};
constexpr Foundation::UInt64 FullAddressSpacePageCount{Foundation::UInt64{1U} << 52U};

Boot::BootInfo g_bootInfo;
constinit BootstrapPageAllocator g_filterAllocator;
constinit BootstrapPageAllocator g_splitAllocator;
constinit BootstrapPageAllocator g_tailAllocator;
constinit BootstrapPageAllocator g_invalidAllocator;
constinit BootstrapPageAllocator g_noMemoryAllocator;
constinit BootstrapPageAllocator g_requestAllocator;
constinit BootstrapPageAllocator g_capacityAllocator;
constinit BootstrapPageAllocator g_highAddressAllocator;
constinit BootstrapPageAllocator g_fullRangeAllocator;
constinit BootstrapPageAllocator g_finalPageAllocator;
constinit BootstrapPageAllocator g_typeBoundaryAllocator;
constinit BootstrapPageAllocator g_limitAllocator;

[[nodiscard]] Boot::PhysicalMemoryKind ConvertFirmwareMemoryType(const Foundation::UInt32 firmwareType) noexcept
{
    switch (firmwareType)
    {
    case 1U:
    case 2U:
        return Boot::PhysicalMemoryKind::Loader;
    case 3U:
    case 4U:
        return Boot::PhysicalMemoryKind::BootServices;
    case 5U:
    case 6U:
        return Boot::PhysicalMemoryKind::RuntimeServices;
    case 7U:
        return Boot::PhysicalMemoryKind::Conventional;
    case 8U:
        return Boot::PhysicalMemoryKind::Unusable;
    case 9U:
        return Boot::PhysicalMemoryKind::AcpiReclaimable;
    case 10U:
        return Boot::PhysicalMemoryKind::AcpiNonVolatile;
    case 11U:
    case 12U:
        return Boot::PhysicalMemoryKind::MemoryMappedIo;
    case 13U:
        return Boot::PhysicalMemoryKind::PalCode;
    case 14U:
        return Boot::PhysicalMemoryKind::Persistent;
    case 15U:
        return Boot::PhysicalMemoryKind::Unaccepted;
    case 0U:
    default:
        return Boot::PhysicalMemoryKind::Reserved;
    }
}

void ResetBootInfo(const Foundation::UInt32 regionCount) noexcept
{
    g_bootInfo.Signature = Boot::BootInfoSignature;
    g_bootInfo.Version = Boot::BootInfoVersion;
    g_bootInfo.SizeBytes = static_cast<Foundation::UInt32>(sizeof(Boot::BootInfo));
    g_bootInfo.MemoryRegionCount = regionCount;
    g_bootInfo.FirmwareDescriptorVersion = 1U;
    g_bootInfo.FirmwareDescriptorSize = TestDescriptorSize;
    g_bootInfo.Reserved = 0U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = static_cast<Foundation::UInt64>(regionCount) * TestDescriptorSize;
}

void SetRegion(const Foundation::Size index, const Foundation::UInt32 firmwareType,
               const Foundation::UInt64 physicalStart, const Foundation::UInt64 pageCount,
               const Foundation::UInt64 attributes, const Foundation::UInt64 virtualStart = 0U) noexcept
{
    g_bootInfo.MemoryRegions[index] = Boot::PhysicalMemoryRegion{
        .PhysicalStart = physicalStart,
        .VirtualStart = virtualStart,
        .PageCount = pageCount,
        .Attributes = attributes,
        .FirmwareType = firmwareType,
        .Kind = ConvertFirmwareMemoryType(firmwareType),
    };
}

[[nodiscard]] bool MatchesRange(const PhysicalPageRange& range, const Foundation::UInt64 physicalStart,
                                const Foundation::UInt64 pageCount, const Foundation::UInt64 attributes) noexcept
{
    return (range.PhysicalStart == physicalStart) && (range.PageCount == pageCount) &&
           (range.Attributes == attributes) && (range.FirmwareType == 7U) && (range.Reserved == 0U);
}

[[nodiscard]] Foundation::UInt64 CalculateRangeFingerprint(const BootstrapPageAllocator& allocator) noexcept
{
    Foundation::UInt64 fingerprint{0xCBF2'9CE4'8422'2325ULL};
    for (Foundation::Size index{0U}; index < allocator.GetFreeRangeCount(); ++index)
    {
        const auto range = allocator.GetFreeRange(index);
        if (!range.HasValue())
        {
            return 0U;
        }
        const PhysicalPageRange& value = range.GetValue();
        fingerprint ^= value.PhysicalStart;
        fingerprint *= 0x0000'0100'0000'01B3ULL;
        fingerprint ^= value.PageCount;
        fingerprint *= 0x0000'0100'0000'01B3ULL;
        fingerprint ^= value.Attributes;
        fingerprint *= 0x0000'0100'0000'01B3ULL;
        fingerprint ^= value.FirmwareType;
        fingerprint *= 0x0000'0100'0000'01B3ULL;
    }
    return fingerprint;
}

void TestFilteringAndCoalescing(TestContext& runner) noexcept
{
    ResetBootInfo(7U);
    SetRegion(0U, 7U, 0U, 1U, 0x11U);
    SetRegion(1U, 0U, 0x1000U, 1U, 0U);
    SetRegion(2U, 7U, 0x2000U, 2U, 0xAAU);
    SetRegion(3U, 7U, 0x4000U, 3U, 0xAAU);
    SetRegion(4U, 7U, 0x7000U, 1U, 0xBBU);
    SetRegion(5U, 4U, 0x8000U, 2U, 0U);
    SetRegion(6U, 7U, 0xA000U, 2U, 0xBBU);

    ASTERIA_EXPECT(runner, "valid conventional ranges initialize the allocator",
                   g_filterAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None);
    ASTERIA_EXPECT(runner, "initialization filters reserved memory, page zero, and boot-services memory",
                   g_filterAllocator.IsInitialized(), g_filterAllocator.GetFreeRangeCount() == 3U,
                   g_filterAllocator.GetFreePageCount() == 8U, g_filterAllocator.GetAllocatedPageCount() == 0U);

    const auto first = g_filterAllocator.GetFreeRange(0U);
    const auto second = g_filterAllocator.GetFreeRange(1U);
    const auto third = g_filterAllocator.GetFreeRange(2U);
    ASTERIA_EXPECT(runner, "adjacent conventional ranges with equal provenance coalesce", first.HasValue(),
                   MatchesRange(first.GetValue(), 0x2000U, 5U, 0xAAU));
    ASTERIA_EXPECT(runner, "different attributes and occupied gaps remain separate", second.HasValue(),
                   MatchesRange(second.GetValue(), 0x7000U, 1U, 0xBBU), third.HasValue(),
                   MatchesRange(third.GetValue(), 0xA000U, 2U, 0xBBU));
    ASTERIA_EXPECT(runner, "free-range inspection is bounds checked", !g_filterAllocator.GetFreeRange(3U).HasValue(),
                   g_filterAllocator.GetFreeRange(3U).GetError() == BootstrapPageAllocatorError::RangeIndexOutOfBounds);
    ASTERIA_EXPECT(runner, "initialization cannot reset live allocation accounting",
                   g_filterAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::AlreadyInitialized);
}

void TestAllocationAndSplitting(TestContext& runner) noexcept
{
    ResetBootInfo(1U);
    SetRegion(0U, 7U, 0x1000U, 8U, 0x1234U);
    ASTERIA_EXPECT(runner, "split test allocator initializes",
                   g_splitAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None);

    const auto aligned = g_splitAllocator.Allocate(2U, 4U);
    ASTERIA_EXPECT(runner, "aligned middle allocation creates leading and trailing ranges", aligned.HasValue(),
                   MatchesRange(aligned.GetValue(), 0x4000U, 2U, 0x1234U), g_splitAllocator.GetFreeRangeCount() == 2U,
                   g_splitAllocator.GetFreePageCount() == 6U);
    const auto leading = g_splitAllocator.GetFreeRange(0U);
    const auto trailing = g_splitAllocator.GetFreeRange(1U);
    ASTERIA_EXPECT(runner, "middle allocation preserves both residual extents", leading.HasValue(),
                   MatchesRange(leading.GetValue(), 0x1000U, 3U, 0x1234U), trailing.HasValue(),
                   MatchesRange(trailing.GetValue(), 0x6000U, 3U, 0x1234U));

    const auto lowest = g_splitAllocator.Allocate(3U, 1U);
    const auto prefix = g_splitAllocator.Allocate(1U, 2U);
    const auto exact = g_splitAllocator.Allocate(2U, 1U);
    ASTERIA_EXPECT(runner, "allocation consistently chooses the lowest suitable address", lowest.HasValue(),
                   lowest.GetValue().PhysicalStart == 0x1000U, prefix.HasValue(),
                   prefix.GetValue().PhysicalStart == 0x6000U, exact.HasValue(),
                   exact.GetValue().PhysicalStart == 0x7000U);
    ASTERIA_EXPECT(runner, "exact consumption empties metadata and exhaustion is explicit",
                   g_splitAllocator.GetFreeRangeCount() == 0U, g_splitAllocator.GetFreePageCount() == 0U,
                   g_splitAllocator.GetAllocatedPageCount() == 8U, !g_splitAllocator.Allocate(1U, 1U).HasValue());

    ResetBootInfo(1U);
    SetRegion(0U, 7U, 0x1000U, 3U, 0U);
    ASTERIA_EXPECT(runner, "tail test allocator initializes",
                   g_tailAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None);
    const auto tail = g_tailAllocator.Allocate(2U, 2U);
    const auto tailRemainder = g_tailAllocator.GetFreeRange(0U);
    ASTERIA_EXPECT(runner, "tail allocation retains only its leading residual extent", tail.HasValue(),
                   tail.GetValue().PhysicalStart == 0x2000U, tailRemainder.HasValue(),
                   MatchesRange(tailRemainder.GetValue(), 0x1000U, 1U, 0U));
}

void TestBootInfoValidation(TestContext& runner) noexcept
{
    ResetBootInfo(1U);
    SetRegion(0U, 7U, 0x1000U, 1U, 0U, 0x8000U);

    g_bootInfo.Signature ^= 1U;
    ASTERIA_EXPECT(runner, "invalid BootInfo signature is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.Signature ^= 1U;

    ++g_bootInfo.Version;
    ASTERIA_EXPECT(runner, "unsupported BootInfo version is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    --g_bootInfo.Version;

    --g_bootInfo.SizeBytes;
    ASTERIA_EXPECT(runner, "incorrect BootInfo extent is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    ++g_bootInfo.SizeBytes;

    g_bootInfo.MemoryRegionCount = 0U;
    ASTERIA_EXPECT(runner, "empty BootInfo map is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.MemoryRegionCount = 1U;

    g_bootInfo.MemoryRegionCount = static_cast<Foundation::UInt32>(Boot::MaximumMemoryRegions + 1U);
    ASTERIA_EXPECT(runner, "oversized BootInfo region count is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.MemoryRegionCount = 1U;

    ++g_bootInfo.FirmwareDescriptorVersion;
    ASTERIA_EXPECT(runner, "unsupported firmware descriptor version is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    --g_bootInfo.FirmwareDescriptorVersion;

    g_bootInfo.FirmwareDescriptorSize = 39U;
    ASTERIA_EXPECT(runner, "short firmware descriptor extent is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.FirmwareDescriptorSize = TestDescriptorSize;

    g_bootInfo.Reserved = 1U;
    ASTERIA_EXPECT(runner, "nonzero reserved BootInfo fields are rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.Reserved = 0U;

    ++g_bootInfo.FirmwareMemoryMapSizeBytes;
    ASTERIA_EXPECT(runner, "inconsistent source memory-map size is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    --g_bootInfo.FirmwareMemoryMapSizeBytes;

    g_bootInfo.MemoryRegions[0U].PageCount = 0U;
    ASTERIA_EXPECT(runner, "zero-page physical region is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.MemoryRegions[0U].PageCount = 1U;

    ++g_bootInfo.MemoryRegions[0U].PhysicalStart;
    ASTERIA_EXPECT(runner, "unaligned physical region is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    --g_bootInfo.MemoryRegions[0U].PhysicalStart;

    g_bootInfo.MemoryRegions[0U].PhysicalStart = MaximumPhysicalPageCount << 12U;
    g_bootInfo.MemoryRegions[0U].PageCount = 2U;
    ASTERIA_EXPECT(runner, "wrapping physical region is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.MemoryRegions[0U].PhysicalStart = 0x1000U;
    g_bootInfo.MemoryRegions[0U].PageCount = 1U;

    ++g_bootInfo.MemoryRegions[0U].VirtualStart;
    ASTERIA_EXPECT(runner, "unaligned nonzero virtual region is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    --g_bootInfo.MemoryRegions[0U].VirtualStart;

    g_bootInfo.MemoryRegions[0U].VirtualStart = MaximumPhysicalPageCount << 12U;
    g_bootInfo.MemoryRegions[0U].PageCount = 2U;
    ASTERIA_EXPECT(runner, "wrapping virtual region is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.MemoryRegions[0U].VirtualStart = 0x8000U;
    g_bootInfo.MemoryRegions[0U].PageCount = 1U;

    g_bootInfo.MemoryRegions[0U].Kind = Boot::PhysicalMemoryKind::Reserved;
    ASTERIA_EXPECT(runner, "firmware type and normalized kind mismatch is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.MemoryRegions[0U].Kind = Boot::PhysicalMemoryKind::Conventional;

    g_bootInfo.MemoryRegions[0U].FirmwareType = 16U;
    g_bootInfo.MemoryRegions[0U].Kind = Boot::PhysicalMemoryKind::Reserved;
    ASTERIA_EXPECT(runner, "the undefined gap between standard and OEM memory types is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.MemoryRegions[0U].FirmwareType = 7U;
    g_bootInfo.MemoryRegions[0U].Kind = Boot::PhysicalMemoryKind::Conventional;

    g_bootInfo.MemoryRegions[0U].Attributes = 1ULL << 63U;
    ASTERIA_EXPECT(runner, "runtime-attributed conventional memory is rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    g_bootInfo.MemoryRegions[0U].Attributes = 0U;

    ResetBootInfo(2U);
    SetRegion(0U, 7U, 0x2000U, 2U, 0U);
    SetRegion(1U, 7U, 0x3000U, 1U, 0U);
    ASTERIA_EXPECT(runner, "overlapping normalized physical ranges are rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);
    SetRegion(1U, 7U, 0x1000U, 1U, 0U);
    ASTERIA_EXPECT(runner, "out-of-order normalized physical ranges are rejected",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::InvalidBootInfo);

    ResetBootInfo(1U);
    SetRegion(0U, 7U, 0x1000U, 1U, 0U);
    ASTERIA_EXPECT(runner, "failed validation attempts leave the allocator reusable and empty",
                   g_invalidAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None);

    ResetBootInfo(1U);
    SetRegion(0U, 7U, 0U, 1U, 0U);
    ASTERIA_EXPECT(runner, "a conventional map containing only page zero has no usable memory",
                   g_noMemoryAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::NoUsableMemory);
    SetRegion(0U, 7U, 0x1000U, 1U, 0U);
    ASTERIA_EXPECT(runner, "no-memory initialization failure does not poison a later valid import",
                   g_noMemoryAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None);
}

void TestRequestValidation(TestContext& runner) noexcept
{
    ASTERIA_EXPECT(runner, "allocation and inspection require initialization",
                   !g_requestAllocator.Allocate(1U, 1U).HasValue(),
                   g_requestAllocator.Allocate(1U, 1U).GetError() == BootstrapPageAllocatorError::NotInitialized,
                   !g_requestAllocator.GetFreeRange(0U).HasValue());

    ResetBootInfo(1U);
    SetRegion(0U, 7U, 0x1000U, 4U, 0U);
    ASTERIA_EXPECT(runner, "request-validation allocator initializes",
                   g_requestAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None);
    ASTERIA_EXPECT(runner, "zero, non-power-of-two, and unrepresentable requests are rejected",
                   !g_requestAllocator.Allocate(0U, 1U).HasValue(), !g_requestAllocator.Allocate(1U, 0U).HasValue(),
                   !g_requestAllocator.Allocate(1U, 3U).HasValue(),
                   !g_requestAllocator.Allocate(MaximumPhysicalPageCount + 1U, 1U).HasValue(),
                   !g_requestAllocator.Allocate(1U, MaximumPhysicalPageCount + 1U).HasValue());
    const auto fragmented = g_requestAllocator.Allocate(5U, 1U);
    const auto unreachableAlignment = g_requestAllocator.Allocate(1U, 1ULL << 51U);
    ASTERIA_EXPECT(runner, "unsatisfied requests fail without changing allocator accounting", !fragmented.HasValue(),
                   fragmented.GetError() == BootstrapPageAllocatorError::InsufficientMemory,
                   !unreachableAlignment.HasValue(),
                   unreachableAlignment.GetError() == BootstrapPageAllocatorError::InsufficientMemory,
                   g_requestAllocator.GetFreePageCount() == 4U, g_requestAllocator.GetAllocatedPageCount() == 0U);
}

void TestRangeCapacity(TestContext& runner) noexcept
{
    ResetBootInfo(static_cast<Foundation::UInt32>(Boot::MaximumMemoryRegions));
    for (Foundation::Size index{0U}; index < Boot::MaximumMemoryRegions; ++index)
    {
        const Foundation::UInt64 startPage = 1U + (static_cast<Foundation::UInt64>(index) * 8U);
        SetRegion(index, 7U, startPage << 12U, 4U, 0U);
    }
    ASTERIA_EXPECT(runner, "maximum normalized range count fits fixed allocator metadata",
                   g_capacityAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None,
                   g_capacityAllocator.GetFreeRangeCount() == Kernel::Memory::MaximumFreePhysicalRanges,
                   g_capacityAllocator.GetFreePageCount() == (Boot::MaximumMemoryRegions * 4U));

    const Foundation::UInt64 fingerprintBefore = CalculateRangeFingerprint(g_capacityAllocator);
    const auto cannotSplit = g_capacityAllocator.Allocate(1U, 2U);
    const auto unchangedFirst = g_capacityAllocator.GetFreeRange(0U);
    ASTERIA_EXPECT(runner, "metadata-full middle split is rejected atomically", !cannotSplit.HasValue(),
                   cannotSplit.GetError() == BootstrapPageAllocatorError::RangeCapacityExceeded,
                   unchangedFirst.HasValue(), MatchesRange(unchangedFirst.GetValue(), 0x1000U, 4U, 0U),
                   g_capacityAllocator.GetFreePageCount() == (Boot::MaximumMemoryRegions * 4U),
                   CalculateRangeFingerprint(g_capacityAllocator) == fingerprintBefore);

    const auto releaseSlot = g_capacityAllocator.Allocate(4U, 1U);
    const auto splitAfterSlot = g_capacityAllocator.Allocate(1U, 2U);
    ASTERIA_EXPECT(runner, "a consumed range frees one metadata slot for a later split", releaseSlot.HasValue(),
                   releaseSlot.GetValue().PhysicalStart == 0x1000U, splitAfterSlot.HasValue(),
                   splitAfterSlot.GetValue().PhysicalStart == 0xA000U,
                   g_capacityAllocator.GetFreeRangeCount() == Kernel::Memory::MaximumFreePhysicalRanges);
}

void TestHighPhysicalAddresses(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 StartPage{MaximumPhysicalPageCount - 8U};
    constexpr Foundation::UInt64 ExpectedAllocationPage{StartPage + 1U};
    ResetBootInfo(1U);
    SetRegion(0U, 7U, StartPage << 12U, 8U, 0x55U);
    ASTERIA_EXPECT(runner, "highest representable multi-page range initializes",
                   g_highAddressAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None);
    const auto allocation = g_highAddressAllocator.Allocate(2U, 8U);
    const auto leading = g_highAddressAllocator.GetFreeRange(0U);
    const auto trailing = g_highAddressAllocator.GetFreeRange(1U);
    ASTERIA_EXPECT(runner, "high-address alignment and middle split do not wrap", allocation.HasValue(),
                   MatchesRange(allocation.GetValue(), ExpectedAllocationPage << 12U, 2U, 0x55U), leading.HasValue(),
                   leading.GetValue().PageCount == 1U, trailing.HasValue(), trailing.GetValue().PageCount == 5U);
}

// The end limit IA-64's frame pool takes, below 4 GiB: the lowest range that fits below it, none
// when no range does, and every range without it.
void TestEndLimit(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 Limit{Foundation::UInt64{1U} << 32U};
    ResetBootInfo(3U);
    SetRegion(0U, 7U, 0x1000U, 2U, 0x11U);
    SetRegion(1U, 7U, Limit - 0x3000U, 6U, 0x22U);
    SetRegion(2U, 7U, Limit + 0x10000U, 8U, 0x33U);
    ASTERIA_EXPECT(runner, "limit test allocator initializes",
                   g_limitAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::None);
    const auto below = g_limitAllocator.AllocateBelow(3U, 1U, Limit);
    const auto refused = g_limitAllocator.AllocateBelow(3U, 1U, Limit);
    const auto unlimited = g_limitAllocator.Allocate(3U, 1U);
    const auto edge = g_limitAllocator.AllocateBelow(2U, 1U, Limit);
    ASTERIA_EXPECT(runner, "a limited allocation ends at or below the limit, else is InsufficientMemory",
                   below.HasValue() && MatchesRange(below.GetValue(), Limit - 0x3000U, 3U, 0x22U),
                   !refused.HasValue() && (refused.GetError() == BootstrapPageAllocatorError::InsufficientMemory),
                   unlimited.HasValue() && MatchesRange(unlimited.GetValue(), Limit, 3U, 0x22U),
                   edge.HasValue() && MatchesRange(edge.GetValue(), 0x1000U, 2U, 0x11U));
}

void TestFullPhysicalRange(TestContext& runner) noexcept
{
    ResetBootInfo(1U);
    SetRegion(0U, 7U, 0U, FullAddressSpacePageCount, 0x66U);
    const BootstrapPageAllocatorError initialized = g_fullRangeAllocator.Initialize(g_bootInfo);
    const auto range = g_fullRangeAllocator.GetFreeRange(0U);
    ASTERIA_EXPECT(runner, "a full-width descriptor is imported after excluding physical page zero",
                   initialized == BootstrapPageAllocatorError::None, range.HasValue(),
                   MatchesRange(range.GetValue(), 0x1000U, MaximumPhysicalPageCount, 0x66U),
                   g_fullRangeAllocator.GetFreePageCount() == MaximumPhysicalPageCount);

    const auto allocation = g_fullRangeAllocator.Allocate(MaximumPhysicalPageCount, 1U);
    ASTERIA_EXPECT(runner, "the complete post-zero physical range allocates without page-index wrap",
                   allocation.HasValue(), MatchesRange(allocation.GetValue(), 0x1000U, MaximumPhysicalPageCount, 0x66U),
                   g_fullRangeAllocator.GetFreeRangeCount() == 0U, g_fullRangeAllocator.GetFreePageCount() == 0U,
                   g_fullRangeAllocator.GetAllocatedPageCount() == MaximumPhysicalPageCount);

    ResetBootInfo(1U);
    SetRegion(0U, 7U, 0xFFFF'FFFF'FFFF'F000ULL, 1U, 0x77U);
    const BootstrapPageAllocatorError finalPageInitialized = g_finalPageAllocator.Initialize(g_bootInfo);
    const auto finalPage = g_finalPageAllocator.Allocate(1U, 1U);
    ASTERIA_EXPECT(runner, "the final physical page initializes and allocates exactly",
                   finalPageInitialized == BootstrapPageAllocatorError::None, finalPage.HasValue(),
                   MatchesRange(finalPage.GetValue(), 0xFFFF'FFFF'FFFF'F000ULL, 1U, 0x77U));
}

void TestTypeNamespaceBoundaries(TestContext& runner) noexcept
{
    constexpr Foundation::UInt32 invalidFirmwareTypes[]{
        16U,
        0x6FFF'FFFFU,
    };
    bool invalidGapRejected{true};
    for (const Foundation::UInt32 firmwareType : invalidFirmwareTypes)
    {
        ResetBootInfo(1U);
        SetRegion(0U, firmwareType, 0x1000U, 1U, 0U);
        invalidGapRejected = invalidGapRejected && (g_typeBoundaryAllocator.Initialize(g_bootInfo) ==
                                                    BootstrapPageAllocatorError::InvalidBootInfo);
    }
    ASTERIA_EXPECT(runner, "both ends of the undefined firmware-type gap are rejected", invalidGapRejected);

    constexpr Foundation::UInt32 reservedFirmwareTypes[]{
        0x7000'0000U,
        0x7FFF'FFFFU,
        0x8000'0000U,
        0xFFFF'FFFFU,
    };
    bool reservedNamespacesAccepted{true};
    for (const Foundation::UInt32 firmwareType : reservedFirmwareTypes)
    {
        ResetBootInfo(1U);
        SetRegion(0U, firmwareType, 0x1000U, 1U, 0U);
        reservedNamespacesAccepted = reservedNamespacesAccepted && (g_typeBoundaryAllocator.Initialize(g_bootInfo) ==
                                                                    BootstrapPageAllocatorError::NoUsableMemory);
    }
    ASTERIA_EXPECT(runner, "OEM and OS firmware-type boundaries remain valid reserved storage",
                   reservedNamespacesAccepted);

    ResetBootInfo(1U);
    SetRegion(0U, 6U, 0x2000U, 2U, 1ULL << 63U);
    ASTERIA_EXPECT(runner, "runtime-attributed non-conventional memory remains valid and unavailable",
                   g_typeBoundaryAllocator.Initialize(g_bootInfo) == BootstrapPageAllocatorError::NoUsableMemory);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"BootstrapPageAllocatorTests"};
    Asteria::Test::TestFilteringAndCoalescing(runner);
    Asteria::Test::TestAllocationAndSplitting(runner);
    Asteria::Test::TestBootInfoValidation(runner);
    Asteria::Test::TestRequestValidation(runner);
    Asteria::Test::TestRangeCapacity(runner);
    Asteria::Test::TestHighPhysicalAddresses(runner);
    Asteria::Test::TestEndLimit(runner);
    Asteria::Test::TestFullPhysicalRange(runner);
    Asteria::Test::TestTypeNamespaceBoundaries(runner);
    return runner.Finish();
}
