// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Scheduler/SchedulingContext.hpp"

#include "Kernel/Object/ObjectRegistry.hpp"

namespace Asteria::Kernel::Scheduler
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::ProcessorMask KnownCpuAffinityMask{
    static_cast<Foundation::ProcessorMask>((1U << MaximumProcessorCount) - 1U)};

[[nodiscard]] constexpr bool IsKnownSchedulingClass(const SchedulingClass value) noexcept
{
    switch (value)
    {
    case SchedulingClass::DeadlineSporadic:
    case SchedulingClass::WeightedFair:
    case SchedulingClass::Background:
    case SchedulingClass::Isolated:
    case SchedulingClass::EmergencyRecovery:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool IsKnownCriticality(const SchedulingCriticality value) noexcept
{
    switch (value)
    {
    case SchedulingCriticality::Low:
    case SchedulingCriticality::Normal:
    case SchedulingCriticality::High:
    case SchedulingCriticality::Emergency:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool TryAdd(const Foundation::UInt64 left, const Foundation::UInt64 right,
                                    Foundation::UInt64& result) noexcept
{
    if (left > (MaximumUInt64 - right))
    {
        return false;
    }
    result = left + right;
    return result != Object::MonotonicTime::Infinite().GetTicks();
}

[[nodiscard]] constexpr Foundation::UInt64 Minimum(const Foundation::UInt64 first,
                                                   const Foundation::UInt64 second) noexcept
{
    return first < second ? first : second;
}

/// Returns dividend modulo divisor with a fixed 64-step restoring division.
/// Preconditions:
/// - divisor is nonzero.
[[nodiscard]] constexpr Foundation::UInt64 UnsignedRemainder(const Foundation::UInt64 dividend,
                                                             const Foundation::UInt64 divisor) noexcept
{
    Foundation::UInt64 remainder{0U};
    for (Foundation::UInt8 iteration{0U}; iteration < 64U; ++iteration)
    {
        const Foundation::UInt8 shift = static_cast<Foundation::UInt8>(63U - iteration);
        const Foundation::UInt64 incoming = (dividend >> shift) & 1U;
        const Foundation::UInt64 distance = divisor - remainder;
        const Foundation::UInt64 reductionThreshold = distance - incoming;
        if (remainder >= reductionThreshold)
        {
            remainder -= reductionThreshold;
        }
        else
        {
            remainder = (remainder << 1U) | incoming;
        }
    }
    return remainder;
}

[[nodiscard]] constexpr SchedulingContextError MapThreadPinError(const Object::ObjectPinError error) noexcept
{
    switch (error)
    {
    case Object::ObjectPinError::Ok:
        return SchedulingContextError::None;

    case Object::ObjectPinError::InvalidArgument:
        return SchedulingContextError::InvalidThread;

    case Object::ObjectPinError::ObjectNotFound:
        return SchedulingContextError::ThreadNotFound;

    case Object::ObjectPinError::TypeMismatch:
        return SchedulingContextError::ThreadTypeMismatch;

    case Object::ObjectPinError::OutputOccupied:
    case Object::ObjectPinError::ReferenceExhausted:
        return SchedulingContextError::ThreadPinUnavailable;
    }
    return SchedulingContextError::ThreadPinUnavailable;
}
} // namespace

SchedulingContextError SchedulingContext::Initialize(const SchedulingContextConfiguration& configuration,
                                                     const Object::ResourceDomainId chargeOwner,
                                                     const Object::MonotonicTime initialTime) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_initialized)
    {
        return SchedulingContextError::AlreadyInitialized;
    }

    const Foundation::ProcessorMask affinityOutsideProfile = static_cast<Foundation::ProcessorMask>(
        configuration.CpuAffinityMask & static_cast<Foundation::ProcessorMask>(~KnownCpuAffinityMask));
    if (!chargeOwner.IsValid() || (configuration.BudgetTicks == 0U) || (configuration.PeriodTicks == 0U) ||
        (configuration.RelativeDeadlineTicks == 0U) ||
        (configuration.BudgetTicks > configuration.RelativeDeadlineTicks) ||
        (configuration.RelativeDeadlineTicks > configuration.PeriodTicks) || (configuration.Weight == 0U) ||
        (configuration.CpuAffinityMask == 0U) || (affinityOutsideProfile != 0U) ||
        !IsKnownSchedulingClass(configuration.Class) || !IsKnownCriticality(configuration.Criticality) ||
        (configuration.Reserved != 0U))
    {
        return SchedulingContextError::InvalidConfiguration;
    }

    Foundation::UInt64 nextReplenishment{0U};
    Foundation::UInt64 absoluteDeadline{0U};
    if (!TryAdd(initialTime.GetTicks(), configuration.PeriodTicks, nextReplenishment) ||
        !TryAdd(initialTime.GetTicks(), configuration.RelativeDeadlineTicks, absoluteDeadline))
    {
        return SchedulingContextError::InvalidConfiguration;
    }

    m_configuration = configuration;
    m_remainingBudgetTicks = configuration.BudgetTicks;
    m_nextReplenishmentTicks = nextReplenishment;
    m_absoluteDeadlineTicks = absoluteDeadline;
    m_lastObservedTicks = initialTime.GetTicks();
    m_initialized = true;
    // The immutable identity is the lock-free publication point. An acquire
    // reader that observes it also observes the complete initialized state.
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return SchedulingContextError::None;
}

SchedulingContextError SchedulingContext::SetInitialHome(const CpuId home) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!home.IsValid())
    {
        return SchedulingContextError::InvalidConfiguration;
    }
    if (!m_initialized)
    {
        return SchedulingContextError::NotInitialized;
    }
    if (m_everRan || m_running)
    {
        return SchedulingContextError::AlreadyRunning;
    }
    if (m_readyQueue != nullptr)
    {
        return SchedulingContextError::InReadyQueue;
    }
    if (m_threadChainLength != 1U)
    {
        return SchedulingContextError::NotBound;
    }
    m_configuration.CpuAffinityMask = Foundation::ProcessorMask{1U} << home.GetValue();
    return SchedulingContextError::None;
}

