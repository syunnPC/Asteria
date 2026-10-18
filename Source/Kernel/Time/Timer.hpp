// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_TIME_TIMER_HPP_INCLUDED
#define ASTERIA_KERNEL_TIME_TIMER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Time/IMonotonicClock.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Object
{
class ObjectRegistry;
}

namespace Asteria::Kernel::Time
{
enum class TimerState : Foundation::UInt8
{
    Uninitialized,
    Disarmed,
    Armed,
    Expired,
};

enum class TimerError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    NotInitialized,
    InvalidDeadline,
    DeadlineReached,
    ClockExhausted,
    NotArmed,
    NotExpired,
    AlreadyExpired,
    ExpirationPending,
    ManagedByQueue,
    InvalidToken,
    StaleToken,
    SequenceExhausted,
};

class TimerArmSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    TimerArmSequencePolicy() = delete;

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

class Timer;
class TimerQueue;

/// Identifies one Timer generation within one ObjectRegistry.
/// Lifetime:
/// - The originating registry outlives every copy and use of this value.
class TimerId final
{
public:
    constexpr TimerId() noexcept = default;

    constexpr TimerId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
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

    [[nodiscard]] constexpr bool IsEqualTo(const TimerId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

/// Names one kernel-internal arm generation of one immovable Timer. This value
/// is never serialized or exposed through the syscall ABI.
/// Lifetime:
/// - The originating Timer outlives every copy and use of this token.
class TimerToken final
{
public:
    constexpr TimerToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetSequence() const noexcept
    {
        return m_sequence;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_timer != nullptr) && (m_sequence != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const TimerToken other) const noexcept
    {
        return (m_timer == other.m_timer) && (m_sequence == other.m_sequence);
    }

private:
    friend class Timer;

    constexpr TimerToken(const Timer& timer, const Foundation::UInt64 sequence) noexcept
        : m_timer{&timer}, m_sequence{sequence}
    {
    }

    const Timer* m_timer{nullptr};
    Foundation::UInt64 m_sequence{0U};
};

struct TimerFailure
{
    Foundation::UInt64 ObservedTicks;
    TimerError Error;
    Foundation::UInt8 Reserved[7];
};

struct TimerSnapshot
{
    TimerToken Token;
    Foundation::UInt64 DeadlineTicks;
    TimerState State;
    Foundation::UInt8 Reserved[7];
};

struct TimerArmReceipt
{
    TimerToken Token;
    Foundation::UInt64 DeadlineTicks;
    Foundation::UInt64 ObservedTicks;
    bool Replaced;
    Foundation::UInt8 Reserved[7];
};

struct TimerExpiration
{
    TimerToken Token;
    Foundation::UInt64 DeadlineTicks;
    Foundation::UInt64 ObservedTicks;
};

/// Owns one architecture-neutral, one-shot monotonic deadline. Arm generations prevent a delayed
/// hardware event from expiring a newer deadline. Expiry is a two-step transaction: TryExpire
/// publishes Expired, and Acknowledge releases that pending notification; the owning TimerQueue
/// consumes it in the same way when it retires an expiration that can never be delivered. State
/// operations may spin and are forbidden from interrupt context and same-CPU reentry.
class Timer final : public Object::IKernelObject
{
public:
    explicit constexpr Timer(IMonotonicClock& clock) noexcept : m_clock{clock}
    {
    }

    /// Preconditions:
    /// - State is Uninitialized or Disarmed, and no operation or TimerToken use
    /// is concurrent, and the Timer belongs to no TimerQueue.
    /// - clock outlives this object.
    ~Timer() noexcept = default;

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&&) = delete;
    Timer& operator=(Timer&&) = delete;

    [[nodiscard]] TimerError Initialize(Object::ResourceDomainId chargeOwner) noexcept;

    /// Arms one finite absolute deadline outside a TimerQueue. Replacing an
    /// Armed generation is atomic and invalidates its old token. Expired must
    /// be acknowledged before another deadline can be armed.
    [[nodiscard]] Foundation::Result<TimerArmReceipt, TimerFailure> ArmAt(Object::MonotonicTime deadline) noexcept;

    /// Cancels the exact Armed generation. A fired generation cannot be
    /// cancelled after TryExpire publishes it.
    [[nodiscard]] Foundation::Result<TimerSnapshot, TimerFailure> Cancel(TimerToken token) noexcept;

    /// Publishes expiration when the injected monotonic clock has reached the
    /// exact token's deadline. An early observation changes no state.
    [[nodiscard]] Foundation::Result<TimerExpiration, TimerFailure> TryExpire(TimerToken token) noexcept;

    /// Consumes the exact pending expiration and returns to Disarmed.
    [[nodiscard]] Foundation::Result<TimerSnapshot, TimerFailure> Acknowledge(TimerToken token) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] TimerSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    friend class TimerQueue;

