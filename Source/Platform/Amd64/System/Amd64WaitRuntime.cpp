// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/System/Amd64WaitRuntime.hpp"

#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"

namespace Asteria::Platform::Amd64::System
{
using Foundation::UInt64;

namespace
{
// Each suspended wait's timer expires this long after the wait was armed; the
// idle loop waits at most WaitTimeoutTicks beyond that deadline.
constexpr UInt64 WaitTimerDelayTicks{100'000'000U};
constexpr UInt64 WaitTimeoutTicks{1'000'000'000U};

[[nodiscard]] Diagnostics::Amd64WaitEvidence& Evidence() noexcept
{
    return Diagnostics::GetAmd64EvidenceRecord().Wait;
}

/// The retained slot of the resumption in progress, or null once the slots are used up.
[[nodiscard]] Diagnostics::Amd64WaitResumptionEvidence* PendingResumption() noexcept
{
    auto& evidence = Evidence();
    return evidence.ResumedWaits < Diagnostics::Amd64EvidenceWaitSlots ? &evidence.Resumptions[evidence.ResumedWaits]
                                                                       : nullptr;
}
} // namespace

Amd64WaitRuntime::Amd64WaitRuntime(Kernel::Object::ObjectRegistry& registry, Kernel::Time::TimerQueue& queue,
                                   Time::Amd64ApicTimer& timer, Kernel::Scheduler::ProcessorRunController& controller,
                                   Scheduler::Amd64ProcessorRunLoop& loop, Kernel::Scheduler::Thread& thread,
                                   Scheduler::Amd64KernelContext& context) noexcept
    : m_registry{registry}, m_queue{queue}, m_timer{timer}, m_controller{controller}, m_loop{loop}, m_thread{thread},
      m_context{context}, m_object{timer}, m_waitSet{registry}, m_deferred{registry, queue},
      m_wait{registry, controller.GetWakeFacade()}, m_dispatcher{registry, m_wait}
{
}

bool Amd64WaitRuntime::Initialize(Kernel::Object::CapabilitySpace& capabilities,
                                  Kernel::Object::RevocationRegistry& revocations) noexcept
{
    if (m_initialized || m_object.Initialize(Kernel::Object::ResourceDomainId{1U}) != Kernel::Time::TimerError::None ||
        m_notification.Initialize(Kernel::Object::ResourceDomainId{1U}) != Kernel::Ipc::NotificationError::None ||
        m_waitSet.Initialize(Kernel::Object::ResourceDomainId{1U}) != Kernel::Ipc::WaitSetError::None)
    {
        return false;
    }
    Kernel::Object::RootCapabilityIssuer issuer{m_registry};
    const auto timer = issuer.CreateObject(m_object);
    const auto notification = issuer.CreateObject(m_notification);
    const auto waitSet = issuer.CreateObject(m_waitSet);
    const auto root = revocations.CreateRoot();
    if (!timer.HasValue() || !notification.HasValue() || !waitSet.HasValue() || !root.HasValue())
    {
        return false;
    }
    m_timerId = {m_registry, timer.GetValue()};
    const Kernel::Ipc::NotificationId notificationId{m_registry, notification.GetValue()};
    if (!m_deferred.Register(m_timerId, m_object, notificationId, m_notification).HasValue() ||
        !m_waitSet.RegisterNotification(notificationId, m_notification, BootstrapWaitTag).HasValue())
    {
        return false;
    }
    const auto capability = issuer.Mint(capabilities, waitSet.GetValue(),
                                        Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Wait},
                                        root.GetValue(), Kernel::Object::MonotonicTime{~UInt64{0U}}, 0U);
    if (!capability.HasValue())
    {
        return false;
    }
    m_capability = capability.GetValue();
    m_initialized = true;
    Evidence().RegisteredTag = BootstrapWaitTag;
    Diagnostics::PublishAmd64Evidence(Evidence().Published);
    return true;
}

void Amd64WaitRuntime::OnTimerInterrupt() noexcept
{
    m_interrupts.StoreRelease(m_interrupts.LoadAcquire() + 1U);
}

bool Amd64WaitRuntime::Arm() noexcept
{
    const UInt64 now = m_timer.GetTicks();
    if (now > ~UInt64{0U} - (WaitTimerDelayTicks + WaitTimeoutTicks) || m_interrupts.LoadAcquire() == ~UInt64{0U})
    {
        return false;
    }
    m_deadline = now + WaitTimerDelayTicks;
    m_armedInterrupts = m_interrupts.LoadAcquire();
    auto* const resumption = PendingResumption();
    if (resumption != nullptr)
    {
        resumption->DeadlineTicks = m_deadline;
    }
    return m_queue.Arm(m_timerId, m_object, Kernel::Object::MonotonicTime{m_deadline}).HasValue();
}

bool Amd64WaitRuntime::RearmDueTimer() noexcept
{
    return m_suspended && Arm();
}

bool Amd64WaitRuntime::IsTimerDue() const noexcept
{
    return m_suspended && (m_timer.GetTicks() >= m_deadline);
}

bool Amd64WaitRuntime::Dispatch(const Kernel::System::SystemCallRequest& request,
                                Kernel::System::SystemCallResponse& response) noexcept
{
    if (!m_initialized || m_suspended ||
        !Kernel::System::WaitSetSystemCallIngress::Dispatch(m_loop, m_dispatcher, request, m_result, m_failure))
    {
        return false;
    }
    auto& evidence = Evidence();
    ++evidence.WaitCalls;
    if (m_result.Disposition == Kernel::System::WaitSetSystemCallDisposition::SuspendPrepared)
    {
        if (!Arm())
        {
            return false;
        }
        ++evidence.SuspendedWaits;
        m_suspended = true;
        if (!m_loop.FinishWaitAndDispatch(m_thread, m_context, m_wait, m_receipt, m_switchFailure) ||
            m_receipt.ThreadStorage != &m_thread ||
            m_receipt.State != Kernel::Scheduler::ProcessorKernelRunLoopState::Active ||
            !Kernel::System::WaitSetSystemCallIngress::ConsumeCompletion(m_loop, m_dispatcher, m_result, m_failure))
        {
            return false;
        }
        m_suspended = false;
        // The response goes back to the image unchanged; its guest test program
        // checks it, and the host checks the record.
        auto* const resumption = PendingResumption();
        if (resumption != nullptr)
        {
            resumption->ResponseError = static_cast<UInt64>(m_result.Response.Error);
            resumption->ResponseValue0 = m_result.Response.Value0;
            resumption->ResponseValue1 = m_result.Response.Value1;
        }
        ++evidence.ResumedWaits;
    }
    Diagnostics::PublishAmd64Evidence(evidence.Published);
    response = m_result.Response;
    return true;
}

bool Amd64WaitRuntime::CompleteFromIdle() noexcept
{
    if (m_deferred.DrainDueRetained() != Kernel::Time::DeferredTimerDrainOutcome::Drained)
    {
        return Reject(3U);
    }
    const auto& drained = m_deferred.GetRetainedDrainReceipt();
    auto* const resumption = PendingResumption();
    if (resumption != nullptr)
    {
        resumption->DeliveredCount = drained.DeliveredCount;
        resumption->DrainStop = static_cast<UInt64>(drained.Stop);
        resumption->DeliveredOwnTimer = (drained.DeliveredCount != 0U) &&
                                                drained.FirstTimerIdentity.IsEqualTo(m_timerId) &&
                                                drained.LastTimerIdentity.IsEqualTo(m_timerId)
                                            ? 1U
                                            : 0U;
        resumption->QueueNextDeadlineTicks = drained.QueueState.NextDeadlineTicks;
        resumption->QueueScheduledEntries = drained.QueueState.ScheduledEntries;
        resumption->QueuePendingEntries = drained.QueueState.PendingEntries;
        resumption->QueueClaimedEntries = drained.QueueState.ClaimedEntries;
    }
    const auto deadline = m_controller.RefreshTimerQueueDeadline(m_timer.GetTicks());
    if (!deadline.HasValue())
    {
        return Reject(4U);
    }
    // The wait must have completed before its thread can be dispatched again.
    const auto woke = m_wait.Poll();
    if (!woke.HasValue() || woke.GetValue().State != Kernel::Ipc::WaitSetThreadDispatcherState::Completed)
    {
        return Reject(static_cast<Foundation::UInt8>(
            0x50U | static_cast<Foundation::UInt8>(
                        woke.HasValue() ? woke.GetValue().State : Kernel::Ipc::WaitSetThreadDispatcherState::Faulted)));
    }
    if (resumption != nullptr)
    {
        resumption->DeadlineState = static_cast<UInt64>(deadline.GetValue().State);
        resumption->CompletedTag = woke.GetValue().Tag;
        resumption->CompletedRegistrationSequence = woke.GetValue().RegistrationSequence;
    }
    return true;
}

bool Amd64WaitRuntime::ResumeFromIdle() noexcept
{
    if (!m_suspended || m_wait.GetSnapshot().State != Kernel::Ipc::WaitSetThreadDispatcherState::Blocked ||
        m_loop.GetSnapshot().State != Kernel::Scheduler::ProcessorKernelRunLoopState::Idle ||
        m_thread.GetSnapshot().State != Kernel::Scheduler::ThreadState::Blocked)
    {
        return Reject(1U);
    }
    for (Foundation::Size attempt{0U}; attempt < 20'000'000U; ++attempt)
    {
#if defined(ASTERIA_TARGET_AMD64)
        __asm__ __volatile__("sti; pause; cli" : : : "memory");
#endif
        // The timer driver publishes an interrupt only once its deadline is due.
        const UInt64 interrupts = m_interrupts.LoadAcquire();
        if ((interrupts != m_armedInterrupts) && (m_timer.GetTicks() < m_deadline))
        {
            // A kernel-mode tick counts here whichever deadline fired: one
            // before the wait's own (a budget replenishment) only made a
            // thread runnable, so the idle loop dispatches it and the wait
            // keeps waiting for its timer.
            m_armedInterrupts = interrupts;
            return true;
        }
        if (interrupts != m_armedInterrupts)
        {
            auto* const resumption = PendingResumption();
            if (resumption != nullptr)
            {
                resumption->ObservedTicks = m_timer.GetTicks();
                resumption->ObservedInterrupts = interrupts - m_armedInterrupts;
            }
            // Once the wake is published the wait no longer awaits its timer,
            // even if the woken thread's admission is rolled back and idle
            // runs again before it resumes.
            if (!CompleteFromIdle())
            {
                return false;
            }
            m_suspended = false;
            return true;
        }
        if (m_timer.GetTicks() > m_deadline + WaitTimeoutTicks)
        {
            return Reject(2U);
        }
    }
    // No interrupt within the bound: the timer never fired.
    return Reject(2U);
}

bool Amd64WaitRuntime::Reject(const Foundation::UInt8 stage) noexcept
{
    m_failureStage = stage;
    return false;
}
} // namespace Asteria::Platform::Amd64::System
