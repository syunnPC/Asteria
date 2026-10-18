// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_SCHEDULING_CONTEXT_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_SCHEDULING_CONTEXT_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Object/CapabilityTypes.hpp"
#include "Kernel/Object/IKernelObject.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

namespace Asteria::Kernel::Scheduler
{
constexpr Foundation::UInt8 MaximumSchedulingContextDonationDepth{8U};

class ReadyQueue;

/// Identifies one Thread generation within one ObjectRegistry.
/// Lifetime:
/// - The originating registry outlives every copy and use of this value,
/// including copies returned in dispatches, receipts, and snapshots.
class ThreadId final
{
public:
    constexpr ThreadId() noexcept = default;

    constexpr ThreadId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
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

    [[nodiscard]] constexpr bool IsEqualTo(const ThreadId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

/// Identifies one SchedulingContext generation within one ObjectRegistry.
/// Lifetime:
/// - The originating registry outlives every copy and use of this value.
class SchedulingContextId final
{
public:
    constexpr SchedulingContextId() noexcept = default;

    constexpr SchedulingContextId(const Object::ObjectRegistry& registry, const Object::ObjectId object) noexcept
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

    [[nodiscard]] constexpr bool IsEqualTo(const SchedulingContextId other) const noexcept
    {
        return (m_registry == other.m_registry) && (m_object.GetValue() == other.m_object.GetValue());
    }

private:
    const Object::ObjectRegistry* m_registry{nullptr};
    Object::ObjectId m_object{};
};

enum class SchedulingClass : Foundation::UInt8
{
    DeadlineSporadic = 1U,
    WeightedFair = 2U,
    Background = 3U,
    Isolated = 4U,
    EmergencyRecovery = 5U,
};

enum class SchedulingCriticality : Foundation::UInt8
{
    Low = 1U,
    Normal = 2U,
    High = 3U,
    Emergency = 4U,
};

enum class SchedulingContextState : Foundation::UInt8
{
    Uninitialized,
    Unbound,
    Bound,
    Donated,
    Running,
    BudgetExhausted,
};

enum class SchedulingContextError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    AlreadyInitialized,
    NotInitialized,
    InvalidThread,
    ThreadRegistryMismatch,
    ThreadNotFound,
    ThreadTypeMismatch,
    ThreadPinUnavailable,
    AlreadyBound,
    NotBound,
    NotCurrentThread,
    DonationActive,
    NoDonation,
    DonationDepthExceeded,
    DonationCycle,
    AlreadyRunning,
    NotRunning,
    InvalidCpu,
    CpuNotAllowed,
    WrongCpu,
    TimeReversed,
    TimeRangeExhausted,
    BudgetUnavailable,
    DeadlineExpired,
    InReadyQueue,
};

struct SchedulingContextConfiguration
{
    Foundation::UInt64 BudgetTicks;
    Foundation::UInt64 PeriodTicks;
    Foundation::UInt64 RelativeDeadlineTicks;
    Foundation::UInt16 Weight;
    Foundation::UInt8 Priority;
    SchedulingCriticality Criticality;
    Foundation::ProcessorMask CpuAffinityMask;
    SchedulingClass Class;
    Foundation::UInt16 Reserved;
};

struct SchedulingContextDispatch
{
    ThreadId Thread;
    Foundation::UInt64 MaximumRunTicks;
    Foundation::UInt16 Weight;
    Foundation::UInt8 Priority;
    SchedulingCriticality Criticality;
    SchedulingClass Class;
    CpuId Processor;
    Foundation::UInt16 Reserved;
};

struct SchedulingContextRunReceipt
{
    ThreadId Thread;
    /// Actual processor time between StartRun and EndRun.
    Foundation::UInt64 ElapsedTicks;
    /// Portion removed from the budget admitted by StartRun.
    Foundation::UInt64 ChargedTicks;
    /// ElapsedTicks beyond that admitted budget. Resource accounting uses
    /// ElapsedTicks, not ChargedTicks, so late preemption remains visible.
    Foundation::UInt64 OverrunTicks;
    Foundation::UInt64 RemainingBudgetTicks;
    Foundation::UInt8 DeadlineMissed;
    Foundation::UInt8 BudgetExhausted;
    Foundation::UInt8 Reserved[6];
};

struct SchedulingContextSnapshot
{
    ThreadId BaseThread;
    ThreadId CurrentThread;
    Foundation::UInt64 RemainingBudgetTicks;
    Foundation::UInt64 NextReplenishmentTicks;
    Foundation::UInt64 AbsoluteDeadlineTicks;
    Foundation::UInt64 RunStartTicks;
    SchedulingContextState State;
    CpuId RunningProcessor;
    Foundation::UInt8 DonationDepth;
    Foundation::UInt8 IsBudgetExhausted;
    Foundation::UInt8 Reserved[4];
};

/// Owns one bounded CPU budget independently of any thread. A context can be bound to one base
/// thread and donated through a bounded, acyclic LIFO call chain. Only the current thread may run
/// on an allowed CPU. ThreadId carries one ObjectRegistry origin and generation, not a user handle.
/// Bind and Donate require that exact registry identity to name a live ObjectType::Thread and
/// retain it through ObjectPin. The caller supplies monotonic time samples; decreasing samples are
/// rejected. State and accounting operations are internally serialized and are forbidden from
/// interrupt context or same-CPU reentry while the lock may already be held. Identity metadata
/// remains lock-free: a concurrent pre-publication GetChargeOwner observes either invalid identity
/// or the complete identity published by a successful Initialize, after which it is immutable.
class SchedulingContext final : public Object::IKernelObject
{
public:
    /// Preconditions:
    /// - objectRegistry outlives this context, every thread pin it owns, and
    /// every ThreadId copied from its dispatches, receipts, or snapshots.
    explicit SchedulingContext(Object::ObjectRegistry& objectRegistry) noexcept : m_objectRegistry{objectRegistry}
    {
    }

