// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/DirectUserMemoryAccess.hpp"

namespace Asteria::Kernel::Memory
{
namespace
{
[[nodiscard]] constexpr Foundation::UInt64 Minimum(const Foundation::UInt64 left,
                                                   const Foundation::UInt64 right) noexcept
{
    return left < right ? left : right;
}
} // namespace

DirectUserMemoryAccessError DirectUserMemoryAccess::Initialize(const AddressSpaceId addressSpace,
                                                               const DirectUserMemoryMapping* const mappings,
                                                               const Foundation::Size mappingCount,
                                                               const AddressSpace* const lifetime) noexcept
{
    if (m_ready)
    {
        return DirectUserMemoryAccessError::AlreadyInitialized;
    }
    if (!addressSpace.IsValid() || (lifetime != nullptr && !lifetime->GetIdentity().IsEqualTo(addressSpace)))
    {
        return DirectUserMemoryAccessError::InvalidAddressSpace;
    }
    if ((mappings == nullptr) || (mappingCount == 0U))
    {
        return DirectUserMemoryAccessError::InvalidMappingList;
    }
    if (mappingCount > MaximumDirectUserMemoryMappings)
    {
        return DirectUserMemoryAccessError::MappingCapacityExceeded;
    }

    Foundation::UInt64 previousStart{0U};
    Foundation::UInt64 previousEnd{0U};
    for (Foundation::Size index{0U}; index < mappingCount; ++index)
    {
        const DirectUserMemoryMapping& mapping = mappings[index];
        if (!IsMappingValid(mapping))
        {
            return DirectUserMemoryAccessError::InvalidMapping;
        }

        const Foundation::UInt64 start = mapping.UserStart.GetValue();
        if ((index != 0U) && (start < previousStart))
        {
            return DirectUserMemoryAccessError::MappingsNotOrdered;
        }
        if ((index != 0U) && (start < previousEnd))
        {
            return DirectUserMemoryAccessError::MappingsOverlap;
        }
        previousStart = start;
        previousEnd = start + GetMappingSizeBytes(mapping);
    }

    for (Foundation::Size index{0U}; index < mappingCount; ++index)
    {
        m_mappings[index] = mappings[index];
    }
    m_mappingCount = mappingCount;
    m_lifetime = lifetime;
    m_addressSpace = addressSpace;
    m_ready = true;
    return DirectUserMemoryAccessError::None;
}

AddressSpaceId DirectUserMemoryAccess::GetAddressSpaceId() const noexcept
{
    return m_ready ? m_addressSpace : AddressSpaceId{};
}

bool DirectUserMemoryAccess::IsReady() const noexcept
{
    return m_ready;
}

UserMemoryAccessError DirectUserMemoryAccess::CopyFromUserValidated(const UserMemoryRange source,
                                                                    const WritableKernelBuffer destination) noexcept
{
    if (!IsRangeAccessible(source, false))
    {
        return UserMemoryAccessError::AccessDenied;
    }
    UserMappingUse use{};
    if (m_lifetime != nullptr)
    {
        const auto pinned = m_lifetime->PinUserMapping(source.GetStartAddress(), source.GetSizeBytes(), false, true);
        if (!pinned.HasValue())
        {
            return UserMemoryAccessError::AccessDenied;
        }
        use = pinned.GetValue();
    }
    CopyFromMappings(source, destination);
    if (m_lifetime != nullptr)
    {
        m_lifetime->ReleaseUserMapping(use);
    }
    return UserMemoryAccessError::None;
}

Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>
DirectUserMemoryAccess::PrepareCopyToUserValidated(const UserMemoryRange destination) noexcept
{
    using ResultType = Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>;

    if (!IsRangeAccessible(destination, true))
    {
        return ResultType::Failure(UserMemoryAccessError::AccessDenied);
    }
    if (m_hasActiveReservation)
    {
        return ResultType::Failure(UserMemoryAccessError::ReservationExhausted);
    }

    Foundation::UInt64 generation{0U};
    if (!DirectUserMemoryReservationGenerationPolicy::TryAdvance(m_lastGeneration, generation))
    {
        return ResultType::Failure(UserMemoryAccessError::ReservationExhausted);
    }

    if (m_lifetime != nullptr)
    {
        const auto pinned =
            m_lifetime->PinUserMapping(destination.GetStartAddress(), destination.GetSizeBytes(), true, true);
        if (!pinned.HasValue())
        {
            return ResultType::Failure(UserMemoryAccessError::AccessDenied);
        }
        m_activeUse = pinned.GetValue();
    }
    m_lastGeneration = generation;
    m_activeGeneration = generation;
    m_activeRange = destination;
    m_hasActiveReservation = true;
    return ResultType::Success(MakeWriteReservation(destination, generation));
}

UserMemoryAccessError
DirectUserMemoryAccess::CancelCopyToUserValidated(const UserMemoryWriteReservation reservation) noexcept
{
    if (!IsCurrentReservation(reservation))
    {
        return UserMemoryAccessError::StaleReservation;
    }
    if (m_lifetime != nullptr)
    {
        m_lifetime->ReleaseUserMapping(m_activeUse);
    }
    m_activeUse = {};
    m_hasActiveReservation = false;
    m_activeRange = UserMemoryRange{};
    m_activeGeneration = 0U;
    return UserMemoryAccessError::None;
}

UserMemoryAccessError DirectUserMemoryAccess::CommitCopyToUserValidated(const UserMemoryWriteReservation reservation,
                                                                        const ReadOnlyKernelBuffer source) noexcept
{
    if (!IsCurrentReservation(reservation))
    {
        return UserMemoryAccessError::StaleReservation;
    }

    CopyToMappings(reservation.GetRange(), source);
    if (m_lifetime != nullptr)
    {
        m_lifetime->ReleaseUserMapping(m_activeUse);
    }
    m_activeUse = {};
    m_hasActiveReservation = false;
    m_activeRange = UserMemoryRange{};
    m_activeGeneration = 0U;
    return UserMemoryAccessError::None;
}

bool DirectUserMemoryAccess::IsMappingValid(const DirectUserMemoryMapping& mapping) noexcept
{
    const Foundation::UInt64 pageCount = mapping.Pages.GetValue();
    const Foundation::UInt64 maximum{~Foundation::UInt64{0U}};
    if ((mapping.UserStart.GetValue() == 0U) || (mapping.DirectStart == nullptr) || (pageCount == 0U) ||
        (pageCount > (maximum / MappingPageSizeBytes)) || !mapping.Permissions.HasOnlyKnownBits() ||
        !mapping.Permissions.Contains(PagePermission::Read) || !mapping.Permissions.Contains(PagePermission::User) ||
        (mapping.Kind != FrameKind::Ram))
    {
        return false;
    }

    const Foundation::UInt64 sizeBytes = pageCount * MappingPageSizeBytes;
    const Foundation::UInt64 userStart = mapping.UserStart.GetValue();
    const Foundation::UIntPointer directStart = reinterpret_cast<Foundation::UIntPointer>(mapping.DirectStart);
    const Foundation::UIntPointer pointerMaximum{~Foundation::UIntPointer{0U}};
    const Foundation::UIntPointer pointerSize = static_cast<Foundation::UIntPointer>(sizeBytes);
    return ((userStart % MappingPageSizeBytes) == 0U) && ((directStart % MappingPageSizeBytes) == 0U) &&
           (userStart <= (maximum - sizeBytes)) && (directStart <= (pointerMaximum - pointerSize));
}

Foundation::UInt64 DirectUserMemoryAccess::GetMappingSizeBytes(const DirectUserMemoryMapping& mapping) noexcept
{
    return mapping.Pages.GetValue() * MappingPageSizeBytes;
}

bool DirectUserMemoryAccess::IsRangeAccessible(const UserMemoryRange range, const bool requireWrite) const noexcept
{
    if (range.IsEmpty())
    {
        return true;
    }

    Foundation::UInt64 cursor = range.GetStartAddress();
    const Foundation::UInt64 rangeEnd = range.GetEndAddress();
    for (Foundation::Size index{0U}; (index < m_mappingCount) && (cursor < rangeEnd); ++index)
    {
        const DirectUserMemoryMapping& mapping = m_mappings[index];
        const Foundation::UInt64 mappingStart = mapping.UserStart.GetValue();
        const Foundation::UInt64 mappingEnd = mappingStart + GetMappingSizeBytes(mapping);
        if (cursor < mappingStart)
        {
            return false;
        }
        if (cursor >= mappingEnd)
        {
            continue;
        }
        if (requireWrite && !mapping.Permissions.Contains(PagePermission::Write))
        {
            return false;
        }
        cursor = Minimum(mappingEnd, rangeEnd);
    }
    return cursor == rangeEnd;
}

bool DirectUserMemoryAccess::IsCurrentReservation(const UserMemoryWriteReservation reservation) const noexcept
{
    return m_hasActiveReservation && reservation.GetRange().IsEqualTo(m_activeRange) &&
           (reservation.GetGeneration() == m_activeGeneration);
}

void DirectUserMemoryAccess::CopyFromMappings(const UserMemoryRange source,
                                              const WritableKernelBuffer destination) const noexcept
{
    Foundation::UInt64 cursor = source.GetStartAddress();
    const Foundation::UInt64 rangeEnd = source.GetEndAddress();
    Foundation::Size destinationOffset{0U};
    for (Foundation::Size index{0U}; (index < m_mappingCount) && (cursor < rangeEnd); ++index)
    {
        const DirectUserMemoryMapping& mapping = m_mappings[index];
        const Foundation::UInt64 mappingStart = mapping.UserStart.GetValue();
        const Foundation::UInt64 mappingEnd = mappingStart + GetMappingSizeBytes(mapping);
        if ((cursor < mappingStart) || (cursor >= mappingEnd))
        {
            continue;
        }

        const Foundation::UInt64 chunkEnd = Minimum(mappingEnd, rangeEnd);
        const Foundation::Size mappingOffset = static_cast<Foundation::Size>(cursor - mappingStart);
        const Foundation::Size chunkSize = static_cast<Foundation::Size>(chunkEnd - cursor);
        for (Foundation::Size byte{0U}; byte < chunkSize; ++byte)
        {
            destination.Data[destinationOffset + byte] = mapping.DirectStart[mappingOffset + byte];
        }
        destinationOffset += chunkSize;
        cursor = chunkEnd;
    }
}

void DirectUserMemoryAccess::CopyToMappings(const UserMemoryRange destination,
                                            const ReadOnlyKernelBuffer source) noexcept
{
    Foundation::UInt64 cursor = destination.GetStartAddress();
    const Foundation::UInt64 rangeEnd = destination.GetEndAddress();
    Foundation::Size sourceOffset{0U};
    for (Foundation::Size index{0U}; (index < m_mappingCount) && (cursor < rangeEnd); ++index)
    {
        DirectUserMemoryMapping& mapping = m_mappings[index];
        const Foundation::UInt64 mappingStart = mapping.UserStart.GetValue();
        const Foundation::UInt64 mappingEnd = mappingStart + GetMappingSizeBytes(mapping);
        if ((cursor < mappingStart) || (cursor >= mappingEnd))
        {
            continue;
        }

        const Foundation::UInt64 chunkEnd = Minimum(mappingEnd, rangeEnd);
        const Foundation::Size mappingOffset = static_cast<Foundation::Size>(cursor - mappingStart);
        const Foundation::Size chunkSize = static_cast<Foundation::Size>(chunkEnd - cursor);
        for (Foundation::Size byte{0U}; byte < chunkSize; ++byte)
        {
            mapping.DirectStart[mappingOffset + byte] = source.Data[sourceOffset + byte];
        }
        sourceOffset += chunkSize;
        cursor = chunkEnd;
    }
}
} // namespace Asteria::Kernel::Memory