SchedulingContextError SchedulingContext::SetInitialTime(const Object::MonotonicTime now) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return SchedulingContextError::NotInitialized;
    }
    if (m_everRan || m_running)
    {
        return SchedulingContextError::AlreadyRunning;
    }
    if (m_readyQueue != nullptr)
    {
        return SchedulingContextError::InReadyQueue;
    }
    if (m_threadChainLength != 1U)
    {
        return SchedulingContextError::NotBound;
    }
    Foundation::UInt64 next{0U};
    Foundation::UInt64 deadline{0U};
    if (!TryAdd(now.GetTicks(), m_configuration.PeriodTicks, next) ||
        !TryAdd(now.GetTicks(), m_configuration.RelativeDeadlineTicks, deadline))
    {
        return SchedulingContextError::InvalidConfiguration;
    }
    m_nextReplenishmentTicks = next;
    m_absoluteDeadlineTicks = deadline;
    m_lastObservedTicks = now.GetTicks();
    return SchedulingContextError::None;
}

SchedulingContextError SchedulingContext::Bind(const ThreadId thread) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return SchedulingContextError::NotInitialized;
    }
    if (!thread.IsValid())
    {
        return SchedulingContextError::InvalidThread;
    }
    if (!thread.IsFromRegistry(m_objectRegistry))
    {
        return SchedulingContextError::ThreadRegistryMismatch;
    }
    if (m_threadChainLength != 0U)
    {
        return SchedulingContextError::AlreadyBound;
    }

    const Object::ObjectPinError pinError =
        m_objectRegistry.PinObject(thread.GetObjectId(), Object::ObjectType::Thread, m_threadPins[0U]);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return MapThreadPinError(pinError);
    }

    m_threadChain[0U] = thread;
    m_threadChainLength = 1U;
    return SchedulingContextError::None;
}

