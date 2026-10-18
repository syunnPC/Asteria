// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_THREAD_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_THREAD_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Scheduler
{
class ReadyQueue;

enum class ThreadState : Foundation::UInt8
{
    Uninitialized,
    Dormant,
    Ready,
    Running,
    Blocked,
    Exited,
};

enum class ThreadRunDisposition : Foundation::UInt8
{
    Ready = 1U,
    Blocked = 2U,
    Exited = 3U,
};

enum class ThreadError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    NotInitialized,
    InvalidCpu,
    InvalidDisposition,
    InvalidState,
    AlreadyRunning,
    NotRunning,
    WrongCpu,
    InvalidBlockToken,
    StaleBlockToken,
    BlockSequenceExhausted,
    AlreadyExited,
    RunSequenceExhausted,
    InReadyQueue,
};

class ThreadRunSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    ThreadRunSequencePolicy() = delete;

    /// Advances current without wrapping. Failure leaves next unchanged.
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

class ThreadBlockSequencePolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    ThreadBlockSequencePolicy() = delete;

    /// Advances current without wrapping. Failure leaves next unchanged.
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

class Thread;

/// Names one block generation of one immovable Thread.
/// Lifetime:
/// - The originating Thread outlives every copy and use of this token.
class ThreadBlockToken final
{
public:
    constexpr ThreadBlockToken() noexcept = default;

    [[nodiscard]] constexpr Foundation::UInt64 GetValue() const noexcept
    {
        return m_value;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return (m_thread != nullptr) && (m_value != 0U);
    }

    [[nodiscard]] constexpr bool IsEqualTo(const ThreadBlockToken other) const noexcept
    {
        return (m_thread == other.m_thread) && (m_value == other.m_value);
    }

private:
    friend class Thread;

    constexpr ThreadBlockToken(const Thread& thread, const Foundation::UInt64 value) noexcept
        : m_thread{&thread}, m_value{value}
    {
    }

    const Thread* m_thread{nullptr};
    Foundation::UInt64 m_value{0U};
};

struct ThreadSnapshot
{
    ThreadBlockToken BlockToken;
    /// Nonzero after the first successful BeginRun and advanced by every later
    /// successful admission. The sequence never wraps or aliases a prior run.
    Foundation::UInt64 RunSequence;
    Foundation::UInt64 ExitCode;
    ThreadState State;
    CpuId RunningProcessor;
    Foundation::UInt8 Reserved[6];
};

/// Owns the architecture-neutral lifecycle of one schedulable thread. Saved registers, an address
/// space, a kernel stack, and queue placement are separate objects. DispatchCoordinator coordinates
/// this state machine with a SchedulingContext; direct callers must not create two simultaneous
/// runs. State operations are internally serialized and may spin; callers prevent same-CPU reentry
/// and do not call them from interrupt context. Identity metadata accessors remain lock-free,
/// bounded, and safe before publication.
class Thread final : public Object::IKernelObject
{
public:
    constexpr Thread() noexcept = default;

    /// Preconditions:
    /// - No run is active and no dispatcher or wait queue retains or accesses
    /// this object, and no ThreadBlockToken copy remains usable.
    /// - No concurrent operation accesses this object during destruction.
    ~Thread() noexcept = default;

    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) = delete;
    Thread& operator=(Thread&&) = delete;

    /// Initializes immutable resource ownership exactly once. A failed first
    /// attempt changes no state and can be retried.
    [[nodiscard]] ThreadError Initialize(Object::ResourceDomainId chargeOwner) noexcept;

    /// Moves a newly initialized dormant thread into the ready state.
    [[nodiscard]] ThreadError MakeReady() noexcept;

    /// Claims a ready thread for one processor and advances RunSequence.
    /// Sequence exhaustion leaves the ready thread unchanged.
    [[nodiscard]] ThreadError BeginRun(CpuId processor) noexcept;

    /// Completes the active run as ready, blocked, or exited. A blocked
    /// completion publishes a new token; an exited completion records exitCode.
    [[nodiscard]] Foundation::Result<ThreadSnapshot, ThreadError>
    FinishRun(CpuId processor, ThreadRunDisposition disposition, Foundation::UInt64 exitCode = 0U) noexcept;

    /// Makes the exact currently blocked instance ready. Older tokens cannot
    /// wake a later block instance.
    [[nodiscard]] ThreadError Wake(ThreadBlockToken token) noexcept;

    /// Terminates a dormant, ready, or blocked thread without dispatching it.
    /// A thread with a ready-queue placement (ready, claimed, or reserved)
    /// is rejected with InReadyQueue; its owner removes the placement first.
    [[nodiscard]] ThreadError Terminate(Foundation::UInt64 exitCode) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] ThreadSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    friend class ReadyQueue;

    [[nodiscard]] bool TryEnterReadyQueue(const ReadyQueue& queue) noexcept;
    [[nodiscard]] bool TryReserveReadyQueue(const ReadyQueue& queue, ThreadBlockToken blockToken) noexcept;
    [[nodiscard]] bool IsInReadyQueue(const ReadyQueue& queue) const noexcept;
    [[nodiscard]] bool LeaveReadyQueue(const ReadyQueue& queue) noexcept;
    [[nodiscard]] ThreadSnapshot GetSnapshotLocked() const noexcept;

    mutable Platform::Synchronization::SpinLock m_synchronization;
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    Foundation::UInt64 m_runSequence{0U};
    Foundation::UInt64 m_blockSequence{0U};
    Foundation::UInt64 m_exitCode{0U};
    const ReadyQueue* m_readyQueue{nullptr};
    ThreadState m_state{ThreadState::Uninitialized};
    CpuId m_runningProcessor{};
    bool m_initialized{false};
};

static_assert(sizeof(ThreadState) == 1U);
static_assert(sizeof(ThreadRunDisposition) == 1U);
static_assert(sizeof(ThreadError) == 1U);
static_assert(sizeof(ThreadBlockToken) == 16U);
static_assert(alignof(ThreadBlockToken) == 8U);
static_assert(sizeof(ThreadSnapshot) == 40U);
static_assert(alignof(ThreadSnapshot) == 8U);
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{0U};
        return ThreadRunSequencePolicy::TryAdvance(0U, next) && (next == 1U);
    }());
static_assert(
    []() constexpr
    {
        Foundation::UInt64 next{17U};
        return !ThreadRunSequencePolicy::TryAdvance(ThreadRunSequencePolicy::Maximum, next) && (next == 17U);
    }());
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_KERNEL_SCHEDULER_THREAD_HPP_INCLUDED
