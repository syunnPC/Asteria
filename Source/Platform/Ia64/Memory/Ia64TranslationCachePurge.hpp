// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_TRANSLATION_CACHE_PURGE_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_TRANSLATION_CACHE_PURGE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Memory/PalTranslationCachePurgeDecoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
struct Ia64LocalTranslationCachePurgeSnapshot
{
    /// Purges run, ptc.e operations they issued and the last purge address.
    Foundation::UInt64 Purges;
    Foundation::UInt64 Operations;
    Foundation::UInt64 LastAddress;
};

/// The local translation-cache purge a runtime AddressSpace backend runs when
/// it activates on a processor whose cache may still hold translations the
/// space has since unmapped (Ia64RuntimeAddressSpacePageTable.md): switching
/// region registers keeps every translation-cache entry of the other RIDs.
///
/// It serves the one processor whose PAL_PTCE_INFO loop it retains (the
/// BSP's, which the boot translation installer validated): ptc.e over the
/// loop's addresses, then srlz.d and srlz.i. Translation misses during the
/// loop refill from the active ledger as usual, so it runs with interruption
/// collection on. The backend calls it directly (the native-preemption
/// corridor admits no indirect call).
///
/// Configure runs once, before any backend can ask for a purge; Purge runs on
/// the configured processor only, with external interrupts masked, which
/// serializes it.
class Ia64LocalTranslationCachePurge final
{
public:
    constexpr Ia64LocalTranslationCachePurge() noexcept = default;
    ~Ia64LocalTranslationCachePurge() noexcept = default;

    Ia64LocalTranslationCachePurge(const Ia64LocalTranslationCachePurge&) = delete;
    Ia64LocalTranslationCachePurge& operator=(const Ia64LocalTranslationCachePurge&) = delete;

    /// Retains processor's validated loop; false when already configured or
    /// when the plan is not one the decoder could have produced (an empty
    /// loop, more than MaximumTranslationCachePurgeOperations operations, or
    /// a count that disagrees with its factors).
    [[nodiscard]] bool Configure(Kernel::Scheduler::CpuId processor, const PalTranslationCachePurgePlan& plan) noexcept;

    /// Whether Purge can run on processor.
    [[nodiscard]] bool CanPurge(Kernel::Scheduler::CpuId processor) const noexcept;

    /// Purges processor's whole local translation cache (translation
    /// registers stay) and serializes; nothing unless CanPurge(processor).
    void Purge(Kernel::Scheduler::CpuId processor) noexcept;

    [[nodiscard]] Ia64LocalTranslationCachePurgeSnapshot GetSnapshot() const noexcept;

private:
    PalTranslationCachePurgePlan m_plan{};
    Kernel::Scheduler::CpuId m_processor{};
    Foundation::UInt64 m_purges{0U};
    Foundation::UInt64 m_operations{0U};
    Foundation::UInt64 m_lastAddress{0U};
    bool m_configured{false};
};
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_TRANSLATION_CACHE_PURGE_HPP_INCLUDED
