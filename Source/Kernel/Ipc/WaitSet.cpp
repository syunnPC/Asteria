// SPDX-License-Identifier: Apache-2.0

#include "WaitSet.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Ipc
{
WaitSet::~WaitSet() noexcept
{
    for (Entry& entry : m_entries)
    {
        if (entry.State == EntryState::Empty)
        {
            continue;
        }
        if (entry.Storage != nullptr)
        {
            if ((entry.State == EntryState::Claimed) && entry.NotificationClaim.IsValid())
            {
                static_cast<void>(entry.Storage->CancelClaimFromWaitSet(*this, entry.NotificationClaim));
            }
            static_cast<void>(entry.Storage->DetachWaitSet(*this));
        }
        ClearEntryLocked(entry);
    }
}

WaitSetFailure WaitSet::MakeFailure(const WaitSetError error, const NotificationError notificationError,
                                    const Object::ObjectPinError pinError) noexcept
{
    return WaitSetFailure{
        .Notification =
            NotificationFailure{
                .Error = notificationError,
                .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
            },
        .Error = error,
        .Pin = pinError,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}

WaitSetError WaitSet::Initialize(const Object::ResourceDomainId chargeOwner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state != WaitSetState::Uninitialized)
    {
        return WaitSetError::AlreadyInitialized;
    }
    if (!chargeOwner.IsValid())
    {
        return WaitSetError::InvalidConfiguration;
    }

    m_lastRegistrationSequence = 0U;
    m_lastClaimSequence = 0U;
    m_activeClaimSequence = 0U;
    m_registeredEntries = 0U;
    m_activeEntryIndex = MaximumWaitSetEntries;
    m_nextScanIndex = 0U;
    m_state = WaitSetState::Ready;
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return WaitSetError::None;
}

Foundation::Result<WaitSetRegistrationReceipt, WaitSetFailure>
WaitSet::RegisterNotification(const NotificationId notificationId, Notification& notification,
                              const Foundation::UInt64 tag) noexcept
{
    using ResultType = Foundation::Result<WaitSetRegistrationReceipt, WaitSetFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetState::Uninitialized)
    {
        return ResultType::Failure(MakeFailure(WaitSetError::NotInitialized));
    }
    if (m_state == WaitSetState::Claimed)
    {
        return ResultType::Failure(MakeFailure(WaitSetError::ClaimActive));
    }
    if (!notificationId.IsValid())
    {
        return ResultType::Failure(MakeFailure(WaitSetError::InvalidNotification));
    }
    if (!notificationId.IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(MakeFailure(WaitSetError::NotificationRegistryMismatch));
    }

    Entry* emptyEntry{nullptr};
    for (Entry& entry : m_entries)
    {
        if (entry.State == EntryState::Empty)
        {
            if (emptyEntry == nullptr)
            {
                emptyEntry = &entry;
            }
            continue;
        }
        if (entry.Identity.IsEqualTo(notificationId) || (entry.Storage == &notification))
        {
            return ResultType::Failure(MakeFailure(WaitSetError::DuplicateNotification));
        }
    }
    if (emptyEntry == nullptr)
    {
        return ResultType::Failure(MakeFailure(WaitSetError::SetFull));
    }

    Foundation::UInt64 nextSequence{m_lastRegistrationSequence};
    if (!WaitSetSequencePolicy::TryAdvance(m_lastRegistrationSequence, nextSequence))
    {
        return ResultType::Failure(MakeFailure(WaitSetError::RegistrationSequenceExhausted));
    }
    if (emptyEntry->Pin.IsValid() || emptyEntry->Identity.IsValid() || (emptyEntry->Storage != nullptr) ||
        emptyEntry->NotificationClaim.IsValid() || (emptyEntry->Tag != 0U) || (emptyEntry->RegistrationSequence != 0U))
    {
        return ResultType::Failure(MakeFailure(WaitSetError::CorruptState));
    }

    const Object::ObjectPinError pinError =
        m_objectRegistry.PinObject(notificationId.GetObjectId(), Object::ObjectType::Notification, emptyEntry->Pin);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return ResultType::Failure(
            MakeFailure(WaitSetError::NotificationPinRejected, NotificationError::None, pinError));
    }
    if (&emptyEntry->Pin.GetObject() != static_cast<Object::IKernelObject*>(&notification))
    {
        emptyEntry->Pin.Reset();
        return ResultType::Failure(MakeFailure(WaitSetError::NotificationInstanceMismatch));
    }

    const NotificationError notificationError = notification.AttachWaitSet(*this);
    if (notificationError != NotificationError::None)
    {
        emptyEntry->Pin.Reset();
        return ResultType::Failure(MakeFailure(WaitSetError::NotificationRejected, notificationError));
    }

    emptyEntry->Identity = notificationId;
    emptyEntry->Storage = &notification;
    emptyEntry->NotificationClaim = NotificationClaimToken{};
    emptyEntry->Tag = tag;
    emptyEntry->RegistrationSequence = nextSequence;
    emptyEntry->State = EntryState::Registered;
    for (Foundation::UInt8& reserved : emptyEntry->Reserved)
    {
        reserved = 0U;
    }
    m_lastRegistrationSequence = nextSequence;
    ++m_registeredEntries;

    return ResultType::Success(WaitSetRegistrationReceipt{
        .Registration = WaitSetRegistrationToken{*this, nextSequence},
        .Notification = notificationId,
        .Tag = tag,
    });
}