SchedulingContextError SchedulingContext::Unbind(const ThreadId thread) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return SchedulingContextError::NotInitialized;
    }
    if (!thread.IsValid())
    {
        return SchedulingContextError::InvalidThread;
    }
    if (!thread.IsFromRegistry(m_objectRegistry))
    {
        return SchedulingContextError::ThreadRegistryMismatch;
    }
    if (m_running)
    {
        return SchedulingContextError::AlreadyRunning;
    }
    if (m_readyQueue != nullptr)
    {
        return SchedulingContextError::InReadyQueue;
    }
    if (m_threadChainLength == 0U)
    {
        return SchedulingContextError::NotBound;
    }
    if (m_threadChainLength != 1U)
    {
        return SchedulingContextError::DonationActive;
    }
    if (!m_threadChain[0U].IsEqualTo(thread))
    {
        return SchedulingContextError::NotCurrentThread;
    }

    m_threadChain[0U] = ThreadId{};
    m_threadChainLength = 0U;
    m_threadPins[0U].Reset();
    return SchedulingContextError::None;
}

SchedulingContextError SchedulingContext::Donate(const ThreadId caller, const ThreadId server) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return SchedulingContextError::NotInitialized;
    }
    if (!caller.IsValid() || !server.IsValid())
    {
        return SchedulingContextError::InvalidThread;
    }
    if (!caller.IsFromRegistry(m_objectRegistry) || !server.IsFromRegistry(m_objectRegistry))
    {
        return SchedulingContextError::ThreadRegistryMismatch;
    }
    if (m_running)
    {
        return SchedulingContextError::AlreadyRunning;
    }
    if (m_readyQueue != nullptr)
    {
        return SchedulingContextError::InReadyQueue;
    }
    if (m_threadChainLength == 0U)
    {
        return SchedulingContextError::NotBound;
    }
    if (!m_threadChain[m_threadChainLength - 1U].IsEqualTo(caller))
    {
        return SchedulingContextError::NotCurrentThread;
    }
    if (ContainsThreadLocked(server))
    {
        return SchedulingContextError::DonationCycle;
    }
    if (m_threadChainLength >= ThreadChainCapacity)
    {
        return SchedulingContextError::DonationDepthExceeded;
    }

    const Object::ObjectPinError pinError =
        m_objectRegistry.PinObject(server.GetObjectId(), Object::ObjectType::Thread, m_threadPins[m_threadChainLength]);
    if (pinError != Object::ObjectPinError::Ok)
    {
        return MapThreadPinError(pinError);
    }

    m_threadChain[m_threadChainLength] = server;
    ++m_threadChainLength;
    return SchedulingContextError::None;
}

SchedulingContextError SchedulingContext::ReturnDonation(const ThreadId server) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return SchedulingContextError::NotInitialized;
    }
    if (!server.IsValid())
    {
        return SchedulingContextError::InvalidThread;
    }
    if (!server.IsFromRegistry(m_objectRegistry))
    {
        return SchedulingContextError::ThreadRegistryMismatch;
    }
    if (m_running)
    {
        return SchedulingContextError::AlreadyRunning;
    }
    if (m_threadChainLength == 0U)
    {
        return SchedulingContextError::NotBound;
    }
    if (m_threadChainLength == 1U)
    {
        return SchedulingContextError::NoDonation;
    }
    if (!m_threadChain[m_threadChainLength - 1U].IsEqualTo(server))
    {
        return SchedulingContextError::NotCurrentThread;
    }

    --m_threadChainLength;
    m_threadChain[m_threadChainLength] = ThreadId{};
    m_threadPins[m_threadChainLength].Reset();
    return SchedulingContextError::None;
}

