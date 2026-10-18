// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_INTERRUPT_INTERRUPT_HPP_INCLUDED
#define ASTERIA_KERNEL_INTERRUPT_INTERRUPT_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Interrupt/IInterruptSource.hpp"
#include "Kernel/Ipc/Notification.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Interrupt
{
enum class InterruptState : Foundation::UInt8
{
    Uninitialized,
    Masked,
    Armed,
    Pending,
    Faulted,
    Quiesced,
};

enum class InterruptCompletion : Foundation::UInt8
{
    KeepMasked,
    Rearm,
};

enum class InterruptError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    NotInitialized,
    InvalidNotification,
    NotificationRegistryMismatch,
    NotificationPinRejected,
    NotificationInstanceMismatch,
    SourceRejected,
    AlreadyArmed,
    AlreadyMasked,
    EventPending,
    InvalidEvent,
    StaleEvent,
    InvalidCompletion,
    EventSequenceExhausted,
    NotificationRejected,
    SpuriousInterrupt,
    Faulted,
    AlreadyQuiesced,
    CorruptState,
};

class InterruptEventSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U} >> 8U};

    InterruptEventSequencePolicy() = delete;

    [[nodiscard]] static constexpr bool TryAdvance(const Foundation::UInt64 current, Foundation::UInt64& next) noexcept
    {
        if (current >= Maximum)
        {
            return false;
        }
        next = current + 1U;
        return true;
    }
};

class Interrupt;

/// Names one pending hardware-event generation of one immovable Interrupt.
/// This token is kernel-internal and is never serialized through the syscall
/// ABI. The originating Interrupt outlives every copy and use of the token.
class InterruptEventToken final
{
public:
    constexpr InterruptEventToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_interrupt != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const InterruptEventToken other) const noexcept
    {
        return (m_interrupt == other.m_interrupt) && (m_sequence == other.m_sequence);
    }