    [[nodiscard]] static TimerFailure MakeFailure(TimerError error, Foundation::UInt64 observedTicks = 0U) noexcept;
    [[nodiscard]] Foundation::Result<TimerArmReceipt, TimerFailure>
    ArmAtLocked(Object::MonotonicTime deadline) noexcept;
    [[nodiscard]] Foundation::Result<TimerSnapshot, TimerFailure> CancelLocked(TimerToken token) noexcept;
    [[nodiscard]] Foundation::Result<TimerExpiration, TimerFailure> TryExpireLocked(TimerToken token) noexcept;
    [[nodiscard]] Foundation::Result<TimerSnapshot, TimerFailure> AcknowledgeLocked(TimerToken token) noexcept;
    [[nodiscard]] Foundation::Result<TimerArmReceipt, TimerFailure>
    ArmAtFromQueue(const TimerQueue& queue, Object::MonotonicTime deadline) noexcept;
    [[nodiscard]] Foundation::Result<TimerSnapshot, TimerFailure> CancelFromQueue(const TimerQueue& queue,
                                                                                  TimerToken token) noexcept;
    [[nodiscard]] Foundation::Result<TimerExpiration, TimerFailure> TryExpireFromQueue(const TimerQueue& queue,
                                                                                       TimerToken token) noexcept;
    [[nodiscard]] TimerError ReleaseFromQueue(const TimerQueue& queue, TimerToken token) noexcept;
    /// Consumes the exact Expired generation of a queue-managed Timer whose
    /// expiration can never be delivered, as a committed delivery followed by
    /// acknowledgement would, and releases queue membership.
    [[nodiscard]] TimerError RetireFromQueue(const TimerQueue& queue, TimerToken token) noexcept;
    [[nodiscard]] TimerError ValidateTokenLocked(TimerToken token) const noexcept;
    [[nodiscard]] TimerSnapshot GetSnapshotLocked() const noexcept;

    IMonotonicClock& m_clock;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Foundation::UInt64 m_deadlineTicks{0U};
    Foundation::UInt64 m_armSequence{0U};
    const TimerQueue* m_timerQueue{nullptr};
    TimerState m_state{TimerState::Uninitialized};
    bool m_initialized{false};
};

static_assert(sizeof(TimerState) == 1U);
static_assert(sizeof(TimerError) == 1U);
static_assert(sizeof(TimerId) == 16U);
static_assert(alignof(TimerId) == 8U);
static_assert(sizeof(TimerToken) == 16U);
static_assert(alignof(TimerToken) == 8U);
static_assert(sizeof(TimerFailure) == 16U);
static_assert(alignof(TimerFailure) == 8U);
static_assert(sizeof(TimerSnapshot) == 32U);
static_assert(alignof(TimerSnapshot) == 8U);
static_assert(sizeof(TimerArmReceipt) == 40U);
static_assert(alignof(TimerArmReceipt) == 8U);
static_assert(sizeof(TimerExpiration) == 32U);
static_assert(alignof(TimerExpiration) == 8U);
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{0U};
        return TimerArmSequencePolicy::TryAdvance(0U, next) && (next == 1U);
    }());
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{17U};
        return !TimerArmSequencePolicy::TryAdvance(TimerArmSequencePolicy::Maximum, next) && (next == 17U);
    }());
} // Namespace Asteria::Kernel::Time

#endif // ASTERIA_KERNEL_TIME_TIMER_HPP_INCLUDED
