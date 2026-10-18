// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/Amd64FrameScrubber.hpp"

#include "Kernel/Memory/PhysicalPageAllocator.hpp"
#if defined(ASTERIA_TARGET_AMD64)
extern "C" Asteria::Foundation::UInt64
AsteriaAmd64ZeroScrubChunk(Asteria::Foundation::UInt64 start, Asteria::Foundation::UInt64 pages,
                           Asteria::Foundation::UInt64* leaf, Asteria::Foundation::UInt64 seed,
                           Asteria::Platform::Amd64::Memory::Amd64FrameScrubSample* samples) noexcept;
#endif

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
using Foundation::UInt64;

constexpr UInt64 PageBytes{4096U};
constexpr UInt64 AddressMask{0x000F'FFFF'FFFF'F000ULL};
constexpr UInt64 PresentBit{UInt64{1U} << 0U};
constexpr UInt64 WritableBit{UInt64{1U} << 1U};
constexpr UInt64 UserBit{UInt64{1U} << 2U};
constexpr UInt64 CacheBits{(UInt64{1U} << 3U) | (UInt64{1U} << 4U)};
constexpr UInt64 AccessedBit{UInt64{1U} << 5U};
constexpr UInt64 DirtyBit{UInt64{1U} << 6U};
constexpr UInt64 LargeOrPatBit{UInt64{1U} << 7U};
constexpr UInt64 GlobalBit{UInt64{1U} << 8U};
constexpr UInt64 ExecuteDisableBit{UInt64{1U} << 63U};
constexpr UInt64 InterruptFlag{UInt64{1U} << 9U};
} // namespace

bool FindAmd64ScrubSlotLeaf(const UInt64 root, const Foundation::Size arenaPages, const UInt64 virtualAddress,
                            const UInt64 slotPhysical, UInt64*& output) noexcept
{
    if ((root == 0U) || ((root & (PageBytes - 1U)) != 0U) || (arenaPages < 4U) || (arenaPages > 64U) ||
        (root >= (UInt64{1U} << 47U)) || (root > ((UInt64{1U} << 47U) - (arenaPages * PageBytes))) ||
        ((virtualAddress & (PageBytes - 1U)) != 0U) || ((slotPhysical & ~AddressMask) != 0U))
    {
        return false;
    }
    UInt64 table{root};
    constexpr Foundation::Size Shifts[4U]{39U, 30U, 21U, 12U};
    for (const Foundation::Size shift : Shifts)
    {
        if ((table < root) || (table >= (root + (arenaPages * PageBytes))))
        {
            return false;
        }
        auto* const entries = reinterpret_cast<UInt64*>(static_cast<Foundation::UIntPointer>(table));
        UInt64* const entry = &entries[(virtualAddress >> shift) & 511U];
        const UInt64 value = *entry;
        if (shift == 12U)
        {
            const UInt64 expected = slotPhysical | PresentBit | WritableBit | ExecuteDisableBit;
            if ((value & ~(AccessedBit | DirtyBit)) != expected)
            {
                return false;
            }
            output = entry;
            return true;
        }
        // Ancestors: present, writable, no large page. A user mapping in the
        // same region may have made them user-accessible; the leaf decides.
        if (((value & PresentBit) == 0U) || ((value & WritableBit) == 0U) || ((value & LargeOrPatBit) != 0U))
        {
            return false;
        }
        table = value & AddressMask;
    }
    return false;
}

#if defined(ASTERIA_TARGET_AMD64)
namespace
{
// Walk seed indices rather than form a slot virtual address in C++. Only
// the leaf that writes through the window forms that address.
[[nodiscard]] bool FindNativeSlotLeaf(const Amd64FrameScrubberConfiguration& configuration,
                                      const Amd64FrameScrubFacts& facts, UInt64*& output) noexcept
{
    const UInt64 root = facts.ActiveRootPhysical;
    const UInt64 indices[]{(Bootstrap::ScrubSlotVirtualAddress >> 39U) & 511U,
                           (Bootstrap::ScrubSlotVirtualAddress >> 30U) & 511U,
                           (Bootstrap::ScrubSlotVirtualAddress >> 21U) & 511U,
                           ((Bootstrap::ScrubSlotVirtualAddress >> 12U) & 511U) + facts.Processor};
    UInt64 table{root};
    for (Foundation::Size level{0U}; level < 4U; ++level)
    {
        if ((table < root) || ((table - root) >= configuration.ArenaPages * PageBytes))
        {
            return false;
        }
        auto* const entries = reinterpret_cast<UInt64*>(static_cast<Foundation::UIntPointer>(table));
        UInt64* const entry = &entries[indices[level]];
        const UInt64 value = *entry;
        if (level == 3U)
        {
            const UInt64 seed = (configuration.SlotPhysical + facts.Processor * PageBytes) | PresentBit | WritableBit |
                                ExecuteDisableBit;
            if ((value & ~(AccessedBit | DirtyBit)) != seed)
            {
                return false;
            }
            output = entry;
            return true;
        }
        if (((value & (PresentBit | WritableBit)) != (PresentBit | WritableBit)) || ((value & LargeOrPatBit) != 0U))
        {
            return false;
        }
        table = value & AddressMask;
    }
    return false;
}
} // namespace
#endif

