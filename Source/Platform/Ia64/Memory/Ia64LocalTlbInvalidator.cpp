// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64LocalTlbInvalidator.hpp"

extern "C" void AsteriaIa64PurgeLocalTranslation(Asteria::Foundation::UInt64 virtualAddress,
                                                 Asteria::Foundation::UInt64 encodedPageSize) noexcept;

extern "C" void AsteriaIa64SerializeLocalTranslationPurge() noexcept;

// The target entry points are in Ia64LocalTlbInvalidator.S.

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt8 MinimumImplementedVirtualAddressBit{50U};
constexpr Foundation::UInt8 MaximumImplementedVirtualAddressBit{60U};
constexpr Foundation::UInt8 RuntimePageShift{12U};
constexpr Foundation::UInt64 RuntimePageSizeBytes{Foundation::UInt64{1U} << RuntimePageShift};
constexpr Foundation::UInt64 RuntimePageMask{RuntimePageSizeBytes - 1U};
constexpr Foundation::UInt64 MaximumInvalidationPages{512U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 EncodedRuntimePageSize{static_cast<Foundation::UInt64>(RuntimePageShift) << 2U};

[[nodiscard]] constexpr bool IsVirtualAddressImplemented(const Foundation::UInt64 address,
                                                         const Foundation::UInt8 implementedMostSignificantBit) noexcept
{
    if (implementedMostSignificantBit == MaximumImplementedVirtualAddressBit)
    {
        return true;
    }

    const Foundation::UInt8 unimplementedBitCount =
        static_cast<Foundation::UInt8>(MaximumImplementedVirtualAddressBit - implementedMostSignificantBit);
    const Foundation::UInt64 mask = (Foundation::UInt64{1U} << unimplementedBitCount) - 1U;
    const Foundation::UInt64 actual = (address >> (implementedMostSignificantBit + 1U)) & mask;
    const Foundation::UInt64 expected =
        ((address & (Foundation::UInt64{1U} << implementedMostSignificantBit)) != 0U) ? mask : 0U;
    return actual == expected;
}
} // namespace

Ia64LocalTlbInvalidatorInitializationError
Ia64LocalTlbInvalidator::Initialize(const PalVirtualMemoryCapabilities& capabilities) noexcept
{
    if (m_state != Ia64LocalTlbInvalidatorState::Uninitialized)
    {
        return Ia64LocalTlbInvalidatorInitializationError::AlreadyInitialized;
    }

    const Foundation::UInt8 implementedMostSignificantBit =
        capabilities.Translation.ImplementedVirtualAddressMostSignificantBit;
    if ((implementedMostSignificantBit < MinimumImplementedVirtualAddressBit) ||
        (implementedMostSignificantBit > MaximumImplementedVirtualAddressBit) ||
        ((capabilities.PurgePageSizeMask & (Foundation::UInt64{1U} << RuntimePageShift)) == 0U))
    {
        return Ia64LocalTlbInvalidatorInitializationError::InvalidCapabilities;
    }

    m_implementedVirtualAddressMostSignificantBit = implementedMostSignificantBit;
    m_state = Ia64LocalTlbInvalidatorState::Ready;
    return Ia64LocalTlbInvalidatorInitializationError::None;
}

Hal::Memory::LocalTlbInvalidationError
Ia64LocalTlbInvalidator::Invalidate(const Kernel::Memory::VirtualAddress start,
                                    const Kernel::Memory::PageCount pages) noexcept
{
    if (m_state != Ia64LocalTlbInvalidatorState::Ready)
    {
        return Hal::Memory::LocalTlbInvalidationError::NotReady;
    }

    const Foundation::UInt64 startValue = start.GetValue();
    const Foundation::UInt64 pageCount = pages.GetValue();
    if (((startValue & RuntimePageMask) != 0U) || (pageCount == 0U))
    {
        return Hal::Memory::LocalTlbInvalidationError::InvalidRange;
    }
    if (pageCount > MaximumInvalidationPages)
    {
        return Hal::Memory::LocalTlbInvalidationError::RangeTooLarge;
    }

    const Foundation::UInt64 inclusiveSpan = (pageCount << RuntimePageShift) - 1U;
    if (startValue > (MaximumUInt64 - inclusiveSpan))
    {
        return Hal::Memory::LocalTlbInvalidationError::InvalidRange;
    }
    const Foundation::UInt64 lastAddress = startValue + inclusiveSpan;
    if (!IsVirtualAddressImplemented(startValue, m_implementedVirtualAddressMostSignificantBit) ||
        !IsVirtualAddressImplemented(lastAddress, m_implementedVirtualAddressMostSignificantBit))
    {
        return Hal::Memory::LocalTlbInvalidationError::InvalidRange;
    }

    for (Foundation::UInt64 page{0U}; page < pageCount; ++page)
    {
        AsteriaIa64PurgeLocalTranslation(startValue + (page << RuntimePageShift), EncodedRuntimePageSize);
    }
    AsteriaIa64SerializeLocalTranslationPurge();
    return Hal::Memory::LocalTlbInvalidationError::None;
}
} // namespace Asteria::Platform::Ia64::Memory
