// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64RegionIdentifierAllocator.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::UInt8 MinimumRegionIdentifierBits{18U};
constexpr Foundation::UInt8 MaximumRegionIdentifierBits{24U};
constexpr Foundation::UInt32 SalRegionIdentifierBase{0x1000U};
constexpr Foundation::UInt32 MaximumSalRegionIdentifier{0x1003U};
constexpr Foundation::UInt32 BootstrapRegionIdentifierBase{0x2000U};
constexpr Foundation::UInt32 MaximumBootstrapRegionIdentifier{0x2007U};

static_assert(MaximumSalRegionIdentifier < BootstrapRegionIdentifierBase);
static_assert(SalRegionIdentifierBase <= MaximumSalRegionIdentifier);
static_assert(MaximumBootstrapRegionIdentifier < Ia64FirstRuntimeRegionIdentifier);
static_assert(MaximumBootstrapRegionIdentifier + 1U == Ia64FirstRuntimeRegionIdentifier);

[[nodiscard]] constexpr bool IsRegionIdentifierWidthValid(const Foundation::UInt8 regionIdentifierBits) noexcept
{
    return (regionIdentifierBits >= MinimumRegionIdentifierBits) &&
           (regionIdentifierBits <= MaximumRegionIdentifierBits);
}

[[nodiscard]] bool IsLegacyCapabilityRecordValid(const PalVirtualMemoryCapabilities& capabilities) noexcept
{
    const TranslationCapabilities& translation = capabilities.Translation;
    return IsRegionIdentifierWidthValid(translation.RegionIdentifierBits) && (translation.Reserved16 == 0U) &&
           (translation.Reserved64 == 0U) && (capabilities.Reserved8 == 0U) && (capabilities.Reserved32 == 0U);
}
} // namespace

Foundation::Result<Foundation::UInt32, Ia64RegionIdentifierAllocatorError>
Ia64RegionIdentifierAllocationReceipt::GetRegionIdentifier(const Foundation::Size regionIndex) const noexcept
{
    using ResultType = Foundation::Result<Foundation::UInt32, Ia64RegionIdentifierAllocatorError>;

    if (!IsValid())
    {
        return ResultType::Failure(Ia64RegionIdentifierAllocatorError::InvalidReceipt);
    }
    if (regionIndex >= Ia64RegionIdentifierCountPerAddressSpace)
    {
        return ResultType::Failure(Ia64RegionIdentifierAllocatorError::RegionIndexOutOfBounds);
    }
    return ResultType::Success(m_firstRegionIdentifier + static_cast<Foundation::UInt32>(regionIndex));
}

Ia64RegionIdentifierAllocatorError
Ia64RegionIdentifierAllocator::Initialize(const Foundation::UInt8 commonRegionIdentifierBits) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return InitializeLocked(commonRegionIdentifierBits);
}

Ia64RegionIdentifierAllocatorError
Ia64RegionIdentifierAllocator::InitializeLocked(const Foundation::UInt8 commonRegionIdentifierBits) noexcept
{
    if (m_state != Ia64RegionIdentifierAllocatorState::Uninitialized)
    {
        return Ia64RegionIdentifierAllocatorError::AlreadyInitialized;
    }
    if (!IsRegionIdentifierWidthValid(commonRegionIdentifierBits))
    {
        return Ia64RegionIdentifierAllocatorError::InvalidCapabilities;
    }

    const Foundation::UInt64 representableLimit = Foundation::UInt64{1U} << commonRegionIdentifierBits;
    constexpr Foundation::UInt64 firstCompleteBlockEndExclusive{
        static_cast<Foundation::UInt64>(Ia64FirstRuntimeRegionIdentifier) + Ia64RegionIdentifierCountPerAddressSpace};
    if (representableLimit < firstCompleteBlockEndExclusive)
    {
        return Ia64RegionIdentifierAllocatorError::InvalidCapabilities;
    }
    m_nextRegionIdentifier = Ia64FirstRuntimeRegionIdentifier;
    m_representableLimitExclusive = static_cast<Foundation::UInt32>(representableLimit);
    m_allocatedAddressSpaceCount = 0U;
    m_regionIdentifierBits = commonRegionIdentifierBits;
    m_state = Ia64RegionIdentifierAllocatorState::Ready;
    return Ia64RegionIdentifierAllocatorError::None;
}

Ia64RegionIdentifierAllocatorError
Ia64RegionIdentifierAllocator::Initialize(const PalVirtualMemoryCapabilities& capabilities) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != Ia64RegionIdentifierAllocatorState::Uninitialized)
    {
        return Ia64RegionIdentifierAllocatorError::AlreadyInitialized;
    }
    if (!IsLegacyCapabilityRecordValid(capabilities))
    {
        return Ia64RegionIdentifierAllocatorError::InvalidCapabilities;
    }
    return InitializeLocked(capabilities.Translation.RegionIdentifierBits);
}

Foundation::Result<Ia64RegionIdentifierAllocationReceipt, Ia64RegionIdentifierAllocatorError>
Ia64RegionIdentifierAllocator::Allocate() noexcept
{
    using ResultType = Foundation::Result<Ia64RegionIdentifierAllocationReceipt, Ia64RegionIdentifierAllocatorError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != Ia64RegionIdentifierAllocatorState::Ready)
    {
        return ResultType::Failure(Ia64RegionIdentifierAllocatorError::NotInitialized);
    }

    const Foundation::UInt32 blockSize = static_cast<Foundation::UInt32>(Ia64RegionIdentifierCountPerAddressSpace);
    if (m_nextRegionIdentifier > (m_representableLimitExclusive - blockSize))
    {
        return ResultType::Failure(Ia64RegionIdentifierAllocatorError::Exhausted);
    }

    const Foundation::UInt32 firstRegionIdentifier = m_nextRegionIdentifier;
    m_nextRegionIdentifier += blockSize;
    ++m_allocatedAddressSpaceCount;
    return ResultType::Success(Ia64RegionIdentifierAllocationReceipt{firstRegionIdentifier});
}

Ia64RegionIdentifierAllocatorSnapshot Ia64RegionIdentifierAllocator::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return Ia64RegionIdentifierAllocatorSnapshot{
        .NextRegionIdentifier = m_nextRegionIdentifier,
        .RepresentableLimitExclusive = m_representableLimitExclusive,
        .AllocatedAddressSpaceCount = m_allocatedAddressSpaceCount,
        .State = m_state,
        .RegionIdentifierBits = m_regionIdentifierBits,
        .Reserved = {0U, 0U},
    };
}
} // namespace Asteria::Platform::Ia64::Memory
