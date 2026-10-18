// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Platform/Ia64/Memory/Ia64RegionIdentifierAllocator.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::Ia64FirstRuntimeRegionIdentifier;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocationReceipt;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocator;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocatorError;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocatorSnapshot;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocatorState;
using Platform::Ia64::Memory::Ia64RegionIdentifierCountPerAddressSpace;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::TranslationCapabilities;

[[nodiscard]] constexpr PalVirtualMemoryCapabilities
ValidCapabilities(const Foundation::UInt8 regionIdentifierBits = 18U) noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            TranslationCapabilities{
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = 48U,
                .ImplementedVirtualAddressMostSignificantBit = 53U,
                .RegionIdentifierBits = regionIdentifierBits,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask = 0x11U,
                .HardwareWalkerImplemented = true,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = ArchitectedMinimumPurgePageSizeMask,
        .InstructionTranslationRegisterCount = 8U,
        .DataTranslationRegisterCount = 8U,
        .ProtectionKeyRegisterCount = 16U,
        .MaximumConcurrentPurgesEncoding = 1U,
        .HashTagIdentifier = 0U,
        .UniqueTranslationCacheCount = 1U,
        .TranslationCacheLevelCount = 1U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] bool SnapshotsEqual(const Ia64RegionIdentifierAllocatorSnapshot& left,
                                  const Ia64RegionIdentifierAllocatorSnapshot& right) noexcept
{
    return (left.NextRegionIdentifier == right.NextRegionIdentifier) &&
           (left.RepresentableLimitExclusive == right.RepresentableLimitExclusive) &&
           (left.AllocatedAddressSpaceCount == right.AllocatedAddressSpaceCount) && (left.State == right.State) &&
           (left.RegionIdentifierBits == right.RegionIdentifierBits) && (left.Reserved[0U] == right.Reserved[0U]) &&
           (left.Reserved[1U] == right.Reserved[1U]);
}

[[nodiscard]] bool InitializationIsRejected(const PalVirtualMemoryCapabilities& capabilities) noexcept
{
    Ia64RegionIdentifierAllocator allocator;
    const Ia64RegionIdentifierAllocatorSnapshot before = allocator.GetSnapshot();
    const Ia64RegionIdentifierAllocatorError error = allocator.Initialize(capabilities);
    return (error == Ia64RegionIdentifierAllocatorError::InvalidCapabilities) &&
           SnapshotsEqual(before, allocator.GetSnapshot());
}