    /// Releases every retained Thread pin.
    /// Preconditions:
    /// - No run is active and no dispatcher retains or accesses this context.
    /// - No concurrent operation accesses this object during destruction.
    ~SchedulingContext() noexcept = default;

    SchedulingContext(const SchedulingContext&) = delete;
    SchedulingContext& operator=(const SchedulingContext&) = delete;
    SchedulingContext(SchedulingContext&&) = delete;
    SchedulingContext& operator=(SchedulingContext&&) = delete;

    /// Initializes the CPU-budget window exactly once.
    /// A failed first attempt leaves the context uninitialized; a later call
    /// returns AlreadyInitialized without changing established state.
    [[nodiscard]] SchedulingContextError Initialize(const SchedulingContextConfiguration& configuration,
                                                    Object::ResourceDomainId chargeOwner,
                                                    Object::MonotonicTime initialTime) noexcept;

    // Home time before the first enqueue/run.
    [[nodiscard]] SchedulingContextError SetInitialTime(Object::MonotonicTime now) noexcept;
    // Before publication/first enqueue only.
    [[nodiscard]] SchedulingContextError SetInitialHome(CpuId home) noexcept;

    [[nodiscard]] SchedulingContextError Bind(ThreadId thread) noexcept;

    /// Releases the base thread. A context with a ready-queue placement
    /// (ready, claimed, or reserved) is rejected with InReadyQueue.
    [[nodiscard]] SchedulingContextError Unbind(ThreadId thread) noexcept;

    /// Transfers use of this context from caller to server until ReturnDonation.
    /// The IPC dispatcher authorizes the edge. This context already retains the
    /// caller and acquires its own generation-specific pin for the server.
    /// A context with a ready-queue placement is rejected with InReadyQueue.
    [[nodiscard]] SchedulingContextError Donate(ThreadId caller, ThreadId server) noexcept;

    /// Returns one donation edge to its immediate caller. This remains
    /// permitted while a reply reservation holds the context: the reserved
    /// caller becomes the current thread before CommitReservedEnqueue
    /// revalidates the pair.
    [[nodiscard]] SchedulingContextError ReturnDonation(ThreadId server) noexcept;

    /// Begins accounting for the current thread on one permitted CPU.
    /// Every failure preserves the complete context and time watermark.
    [[nodiscard]] Foundation::Result<SchedulingContextDispatch, SchedulingContextError>
    StartRun(ThreadId thread, CpuId processor, Object::MonotonicTime now) noexcept;

    /// Stops the active run and charges its admitted budget. DeadlineMissed
    /// refers to the deadline fixed by StartRun. If late preemption enters the
    /// next period, that window's budget is reduced by the time already spent
    /// in it, so a stop exactly on the boundary leaves it whole; a run that
    /// covered a whole later window leaves the current window unavailable.
    /// Elapsed time cannot be refused: if the window after the stop is
    /// unrepresentable, the run still ends and the context keeps zero budget
    /// with Infinite deadline and replenishment, so it is never admitted again.
    [[nodiscard]] Foundation::Result<SchedulingContextRunReceipt, SchedulingContextError>
    EndRun(CpuId processor, Object::MonotonicTime now) noexcept;

