// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_MEMORY_IA64_LOCAL_TLB_INVALIDATOR_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_MEMORY_IA64_LOCAL_TLB_INVALIDATOR_HPP_INCLUDED

#include "Hal/Memory/ILocalTlbInvalidator.hpp"
#include "Platform/Ia64/Memory/PalVirtualMemoryDecoder.hpp"

namespace Asteria::Platform::Ia64::Memory
{
enum class Ia64LocalTlbInvalidatorState : Foundation::UInt8
{
    Uninitialized,
    Ready,
};

enum class Ia64LocalTlbInvalidatorInitializationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidCapabilities,
};

struct Ia64LocalTlbInvalidatorSnapshot
{
    Ia64LocalTlbInvalidatorState State;
    Foundation::UInt8 ImplementedVirtualAddressMostSignificantBit;
    Foundation::UInt8 Reserved[6];
};

/// Performs bounded PTC.L invalidation on the calling IA-64 processor.
///
/// The retained address width comes from PAL_VM_SUMMARY decoded on this exact
/// processor. The owner invokes every operation on that processor at CPL0,
/// with PSR.vm clear, migration disabled, and processor-local re-entry
/// excluded. PTC.L purges dynamic instruction and data translations but not
/// translation-register entries; runtime mappings never replace a locked TR.
class Ia64LocalTlbInvalidator final : public Hal::Memory::ILocalTlbInvalidator
{
public:
    constexpr Ia64LocalTlbInvalidator() noexcept = default;

    [[nodiscard]] Ia64LocalTlbInvalidatorInitializationError
    Initialize(const PalVirtualMemoryCapabilities& capabilities) noexcept;

    [[nodiscard]] Hal::Memory::LocalTlbInvalidationError Invalidate(Kernel::Memory::VirtualAddress start,
                                                                    Kernel::Memory::PageCount pages) noexcept override;

    [[nodiscard]] constexpr Ia64LocalTlbInvalidatorSnapshot GetSnapshot() const noexcept
    {
        return Ia64LocalTlbInvalidatorSnapshot{
            .State = m_state,
            .ImplementedVirtualAddressMostSignificantBit = m_implementedVirtualAddressMostSignificantBit,
            .Reserved = {},
        };
    }

    ~Ia64LocalTlbInvalidator() noexcept = default;

    Ia64LocalTlbInvalidator(const Ia64LocalTlbInvalidator&) = delete;
    Ia64LocalTlbInvalidator& operator=(const Ia64LocalTlbInvalidator&) = delete;
    Ia64LocalTlbInvalidator(Ia64LocalTlbInvalidator&&) = delete;
    Ia64LocalTlbInvalidator& operator=(Ia64LocalTlbInvalidator&&) = delete;

private:
    Foundation::UInt8 m_implementedVirtualAddressMostSignificantBit{0U};
    Ia64LocalTlbInvalidatorState m_state{Ia64LocalTlbInvalidatorState::Uninitialized};
};

static_assert(sizeof(Ia64LocalTlbInvalidatorState) == 1U);
static_assert(sizeof(Ia64LocalTlbInvalidatorInitializationError) == 1U);
static_assert(sizeof(Ia64LocalTlbInvalidatorSnapshot) == 8U);
static_assert(alignof(Ia64LocalTlbInvalidatorSnapshot) == 1U);
} // namespace Asteria::Platform::Ia64::Memory

#endif // ASTERIA_PLATFORM_IA64_MEMORY_IA64_LOCAL_TLB_INVALIDATOR_HPP_INCLUDED
