// SPDX-License-Identifier: Apache-2.0

#include "Thread.hpp"

namespace Asteria::Kernel::Scheduler
{
namespace
{
[[nodiscard]] constexpr bool IsKnownDisposition(const ThreadRunDisposition disposition) noexcept
{
    switch (disposition)
    {
    case ThreadRunDisposition::Ready:
    case ThreadRunDisposition::Blocked:
    case ThreadRunDisposition::Exited:
        return true;
    }
    return false;
}
} // namespace

ThreadError Thread::Initialize(const Object::ResourceDomainId chargeOwner) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_initialized)
    {
        return ThreadError::AlreadyInitialized;
    }
    if (!chargeOwner.IsValid())
    {
        return ThreadError::InvalidConfiguration;
    }

    m_state = ThreadState::Dormant;
    m_initialized = true;
    m_chargeOwnerValue.StoreRelease(chargeOwner.GetValue());
    return ThreadError::None;
}

ThreadError Thread::MakeReady() noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return ThreadError::NotInitialized;
    }
    if (m_state == ThreadState::Exited)
    {
        return ThreadError::AlreadyExited;
    }
    if (m_state == ThreadState::Running)
    {
        return ThreadError::AlreadyRunning;
    }
    if (m_state != ThreadState::Dormant)
    {
        return ThreadError::InvalidState;
    }

    m_state = ThreadState::Ready;
    return ThreadError::None;
}

ThreadError Thread::BeginRun(const CpuId processor) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return ThreadError::NotInitialized;
    }
    if (!processor.IsValid())
    {
        return ThreadError::InvalidCpu;
    }
    if (m_state == ThreadState::Exited)
    {
        return ThreadError::AlreadyExited;
    }
    if (m_state == ThreadState::Running)
    {
        return ThreadError::AlreadyRunning;
    }
    if (m_state != ThreadState::Ready)
    {
        return ThreadError::InvalidState;
    }

    Foundation::UInt64 nextRunSequence = m_runSequence;
    if (!ThreadRunSequencePolicy::TryAdvance(m_runSequence, nextRunSequence))
    {
        return ThreadError::RunSequenceExhausted;
    }

    m_runSequence = nextRunSequence;
    m_runningProcessor = processor;
    m_state = ThreadState::Running;
    return ThreadError::None;
}

Foundation::Result<ThreadSnapshot, ThreadError> Thread::FinishRun(const CpuId processor,
                                                                  const ThreadRunDisposition disposition,
                                                                  const Foundation::UInt64 exitCode) noexcept
{
    using ResultType = Foundation::Result<ThreadSnapshot, ThreadError>;

    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return ResultType::Failure(ThreadError::NotInitialized);
    }
    if (!processor.IsValid())
    {
        return ResultType::Failure(ThreadError::InvalidCpu);
    }
    if (!IsKnownDisposition(disposition) || ((disposition != ThreadRunDisposition::Exited) && (exitCode != 0U)))
    {
        return ResultType::Failure(ThreadError::InvalidDisposition);
    }
    if (m_state != ThreadState::Running)
    {
        return ResultType::Failure(ThreadError::NotRunning);
    }
    if (m_runningProcessor.GetValue() != processor.GetValue())
    {
        return ResultType::Failure(ThreadError::WrongCpu);
    }
    Foundation::UInt64 nextBlockSequence = m_blockSequence;
    if ((disposition == ThreadRunDisposition::Blocked) &&
        !ThreadBlockSequencePolicy::TryAdvance(m_blockSequence, nextBlockSequence))
    {
        return ResultType::Failure(ThreadError::BlockSequenceExhausted);
    }

    m_runningProcessor = CpuId{};
    switch (disposition)
    {
    case ThreadRunDisposition::Ready:
        m_state = ThreadState::Ready;
        break;

    case ThreadRunDisposition::Blocked:
        m_blockSequence = nextBlockSequence;
        m_state = ThreadState::Blocked;
        break;

    case ThreadRunDisposition::Exited:
        m_exitCode = exitCode;
        m_state = ThreadState::Exited;
        break;
    }

    return ResultType::Success(GetSnapshotLocked());
}

ThreadError Thread::Wake(const ThreadBlockToken token) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return ThreadError::NotInitialized;
    }
    if (!token.IsValid() || (token.m_thread != this))
    {
        return ThreadError::InvalidBlockToken;
    }
    if (m_state == ThreadState::Exited)
    {
        return ThreadError::AlreadyExited;
    }
    if (m_state != ThreadState::Blocked)
    {
        return ThreadError::InvalidState;
    }
    if (token.GetValue() != m_blockSequence)
    {
        return ThreadError::StaleBlockToken;
    }

    m_state = ThreadState::Ready;
    return ThreadError::None;
}

ThreadError Thread::Terminate(const Foundation::UInt64 exitCode) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (!m_initialized)
    {
        return ThreadError::NotInitialized;
    }
    if (m_state == ThreadState::Exited)
    {
        return ThreadError::AlreadyExited;
    }
    if (m_state == ThreadState::Running)
    {
        return ThreadError::AlreadyRunning;
    }
    if (m_readyQueue != nullptr)
    {
        return ThreadError::InReadyQueue;
    }

    m_runningProcessor = CpuId{};
    m_exitCode = exitCode;
    m_state = ThreadState::Exited;
    return ThreadError::None;
}

bool Thread::IsInitialized() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_initialized;
}

ThreadSnapshot Thread::GetSnapshot() const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return GetSnapshotLocked();
}

Object::ObjectType Thread::GetObjectType() const noexcept
{
    return Object::ObjectType::Thread;
}

Object::ResourceDomainId Thread::GetChargeOwner() const noexcept
{
    return Object::ResourceDomainId{m_chargeOwnerValue.LoadAcquire()};
}

bool Thread::TryEnterReadyQueue(const ReadyQueue& queue) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if ((m_state != ThreadState::Ready) || (m_readyQueue != nullptr))
    {
        return false;
    }
    m_readyQueue = &queue;
    return true;
}

bool Thread::TryReserveReadyQueue(const ReadyQueue& queue, const ThreadBlockToken blockToken) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    const ThreadBlockToken current =
        m_state == ThreadState::Blocked ? ThreadBlockToken{*this, m_blockSequence} : ThreadBlockToken{};
    if (!current.IsEqualTo(blockToken) || (m_readyQueue != nullptr))
    {
        return false;
    }
    m_readyQueue = &queue;
    return true;
}

bool Thread::IsInReadyQueue(const ReadyQueue& queue) const noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    return m_readyQueue == &queue;
}

bool Thread::LeaveReadyQueue(const ReadyQueue& queue) noexcept
{
    Platform::Synchronization::SpinLockGuard guard{m_synchronization};
    if (m_readyQueue != &queue)
    {
        return false;
    }
    m_readyQueue = nullptr;
    return true;
}

ThreadSnapshot Thread::GetSnapshotLocked() const noexcept
{
    return ThreadSnapshot{
        .BlockToken = m_state == ThreadState::Blocked ? ThreadBlockToken{*this, m_blockSequence} : ThreadBlockToken{},
        .RunSequence = m_runSequence,
        .ExitCode = m_exitCode,
        .State = m_state,
        .RunningProcessor = m_runningProcessor,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}
} // namespace Asteria::Kernel::Scheduler
