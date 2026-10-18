// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_FRAME_SCRUBBER_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_FRAME_SCRUBBER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/AddressSpacePool.hpp"
#include "Kernel/Memory/IFrameScrubber.hpp"
#include "Kernel/Memory/PhysicalPageAllocator.hpp"
#include "Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Platform::Amd64::Memory
{
/// The processors with a scrub slot: one per processor the runtime pool supports.
inline constexpr Foundation::Size Amd64FrameScrubberProcessors{Bootstrap::ProcessorStackSlots};
/// The roots the scrubber can find active: the runtime pool's four boot
/// roots and one per AddressSpace pool slot.
inline constexpr Foundation::Size Amd64FrameScrubberRoots{Bootstrap::BootRootCount +
                                                          Kernel::Memory::AddressSpacePool::Capacity};

/// What the boot runtime gives the scrubber: the frame pool it may write,
/// processor 0's scrub slot (the slot leaf's address and slot page; processor
/// p's are p pages further), the processors with a slot, and the size of
/// every root's page-table arena. The roots come later, through PublishRoot.
struct Amd64FrameScrubberConfiguration
{
    Foundation::UInt64 PoolPhysicalStart;
    Foundation::UInt64 PoolPages;
    Foundation::UInt64 SlotVirtualAddress;
    Foundation::UInt64 SlotPhysical;
    Foundation::Size Processors;
    Foundation::Size ArenaPages;
    const Kernel::Memory::PhysicalPageAllocator* Allocator{nullptr};
};

/// What the processor and its active AddressSpace show when a scrub begins.
struct Amd64FrameScrubFacts
{
    Foundation::UInt64 ControlRegister3;
    Foundation::UInt64 Flags;
    /// The processor's index from its per-CPU block (%gs:16).
    Foundation::UInt64 Processor;
    /// The root table of the active AddressSpace, or zero if it is none of
    /// the published roots.
    Foundation::UInt64 ActiveRootPhysical;
    bool ActiveReady;
    Foundation::ProcessorMask ResidentProcessorMask;
    Foundation::UInt8 Reserved[6];
};

enum class Amd64FrameScrubDecision : Foundation::UInt8
{
    /// Scrub preconditions hold for this processor's slot; the window opens.
    Open,
    /// Not now: the run stays dirty.
    Defer,
    /// A run outside the frame pool, or a slot leaf that is not its seed.
    Fault,
};

/// The scrub slot's leaf for virtualAddress under root, whose page tables lie
/// in the identity-mapped arena of arenaPages pages from root: present
/// writable ancestors without large pages, and a present supervisor
/// writable no-execute write-back non-global leaf of slotPhysical (its
/// accessed and dirty bits aside). Failure leaves output unchanged.
[[nodiscard]] bool FindAmd64ScrubSlotLeaf(Foundation::UInt64 root, Foundation::Size arenaPages,
                                          Foundation::UInt64 virtualAddress, Foundation::UInt64 slotPhysical,
                                          Foundation::UInt64*& output) noexcept;

/// Decides whether the run [start, start + pages) may be zeroed through the slot of facts.Processor
/// now: interrupts off, in a known active root, through the processor's own slot. On
/// Open, leaf is that slot's leaf in the active root.
[[nodiscard]] Amd64FrameScrubDecision DecideAmd64FrameScrub(const Amd64FrameScrubberConfiguration& configuration,
                                                            const Amd64FrameScrubFacts& facts, Foundation::UInt64 start,
                                                            Foundation::UInt64 pages,
                                                            Foundation::UInt64*& leaf) noexcept;

struct Amd64FrameScrubberSnapshot
{
    Foundation::UInt64 ZeroedPages;
    Foundation::UInt64 Deferrals;
};

/// The pages the scrubber samples before it zeroes them (Source/User/System/AhciPort.hpp evidence:
/// the late write a parked pin kept landed in its own Frame), and the bytes of each it keeps.
inline constexpr Foundation::Size Amd64FrameScrubberWatches{2U};
inline constexpr Foundation::Size Amd64FrameScrubberSampleWords{4U};

/// A watched page: its physical address, whether a scrub sampled it since
/// the watch began, and its first words as that scrub found them.
struct Amd64FrameScrubSample
{
    Foundation::UInt64 Physical;
    Foundation::UInt64 Sampled;
    Foundation::UInt64 Words[Amd64FrameScrubberSampleWords];
};

/// Zeroes dirty frame-pool runs on whichever processor calls it, through that processor's own scrub
/// slot (its index from %gs:16, never an argument): repoints the active root's slot leaf at each
/// page, invalidates it locally, zeroes the page, and restores the leaf to the slot page. It finds
/// the active root in a table that AddressSpace creation and retirement update (PublishRoot,
/// WithdrawRoot), published with release and read with acquire. Called with interrupts disabled,
/// outside an ISR, by the ObjectManager.
class Amd64FrameScrubber final : public Kernel::Memory::IFrameScrubber
{
public:
    constexpr Amd64FrameScrubber() noexcept = default;

    Amd64FrameScrubber(const Amd64FrameScrubber&) = delete;
    Amd64FrameScrubber& operator=(const Amd64FrameScrubber&) = delete;

    /// Once only, before the first scrub.
    [[nodiscard]] bool Configure(const Amd64FrameScrubberConfiguration& configuration) noexcept;

    /// Makes root table rootPhysical, space's, findable at index (below
    /// Amd64FrameScrubberRoots): its space first, then its root with release.
    /// Each created root holds every slot leaf from its seed. The caller owns
    /// index (a boot root's, or its AddressSpace pool slot's) and publishes a
    /// space only once it is Ready.
    [[nodiscard]] bool PublishRoot(Foundation::Size index, const Kernel::Memory::AddressSpace& space,
                                   Foundation::UInt64 rootPhysical) noexcept;

    /// Forgets index's root before its space is retired: its root first,
    /// with release. A root is retired only quiesced, so no processor holds
    /// it in CR3 and no scrub that found it is still using its space.
    void WithdrawRoot(Foundation::Size index) noexcept;

    [[nodiscard]] Kernel::Memory::FrameScrubError Zero(Kernel::Memory::PhysicalAddress physicalStart,
                                                       Foundation::UInt64 pages) noexcept override;

    /// Every processor's zeroed pages and deferrals.
    [[nodiscard]] Amd64FrameScrubberSnapshot GetSnapshot() const noexcept;

    /// Watches the page at physical in slot (below Amd64FrameScrubberWatches),
    /// forgetting what slot held: the next scrub of that page keeps its
    /// first words before it zeroes it. CPU0, as the scrubs.
    void Watch(Foundation::Size slot, Foundation::UInt64 physical) noexcept;
    [[nodiscard]] const Amd64FrameScrubSample& GetSample(Foundation::Size slot) const noexcept;

private:
    struct RootEntry
    {
        // The root table's physical address, zero for none: the published
        // word. Space holds the AddressSpace's address while it is nonzero.
        Platform::Synchronization::AtomicUInt64 RootPhysical;
        Platform::Synchronization::AtomicUInt64 Space;
    };

    [[nodiscard, gnu::noinline]] Kernel::Memory::FrameScrubError ZeroChunk(Kernel::Memory::PhysicalAddress start,
                                                                           Foundation::UInt64 pages) noexcept;
    [[nodiscard, gnu::noinline]] Amd64FrameScrubFacts Observe() const noexcept;
    Amd64FrameScrubberConfiguration m_configuration{};
    RootEntry m_roots[Amd64FrameScrubberRoots]{};
    // Each processor's counts, which only that processor writes.
    Foundation::UInt64 m_zeroedPages[Amd64FrameScrubberProcessors]{};
    Foundation::UInt64 m_deferrals[Amd64FrameScrubberProcessors]{};
    Amd64FrameScrubSample m_samples[Amd64FrameScrubberWatches]{};
    bool m_configured{false};
};

static_assert(sizeof(Amd64FrameScrubFacts) == 56U);
} // Namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_FRAME_SCRUBBER_HPP_INCLUDED