Foundation::Result<WaitSetSnapshot, WaitSetFailure>
WaitSet::Unregister(const WaitSetRegistrationToken registration) noexcept
{
    using ResultType = Foundation::Result<WaitSetSnapshot, WaitSetFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetState::Uninitialized)
    {
        return ResultType::Failure(MakeFailure(WaitSetError::NotInitialized));
    }
    if (m_state == WaitSetState::Claimed)
    {
        return ResultType::Failure(MakeFailure(WaitSetError::ClaimActive));
    }
    if (!registration.IsValid() || (registration.m_waitSet != this))
    {
        return ResultType::Failure(MakeFailure(WaitSetError::InvalidRegistration));
    }

    Entry* const entry = FindRegistrationLocked(registration);
    if (entry == nullptr)
    {
        return ResultType::Failure(MakeFailure(WaitSetError::StaleRegistration));
    }
    if (!entry->Pin.IsValid() || !entry->Identity.IsValid() || (entry->Storage == nullptr) ||
        (entry->State != EntryState::Registered))
    {
        return ResultType::Failure(MakeFailure(WaitSetError::CorruptState));
    }

    const NotificationError notificationError = entry->Storage->DetachWaitSet(*this);
    if (notificationError != NotificationError::None)
    {
        return ResultType::Failure(MakeFailure(WaitSetError::NotificationRejected, notificationError));
    }

    ClearEntryLocked(*entry);
    --m_registeredEntries;
    return ResultType::Success(GetSnapshotLocked());
}

Foundation::Result<WaitSetNotificationClaim, WaitSetFailure> WaitSet::ClaimReady() noexcept
{
    using ResultType = Foundation::Result<WaitSetNotificationClaim, WaitSetFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_state == WaitSetState::Uninitialized)
    {
        return ResultType::Failure(MakeFailure(WaitSetError::NotInitialized));
    }
    if (m_state == WaitSetState::Claimed)
    {
        return ResultType::Failure(MakeFailure(WaitSetError::ClaimActive));
    }
    if (m_registeredEntries == 0U)
    {
        return ResultType::Failure(MakeFailure(WaitSetError::Empty));
    }

    Foundation::UInt64 nextSequence{m_lastClaimSequence};
    if (!WaitSetSequencePolicy::TryAdvance(m_lastClaimSequence, nextSequence))
    {
        return ResultType::Failure(MakeFailure(WaitSetError::ClaimSequenceExhausted));
    }

    for (Foundation::Size offset{0U}; offset < MaximumWaitSetEntries; ++offset)
    {
        const Foundation::Size index = (m_nextScanIndex + offset) % MaximumWaitSetEntries;
        Entry& entry = m_entries[index];
        if (entry.State == EntryState::Empty)
        {
            continue;
        }
        if ((entry.State != EntryState::Registered) || !entry.Pin.IsValid() || !entry.Identity.IsValid() ||
            (entry.Storage == nullptr) || (entry.RegistrationSequence == 0U) || entry.NotificationClaim.IsValid())
        {
            return ResultType::Failure(MakeFailure(WaitSetError::CorruptState));
        }

        const auto notificationClaim = entry.Storage->ClaimFromWaitSet(*this);
        if (!notificationClaim.HasValue())
        {
            if (notificationClaim.GetError().Error == NotificationError::WouldBlock)
            {
                continue;
            }
            return ResultType::Failure(
                MakeFailure(WaitSetError::NotificationRejected, notificationClaim.GetError().Error));
        }

        entry.NotificationClaim = notificationClaim.GetValue();
        entry.State = EntryState::Claimed;
        m_lastClaimSequence = nextSequence;
        m_activeClaimSequence = nextSequence;
        m_activeEntryIndex = index;
        m_nextScanIndex = (index + 1U) % MaximumWaitSetEntries;
        m_state = WaitSetState::Claimed;
        return ResultType::Success(WaitSetNotificationClaim{
            .Claim = WaitSetClaimToken{*this, nextSequence},
            .Registration =
                WaitSetRegistrationToken{
                    *this,
                    entry.RegistrationSequence,
                },
            .Notification = entry.Identity,
            .Tag = entry.Tag,
        });
    }

    return ResultType::Failure(MakeFailure(WaitSetError::WouldBlock));
}