Amd64FrameScrubDecision DecideAmd64FrameScrub(const Amd64FrameScrubberConfiguration& configuration,
                                              const Amd64FrameScrubFacts& facts, const UInt64 start, const UInt64 pages,
                                              UInt64*& leaf) noexcept
{
    const UInt64 poolStart = configuration.PoolPhysicalStart;
    const UInt64 poolEnd = poolStart + (configuration.PoolPages * PageBytes);
    if (configuration.Allocator != nullptr
            ? !configuration.Allocator->Contains(Kernel::Memory::PhysicalAddress{start}, pages)
            : ((pages == 0U) || ((start & (PageBytes - 1U)) != 0U) || (start < poolStart) || (start >= poolEnd) ||
               (pages > ((poolEnd - start) / PageBytes))))
    {
        return Amd64FrameScrubDecision::Fault;
    }
    // A known active root suffices. Residency keeps it alive; no
    // AddressSpace snapshot lock or exclusive residency gate.
    if (((facts.Flags & InterruptFlag) != 0U) || (facts.Processor >= configuration.Processors) ||
        (facts.Processor >= Amd64FrameScrubberProcessors) || (facts.ActiveRootPhysical == 0U) ||
        ((facts.ControlRegister3 & AddressMask) != facts.ActiveRootPhysical))
    {
        return Amd64FrameScrubDecision::Defer;
    }
#if defined(ASTERIA_TARGET_AMD64)
    const bool found = FindNativeSlotLeaf(configuration, facts, leaf);
#else
    const bool found = FindAmd64ScrubSlotLeaf(facts.ActiveRootPhysical, configuration.ArenaPages,
                                              configuration.SlotVirtualAddress + facts.Processor * PageBytes,
                                              configuration.SlotPhysical + facts.Processor * PageBytes, leaf);
#endif
    return found ? Amd64FrameScrubDecision::Open : Amd64FrameScrubDecision::Fault;
}

bool Amd64FrameScrubber::Configure(const Amd64FrameScrubberConfiguration& configuration) noexcept
{
    if (m_configured || (configuration.Allocator == nullptr && configuration.PoolPages == 0U) ||
        (configuration.Processors == 0U) || (configuration.Processors > Amd64FrameScrubberProcessors))
    {
        return false;
    }
    m_configuration = configuration;
    m_configured = true;
    return true;
}

bool Amd64FrameScrubber::PublishRoot(const Foundation::Size index, const Kernel::Memory::AddressSpace& space,
                                     const UInt64 rootPhysical) noexcept
{
    if ((index >= Amd64FrameScrubberRoots) || (rootPhysical == 0U) || ((rootPhysical & ~AddressMask) != 0U) ||
        (m_roots[index].RootPhysical.LoadAcquire() != 0U))
    {
        return false;
    }
    m_roots[index].Space.StoreRelease(static_cast<UInt64>(reinterpret_cast<Foundation::UIntPointer>(&space)));
    m_roots[index].RootPhysical.StoreRelease(rootPhysical);
    return true;
}

void Amd64FrameScrubber::WithdrawRoot(const Foundation::Size index) noexcept
{
    if (index < Amd64FrameScrubberRoots)
    {
        m_roots[index].RootPhysical.StoreRelease(0U);
        m_roots[index].Space.StoreRelease(0U);
    }
}

Amd64FrameScrubberSnapshot Amd64FrameScrubber::GetSnapshot() const noexcept
{
    Amd64FrameScrubberSnapshot snapshot{.ZeroedPages = 0U, .Deferrals = 0U};
    for (Foundation::Size processor{0U}; processor < Amd64FrameScrubberProcessors; ++processor)
    {
        snapshot.ZeroedPages += __atomic_load_n(&m_zeroedPages[processor], __ATOMIC_ACQUIRE);
        snapshot.Deferrals += __atomic_load_n(&m_deferrals[processor], __ATOMIC_ACQUIRE);
    }
    return snapshot;
}