Foundation::Result<SchedulingContextDispatch, SchedulingContextError>
SchedulingContext::StartRun(const ThreadId thread, const CpuId processor, const Object::MonotonicTime now) noexcept
{
    using ResultType = Foundation::Result<SchedulingContextDispatch, SchedulingContextError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return ResultType::Failure(SchedulingContextError::NotInitialized);
    }
    if (!thread.IsValid())
    {
        return ResultType::Failure(SchedulingContextError::InvalidThread);
    }
    if (!thread.IsFromRegistry(m_objectRegistry))
    {
        return ResultType::Failure(SchedulingContextError::ThreadRegistryMismatch);
    }
    if (!processor.IsValid())
    {
        return ResultType::Failure(SchedulingContextError::InvalidCpu);
    }
    if (m_running)
    {
        return ResultType::Failure(SchedulingContextError::AlreadyRunning);
    }
    if (m_threadChainLength == 0U)
    {
        return ResultType::Failure(SchedulingContextError::NotBound);
    }
    if (!m_threadChain[m_threadChainLength - 1U].IsEqualTo(thread))
    {
        return ResultType::Failure(SchedulingContextError::NotCurrentThread);
    }
    const Foundation::ProcessorMask processorMask =
        static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processor.GetValue());
    if ((m_configuration.CpuAffinityMask & processorMask) == 0U)
    {
        return ResultType::Failure(SchedulingContextError::CpuNotAllowed);
    }

    const Foundation::UInt64 previousBudgetTicks = m_remainingBudgetTicks;
    const Foundation::UInt64 previousReplenishmentTicks = m_nextReplenishmentTicks;
    const Foundation::UInt64 previousDeadlineTicks = m_absoluteDeadlineTicks;
    const Foundation::UInt64 previousObservedTicks = m_lastObservedTicks;
    const SchedulingContextError refreshError = RefreshForTimeLocked(now);
    if (refreshError != SchedulingContextError::None)
    {
        return ResultType::Failure(refreshError);
    }
    if (m_remainingBudgetTicks == 0U)
    {
        m_remainingBudgetTicks = previousBudgetTicks;
        m_nextReplenishmentTicks = previousReplenishmentTicks;
        m_absoluteDeadlineTicks = previousDeadlineTicks;
        m_lastObservedTicks = previousObservedTicks;
        return ResultType::Failure(SchedulingContextError::BudgetUnavailable);
    }
    if (now.GetTicks() >= m_absoluteDeadlineTicks)
    {
        m_remainingBudgetTicks = previousBudgetTicks;
        m_nextReplenishmentTicks = previousReplenishmentTicks;
        m_absoluteDeadlineTicks = previousDeadlineTicks;
        m_lastObservedTicks = previousObservedTicks;
        return ResultType::Failure(SchedulingContextError::DeadlineExpired);
    }

    const Foundation::UInt64 deadlineTicks = m_absoluteDeadlineTicks - now.GetTicks();
    const Foundation::UInt64 replenishmentTicks = m_nextReplenishmentTicks - now.GetTicks();
    const Foundation::UInt64 maximumRunTicks =
        Minimum(m_remainingBudgetTicks, Minimum(deadlineTicks, replenishmentTicks));
    if (maximumRunTicks == 0U)
    {
        m_remainingBudgetTicks = previousBudgetTicks;
        m_nextReplenishmentTicks = previousReplenishmentTicks;
        m_absoluteDeadlineTicks = previousDeadlineTicks;
        m_lastObservedTicks = previousObservedTicks;
        return ResultType::Failure(SchedulingContextError::BudgetUnavailable);
    }

    m_running = true;
    m_runningProcessor = processor;
    m_everRan = true;
    m_runStartTicks = now.GetTicks();
    return ResultType::Success(SchedulingContextDispatch{
        .Thread = thread,
        .MaximumRunTicks = maximumRunTicks,
        .Weight = m_configuration.Weight,
        .Priority = m_configuration.Priority,
        .Criticality = m_configuration.Criticality,
        .Class = m_configuration.Class,
        .Processor = processor,
        .Reserved = 0U,
    });
}