Foundation::Result<WaitSetSnapshot, WaitSetFailure> WaitSet::CommitClaim(const WaitSetClaimToken claim) noexcept
{
    using ResultType = Foundation::Result<WaitSetSnapshot, WaitSetFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const WaitSetError validation = ValidateClaimLocked(claim);
    if (validation != WaitSetError::None)
    {
        return ResultType::Failure(MakeFailure(validation));
    }

    Entry& entry = m_entries[m_activeEntryIndex];
    const auto notificationResult = entry.Storage->CommitClaimFromWaitSet(*this, entry.NotificationClaim);
    if (!notificationResult.HasValue())
    {
        return ResultType::Failure(
            MakeFailure(WaitSetError::NotificationRejected, notificationResult.GetError().Error));
    }

    entry.NotificationClaim = NotificationClaimToken{};
    entry.State = EntryState::Registered;
    m_activeClaimSequence = 0U;
    m_activeEntryIndex = MaximumWaitSetEntries;
    m_state = WaitSetState::Ready;
    return ResultType::Success(GetSnapshotLocked());
}

Foundation::Result<WaitSetSnapshot, WaitSetFailure> WaitSet::CancelClaim(const WaitSetClaimToken claim) noexcept
{
    using ResultType = Foundation::Result<WaitSetSnapshot, WaitSetFailure>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const WaitSetError validation = ValidateClaimLocked(claim);
    if (validation != WaitSetError::None)
    {
        return ResultType::Failure(MakeFailure(validation));
    }

    Entry& entry = m_entries[m_activeEntryIndex];
    const auto notificationResult = entry.Storage->CancelClaimFromWaitSet(*this, entry.NotificationClaim);
    if (!notificationResult.HasValue())
    {
        return ResultType::Failure(
            MakeFailure(WaitSetError::NotificationRejected, notificationResult.GetError().Error));
    }

    entry.NotificationClaim = NotificationClaimToken{};
    entry.State = EntryState::Registered;
    m_activeClaimSequence = 0U;
    m_activeEntryIndex = MaximumWaitSetEntries;
    m_state = WaitSetState::Ready;
    return ResultType::Success(GetSnapshotLocked());
}

bool WaitSet::IsInitialized() const noexcept
{
    return m_chargeOwnerValue.LoadAcquire() != 0U;
}

WaitSetSnapshot WaitSet::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return GetSnapshotLocked();
}

Object::ObjectType WaitSet::GetObjectType() const noexcept
{
    return Object::ObjectType::WaitSet;
}

Object::ResourceDomainId WaitSet::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

WaitSet::Entry* WaitSet::FindRegistrationLocked(const WaitSetRegistrationToken registration) noexcept
{
    for (Entry& entry : m_entries)
    {
        if ((entry.State != EntryState::Empty) && (entry.RegistrationSequence == registration.m_sequence))
        {
            return &entry;
        }
    }
    return nullptr;
}

WaitSetError WaitSet::ValidateClaimLocked(const WaitSetClaimToken claim) const noexcept
{
    if (m_state == WaitSetState::Uninitialized)
    {
        return WaitSetError::NotInitialized;
    }
    if (!claim.IsValid() || (claim.m_waitSet != this))
    {
        return WaitSetError::InvalidClaim;
    }
    if ((m_state != WaitSetState::Claimed) || (claim.m_sequence != m_activeClaimSequence))
    {
        return WaitSetError::StaleClaim;
    }
    if ((m_activeEntryIndex >= MaximumWaitSetEntries) || (m_entries[m_activeEntryIndex].State != EntryState::Claimed) ||
        (m_entries[m_activeEntryIndex].Storage == nullptr) ||
        !m_entries[m_activeEntryIndex].NotificationClaim.IsValid())
    {
        return WaitSetError::CorruptState;
    }
    return WaitSetError::None;
}

WaitSetSnapshot WaitSet::GetSnapshotLocked() const noexcept
{
    return WaitSetSnapshot{
        .LastRegistrationSequence = m_lastRegistrationSequence,
        .LastClaimSequence = m_lastClaimSequence,
        .State = m_state,
        .RegisteredEntries = static_cast<Foundation::UInt8>(m_registeredEntries),
        .ClaimedEntries = static_cast<Foundation::UInt8>(m_state == WaitSetState::Claimed),
        .NextScanIndex = static_cast<Foundation::UInt8>(m_nextScanIndex),
        .Reserved = {0U, 0U, 0U, 0U},
    };
}

void WaitSet::ClearEntryLocked(Entry& entry) noexcept
{
    entry.Pin.Reset();
    entry.Identity = NotificationId{};
    entry.Storage = nullptr;
    entry.NotificationClaim = NotificationClaimToken{};
    entry.Tag = 0U;
    entry.RegistrationSequence = 0U;
    entry.State = EntryState::Empty;
    for (Foundation::UInt8& reserved : entry.Reserved)
    {
        reserved = 0U;
    }
}
} // namespace Asteria::Kernel::Ipc