// The active root is the one CR3 holds. It cannot be retired while this processor runs on it. Only
// the root table's publication is read; no AddressSpace lock is needed.
Amd64FrameScrubFacts Amd64FrameScrubber::Observe() const noexcept
{
    Amd64FrameScrubFacts facts{};
#if defined(ASTERIA_TARGET_AMD64)
    __asm__ __volatile__("mov %%cr3,%0; pushfq; popq %1; movq %%gs:16,%2"
                         : "=r"(facts.ControlRegister3), "=r"(facts.Flags), "=r"(facts.Processor)
                         :
                         : "memory");
#else
    facts.Flags = InterruptFlag;
#endif
    const UInt64 active = facts.ControlRegister3 & AddressMask;
    for (const RootEntry& root : m_roots)
    {
        if ((active == 0U) || (root.RootPhysical.LoadAcquire() != active))
        {
            continue;
        }
        facts.ActiveRootPhysical = active;
        facts.ActiveReady = true;
        break;
    }
    return facts;
}

void Amd64FrameScrubber::Watch(const Foundation::Size slot, const UInt64 physical) noexcept
{
    if (slot < Amd64FrameScrubberWatches)
    {
        m_samples[slot] = Amd64FrameScrubSample{.Physical = physical, .Sampled = 0U, .Words = {}};
    }
}

const Amd64FrameScrubSample& Amd64FrameScrubber::GetSample(const Foundation::Size slot) const noexcept
{
    return m_samples[(slot < Amd64FrameScrubberWatches) ? slot : 0U];
}

Kernel::Memory::FrameScrubError Amd64FrameScrubber::Zero(const Kernel::Memory::PhysicalAddress physicalStart,
                                                         const UInt64 pages) noexcept
{
    if (!m_configured)
    {
        return Kernel::Memory::FrameScrubError::Deferred;
    }
    const UInt64 start = physicalStart.GetValue();
    const UInt64 pool = m_configuration.PoolPhysicalStart;
    if (m_configuration.Allocator != nullptr ? !m_configuration.Allocator->Contains(physicalStart, pages)
                                             : ((pages == 0U) || (start < pool) || ((start % PageBytes) != 0U) ||
                                                (((start - pool) / PageBytes) >= m_configuration.PoolPages) ||
                                                (pages > (m_configuration.PoolPages - (start - pool) / PageBytes))))
    {
        return Kernel::Memory::FrameScrubError::Faulted;
    }
    for (UInt64 offset{0U}; offset < pages; offset += Kernel::Memory::PhysicalPageAllocator::MaximumScrubPages)
    {
        const UInt64 count = ((pages - offset) < Kernel::Memory::PhysicalPageAllocator::MaximumScrubPages)
                                 ? (pages - offset)
                                 : Kernel::Memory::PhysicalPageAllocator::MaximumScrubPages;
        const auto error = ZeroChunk(Kernel::Memory::PhysicalAddress{start + offset * PageBytes}, count);
        if (error != Kernel::Memory::FrameScrubError::None)
        {
            return error;
        }
    }
    return Kernel::Memory::FrameScrubError::None;
}

Kernel::Memory::FrameScrubError Amd64FrameScrubber::ZeroChunk(const Kernel::Memory::PhysicalAddress physicalStart,
                                                              const UInt64 pages) noexcept
{
    if (!m_configured)
    {
        return Kernel::Memory::FrameScrubError::Deferred;
    }
    UInt64* leaf{nullptr};
    const Amd64FrameScrubFacts facts = Observe();
    // An index beyond the slots counts its deferral nowhere.
    const Foundation::Size processor =
        (facts.Processor < Amd64FrameScrubberProcessors) ? static_cast<Foundation::Size>(facts.Processor) : 0U;
    switch (DecideAmd64FrameScrub(m_configuration, facts, physicalStart.GetValue(), pages, leaf))
    {
    case Amd64FrameScrubDecision::Open:
        break;
    case Amd64FrameScrubDecision::Defer:
        __atomic_fetch_add(&m_deferrals[processor], (facts.Processor < Amd64FrameScrubberProcessors) ? 1U : 0U,
                           __ATOMIC_RELAXED);
        return Kernel::Memory::FrameScrubError::Deferred;
    case Amd64FrameScrubDecision::Fault:
        return Kernel::Memory::FrameScrubError::Faulted;
    }
#if defined(ASTERIA_TARGET_AMD64)
    const UInt64 seed = *leaf & ~(AccessedBit | DirtyBit);
    if (AsteriaAmd64ZeroScrubChunk(physicalStart.GetValue(), pages, leaf, seed, m_samples) != 0U)
    {
        return Kernel::Memory::FrameScrubError::Faulted;
    }
    __atomic_fetch_add(&m_zeroedPages[processor], pages, __ATOMIC_RELAXED);
    return Kernel::Memory::FrameScrubError::None;
#else
    static_cast<void>(leaf);
    return Kernel::Memory::FrameScrubError::Deferred;
#endif
}
} // Namespace Asteria::Platform::Amd64::Memory
