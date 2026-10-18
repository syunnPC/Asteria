// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Memory/RecordUserMemoryAccess.hpp"

#include "Kernel/Memory/DirectUserMemoryAccess.hpp"

namespace Asteria::Kernel::Memory
{
bool RecordUserMemoryAccess::Bind(const AddressSpace& space, const AddressSpaceId identity,
                                  const bool includeSeed) noexcept
{
    if ((m_space != nullptr) || !identity.IsValid())
    {
        return false;
    }
    m_space = &space;
    m_includeSeed = includeSeed;
    m_identity = identity;
    return true;
}

bool RecordUserMemoryAccess::Reset() noexcept
{
    if (m_hasActiveReservation)
    {
        return false;
    }
    m_space = nullptr;
    m_identity = AddressSpaceId{};
    return true;
}

AddressSpaceId RecordUserMemoryAccess::GetAddressSpaceId() const noexcept
{
    return (m_space != nullptr) ? m_identity : AddressSpaceId{};
}

bool RecordUserMemoryAccess::IsReady() const noexcept
{
    return m_space != nullptr;
}

UserMemoryAccessError RecordUserMemoryAccess::CopyFromUserValidated(const UserMemoryRange source,
                                                                    const WritableKernelBuffer destination) noexcept
{
    const auto pinned = m_space->PinUserMapping(source.GetStartAddress(), source.GetSizeBytes(), false, m_includeSeed);
    if (!pinned.HasValue())
    {
        return UserMemoryAccessError::AccessDenied;
    }
    // The admitted mapping is present in the active root.
    const auto* const bytes = reinterpret_cast<const volatile Foundation::UInt8*>(
        static_cast<Foundation::UIntPointer>(source.GetStartAddress()));
    for (Foundation::Size index{0U}; index < static_cast<Foundation::Size>(source.GetSizeBytes()); ++index)
    {
        destination.Data[index] = bytes[index];
    }
    m_space->ReleaseUserMapping(pinned.GetValue());
    return UserMemoryAccessError::None;
}

Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>
RecordUserMemoryAccess::PrepareCopyToUserValidated(const UserMemoryRange destination) noexcept
{
    using ResultType = Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>;

    Foundation::UInt64 generation{0U};
    if (m_hasActiveReservation ||
        !DirectUserMemoryReservationGenerationPolicy::TryAdvance(m_lastGeneration, generation))
    {
        return ResultType::Failure(UserMemoryAccessError::ReservationExhausted);
    }
    const auto pinned =
        m_space->PinUserMapping(destination.GetStartAddress(), destination.GetSizeBytes(), true, m_includeSeed);
    if (!pinned.HasValue())
    {
        return ResultType::Failure(UserMemoryAccessError::AccessDenied);
    }
    m_activeUse = pinned.GetValue();
    m_lastGeneration = generation;
    m_activeGeneration = generation;
    m_activeRange = destination;
    m_hasActiveReservation = true;
    return ResultType::Success(MakeWriteReservation(destination, generation));
}

UserMemoryAccessError
RecordUserMemoryAccess::CancelCopyToUserValidated(const UserMemoryWriteReservation reservation) noexcept
{
    if (!IsCurrentReservation(reservation))
    {
        return UserMemoryAccessError::StaleReservation;
    }
    m_space->ReleaseUserMapping(m_activeUse);
    m_activeUse = {};
    m_hasActiveReservation = false;
    m_activeRange = UserMemoryRange{};
    m_activeGeneration = 0U;
    return UserMemoryAccessError::None;
}

// The reservation's mapping cannot have gone since Prepare, so
// the publication cannot fail once it begins.
UserMemoryAccessError RecordUserMemoryAccess::CommitCopyToUserValidated(const UserMemoryWriteReservation reservation,
                                                                        const ReadOnlyKernelBuffer source) noexcept
{
    if (!IsCurrentReservation(reservation))
    {
        return UserMemoryAccessError::StaleReservation;
    }
    const UserMemoryRange range = reservation.GetRange();
    auto* const bytes =
        reinterpret_cast<volatile Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(range.GetStartAddress()));
    for (Foundation::Size index{0U}; index < static_cast<Foundation::Size>(range.GetSizeBytes()); ++index)
    {
        bytes[index] = source.Data[index];
    }
    m_space->ReleaseUserMapping(m_activeUse);
    m_activeUse = {};
    m_hasActiveReservation = false;
    m_activeRange = UserMemoryRange{};
    m_activeGeneration = 0U;
    return UserMemoryAccessError::None;
}

bool RecordUserMemoryAccess::IsCurrentReservation(const UserMemoryWriteReservation reservation) const noexcept
{
    return m_hasActiveReservation && reservation.GetRange().IsEqualTo(m_activeRange) &&
           (reservation.GetGeneration() == m_activeGeneration);
}
} // Namespace Asteria::Kernel::Memory
