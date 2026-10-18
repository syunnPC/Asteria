// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Memory/Amd64LocalTlbInvalidator.hpp"

#include "Platform/Amd64/Memory/RuntimePageTableMapper.hpp"

extern "C" void AsteriaAmd64InvalidatePage(Asteria::Foundation::UInt64 virtualAddress) noexcept;

namespace Asteria::Platform::Amd64::Memory
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 LowerCanonicalEnd{0x0000'8000'0000'0000ULL};
constexpr Foundation::UInt64 UpperCanonicalStart{0xFFFF'8000'0000'0000ULL};

[[nodiscard]] constexpr bool IsCanonical(const Foundation::UInt64 address) noexcept
{
    return (address < LowerCanonicalEnd) || (address >= UpperCanonicalStart);
}
} // namespace

Hal::Memory::LocalTlbInvalidationError
Amd64LocalTlbInvalidator::Invalidate(const Kernel::Memory::VirtualAddress start,
                                     const Kernel::Memory::PageCount pages) noexcept
{
    const Foundation::UInt64 startValue = start.GetValue();
    const Foundation::UInt64 pageCount = pages.GetValue();
    if ((startValue & 0xFFFU) != 0U || (pageCount == 0U))
    {
        return Hal::Memory::LocalTlbInvalidationError::InvalidRange;
    }
    if (pageCount > MaximumRuntimeMappingPages)
    {
        return Hal::Memory::LocalTlbInvalidationError::RangeTooLarge;
    }
    const Foundation::UInt64 sizeBytes = pageCount << 12U;
    if ((startValue > (MaximumUInt64 - sizeBytes)) || !IsCanonical(startValue) ||
        !IsCanonical(startValue + sizeBytes - 1U))
    {
        return Hal::Memory::LocalTlbInvalidationError::InvalidRange;
    }

    for (Foundation::UInt64 page{0U}; page < pageCount; ++page)
    {
        AsteriaAmd64InvalidatePage(startValue + (page << 12U));
    }
    return Hal::Memory::LocalTlbInvalidationError::None;
}
} // namespace Asteria::Platform::Amd64::Memory
