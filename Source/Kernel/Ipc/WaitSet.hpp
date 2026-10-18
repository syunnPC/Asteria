// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_WAIT_SET_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_WAIT_SET_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Ipc
{
constexpr Foundation::Size MaximumWaitSetEntries{32U};

enum class WaitSetState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    Claimed,
};

enum class WaitSetError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    NotInitialized,
    InvalidNotification,
    NotificationRegistryMismatch,
    SetFull,
    DuplicateNotification,
    NotificationPinRejected,
    NotificationInstanceMismatch,
    NotificationRejected,
    RegistrationSequenceExhausted,
    InvalidRegistration,
    StaleRegistration,
    Empty,
    WouldBlock,
    ClaimActive,
    ClaimSequenceExhausted,
    InvalidClaim,
    StaleClaim,
    CorruptState,
};

class WaitSetSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    WaitSetSequencePolicy() = delete;

    [[nodiscard]] static constexpr bool TryAdvance(const Foundation::UInt64 current, Foundation::UInt64& next) noexcept
    {
        if (current == Maximum)
        {
            return false;
        }
        next = current + 1U;
        return true;
    }
};

class WaitSet;

/// Names one registration generation in one WaitSet.
/// Lifetime:
/// - The originating WaitSet outlives every copy and use of this token.
class WaitSetRegistrationToken final
{
public:
    constexpr WaitSetRegistrationToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_waitSet != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const WaitSetRegistrationToken other) const noexcept
    {
        return (m_waitSet == other.m_waitSet) && (m_sequence == other.m_sequence);
    }