Foundation::Result<SchedulingContextRunReceipt, SchedulingContextError>
SchedulingContext::EndRun(const CpuId processor, const Object::MonotonicTime now) noexcept
{
    using ResultType = Foundation::Result<SchedulingContextRunReceipt, SchedulingContextError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return ResultType::Failure(SchedulingContextError::NotInitialized);
    }
    if (!processor.IsValid())
    {
        return ResultType::Failure(SchedulingContextError::InvalidCpu);
    }
    if (!m_running)
    {
        return ResultType::Failure(SchedulingContextError::NotRunning);
    }
    if (m_runningProcessor.GetValue() != processor.GetValue())
    {
        return ResultType::Failure(SchedulingContextError::WrongCpu);
    }
    if ((now.GetTicks() < m_lastObservedTicks) || (now.GetTicks() < m_runStartTicks))
    {
        return ResultType::Failure(SchedulingContextError::TimeReversed);
    }

    const ThreadId currentThread = m_threadChain[m_threadChainLength - 1U];
    const Foundation::UInt64 runDeadlineTicks = m_absoluteDeadlineTicks;
    const Foundation::UInt64 elapsedTicks = now.GetTicks() - m_runStartTicks;
    const Foundation::UInt64 chargedTicks = Minimum(elapsedTicks, m_remainingBudgetTicks);
    const Foundation::UInt64 overrunTicks = elapsedTicks - chargedTicks;
    Foundation::UInt64 remainingBudgetTicks = m_remainingBudgetTicks - chargedTicks;
    Foundation::UInt64 nextReplenishmentTicks = m_nextReplenishmentTicks;
    Foundation::UInt64 absoluteDeadlineTicks = m_absoluteDeadlineTicks;
    if (now.GetTicks() >= m_nextReplenishmentTicks)
    {
        const Foundation::UInt64 elapsedAfterBoundary = now.GetTicks() - m_nextReplenishmentTicks;
        const Foundation::UInt64 periodRemainder = UnsignedRemainder(elapsedAfterBoundary, m_configuration.PeriodTicks);
        const Foundation::UInt64 currentPeriodStart = now.GetTicks() - periodRemainder;
        if (!TryAdd(currentPeriodStart, m_configuration.PeriodTicks, nextReplenishmentTicks) ||
            !TryAdd(currentPeriodStart, m_configuration.RelativeDeadlineTicks, absoluteDeadlineTicks))
        {
            nextReplenishmentTicks = MaximumUInt64;
            absoluteDeadlineTicks = MaximumUInt64;
            remainingBudgetTicks = 0U;
        }
        else if (elapsedAfterBoundary >= m_configuration.PeriodTicks)
        {
            // A run that covered a whole window after the boundary was never
            // stopped by its budget; the current window stays unavailable.
            remainingBudgetTicks = 0U;
        }
        else
        {
            // A stop observed after the boundary consumed only the time since
            // it: the current window keeps its budget less that overrun, so a
            // stop exactly on the boundary leaves it whole.
            remainingBudgetTicks = m_configuration.BudgetTicks - Minimum(m_configuration.BudgetTicks, periodRemainder);
        }
    }

    m_remainingBudgetTicks = remainingBudgetTicks;
    m_nextReplenishmentTicks = nextReplenishmentTicks;
    m_absoluteDeadlineTicks = absoluteDeadlineTicks;
    m_lastObservedTicks = now.GetTicks();
    m_runStartTicks = 0U;
    m_runningProcessor = CpuId{};
    m_running = false;

    return ResultType::Success(SchedulingContextRunReceipt{
        .Thread = currentThread,
        .ElapsedTicks = elapsedTicks,
        .ChargedTicks = chargedTicks,
        .OverrunTicks = overrunTicks,
        .RemainingBudgetTicks = remainingBudgetTicks,
        .DeadlineMissed = static_cast<Foundation::UInt8>(now.GetTicks() > runDeadlineTicks ? 1U : 0U),
        .BudgetExhausted = static_cast<Foundation::UInt8>(remainingBudgetTicks == 0U ? 1U : 0U),
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    });
}

