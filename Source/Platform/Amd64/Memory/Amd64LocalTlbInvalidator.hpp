// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_LOCAL_TLB_INVALIDATOR_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_LOCAL_TLB_INVALIDATOR_HPP_INCLUDED

#include "Hal/Memory/ILocalTlbInvalidator.hpp"

namespace Asteria::Platform::Amd64::Memory
{
/// Performs bounded INVLPG invalidation on the calling AMD64 processor.
class Amd64LocalTlbInvalidator final : public Hal::Memory::ILocalTlbInvalidator
{
public:
    [[nodiscard]] Hal::Memory::LocalTlbInvalidationError Invalidate(Kernel::Memory::VirtualAddress start,
                                                                    Kernel::Memory::PageCount pages) noexcept override;
};
} // namespace Asteria::Platform::Amd64::Memory

#endif // ASTERIA_PLATFORM_AMD64_MEMORY_AMD64_LOCAL_TLB_INVALIDATOR_HPP_INCLUDED