private:
    friend class Interrupt;

    constexpr InterruptEventToken(const Interrupt& interrupt, const Foundation::UInt64 sequence) noexcept
        : m_interrupt{&interrupt}, m_sequence{sequence}
    {
    }

    const Interrupt* m_interrupt{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

struct InterruptFailure
{
    Ipc::NotificationFailure Notification;
    Hal::Interrupt::InterruptSourceError Source;
    Object::ObjectPinError Pin;
    InterruptError Error;
    Foundation::UInt8 Reserved[5];
};

struct InterruptDelivery
{
    InterruptEventToken Event;
    bool NewEvent;
    Foundation::UInt8 Reserved[7];
};

struct InterruptSnapshot
{
    Ipc::NotificationId Notification;
    InterruptEventToken PendingEvent;
    Foundation::UInt64 LastEventSequence;
    InterruptState State;
    bool SourceClaimed;
    Foundation::UInt8 Reserved[6];
};

/// Owns one dedicated interrupt source and publishes its events through one
/// exact Notification generation.
///
/// Normal operations run on the source's bound processor with local interrupts
/// disabled and serialize on the object lock. HandleInterrupt is the only ISR
/// operation; it masks and acknowledges the hardware before publishing a new
/// generation and signaling the Notification. The source, registry,
/// Notification storage, and this object remain alive and immovable until a
/// successful Quiesce and registry collection.
class Interrupt final : public Object::IKernelObject
{
public:
    constexpr Interrupt(Object::ObjectRegistry& registry, Hal::Interrupt::IInterruptSource& source) noexcept
        : m_registry{registry}, m_source{source}
    {
    }

    /// Preconditions:
    /// - State is Uninitialized or Quiesced.
    /// - The source has no entered handler and no concurrent operation remains.
    /// - Every InterruptEventToken copy is no longer used.
    ~Interrupt() noexcept = default;

    Interrupt(const Interrupt&) = delete;
    Interrupt& operator=(const Interrupt&) = delete;
    Interrupt(Interrupt&&) = delete;
    Interrupt& operator=(Interrupt&&) = delete;

    /// Pins notification, claims the dedicated masked source, and publishes
    /// immutable identity metadata exactly once.
    [[nodiscard]] InterruptFailure Initialize(Object::ResourceDomainId chargeOwner, Ipc::NotificationId notificationId,
                                              Ipc::Notification& notification) noexcept;

    /// Unmasks a Masked source. The caller keeps local interrupts disabled
    /// until this operation returns. An edge that arrived while masked is
    /// delivered at once, as a new pending event.
    [[nodiscard]] InterruptFailure Arm() noexcept;

    /// Masks an Armed source without fabricating or discarding an event.
    [[nodiscard]] InterruptFailure Mask() noexcept;

    /// Masks and acknowledges the entered hardware event, then coalesces it
    /// into the bound Notification. An edge that arrives while Masked is
    /// remembered for the next Arm or Rearm; one that arrives while Pending
    /// coalesces into the unacknowledged event, so a driver reads its device's
    /// status again after it rearms. This method is ISR-safe and never locks.
    [[nodiscard]] Foundation::Result<InterruptDelivery, InterruptFailure> HandleInterrupt() noexcept;

    /// Resolves the exact pending generation and either stays masked or
    /// re-enables the source. A failed nonterminal rearm keeps the event Pending.
    [[nodiscard]] Foundation::Result<InterruptSnapshot, InterruptFailure>
    Acknowledge(InterruptEventToken event, InterruptCompletion completion) noexcept;

    /// Releases a Masked source and the retained Notification pin. A Faulted
    /// source may be released only after platform recovery makes Release safe
    /// again. This is the required terminal transition before destruction.
    [[nodiscard]] InterruptFailure Quiesce() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] InterruptSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    static constexpr Foundation::UInt64 StateMask{0xffU};
    static constexpr Foundation::Size SequenceShift{8U};

    [[nodiscard]] static constexpr Foundation::UInt64 EncodeState(const InterruptState state,
                                                                  const Foundation::UInt64 sequence) noexcept
    {
        return (sequence << SequenceShift) | static_cast<Foundation::UInt64>(state);
    }

    [[nodiscard]] static constexpr InterruptState DecodeState(const Foundation::UInt64 word) noexcept
    {
        return static_cast<InterruptState>(word & StateMask);
    }

    [[nodiscard]] static constexpr Foundation::UInt64 DecodeSequence(const Foundation::UInt64 word) noexcept
    {
        return word >> SequenceShift;
    }

    [[nodiscard]] static constexpr bool StateIsKnown(const InterruptState state) noexcept
    {
        return (state == InterruptState::Uninitialized) || (state == InterruptState::Masked) ||
               (state == InterruptState::Armed) || (state == InterruptState::Pending) ||
               (state == InterruptState::Faulted) || (state == InterruptState::Quiesced);
    }

    [[nodiscard]] static InterruptFailure
    MakeFailure(InterruptError error,
                Hal::Interrupt::InterruptSourceError source = Hal::Interrupt::InterruptSourceError::None,
                Object::ObjectPinError pin = Object::ObjectPinError::Ok,
                Ipc::NotificationError notification = Ipc::NotificationError::None) noexcept;
    [[nodiscard]] static constexpr bool
    SourceFailureIsTerminal(const Hal::Interrupt::InterruptSourceError error) noexcept
    {
        return (error != Hal::Interrupt::InterruptSourceError::None) &&
               (error != Hal::Interrupt::InterruptSourceError::NotReady);
    }
    [[nodiscard]] InterruptError ValidateEventLocked(InterruptEventToken event,
                                                     Foundation::UInt64 stateWord) const noexcept;
    [[nodiscard]] InterruptSnapshot GetSnapshotLocked() const noexcept;
    void EnterFaulted(const Foundation::UInt64 sequence) noexcept;
    /// After arming: an edge that arrived while masked becomes the next
    /// pending generation at once, its Notification signaled.
    void DeliverMissedLocked(Foundation::UInt64 sequence) noexcept;

    Object::ObjectRegistry& m_registry;
    Hal::Interrupt::IInterruptSource& m_source;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Platform::Synchronization::AtomicUInt64 m_stateWord;
    /// Nonzero once an edge arrived while Masked (a message source's pending
    /// bit): it is delivered at the next Arm or Rearm, never dropped.
    Platform::Synchronization::AtomicUInt64 m_missed;
    Object::ObjectPin m_notificationPin;
    Ipc::NotificationId m_notificationIdentity{};
    Ipc::Notification* m_notification{nullptr};
    bool m_sourceClaimed{false};
};

static_assert(sizeof(InterruptState) == 1U);
static_assert(sizeof(InterruptCompletion) == 1U);
static_assert(sizeof(InterruptError) == 1U);
static_assert(sizeof(InterruptEventToken) == 16U);
static_assert(alignof(InterruptEventToken) == 8U);
static_assert(sizeof(InterruptFailure) == 16U);
static_assert(alignof(InterruptFailure) == 1U);
static_assert(sizeof(InterruptDelivery) == 24U);
static_assert(alignof(InterruptDelivery) == 8U);
static_assert(sizeof(InterruptSnapshot) == 48U);
static_assert(alignof(InterruptSnapshot) == 8U);
static_assert(sizeof(Foundation::Result<InterruptDelivery, InterruptFailure>) == 48U);
static_assert(alignof(Foundation::Result<InterruptDelivery, InterruptFailure>) == 8U);
static_assert(sizeof(Foundation::Result<InterruptSnapshot, InterruptFailure>) == 72U);
static_assert(alignof(Foundation::Result<InterruptSnapshot, InterruptFailure>) == 8U);
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{0U};
        return InterruptEventSequencePolicy::TryAdvance(0U, next) && (next == 1U);
    }());
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{17U};
        return !InterruptEventSequencePolicy::TryAdvance(InterruptEventSequencePolicy::Maximum, next) && (next == 17U);
    }());
} // namespace Asteria::Kernel::Interrupt

#endif // ASTERIA_KERNEL_INTERRUPT_INTERRUPT_HPP_INCLUDED