Foundation::Result<Object::MonotonicTime, SchedulingContextError>
SchedulingContext::GetNextReplenishmentAfter(const Object::MonotonicTime now) const noexcept
{
    using ResultType = Foundation::Result<Object::MonotonicTime, SchedulingContextError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return ResultType::Failure(SchedulingContextError::NotInitialized);
    }
    if (m_running)
    {
        return ResultType::Failure(SchedulingContextError::AlreadyRunning);
    }
    if (now.GetTicks() < m_lastObservedTicks)
    {
        return ResultType::Failure(SchedulingContextError::TimeReversed);
    }
    if (m_nextReplenishmentTicks == Object::MonotonicTime::Infinite().GetTicks())
    {
        return ResultType::Failure(SchedulingContextError::TimeRangeExhausted);
    }
    if (now.GetTicks() < m_nextReplenishmentTicks)
    {
        return ResultType::Success(Object::MonotonicTime{m_nextReplenishmentTicks});
    }

    const Foundation::UInt64 elapsedAfterBoundary = now.GetTicks() - m_nextReplenishmentTicks;
    const Foundation::UInt64 periodRemainder = UnsignedRemainder(elapsedAfterBoundary, m_configuration.PeriodTicks);
    const Foundation::UInt64 currentPeriodStart = now.GetTicks() - periodRemainder;
    Foundation::UInt64 nextReplenishment{0U};
    if (!TryAdd(currentPeriodStart, m_configuration.PeriodTicks, nextReplenishment))
    {
        return ResultType::Failure(SchedulingContextError::TimeRangeExhausted);
    }
    return ResultType::Success(Object::MonotonicTime{nextReplenishment});
}

Object::MonotonicTime SchedulingContext::GetEffectiveDeadline(const Object::MonotonicTime now) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return Object::MonotonicTime::Infinite();
    }

    // A saturated context (see EndRun) keeps Infinite boundaries and zero
    // budget, so both paths below report Infinite for it.
    Foundation::UInt64 budgetTicks = m_remainingBudgetTicks;
    Foundation::UInt64 deadlineTicks = m_absoluteDeadlineTicks;
    Foundation::UInt64 boundaryTicks = m_nextReplenishmentTicks;
    if (now.GetTicks() >= boundaryTicks)
    {
        // The same fixed-phase roll as RefreshForTimeLocked, unpublished.
        const Foundation::UInt64 periodRemainder =
            UnsignedRemainder(now.GetTicks() - boundaryTicks, m_configuration.PeriodTicks);
        const Foundation::UInt64 currentPeriodStart = now.GetTicks() - periodRemainder;
        if (!TryAdd(currentPeriodStart, m_configuration.PeriodTicks, boundaryTicks) ||
            !TryAdd(currentPeriodStart, m_configuration.RelativeDeadlineTicks, deadlineTicks))
        {
            return Object::MonotonicTime::Infinite();
        }
        budgetTicks = m_configuration.BudgetTicks;
    }
    if ((budgetTicks != 0U) && (now.GetTicks() < deadlineTicks))
    {
        return Object::MonotonicTime{deadlineTicks};
    }

    // Windows are fixed-phase: the next one starts at the current boundary.
    Foundation::UInt64 nextDeadlineTicks{0U};
    if (!TryAdd(boundaryTicks, m_configuration.RelativeDeadlineTicks, nextDeadlineTicks))
    {
        return Object::MonotonicTime::Infinite();
    }
    return Object::MonotonicTime{nextDeadlineTicks};
}

bool SchedulingContext::IsReady() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_initialized;
}

SchedulingContextConfiguration SchedulingContext::GetConfiguration() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_configuration;
}

SchedulingContextSnapshot SchedulingContext::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const ThreadId baseThread = m_threadChainLength == 0U ? ThreadId{} : m_threadChain[0U];
    const ThreadId currentThread = m_threadChainLength == 0U ? ThreadId{} : m_threadChain[m_threadChainLength - 1U];
    const Foundation::Size donationDepth = m_threadChainLength == 0U ? 0U : m_threadChainLength - 1U;
    return SchedulingContextSnapshot{
        .BaseThread = baseThread,
        .CurrentThread = currentThread,
        .RemainingBudgetTicks = m_remainingBudgetTicks,
        .NextReplenishmentTicks = m_nextReplenishmentTicks,
        .AbsoluteDeadlineTicks = m_absoluteDeadlineTicks,
        .RunStartTicks = m_runStartTicks,
        .State = GetStateLocked(),
        .RunningProcessor = m_runningProcessor,
        .DonationDepth = static_cast<Foundation::UInt8>(donationDepth),
        .IsBudgetExhausted = static_cast<Foundation::UInt8>(m_remainingBudgetTicks == 0U ? 1U : 0U),
        .Reserved = {0U, 0U, 0U, 0U},
    };
}