void TestInitialization(TestContext& runner) noexcept
{
    Ia64RegionIdentifierAllocator uninitialized;
    const Ia64RegionIdentifierAllocatorSnapshot fresh = uninitialized.GetSnapshot();
    const auto premature = uninitialized.Allocate();
    ASTERIA_EXPECT(runner, "allocation before initialization leaves fresh state unchanged", !premature.HasValue(),
                   premature.GetError() == Ia64RegionIdentifierAllocatorError::NotInitialized,
                   SnapshotsEqual(fresh, uninitialized.GetSnapshot()));

    Ia64RegionIdentifierAllocator narrowWidth;
    Ia64RegionIdentifierAllocator wideWidth;
    ASTERIA_EXPECT(
        runner, "direct common RID widths outside eighteen through twenty-four bits are rejected atomically",
        narrowWidth.Initialize(Foundation::UInt8{17U}) == Ia64RegionIdentifierAllocatorError::InvalidCapabilities,
        wideWidth.Initialize(Foundation::UInt8{25U}) == Ia64RegionIdentifierAllocatorError::InvalidCapabilities,
        SnapshotsEqual(fresh, narrowWidth.GetSnapshot()), SnapshotsEqual(fresh, wideWidth.GetSnapshot()));

    PalVirtualMemoryCapabilities invalid = ValidCapabilities();
    invalid.Translation.RegionIdentifierBits = 17U;
    ASTERIA_EXPECT(runner, "a seventeen-bit RID capability is rejected atomically", InitializationIsRejected(invalid));
    invalid = ValidCapabilities();
    invalid.Translation.RegionIdentifierBits = 25U;
    ASTERIA_EXPECT(runner, "a twenty-five-bit RID capability is rejected atomically",
                   InitializationIsRejected(invalid));

    Ia64RegionIdentifierAllocator minimum;
    ASTERIA_EXPECT(runner, "the architected eighteen-bit boundary initializes exactly",
                   minimum.Initialize(Foundation::UInt8{18U}) == Ia64RegionIdentifierAllocatorError::None,
                   minimum.GetSnapshot().NextRegionIdentifier == 0x2008U,
                   minimum.GetSnapshot().RepresentableLimitExclusive == 0x40000U,
                   minimum.GetSnapshot().RegionIdentifierBits == 18U);

    Ia64RegionIdentifierAllocator maximum;
    ASTERIA_EXPECT(runner, "the architected twenty-four-bit boundary initializes without wrap",
                   maximum.Initialize(Foundation::UInt8{24U}) == Ia64RegionIdentifierAllocatorError::None,
                   maximum.GetSnapshot().NextRegionIdentifier == 0x2008U,
                   maximum.GetSnapshot().RepresentableLimitExclusive == 0x100'0000U,
                   maximum.GetSnapshot().RegionIdentifierBits == 24U);
    const Ia64RegionIdentifierAllocatorSnapshot initialized = maximum.GetSnapshot();
    ASTERIA_EXPECT(runner, "a successful initialization cannot be replaced",
                   maximum.Initialize(Foundation::UInt8{18U}) == Ia64RegionIdentifierAllocatorError::AlreadyInitialized,
                   SnapshotsEqual(initialized, maximum.GetSnapshot()));

    bool everyReservedFieldRejected{true};
    invalid = ValidCapabilities();
    invalid.Translation.Reserved16 = 1U;
    everyReservedFieldRejected = everyReservedFieldRejected && InitializationIsRejected(invalid);
    invalid = ValidCapabilities();
    invalid.Translation.Reserved64 = 1U;
    everyReservedFieldRejected = everyReservedFieldRejected && InitializationIsRejected(invalid);
    invalid = ValidCapabilities();
    invalid.Reserved8 = 1U;
    everyReservedFieldRejected = everyReservedFieldRejected && InitializationIsRejected(invalid);
    invalid = ValidCapabilities();
    invalid.Reserved32 = 1U;
    everyReservedFieldRejected = everyReservedFieldRejected && InitializationIsRejected(invalid);
    ASTERIA_EXPECT(runner, "every reserved PAL capability field is rejected atomically", everyReservedFieldRejected);

    Ia64RegionIdentifierAllocator legacy;
    ASTERIA_EXPECT(runner, "the legacy PAL-record wrapper delegates its validated RID width",
                   legacy.Initialize(ValidCapabilities(20U)) == Ia64RegionIdentifierAllocatorError::None,
                   legacy.GetSnapshot().RegionIdentifierBits == 20U);
}

