// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Memory/Amd64FrameScrubber.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Platform::Amd64::Memory::Amd64FrameScrubDecision;
using Asteria::Platform::Amd64::Memory::Amd64FrameScrubFacts;
using Asteria::Platform::Amd64::Memory::Amd64FrameScrubberConfiguration;
using Asteria::Platform::Amd64::Memory::DecideAmd64FrameScrub;
using Asteria::Platform::Amd64::Memory::FindAmd64ScrubSlotLeaf;

constexpr UInt64 SlotVirtualAddress{0x7000'1000U};
constexpr UInt64 SlotPhysical{0x0123'4000U};
constexpr UInt64 SeedLeaf{SlotPhysical | 0x8000'0000'0000'0003ULL};
constexpr UInt64 PoolStart{0x0200'0000U};
alignas(4096U) UInt64 g_tables[4U][512U]{};

[[nodiscard]] UInt64 Root() noexcept
{
    return reinterpret_cast<Asteria::Foundation::UIntPointer>(g_tables);
}

// Processor 1's slot leaf: the next page's, in the same table.
constexpr UInt64 SecondSeedLeaf{(SlotPhysical + 0x1000U) | 0x8000'0000'0000'0003ULL};

// PML4[0] -> PDPT[1] -> PD[384] -> PT[1], the path of 0x70001000, and PT[2]
// for processor 1's slot at 0x70002000.
void Build() noexcept
{
    const UInt64 root = Root();
    g_tables[0U][0U] = (root + 4096U) | 3U;
    g_tables[1U][1U] = (root + 8192U) | 7U;
    g_tables[2U][384U] = (root + 12288U) | 3U;
    g_tables[3U][1U] = SeedLeaf;
    g_tables[3U][2U] = SecondSeedLeaf;
}

[[nodiscard]] Amd64FrameScrubberConfiguration Configuration() noexcept
{
    return Amd64FrameScrubberConfiguration{
        .PoolPhysicalStart = PoolStart,
        .PoolPages = 16U,
        .SlotVirtualAddress = SlotVirtualAddress,
        .SlotPhysical = SlotPhysical,
        .Processors = 2U,
        .ArenaPages = 4U,
    };
}

[[nodiscard]] Amd64FrameScrubFacts Ready() noexcept
{
    return Amd64FrameScrubFacts{
        .ControlRegister3 = Root(),
        .Flags = 0x2U,
        .Processor = 0U,
        .ActiveRootPhysical = Root(),
        .ActiveReady = true,
        .ResidentProcessorMask = 1U,
        .Reserved = {},
    };
}

void TestFinder(TestContext& runner) noexcept
{
    Build();
    UInt64* leaf{nullptr};
    const bool found = FindAmd64ScrubSlotLeaf(Root(), 4U, SlotVirtualAddress, SlotPhysical, leaf);
    g_tables[3U][1U] = SeedLeaf | 0x60U;
    UInt64* used{nullptr};
    const bool accessed = FindAmd64ScrubSlotLeaf(Root(), 4U, SlotVirtualAddress, SlotPhysical, used);
    ASTERIA_EXPECT(runner, "the slot leaf is found through a user-promoted ancestor, accessed and dirty or not", found,
                   leaf == &g_tables[3U][1U], accessed, used == leaf);

    constexpr UInt64 Rejected[]{
        SeedLeaf + 0x1000U,                   // Another page
        SeedLeaf | 0x4U,                      // User
        SeedLeaf & ~0x8000'0000'0000'0000ULL, // executable
        SeedLeaf | 0x8U,                      // write-through
        SeedLeaf | 0x10U,                     // uncached
        SeedLeaf | 0x80U,                     // PAT
        SeedLeaf | 0x100U,                    // global
        SeedLeaf & ~0x2ULL,                   // read-only
        0U,                                   // absent
    };
    bool rejected{true};
    for (const UInt64 value : Rejected)
    {
        g_tables[3U][1U] = value;
        UInt64* output{nullptr};
        rejected = rejected && !FindAmd64ScrubSlotLeaf(Root(), 4U, SlotVirtualAddress, SlotPhysical, output) &&
                   (output == nullptr);
    }
    g_tables[3U][1U] = SeedLeaf;
    g_tables[2U][384U] = (Root() + 12288U) | 0x83U;
    UInt64* large{nullptr};
    const bool largeRejected = !FindAmd64ScrubSlotLeaf(Root(), 4U, SlotVirtualAddress, SlotPhysical, large);
    g_tables[2U][384U] = (Root() + 4U * 4096U) | 3U;
    UInt64* outside{nullptr};
    const bool outsideRejected = !FindAmd64ScrubSlotLeaf(Root(), 4U, SlotVirtualAddress, SlotPhysical, outside);
    Build();
    UInt64* geometry{nullptr};
    ASTERIA_EXPECT(
        runner, "a leaf that is not the slot's seed, or a malformed path or arena, is refused", rejected, largeRejected,
        outsideRejected, !FindAmd64ScrubSlotLeaf(Root() + 1U, 4U, SlotVirtualAddress, SlotPhysical, geometry),
        !FindAmd64ScrubSlotLeaf(Root(), 3U, SlotVirtualAddress, SlotPhysical, geometry),
        !FindAmd64ScrubSlotLeaf(Root(), 4U, SlotVirtualAddress + 1U, SlotPhysical, geometry), geometry == nullptr);
}

void TestDecision(TestContext& runner) noexcept
{
    Build();
    const Amd64FrameScrubberConfiguration configuration = Configuration();
    const auto decide = [&](const Amd64FrameScrubFacts& facts, const UInt64 start, const UInt64 pages) noexcept
    {
        UInt64* leaf{nullptr};
        return DecideAmd64FrameScrub(configuration, facts, start, pages, leaf);
    };
    UInt64* opened{nullptr};
    const Amd64FrameScrubDecision open = DecideAmd64FrameScrub(configuration, Ready(), PoolStart, 16U, opened);
    ASTERIA_EXPECT(runner, "the window opens on CPU0 with interrupts off in a Ready root resident there alone",
                   open == Amd64FrameScrubDecision::Open, opened == &g_tables[3U][1U]);

    // Memory bootstrap: each processor scrubs through its own
    // slot.
    Amd64FrameScrubFacts second = Ready();
    second.Processor = 1U;
    second.ResidentProcessorMask = 2U;
    UInt64* secondLeaf{nullptr};
    const Amd64FrameScrubDecision secondOpen = DecideAmd64FrameScrub(configuration, second, PoolStart, 1U, secondLeaf);
    g_tables[3U][2U] = SeedLeaf;
    const Amd64FrameScrubDecision foreignSlot = decide(second, PoolStart, 1U);
    Build();
    ASTERIA_EXPECT(runner, "processor 1 opens its own slot's leaf, and a leaf of another slot faults",
                   secondOpen == Amd64FrameScrubDecision::Open, secondLeaf == &g_tables[3U][2U],
                   foreignSlot == Amd64FrameScrubDecision::Fault);

    Amd64FrameScrubFacts interrupts = Ready();
    interrupts.Flags |= 0x200U;
    // Resident on CPU0 alone, but scrubbing on processor 1: still deferred
    // until residency is dropped; and a processor without a slot.
    Amd64FrameScrubFacts otherProcessor = Ready();
    otherProcessor.Processor = 1U;
    Amd64FrameScrubFacts slotless = Ready();
    slotless.Processor = 2U;
    slotless.ResidentProcessorMask = 4U;
    Amd64FrameScrubFacts unknownRoot = Ready();
    unknownRoot.ActiveRootPhysical = 0U;
    Amd64FrameScrubFacts otherRoot = Ready();
    otherRoot.ControlRegister3 = Root() + 0x10000U;
    Amd64FrameScrubFacts pending = Ready();
    pending.ActiveReady = false;
    Amd64FrameScrubFacts shared = Ready();
    shared.ResidentProcessorMask = 0x3U;
    ASTERIA_EXPECT(runner, "any other processor state defers the scrub",
                   decide(interrupts, PoolStart, 1U) == Amd64FrameScrubDecision::Defer,
                   decide(otherProcessor, PoolStart, 1U) == Amd64FrameScrubDecision::Open,
                   decide(slotless, PoolStart, 1U) == Amd64FrameScrubDecision::Defer,
                   decide(unknownRoot, PoolStart, 1U) == Amd64FrameScrubDecision::Defer,
                   decide(otherRoot, PoolStart, 1U) == Amd64FrameScrubDecision::Defer,
                   decide(pending, PoolStart, 1U) == Amd64FrameScrubDecision::Open,
                   decide(shared, PoolStart, 1U) == Amd64FrameScrubDecision::Open);

    g_tables[3U][1U] = SeedLeaf + 0x1000U;
    const Amd64FrameScrubDecision corrupt = decide(Ready(), PoolStart, 1U);
    Build();
    ASTERIA_EXPECT(runner, "a run outside the frame pool, or a slot leaf off its seed, faults",
                   decide(Ready(), PoolStart - 0x1000U, 1U) == Amd64FrameScrubDecision::Fault,
                   decide(Ready(), PoolStart + 0x800U, 1U) == Amd64FrameScrubDecision::Fault,
                   decide(Ready(), PoolStart + 15U * 0x1000U, 2U) == Amd64FrameScrubDecision::Fault,
                   decide(Ready(), PoolStart, 0U) == Amd64FrameScrubDecision::Fault,
                   decide(interrupts, PoolStart + 16U * 0x1000U, 1U) == Amd64FrameScrubDecision::Fault,
                   corrupt == Amd64FrameScrubDecision::Fault);

    Asteria::Platform::Amd64::Memory::Amd64FrameScrubber scrubber;
    Amd64FrameScrubberConfiguration noSlots = configuration;
    noSlots.Processors = 0U;
    Amd64FrameScrubberConfiguration tooMany = configuration;
    tooMany.Processors = Asteria::Platform::Amd64::Memory::Amd64FrameScrubberProcessors + 1U;
    ASTERIA_EXPECT(runner, "an unconfigured scrubber defers, and configuration needs a slot per processor",
                   scrubber.Zero(Asteria::Kernel::Memory::PhysicalAddress{PoolStart}, 1U) ==
                       Asteria::Kernel::Memory::FrameScrubError::Deferred,
                   !scrubber.Configure(noSlots), !scrubber.Configure(tooMany), scrubber.Configure(configuration),
                   !scrubber.Configure(configuration));
}

// The roots table: a root is published at an index once, withdrawn before
// its space is retired, and published again after.
void TestRoots(TestContext& runner) noexcept
{
    using Asteria::Platform::Amd64::Memory::Amd64FrameScrubberRoots;
    Asteria::Platform::Amd64::Memory::Amd64FrameScrubber scrubber;
    Asteria::Kernel::Memory::AddressSpace space;
    const bool configured = scrubber.Configure(Configuration());
    const bool published = scrubber.PublishRoot(Amd64FrameScrubberRoots - 1U, space, Root());
    const bool twice = scrubber.PublishRoot(Amd64FrameScrubberRoots - 1U, space, Root());
    scrubber.WithdrawRoot(Amd64FrameScrubberRoots - 1U);
    const bool again = scrubber.PublishRoot(Amd64FrameScrubberRoots - 1U, space, Root());
    ASTERIA_EXPECT(runner, "a root is published once per index, and again after it is withdrawn", configured, published,
                   !twice, again, !scrubber.PublishRoot(Amd64FrameScrubberRoots, space, Root()),
                   !scrubber.PublishRoot(0U, space, 0U), !scrubber.PublishRoot(0U, space, Root() + 1U));
}
} // namespace

int main()
{
    TestContext runner{"Amd64FrameScrubberTests"};
    TestFinder(runner);
    TestDecision(runner);
    TestRoots(runner);
    return runner.Finish();
}
