// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64TranslationCachePurge.hpp"

extern "C" void AsteriaIa64PurgeTranslationCacheEntry(Asteria::Foundation::UInt64 address) noexcept;
extern "C" void AsteriaIa64SerializeTranslationCachePurge() noexcept;

// The target entry points are in Ia64TranslationCachePurge.S; a hosted build
// has no translation cache to purge.
#if !defined(__ia64__)
extern "C" void AsteriaIa64PurgeTranslationCacheEntry(const Asteria::Foundation::UInt64) noexcept
{
}

extern "C" void AsteriaIa64SerializeTranslationCachePurge() noexcept
{
}
#endif

namespace Asteria::Platform::Ia64::Memory
{
bool Ia64LocalTranslationCachePurge::Configure(const Kernel::Scheduler::CpuId processor,
                                               const PalTranslationCachePurgePlan& plan) noexcept
{
    if (m_configured || !processor.IsValid() || (plan.OuterCount == 0U) || (plan.InnerCount == 0U) ||
        (plan.OperationCount !=
         (static_cast<Foundation::UInt64>(plan.OuterCount) * static_cast<Foundation::UInt64>(plan.InnerCount))) ||
        (plan.OperationCount > MaximumTranslationCachePurgeOperations))
    {
        return false;
    }
    m_plan = plan;
    m_processor = processor;
    m_configured = true;
    return true;
}

bool Ia64LocalTranslationCachePurge::CanPurge(const Kernel::Scheduler::CpuId processor) const noexcept
{
    return m_configured && processor.IsValid() && (processor.GetValue() == m_processor.GetValue());
}

void Ia64LocalTranslationCachePurge::Purge(const Kernel::Scheduler::CpuId processor) noexcept
{
    if (!CanPurge(processor))
    {
        return;
    }
    // The loop PAL_PTCE_INFO prescribes, as the boot installer runs it; the
    // decoder proved that no address in it overflows.
    Foundation::UInt64 address{m_plan.BaseAddress};
    for (Foundation::UInt32 outer{0U}; outer < m_plan.OuterCount; ++outer)
    {
        for (Foundation::UInt32 inner{0U}; inner < m_plan.InnerCount; ++inner)
        {
            AsteriaIa64PurgeTranslationCacheEntry(address);
            m_lastAddress = address;
            ++m_operations;
            address += static_cast<Foundation::UInt64>(m_plan.InnerStride);
        }
        address += static_cast<Foundation::UInt64>(m_plan.OuterStride);
    }
    AsteriaIa64SerializeTranslationCachePurge();
    ++m_purges;
}

Ia64LocalTranslationCachePurgeSnapshot Ia64LocalTranslationCachePurge::GetSnapshot() const noexcept
{
    return Ia64LocalTranslationCachePurgeSnapshot{
        .Purges = m_purges,
        .Operations = m_operations,
        .LastAddress = m_lastAddress,
    };
}
} // namespace Asteria::Platform::Ia64::Memory