void TestExactReceiptsAndIndexBounds(TestContext& runner) noexcept
{
    Ia64RegionIdentifierAllocator allocator;
    const bool initialized = allocator.Initialize(Foundation::UInt8{18U}) == Ia64RegionIdentifierAllocatorError::None;
    const auto first = allocator.Allocate();
    const auto second = allocator.Allocate();
    bool exactFirstBlock = first.HasValue();
    bool exactSecondBlock = second.HasValue();
    for (Foundation::Size region{0U}; region < Ia64RegionIdentifierCountPerAddressSpace; ++region)
    {
        if (first.HasValue())
        {
            const auto identifier = first.GetValue().GetRegionIdentifier(region);
            exactFirstBlock =
                exactFirstBlock && identifier.HasValue() &&
                (identifier.GetValue() == Ia64FirstRuntimeRegionIdentifier + static_cast<Foundation::UInt32>(region));
        }
        if (second.HasValue())
        {
            const auto identifier = second.GetValue().GetRegionIdentifier(region);
            exactSecondBlock = exactSecondBlock && identifier.HasValue() &&
                               (identifier.GetValue() ==
                                Ia64FirstRuntimeRegionIdentifier +
                                    static_cast<Foundation::UInt32>(Ia64RegionIdentifierCountPerAddressSpace + region));
        }
    }
    ASTERIA_EXPECT(runner, "the first two allocations return exact consecutive aligned blocks", initialized,
                   exactFirstBlock, exactSecondBlock);

    Ia64RegionIdentifierAllocationReceipt firstReceipt{};
    if (first.HasValue())
    {
        firstReceipt = first.GetValue();
    }
    const auto upperBound = firstReceipt.GetRegionIdentifier(Ia64RegionIdentifierCountPerAddressSpace);
    const auto extremeBound = firstReceipt.GetRegionIdentifier(~Foundation::Size{0U});
    ASTERIA_EXPECT(runner, "receipt indexing rejects every value outside virtual regions zero through seven",
                   !upperBound.HasValue(),
                   upperBound.GetError() == Ia64RegionIdentifierAllocatorError::RegionIndexOutOfBounds,
                   !extremeBound.HasValue(),
                   extremeBound.GetError() == Ia64RegionIdentifierAllocatorError::RegionIndexOutOfBounds);

    const Ia64RegionIdentifierAllocationReceipt invalidReceipt{};
    const auto invalidLookup = invalidReceipt.GetRegionIdentifier(0U);
    ASTERIA_EXPECT(runner, "a default receipt cannot expose an unsafe identifier sentinel", !invalidLookup.HasValue(),
                   invalidLookup.GetError() == Ia64RegionIdentifierAllocatorError::InvalidReceipt);
}