private:
    friend class WaitSet;

    constexpr WaitSetRegistrationToken(const WaitSet& waitSet, const Foundation::UInt64 sequence) noexcept
        : m_waitSet{&waitSet}, m_sequence{sequence}
    {
    }

    const WaitSet* m_waitSet{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

/// Names one unresolved readiness claim from one WaitSet.
/// Lifetime:
/// - The originating WaitSet outlives every copy and use of this token.
class WaitSetClaimToken final
{
public:
    constexpr WaitSetClaimToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_waitSet != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const WaitSetClaimToken other) const noexcept
    {
        return (m_waitSet == other.m_waitSet) && (m_sequence == other.m_sequence);
    }

private:
    friend class WaitSet;

    constexpr WaitSetClaimToken(const WaitSet& waitSet, const Foundation::UInt64 sequence) noexcept
        : m_waitSet{&waitSet}, m_sequence{sequence}
    {
    }

    const WaitSet* m_waitSet{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

struct WaitSetFailure
{
    NotificationFailure Notification;
    WaitSetError Error;
    Object::ObjectPinError Pin;
    Foundation::UInt8 Reserved[6];
};

struct WaitSetRegistrationReceipt
{
    WaitSetRegistrationToken Registration;
    NotificationId Notification;
    Foundation::UInt64 Tag;
};

struct WaitSetNotificationClaim
{
    WaitSetClaimToken Claim;
    WaitSetRegistrationToken Registration;
    NotificationId Notification;
    Foundation::UInt64 Tag;
};

struct WaitSetSnapshot
{
    Foundation::UInt64 LastRegistrationSequence;
    Foundation::UInt64 LastClaimSequence;
    WaitSetState State;
    Foundation::UInt8 RegisteredEntries;
    Foundation::UInt8 ClaimedEntries;
    Foundation::UInt8 NextScanIndex;
    Foundation::UInt8 Reserved[4];
};

/// Fixed-capacity readiness aggregator for exact Notification generations. Registration retains an
/// ObjectRegistry pin and gives the WaitSet exclusive claim ownership of that Notification.
/// ClaimReady scans at most 32 entries in round-robin order and keeps both lifetimes until
/// CommitClaim or CancelClaim resolves the exact generation.
class WaitSet final : public Object::IKernelObject
{
public:
    /// Preconditions:
    /// - objectRegistry outlives this WaitSet and every retained registration
    /// pin, readiness claim, and token copy.
    explicit constexpr WaitSet(Object::ObjectRegistry& objectRegistry) noexcept : m_objectRegistry{objectRegistry}
    {
    }

    /// Detaches every registered Notification and releases its pin.
    /// Preconditions:
    /// - State is Uninitialized or Ready, with no concurrent operation,
    /// outstanding token use, or external claim.
    /// - A registered WaitSet has completed registry collection.
    ~WaitSet() noexcept;

    WaitSet(const WaitSet&) = delete;
    WaitSet& operator=(const WaitSet&) = delete;
    WaitSet(WaitSet&&) = delete;
    WaitSet& operator=(WaitSet&&) = delete;

    [[nodiscard]] WaitSetError Initialize(Object::ResourceDomainId chargeOwner) noexcept;

    /// Installs one exact Notification generation. tag is returned unchanged
    /// by each readiness claim and has no kernel-defined meaning.
    [[nodiscard]] Foundation::Result<WaitSetRegistrationReceipt, WaitSetFailure>
    RegisterNotification(NotificationId notificationId, Notification& notification, Foundation::UInt64 tag) noexcept;

    /// Removes one exact registration and releases its retained pin.
    [[nodiscard]] Foundation::Result<WaitSetSnapshot, WaitSetFailure>
    Unregister(WaitSetRegistrationToken registration) noexcept;

    /// Claims the next pending Notification in bounded round-robin order.
    [[nodiscard]] Foundation::Result<WaitSetNotificationClaim, WaitSetFailure> ClaimReady() noexcept;

    /// Consumes the exact active readiness claim.
    [[nodiscard]] Foundation::Result<WaitSetSnapshot, WaitSetFailure> CommitClaim(WaitSetClaimToken claim) noexcept;

    /// Returns the exact active readiness claim to its Notification.
    [[nodiscard]] Foundation::Result<WaitSetSnapshot, WaitSetFailure> CancelClaim(WaitSetClaimToken claim) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] WaitSetSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    enum class EntryState : Foundation::UInt8
    {
        Empty,
        Registered,
        Claimed,
    };

    struct Entry
    {
        Object::ObjectPin Pin;
        NotificationId Identity;
        Notification* Storage;
        NotificationClaimToken NotificationClaim;
        Foundation::UInt64 Tag;
        Foundation::UInt64 RegistrationSequence;
        EntryState State;
        Foundation::UInt8 Reserved[7];
    };

    [[nodiscard]] static WaitSetFailure
    MakeFailure(WaitSetError error, NotificationError notificationError = NotificationError::None,
                Object::ObjectPinError pinError = Object::ObjectPinError::Ok) noexcept;
    [[nodiscard]] Entry* FindRegistrationLocked(WaitSetRegistrationToken registration) noexcept;
    [[nodiscard]] WaitSetError ValidateClaimLocked(WaitSetClaimToken claim) const noexcept;
    [[nodiscard]] WaitSetSnapshot GetSnapshotLocked() const noexcept;
    void ClearEntryLocked(Entry& entry) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Entry m_entries[MaximumWaitSetEntries]{};
    Foundation::UInt64 m_lastRegistrationSequence{0U};
    Foundation::UInt64 m_lastClaimSequence{0U};
    Foundation::UInt64 m_activeClaimSequence{0U};
    Foundation::Size m_registeredEntries{0U};
    Foundation::Size m_activeEntryIndex{MaximumWaitSetEntries};
    Foundation::Size m_nextScanIndex{0U};
    WaitSetState m_state{WaitSetState::Uninitialized};
};

static_assert(sizeof(WaitSetState) == 1U);
static_assert(sizeof(WaitSetError) == 1U);
static_assert(sizeof(WaitSetRegistrationToken) == 16U);
static_assert(alignof(WaitSetRegistrationToken) == 8U);
static_assert(sizeof(WaitSetClaimToken) == 16U);
static_assert(alignof(WaitSetClaimToken) == 8U);
static_assert(sizeof(WaitSetFailure) == 16U);
static_assert(alignof(WaitSetFailure) == 1U);
static_assert(sizeof(WaitSetRegistrationReceipt) == 40U);
static_assert(alignof(WaitSetRegistrationReceipt) == 8U);
static_assert(sizeof(WaitSetNotificationClaim) == 56U);
static_assert(alignof(WaitSetNotificationClaim) == 8U);
static_assert(sizeof(WaitSetSnapshot) == 24U);
static_assert(alignof(WaitSetSnapshot) == 8U);
static_assert(sizeof(Foundation::Result<WaitSetRegistrationReceipt, WaitSetFailure>) == 64U);
static_assert(alignof(Foundation::Result<WaitSetRegistrationReceipt, WaitSetFailure>) == 8U);
static_assert(sizeof(Foundation::Result<WaitSetNotificationClaim, WaitSetFailure>) == 80U);
static_assert(alignof(Foundation::Result<WaitSetNotificationClaim, WaitSetFailure>) == 8U);
static_assert(sizeof(Foundation::Result<WaitSetSnapshot, WaitSetFailure>) == 48U);
static_assert(alignof(Foundation::Result<WaitSetSnapshot, WaitSetFailure>) == 8U);
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{0U};
        return WaitSetSequencePolicy::TryAdvance(0U, next) && (next == 1U);
    }());
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{17U};
        return !WaitSetSequencePolicy::TryAdvance(WaitSetSequencePolicy::Maximum, next) && (next == 17U);
    }());
} // Namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_WAIT_SET_HPP_INCLUDED
