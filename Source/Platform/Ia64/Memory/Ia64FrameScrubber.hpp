// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_FRAME_SCRUBBER_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_FRAME_SCRUBBER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/IFrameScrubber.hpp"
#include "Kernel/Memory/PhysicalPageAllocator.hpp"

namespace Asteria::Platform::Ia64::Memory
{
struct Ia64FrameScrubberSnapshot
{
    Foundation::UInt64 ZeroedPages;
    Foundation::UInt64 Faults;
};

/// The pages the scrubber samples before it zeroes them (Source/User/System/AhciPort.hpp evidence:
/// the late write a parked pin kept landed in its own Frame), and the words of each it keeps.
inline constexpr Foundation::Size Ia64FrameScrubberWatches{2U};
inline constexpr Foundation::Size Ia64FrameScrubberSampleWords{4U};

/// A watched page: its physical address, whether a scrub sampled it since
/// the watch began, and its first words as that scrub found them.
struct Ia64FrameScrubSample
{
    Foundation::UInt64 Physical;
    Foundation::UInt64 Sampled;
    Foundation::UInt64 Words[Ia64FrameScrubberSampleWords];
};

/// Zeroes dirty frame-pool runs in physical mode: with external interrupts masked, the RSE lazy and
/// data translation off, it stores zeros through the run's cacheable physical addresses (bit 63
/// clear), the attribute the pool's user mappings use. It needs no window and so never defers; a
/// run outside the configured pool, or a call before Configure, is Faulted and writes nothing.
/// Called with interrupts disabled, outside an ISR, by the ObjectManager.
class Ia64FrameScrubber final : public Kernel::Memory::IFrameScrubber
{
public:
    constexpr Ia64FrameScrubber() noexcept = default;

    Ia64FrameScrubber(const Ia64FrameScrubber&) = delete;
    Ia64FrameScrubber& operator=(const Ia64FrameScrubber&) = delete;

    /// Once only, before the first scrub: the frame pool's pages, page
    /// aligned, nonempty and below the uncacheable alias bit.
    [[nodiscard]] bool Configure(const Kernel::Memory::PhysicalPageAllocator& allocator) noexcept
    {
        if (m_configured)
        {
            return false;
        }
        m_allocator = &allocator;
        m_configured = true;
        return true;
    }
    [[nodiscard]] bool Configure(Foundation::UInt64 poolPhysicalStart, Foundation::UInt64 poolPages) noexcept;

    [[nodiscard]] Kernel::Memory::FrameScrubError Zero(Kernel::Memory::PhysicalAddress physicalStart,
                                                       Foundation::UInt64 pages) noexcept override;

    [[nodiscard]] Ia64FrameScrubberSnapshot GetSnapshot() const noexcept
    {
        return Ia64FrameScrubberSnapshot{.ZeroedPages = m_zeroedPages, .Faults = m_faults};
    }

    /// Watches the page at physical in slot (below Ia64FrameScrubberWatches),
    /// forgetting what slot held: the next scrub of a run holding that page
    /// keeps its first words, read through the same cacheable physical
    /// addresses, before it zeroes it. Same context as the scrubs.
    void Watch(Foundation::Size slot, Foundation::UInt64 physical) noexcept;
    [[nodiscard]] const Ia64FrameScrubSample& GetSample(Foundation::Size slot) const noexcept;

private:
    /// Keeps the first words of every watched page of the run of pages at
    /// start that no scrub sampled yet.
    void Sample(Foundation::UInt64 start, Foundation::UInt64 pages) noexcept;

    const Kernel::Memory::PhysicalPageAllocator* m_allocator{nullptr};
    Foundation::UInt64 m_poolStart{0U};
    Foundation::UInt64 m_poolPages{0U};
    Foundation::UInt64 m_zeroedPages{0U};
    Foundation::UInt64 m_faults{0U};
    Ia64FrameScrubSample m_samples[Ia64FrameScrubberWatches]{};
    bool m_configured{false};
};
} // Namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_FRAME_SCRUBBER_HPP_INCLUDED