void TestExhaustionAndNonReuse(TestContext& runner) noexcept
{
    constexpr Foundation::UInt32 LimitExclusive{0x40000U};
    constexpr Foundation::UInt32 BlockSize{8U};
    constexpr Foundation::UInt32 MaximumBlocks{(LimitExclusive - Ia64FirstRuntimeRegionIdentifier) / BlockSize};
    static_assert(MaximumBlocks == 31'743U);

    Ia64RegionIdentifierAllocator allocator;
    const bool initialized = allocator.Initialize(Foundation::UInt8{18U}) == Ia64RegionIdentifierAllocatorError::None;
    bool everyBlockIsExact{initialized};
    Foundation::UInt32 lastBase{0U};
    for (Foundation::UInt32 block{0U}; block < MaximumBlocks; ++block)
    {
        const auto allocated = allocator.Allocate();
        if (!allocated.HasValue())
        {
            everyBlockIsExact = false;
            continue;
        }
        const auto first = allocated.GetValue().GetRegionIdentifier(0U);
        const auto last = allocated.GetValue().GetRegionIdentifier(7U);
        if (!first.HasValue() || !last.HasValue())
        {
            everyBlockIsExact = false;
            continue;
        }
        const Foundation::UInt32 expectedBase = Ia64FirstRuntimeRegionIdentifier + (block * BlockSize);
        everyBlockIsExact =
            everyBlockIsExact && (first.GetValue() == expectedBase) && (last.GetValue() == expectedBase + 7U);
        lastBase = first.GetValue();
    }
    const Ia64RegionIdentifierAllocatorSnapshot exhausted = allocator.GetSnapshot();
    const auto firstFailure = allocator.Allocate();
    const Ia64RegionIdentifierAllocatorSnapshot afterFirstFailure = allocator.GetSnapshot();
    const auto secondFailure = allocator.Allocate();
    const Ia64RegionIdentifierAllocatorSnapshot afterSecondFailure = allocator.GetSnapshot();
    ASTERIA_EXPECT(runner, "every eighteen-bit block is issued once and exhaustion changes no state", everyBlockIsExact,
                   lastBase == LimitExclusive - BlockSize, exhausted.NextRegionIdentifier == LimitExclusive,
                   exhausted.AllocatedAddressSpaceCount == MaximumBlocks, !firstFailure.HasValue(),
                   firstFailure.GetError() == Ia64RegionIdentifierAllocatorError::Exhausted, !secondFailure.HasValue(),
                   secondFailure.GetError() == Ia64RegionIdentifierAllocatorError::Exhausted,
                   SnapshotsEqual(exhausted, afterFirstFailure), SnapshotsEqual(exhausted, afterSecondFailure));
}

void TestConcurrentAllocation(TestContext& runner) noexcept
{
    constexpr Foundation::Size WorkerCount{4U};
    constexpr Foundation::Size AllocationsPerWorker{256U};
    constexpr Foundation::Size TotalAllocations{WorkerCount * AllocationsPerWorker};
    Foundation::UInt32 bases[WorkerCount][AllocationsPerWorker]{};
    bool workerSucceeded[WorkerCount]{true, true, true, true};
    Ia64RegionIdentifierAllocator allocator;
    const bool initialized = allocator.Initialize(Foundation::UInt8{18U}) == Ia64RegionIdentifierAllocatorError::None;

    const auto worker = [&allocator, &bases, &workerSucceeded](const Foundation::Size workerIndex) noexcept
    {
        for (Foundation::Size index{0U}; index < AllocationsPerWorker; ++index)
        {
            const auto allocated = allocator.Allocate();
            if (!allocated.HasValue())
            {
                workerSucceeded[workerIndex] = false;
                continue;
            }
            const auto first = allocated.GetValue().GetRegionIdentifier(0U);
            const auto last = allocated.GetValue().GetRegionIdentifier(7U);
            if (!first.HasValue() || !last.HasValue() || (last.GetValue() != first.GetValue() + 7U))
            {
                workerSucceeded[workerIndex] = false;
                continue;
            }
            bases[workerIndex][index] = first.GetValue();
        }
    };

    std::thread firstWorker{worker, Foundation::Size{0U}};
    std::thread secondWorker{worker, Foundation::Size{1U}};
    std::thread thirdWorker{worker, Foundation::Size{2U}};
    std::thread fourthWorker{worker, Foundation::Size{3U}};
    firstWorker.join();
    secondWorker.join();
    thirdWorker.join();
    fourthWorker.join();

    bool seen[TotalAllocations]{};
    bool uniqueAndAligned{true};
    for (Foundation::Size workerIndex{0U}; workerIndex < WorkerCount; ++workerIndex)
    {
        uniqueAndAligned = uniqueAndAligned && workerSucceeded[workerIndex];
        for (Foundation::Size index{0U}; index < AllocationsPerWorker; ++index)
        {
            const Foundation::UInt32 base = bases[workerIndex][index];
            if ((base < Ia64FirstRuntimeRegionIdentifier) || ((base & 7U) != 0U))
            {
                uniqueAndAligned = false;
                continue;
            }
            const Foundation::Size block =
                static_cast<Foundation::Size>((base - Ia64FirstRuntimeRegionIdentifier) / 8U);
            if ((block >= TotalAllocations) || seen[block])
            {
                uniqueAndAligned = false;
                continue;
            }
            seen[block] = true;
        }
    }
    for (Foundation::Size block{0U}; block < TotalAllocations; ++block)
    {
        uniqueAndAligned = uniqueAndAligned && seen[block];
    }

    const Ia64RegionIdentifierAllocatorSnapshot snapshot = allocator.GetSnapshot();
    ASTERIA_EXPECT(runner, "concurrent allocation produces unique aligned blocks with exact accounting", initialized,
                   uniqueAndAligned, snapshot.State == Ia64RegionIdentifierAllocatorState::Ready,
                   snapshot.AllocatedAddressSpaceCount == TotalAllocations,
                   snapshot.NextRegionIdentifier ==
                       Ia64FirstRuntimeRegionIdentifier + static_cast<Foundation::UInt32>(TotalAllocations * 8U));
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64RegionIdentifierAllocatorTests"};
    Asteria::Test::TestInitialization(runner);
    Asteria::Test::TestExactReceiptsAndIndexBounds(runner);
    Asteria::Test::TestExhaustionAndNonReuse(runner);
    Asteria::Test::TestConcurrentAllocation(runner);
    return runner.Finish();
}
