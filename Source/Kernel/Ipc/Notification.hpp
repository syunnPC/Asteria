// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_IPC_NOTIFICATION_HPP_INCLUDED
#define ASTERIA_KERNEL_IPC_NOTIFICATION_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Ipc
{
enum class NotificationState : Foundation::UInt8
{
    Uninitialized,
    Ready,
    Claimed,
};

enum class NotificationError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    NotInitialized,
    WouldBlock,
    ClaimActive,
    InvalidClaim,
    StaleClaim,
    ClaimSequenceExhausted,
    ManagedByWaitSet,
    WaitSetMismatch,
};

class NotificationClaimSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    NotificationClaimSequencePolicy() = delete;

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

class Notification;
class WaitSet;

/// Identifies one Notification generation within one ObjectRegistry.
/// Lifetime:
/// - The originating registry outlives every copy and use of this value.
class NotificationId final
{
public:
    constexpr NotificationId() noexcept = default;

    constexpr NotificationId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
        : m_registry{&registry}, m_object{object}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_object.GetValue();
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_registry != nullptr) && m_object.IsValid();
    }

    [[nodiscard]] constexpr Object::ObjectId GetObjectId() const noexcept
    {
        return m_object;
    }

    [[nodiscard]] constexpr bool IsFromRegistry(const Object::ObjectRegistry& registry) const noexcept
    {
        return m_registry == &registry;
    }

    [[nodiscard]] constexpr bool IsEqualTo(const NotificationId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

/// Names one claimed notification generation. This token is kernel-internal
/// and is never serialized through the syscall ABI.
/// Lifetime:
/// - The originating Notification outlives every copy and use of this token.
class NotificationClaimToken final
{
public:
    constexpr NotificationClaimToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_notification != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const NotificationClaimToken other) const noexcept
    {
        return (m_notification == other.m_notification) && (m_sequence == other.m_sequence);
    }

private:
    friend class Notification;

    constexpr NotificationClaimToken(const Notification& notification, const Foundation::UInt64 sequence) noexcept
        : m_notification{&notification}, m_sequence{sequence}
    {
    }

    const Notification* m_notification{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

struct NotificationFailure
{
    NotificationError Error;
    Foundation::UInt8 Reserved[7];
};

struct NotificationSnapshot
{
    Foundation::UInt64 LastClaimSequence;
    NotificationState State;
    Foundation::UInt8 Pending;
    Foundation::UInt8 Reserved[6];
};

/// One coalescing event bit with an explicit receive transaction. Signal is the ISR-safe producer
/// operation. Normal context consumes the pending bit through Claim followed by exactly one
/// CommitClaim or CancelClaim. A signal published while a claim is active remains pending for a
/// later claim.
class Notification final : public Object::IKernelObject
{
public:
    constexpr Notification() noexcept = default;

    /// Preconditions:
    /// - State is Uninitialized or Ready, with no concurrent operation, active
    /// claim, WaitSet membership, or outstanding claim-token use.
    /// - A registered object has completed registry collection. Producers have
    /// stopped; only then may a remaining pending bit be abandoned.
    ~Notification() noexcept = default;

    Notification(const Notification&) = delete;
    Notification& operator=(const Notification&) = delete;
    Notification(Notification&&) = delete;
    Notification& operator=(Notification&&) = delete;

    [[nodiscard]] NotificationError Initialize(Object::ResourceDomainId chargeOwner) noexcept;

    /// Coalesces one event into the pending bit.
    /// Blocking: wait-free; performs one lock-free atomic exchange.
    /// Allocation: does not allocate.
    /// Interrupt safety: safe from an ISR after initialization publication.
    [[nodiscard]] NotificationError Signal() noexcept;

    /// Moves the pending bit into one private claim generation.
    /// Blocking: may spin on the claim lock; not ISR-safe.
    [[nodiscard]] Foundation::Result<NotificationClaimToken, NotificationFailure> Claim() noexcept;

    /// Consumes the exact active claim. Signals published after Claim remain
    /// pending and are not consumed by this operation.
    [[nodiscard]] Foundation::Result<NotificationSnapshot, NotificationFailure>
    CommitClaim(NotificationClaimToken token) noexcept;

    /// Returns the exact active claim to the pending bit.
    [[nodiscard]] Foundation::Result<NotificationSnapshot, NotificationFailure>
    CancelClaim(NotificationClaimToken token) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] NotificationSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    friend class WaitSet;

    [[nodiscard]] static NotificationFailure MakeFailure(NotificationError error) noexcept;
    [[nodiscard]] Foundation::Result<NotificationClaimToken, NotificationFailure> ClaimLocked() noexcept;
    [[nodiscard]] Foundation::Result<NotificationSnapshot, NotificationFailure>
    CommitClaimLocked(NotificationClaimToken token) noexcept;
    [[nodiscard]] Foundation::Result<NotificationSnapshot, NotificationFailure>
    CancelClaimLocked(NotificationClaimToken token) noexcept;
    [[nodiscard]] NotificationError AttachWaitSet(const WaitSet& waitSet) noexcept;
    [[nodiscard]] NotificationError DetachWaitSet(const WaitSet& waitSet) noexcept;
    [[nodiscard]] Foundation::Result<NotificationClaimToken, NotificationFailure>
    ClaimFromWaitSet(const WaitSet& waitSet) noexcept;
    [[nodiscard]] Foundation::Result<NotificationSnapshot, NotificationFailure>
    CommitClaimFromWaitSet(const WaitSet& waitSet, NotificationClaimToken token) noexcept;
    [[nodiscard]] Foundation::Result<NotificationSnapshot, NotificationFailure>
    CancelClaimFromWaitSet(const WaitSet& waitSet, NotificationClaimToken token) noexcept;
    [[nodiscard]] NotificationError ValidateClaimLocked(NotificationClaimToken token) const noexcept;
    [[nodiscard]] NotificationSnapshot GetSnapshotLocked() const noexcept;

    mutable Platform::Synchronization::SpinLock m_claimLock;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Platform::Synchronization::AtomicUInt64 m_pending;
    Foundation::UInt64 m_lastClaimSequence{0U};
    Foundation::UInt64 m_activeClaimSequence{0U};
    const WaitSet* m_waitSet{nullptr};
    NotificationState m_state{NotificationState::Uninitialized};
};

static_assert(sizeof(NotificationState) == 1U);
static_assert(sizeof(NotificationError) == 1U);
static_assert(sizeof(NotificationId) == 16U);
static_assert(alignof(NotificationId) == 8U);
static_assert(sizeof(NotificationClaimToken) == 16U);
static_assert(alignof(NotificationClaimToken) == 8U);
static_assert(sizeof(NotificationFailure) == 8U);
static_assert(alignof(NotificationFailure) == 1U);
static_assert(sizeof(NotificationSnapshot) == 16U);
static_assert(alignof(NotificationSnapshot) == 8U);
static_assert(sizeof(Foundation::Result<NotificationClaimToken, NotificationFailure>) == 32U);
static_assert(alignof(Foundation::Result<NotificationClaimToken, NotificationFailure>) == 8U);
static_assert(sizeof(Foundation::Result<NotificationSnapshot, NotificationFailure>) == 32U);
static_assert(alignof(Foundation::Result<NotificationSnapshot, NotificationFailure>) == 8U);
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{0U};
        return NotificationClaimSequencePolicy::TryAdvance(0U, next) && (next == 1U);
    }());
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{17U};
        return !NotificationClaimSequencePolicy::TryAdvance(NotificationClaimSequencePolicy::Maximum, next) &&
               (next == 17U);
    }());
} // Namespace Asteria::Kernel::Ipc

#endif // ASTERIA_KERNEL_IPC_NOTIFICATION_HPP_INCLUDED
