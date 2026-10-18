// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64FrameScrubber.hpp"
#include "Kernel/Memory/PhysicalPageAllocator.hpp"

extern "C" void AsteriaIa64ZeroPhysicalRun(Asteria::Foundation::UInt64 physicalStart,
                                           Asteria::Foundation::UInt64 bytes) noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadPhysicalQuad(Asteria::Foundation::UInt64 physical) noexcept;

// The target entry points are in Ia64FrameScrubber.S; the hosted build zeroes
// and reads the host memory the test hands out as its pool.
#if !defined(__ia64__)
extern "C" void AsteriaIa64ZeroPhysicalRun(const Asteria::Foundation::UInt64 physicalStart,
                                           const Asteria::Foundation::UInt64 bytes) noexcept
{
    auto* const words = reinterpret_cast<volatile Asteria::Foundation::UInt64*>(
        static_cast<Asteria::Foundation::UIntPointer>(physicalStart));
    for (Asteria::Foundation::UInt64 index{0U}; index < (bytes / sizeof(Asteria::Foundation::UInt64)); ++index)
    {
        words[index] = 0U;
    }
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadPhysicalQuad(const Asteria::Foundation::UInt64 physical) noexcept
{
    return *reinterpret_cast<const volatile Asteria::Foundation::UInt64*>(
        static_cast<Asteria::Foundation::UIntPointer>(physical));
}
#endif

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt64 PageBytes{Kernel::Memory::MappingPageSizeBytes};
// Physical addresses with bit 63 set are the uncacheable alias.
constexpr Foundation::UInt64 UncacheableAlias{Foundation::UInt64{1U} << 63U};
} // Namespace

bool Ia64FrameScrubber::Configure(const Foundation::UInt64 poolPhysicalStart,
                                  const Foundation::UInt64 poolPages) noexcept
{
    if (m_configured || (poolPages == 0U) || ((poolPhysicalStart % PageBytes) != 0U) ||
        (poolPhysicalStart >= UncacheableAlias) || (poolPages > ((UncacheableAlias - poolPhysicalStart) / PageBytes)))
    {
        return false;
    }
    m_poolStart = poolPhysicalStart;
    m_poolPages = poolPages;
    m_configured = true;
    return true;
}

Kernel::Memory::FrameScrubError Ia64FrameScrubber::Zero(const Kernel::Memory::PhysicalAddress physicalStart,
                                                        const Foundation::UInt64 pages) noexcept
{
    const Foundation::UInt64 start = physicalStart.GetValue();
    if (!m_configured ||
        (m_allocator != nullptr ? !m_allocator->Contains(physicalStart, pages)
                                : ((pages == 0U) || ((start % PageBytes) != 0U) || (start < m_poolStart) ||
                                   (((start - m_poolStart) / PageBytes) >= m_poolPages) ||
                                   (pages > (m_poolPages - ((start - m_poolStart) / PageBytes))))))
    {
        ++m_faults;
        return Kernel::Memory::FrameScrubError::Faulted;
    }
    Sample(start, pages);
    // No physical-mode masked visit exceeds 16 pages.
    for (Foundation::UInt64 offset{0U}; offset < pages;
         offset += Kernel::Memory::PhysicalPageAllocator::MaximumScrubPages)
    {
        const Foundation::UInt64 count = ((pages - offset) < Kernel::Memory::PhysicalPageAllocator::MaximumScrubPages)
                                             ? (pages - offset)
                                             : Kernel::Memory::PhysicalPageAllocator::MaximumScrubPages;
        AsteriaIa64ZeroPhysicalRun(start + offset * PageBytes, count * PageBytes);
    }
    m_zeroedPages += pages;
    return Kernel::Memory::FrameScrubError::None;
}

void Ia64FrameScrubber::Watch(const Foundation::Size slot, const Foundation::UInt64 physical) noexcept
{
    if (slot < Ia64FrameScrubberWatches)
    {
        m_samples[slot] = Ia64FrameScrubSample{.Physical = physical, .Sampled = 0U, .Words = {}};
    }
}

const Ia64FrameScrubSample& Ia64FrameScrubber::GetSample(const Foundation::Size slot) const noexcept
{
    return m_samples[(slot < Ia64FrameScrubberWatches) ? slot : 0U];
}

// Zero checked the run against the pool, so a watched page inside it is a
// pool page: its first words are read with the pool's cacheable attribute.
void Ia64FrameScrubber::Sample(const Foundation::UInt64 start, const Foundation::UInt64 pages) noexcept
{
    for (Ia64FrameScrubSample& sample : m_samples)
    {
        if ((sample.Physical == 0U) || (sample.Sampled != 0U) || ((sample.Physical % PageBytes) != 0U) ||
            (sample.Physical < start) || (((sample.Physical - start) / PageBytes) >= pages))
        {
            continue;
        }
        for (Foundation::Size index{0U}; index < Ia64FrameScrubberSampleWords; ++index)
        {
            sample.Words[index] = AsteriaIa64ReadPhysicalQuad(sample.Physical + (index * sizeof(Foundation::UInt64)));
        }
        sample.Sampled = 1U;
    }
}
} // Namespace Asteria::Platform::Ia64::Memory