    /// Returns the first replenishment boundary strictly after now without
    /// changing accounting state. This remains correct after StartRun rolls a
    /// provisional refresh back because the current deadline already expired.
    [[nodiscard]] Foundation::Result<Object::MonotonicTime, SchedulingContextError>
    GetNextReplenishmentAfter(Object::MonotonicTime now) const noexcept;

    /// Returns the absolute deadline of the earliest budget window, at or after now, in which
    /// StartRun can admit this context: the deadline of the window containing now while that window
    /// still has budget and now precedes its deadline, otherwise the deadline of the following
    /// window. The lazy refresh StartRun would perform is computed, not published. MonotonicTime:
    /// Infinite means that no such window is representable or that the context is uninitialized.
    /// While the context state is unchanged, the result stays constant for every later sample below
    /// it.
    [[nodiscard]] Object::MonotonicTime GetEffectiveDeadline(Object::MonotonicTime now) const noexcept;

    [[nodiscard]] bool IsReady() const noexcept;
    [[nodiscard]] SchedulingContextConfiguration GetConfiguration() const noexcept;
    [[nodiscard]] SchedulingContextSnapshot GetSnapshot() const noexcept;

    [[nodiscard]] Object::ObjectType GetObjectType() const noexcept override;
    [[nodiscard]] Object::ResourceDomainId GetChargeOwner() const noexcept override;

private:
    friend class ReadyQueue;

    static constexpr Foundation::Size ThreadChainCapacity{
        static_cast<Foundation::Size>(MaximumSchedulingContextDonationDepth) + 1U};

    [[nodiscard]] SchedulingContextError RefreshForTimeLocked(Object::MonotonicTime now) noexcept;
    [[nodiscard]] bool TryEnterReadyQueue(const ReadyQueue& queue) noexcept;
    [[nodiscard]] bool IsInReadyQueue(const ReadyQueue& queue) const noexcept;
    [[nodiscard]] bool LeaveReadyQueue(const ReadyQueue& queue) noexcept;
    [[nodiscard]] bool ContainsThread(ThreadId thread) const noexcept;
    [[nodiscard]] SchedulingContextState GetStateLocked() const noexcept;
    [[nodiscard]] bool ContainsThreadLocked(ThreadId thread) const noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    mutable Platform::Synchronization::SpinLock m_synchronization;
    SchedulingContextConfiguration m_configuration{};
    Platform::Synchronization::AtomicUInt64 m_chargeOwnerValue;
    ThreadId m_threadChain[ThreadChainCapacity]{};
    Object::ObjectPin m_threadPins[ThreadChainCapacity]{};
    Foundation::Size m_threadChainLength{0U};
    Foundation::UInt64 m_remainingBudgetTicks{0U};
    Foundation::UInt64 m_nextReplenishmentTicks{0U};
    Foundation::UInt64 m_absoluteDeadlineTicks{0U};
    Foundation::UInt64 m_lastObservedTicks{0U};
    Foundation::UInt64 m_runStartTicks{0U};
    const ReadyQueue* m_readyQueue{nullptr};
    CpuId m_runningProcessor{};
    bool m_initialized{false};
    bool m_running{false};
    bool m_everRan{false};
};

static_assert(sizeof(ThreadId) == 16U);
static_assert(alignof(ThreadId) == 8U);
static_assert(sizeof(SchedulingContextId) == 16U);
static_assert(alignof(SchedulingContextId) == 8U);
static_assert(sizeof(SchedulingClass) == 1U);
static_assert(sizeof(SchedulingCriticality) == 1U);
static_assert(sizeof(SchedulingContextState) == 1U);
static_assert(sizeof(SchedulingContextError) == 1U);
static_assert(sizeof(SchedulingContextConfiguration) == 48U);
static_assert(alignof(SchedulingContextConfiguration) == 8U);
static_assert(sizeof(SchedulingContextDispatch) == 32U);
static_assert(alignof(SchedulingContextDispatch) == 8U);
static_assert(sizeof(SchedulingContextRunReceipt) == 56U);
static_assert(alignof(SchedulingContextRunReceipt) == 8U);
static_assert(sizeof(SchedulingContextSnapshot) == 72U);
static_assert(alignof(SchedulingContextSnapshot) == 8U);
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_KERNEL_SCHEDULER_SCHEDULING_CONTEXT_HPP_INCLUDED
