// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_MEMORY_I_LOCAL_TLB_INVALIDATOR_HPP_INCLUDED
#define ASTERIA_HAL_MEMORY_I_LOCAL_TLB_INVALIDATOR_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Memory/MemoryTypes.hpp"

namespace Asteria::Hal::Memory
{
enum class LocalTlbInvalidationError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidRange,
    RangeTooLarge,
};

/// Invalidates one bounded virtual range on the calling processor only.
///
/// The shootdown transport invokes this primitive on each target processor,
/// then acknowledges the matching AddressSpace generation. The caller owns
/// processor-local serialization and prevents migration during the call.
class ILocalTlbInvalidator
{
public:
    [[nodiscard]] virtual LocalTlbInvalidationError Invalidate(Kernel::Memory::VirtualAddress start,
                                                               Kernel::Memory::PageCount pages) noexcept = 0;

protected:
    constexpr ILocalTlbInvalidator() noexcept = default;
    ~ILocalTlbInvalidator() noexcept = default;
};

static_assert(sizeof(LocalTlbInvalidationError) == 1U);
} // namespace Asteria::Hal::Memory

#endif // ASTERIA_HAL_MEMORY_I_LOCAL_TLB_INVALIDATOR_HPP_INCLUDED