Object::ObjectType SchedulingContext::GetObjectType() const noexcept
{
    return Object::ObjectType::SchedulingContext;
}

Object::ResourceDomainId SchedulingContext::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

bool SchedulingContext::TryEnterReadyQueue(const ReadyQueue& queue) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const SchedulingContextState state = GetStateLocked();
    if (((state != SchedulingContextState::Bound) && (state != SchedulingContextState::Donated) &&
         (state != SchedulingContextState::BudgetExhausted)) ||
        (m_readyQueue != nullptr))
    {
        return false;
    }
    m_readyQueue = &queue;
    return true;
}

bool SchedulingContext::IsInReadyQueue(const ReadyQueue& queue) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_readyQueue == &queue;
}

bool SchedulingContext::LeaveReadyQueue(const ReadyQueue& queue) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_readyQueue != &queue)
    {
        return false;
    }
    m_readyQueue = nullptr;
    return true;
}

bool SchedulingContext::ContainsThread(const ThreadId thread) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return ContainsThreadLocked(thread);
}

SchedulingContextError SchedulingContext::RefreshForTimeLocked(const Object::MonotonicTime now) noexcept
{
    if (now.GetTicks() < m_lastObservedTicks)
    {
        return SchedulingContextError::TimeReversed;
    }
    if (now.GetTicks() < m_nextReplenishmentTicks)
    {
        m_lastObservedTicks = now.GetTicks();
        return SchedulingContextError::None;
    }

    const Foundation::UInt64 elapsedAfterBoundary = now.GetTicks() - m_nextReplenishmentTicks;
    const Foundation::UInt64 periodRemainder = UnsignedRemainder(elapsedAfterBoundary, m_configuration.PeriodTicks);
    const Foundation::UInt64 currentPeriodStart = now.GetTicks() - periodRemainder;
    Foundation::UInt64 nextReplenishment{0U};
    Foundation::UInt64 absoluteDeadline{0U};
    if (!TryAdd(currentPeriodStart, m_configuration.PeriodTicks, nextReplenishment) ||
        !TryAdd(currentPeriodStart, m_configuration.RelativeDeadlineTicks, absoluteDeadline))
    {
        return SchedulingContextError::TimeRangeExhausted;
    }

    m_remainingBudgetTicks = m_configuration.BudgetTicks;
    m_nextReplenishmentTicks = nextReplenishment;
    m_absoluteDeadlineTicks = absoluteDeadline;
    m_lastObservedTicks = now.GetTicks();
    return SchedulingContextError::None;
}

SchedulingContextState SchedulingContext::GetStateLocked() const noexcept
{
    if (!m_initialized)
    {
        return SchedulingContextState::Uninitialized;
    }
    if (m_threadChainLength == 0U)
    {
        return SchedulingContextState::Unbound;
    }
    if (m_running)
    {
        return SchedulingContextState::Running;
    }
    if (m_remainingBudgetTicks == 0U)
    {
        return SchedulingContextState::BudgetExhausted;
    }
    if (m_threadChainLength > 1U)
    {
        return SchedulingContextState::Donated;
    }
    return SchedulingContextState::Bound;
}

bool SchedulingContext::ContainsThreadLocked(const ThreadId thread) const noexcept
{
    for (Foundation::Size index{0U}; index < m_threadChainLength; ++index)
    {
        if (m_threadChain[index].IsEqualTo(thread))
        {
            return true;
        }
    }
    return false;
}
} // namespace Asteria::Kernel::Scheduler
