// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_PROCESSOR_KERNEL_RUN_LOOP_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_PROCESSOR_KERNEL_RUN_LOOP_HPP_INCLUDED
#include "Platform/Synchronization/ProcessorContext.hpp"

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/ProcessorAddressSpaceController.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Scheduler/ProcessorRunController.hpp"

namespace Asteria::Kernel::Scheduler
{
/// Upper bound on consecutive attempts to select the idle root while each
/// attempt is rejected with AdmissionRollbackSafe.
constexpr Foundation::Size MaximumIdleRootTransitionAttempts{65536U};

enum class ProcessorKernelRunLoopState : Foundation::UInt8
{
    Idle,
    Active,
    ExternalFramePending,
    Faulted,
};

enum class ProcessorKernelRunLoopResumeKind : Foundation::UInt8
{
    None,
    Cooperative,
    ExternalFrame,
};

enum class ProcessorKernelRunLoopError : Foundation::UInt8
{
    None,
    NotIdle,
    NotActive,
    BindingCapacityExceeded,
    BindingIdentityUnavailable,
    BindingIdentityMismatch,
    ThreadNotInitialized,
    ThreadAlreadyBound,
    ContextAlreadyBound,
    ThreadNotBound,
    ContextNotPrepared,
    ProtectionDomainNotReady,
    WrongActiveThread,
    WrongActiveContext,
    ControllerRejected,
    ControllerEntryRejected,
    ControllerRollbackRejected,
    AddressSpaceControllerRejected,
    AddressSpaceControllerRollbackRejected,
    NativeSwitchRejected,
    BindingGenerationExhausted,
    ExternalFrameAlreadyRetained,
    ExternalFrameRetained,
    ExternalFrameNotRetained,
    ExternalFrameOwnershipMismatch,
    ExternalFrameResumePending,
    ExternalFrameResumeNotPending,
    ExternalFrameResumeMismatch,
    ExternalFrameResumeGenerationExhausted,
    ExternalFrameContinuationRejected,
    ExternalFrameCoordinatorScopeMismatch,
    CompletionObserverRetryableRejected,
    CompletionObserverTerminalRejected,
    InvariantViolation,
};

struct ProcessorKernelRunLoopFailure
{
    Time::ProcessorDeadlineReceipt DeadlineReceipt;
    Thread* ThreadStorage;
    ProcessorKernelRunLoopError Error;
    ProcessorRunError ControllerError;
    ProcessorSchedulerError SchedulerError;
    Time::ProcessorDeadlineError DeadlineError;
    Memory::ProcessorAddressSpaceControllerFailure AddressSpaceController;
    BlockedRunCommitOutcome BlockedRunCommit;
    Foundation::UInt8 ActiveRunRetained;
    Foundation::UInt8 NativeError;
    ProcessorRunExternalStopError ExternalStop;
};

struct ProcessorKernelRunLoopReceipt
{
    Thread* ThreadStorage;
    Foundation::UInt64 ExternalFrameToken;
    Foundation::UInt64 ExternalFrameResumeGeneration;
    Foundation::UInt64 NativeBindingGeneration;
    Foundation::UInt64 StoppedRunSequence;
    Foundation::UInt64 AdmissionRunSequence;
    Foundation::UInt64 AdmissionDeadlineTicks;
    ProcessorKernelRunLoopState State;
    ProcessorKernelRunLoopResumeKind ResumeKind;
    Foundation::UInt8 Reserved[6];
};

struct ProcessorActiveProtectionDomainBinding
{
    Protection::ProtectionDomain* Storage;
    Protection::ProtectionDomainId Identity;
    Memory::AddressSpace* AddressSpaceStorage;
    Memory::AddressSpaceId AddressSpaceIdentity;
};

struct ProcessorKernelRunLoopSnapshot
{
    Thread* ActiveThread;
    Protection::ProtectionDomain* ActiveProtectionDomain;
    Protection::ProtectionDomainId ActiveProtectionDomainIdentity;
    Memory::AddressSpace* ActiveAddressSpace;
    Memory::AddressSpaceId ActiveAddressSpaceIdentity;
    Foundation::Size BindingCount;
    ProcessorKernelRunLoopState State;
    Foundation::UInt8 IdleFailurePending;
    Foundation::UInt8 ExternalFrameRetained;
    Foundation::UInt8 ExternalFrameResumePending;
    Foundation::UInt8 Reserved[4];
};

class ProcessorBindingReservation final
{
public:
    constexpr ProcessorBindingReservation() noexcept = default;
    ProcessorBindingReservation(const ProcessorBindingReservation&) = delete;
    ProcessorBindingReservation& operator=(const ProcessorBindingReservation&) = delete;

private:
    template <typename> friend class ProcessorKernelRunLoop;
    const void* m_owner{};
};

/// Connects one ProcessorRunController to one target-native cooperative context type.
/// TNativeContext is resolved statically by the target build and provides IsPrepared,
/// SwitchTo(TNativeContext&), SaveExternalFrameIdleContinuation(UInt64), and
/// GetExternalFrameIdleContinuationGeneration. The continuation hooks are invoked only on the exact
/// owning idle/coordinator context with interrupts disabled; they publish a dedicated idle-dispatch
/// landing, not a return into the caller's Refresh/Commit path. The owning CPU calls every
/// operation with local interrupts disabled. The idle context is the context executing
/// DispatchFromIdle; a bound thread calls FinishAndDispatch from its own exact native context. A
/// successful native switch returns only if that suspended context is selected again later. The
/// higher scheduler owner serializes binding changes across CPUs and keeps every Thread and native
/// context bound to at most one run loop. Each binding also names the immutable ProtectionDomain
/// used by that context. Once a native context has entered a run loop, its suspended continuation
/// remains bound to that exact run loop and ProtectionDomain until the Thread exits and the context
/// is retired. Live migration of an entered native context is not supported by this slice.
template <typename TNativeContext> class ProcessorKernelRunLoop final
{
public:
    /// Preconditions:
    /// - controller, addressSpaces, and idleContext outlive this object;
    /// - controller and addressSpaces own the same valid processor;
    /// - addressSpaces is Ready and owns the current idle root before the
    /// first operation;
    /// - idleContext is the currently executing CPU context and is not bound
    /// to a Thread;
    /// - the controller is Idle and has no retained run.
    constexpr ProcessorKernelRunLoop(ProcessorRunController& controller,
                                     Memory::ProcessorAddressSpaceController& addressSpaces,
                                     TNativeContext& idleContext) noexcept
        : m_controller{controller}, m_addressSpaces{addressSpaces}, m_idleContext{idleContext},
          m_activeContext{&idleContext}
    {
    }

    /// Preconditions:
    /// - State is Idle and no saved native context can resume a suspended
    /// invocation into this object;
    /// - RetainedExternalFrameCount is zero and no external-frame admission
    /// or resume action is selected or pending;
    /// - all bindings were removed or their Thread and native context storage
    /// still outlive this object.
    ~ProcessorKernelRunLoop() noexcept = default;

    ProcessorKernelRunLoop(const ProcessorKernelRunLoop&) = delete;
    ProcessorKernelRunLoop& operator=(const ProcessorKernelRunLoop&) = delete;
    ProcessorKernelRunLoop(ProcessorKernelRunLoop&&) = delete;
    ProcessorKernelRunLoop& operator=(ProcessorKernelRunLoop&&) = delete;

    // A poster reserves capacity without reading or operating another processor's bindings. The
    // pool retains the token in its native slot until the home consumes it or start fails.
    [[nodiscard]] bool ReserveBinding(ProcessorBindingReservation& reservation) noexcept
    {
        if (reservation.m_owner != nullptr)
        {
            return false;
        }
        auto used = __atomic_load_n(&m_bindingAllocations, __ATOMIC_ACQUIRE);
        for (Foundation::Size attempt{0U}; attempt < MaximumReadyQueueEntries; ++attempt)
        {
            if (used >= MaximumReadyQueueEntries)
            {
                return false;
            }
            if (__atomic_compare_exchange_n(&m_bindingAllocations, &used, used + 1U, false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
            {
                reservation.m_owner = this;
                return true;
            }
        }
        return false;
    }
    // Includes unpublished reservations, so simultaneous creators cannot overbook a home. No
    // foreign binding array is observed .
    [[nodiscard]] Foundation::Size GetPlacementLoad() const noexcept
    {
        return __atomic_load_n(&m_bindingAllocations, __ATOMIC_ACQUIRE);
    }
    void CancelBinding(ProcessorBindingReservation& reservation) noexcept
    {
        if (reservation.m_owner == this)
        {
            reservation.m_owner = nullptr;
            static_cast<void>(__atomic_fetch_sub(&m_bindingAllocations, 1U, __ATOMIC_ACQ_REL));
        }
    }

    /// Adds one stable Thread/native-context/ProtectionDomain association. It only appends, so it
    /// is allowed while the idle context or a bound thread executes on this processor (a running
    /// thread creating another, Documentation/Architecture/SystemCallAbi.md). Preconditions: -
    /// thread and context are not bound to any other processor run loop; - context has never been
    /// selected by this or any other run loop; - protectionDomain remains Ready, alive, and
    /// immovable until Unbind; - no CPU is executing or switching to context; - cross-CPU binding
    /// ownership is serialized by the scheduler owner.
    [[nodiscard]] ProcessorKernelRunLoopError Bind(Thread& thread, TNativeContext& context,
                                                   Protection::ProtectionDomain& protectionDomain,
                                                   ProcessorBindingReservation* reservation = nullptr) noexcept
    {
        if ((m_state != ProcessorKernelRunLoopState::Idle) && (m_state != ProcessorKernelRunLoopState::Active))
        {
            return ProcessorKernelRunLoopError::NotIdle;
        }
        if (!thread.IsInitialized())
        {
            return ProcessorKernelRunLoopError::ThreadNotInitialized;
        }
        if (!context.IsPrepared())
        {
            return ProcessorKernelRunLoopError::ContextNotPrepared;
        }
        const auto protectionBinding = protectionDomain.GetBinding();
        if (!protectionBinding.HasValue() || !protectionBinding.GetValue().Identity.IsValid() ||
            (protectionBinding.GetValue().AddressSpaceStorage == nullptr) ||
            !protectionBinding.GetValue().AddressSpace.IsValid())
        {
            return ProcessorKernelRunLoopError::ProtectionDomainNotReady;
        }
        const Memory::AddressSpaceSnapshot addressSpaceSnapshot =
            protectionBinding.GetValue().AddressSpaceStorage->GetSnapshot();
        if ((addressSpaceSnapshot.State != Memory::AddressSpaceState::Ready) ||
            !addressSpaceSnapshot.Identity.IsEqualTo(protectionBinding.GetValue().AddressSpace))
        {
            return ProcessorKernelRunLoopError::ProtectionDomainNotReady;
        }
        if (&context == &m_idleContext)
        {
            return ProcessorKernelRunLoopError::ContextAlreadyBound;
        }
        for (Foundation::Size index{0U}; index < m_bindingCount; ++index)
        {
            if (m_bindings[index].ThreadStorage == &thread)
            {
                return ProcessorKernelRunLoopError::ThreadAlreadyBound;
            }
            if (m_bindings[index].ContextStorage == &context)
            {
                return ProcessorKernelRunLoopError::ContextAlreadyBound;
            }
        }
        if (m_bindingCount == MaximumReadyQueueEntries)
        {
            return ProcessorKernelRunLoopError::BindingCapacityExceeded;
        }
        if (m_nextBindingGeneration == 0U)
        {
            return ProcessorKernelRunLoopError::BindingGenerationExhausted;
        }

        ProcessorBindingReservation local;
        if (reservation == nullptr)
        {
            if (!ReserveBinding(local))
            {
                return ProcessorKernelRunLoopError::BindingCapacityExceeded;
            }
            reservation = &local;
        }
        if (reservation->m_owner != this)
        {
            return ProcessorKernelRunLoopError::BindingCapacityExceeded;
        }
        reservation->m_owner = nullptr;

        m_bindings[m_bindingCount] = Binding{
            .ThreadStorage = &thread,
            .ThreadIdentity = {},
            .SchedulingContextIdentity = {},
            .ContextStorage = &context,
            .ProtectionDomainStorage = &protectionDomain,
            .ProtectionDomainIdentity = protectionBinding.GetValue().Identity,
            .AddressSpaceStorage = protectionBinding.GetValue().AddressSpaceStorage,
            .AddressSpaceIdentity = protectionBinding.GetValue().AddressSpace,
            .Generation = m_nextBindingGeneration,
            .RetainedExternalFrame = {},
            .ExternalFrameResumeGeneration = 0U,
            .ExternalFrameAdmissionRunSequence = 0U,
            .ExternalFrameAdmissionDeadlineTicks = 0U,
        };
        m_nextBindingGeneration =
            m_nextBindingGeneration == ~Foundation::UInt64{0U} ? 0U : m_nextBindingGeneration + 1U;
        ++m_bindingCount;
        return ProcessorKernelRunLoopError::None;
    }

    /// Removes an association that was never entered, or one whose Thread has
    /// exited and whose native continuation will be permanently retired. This
    /// does not make an entered Ready or Blocked context transferable. The
    /// caller retires the Thread and context only after this succeeds.
    /// While the idle context executes, any such association may go. While a bound thread executes
    /// (a thread reclaiming another), only another thread's association goes,
    /// and only one the loop can prove retired: never enqueued (no identity recorded) or Exited. No
    /// run-loop operation keeps a binding's index or address across calls (each finds its binding
    /// by Thread again, and a pending external admission by generation), so compacting the table
    /// under a running thread is safe. Preconditions: - the Thread has been removed from every
    /// ready-queue placement; - the association was never entered, or the Thread is Exited and the
    /// saved context will never be resumed; - cross-CPU binding ownership is serialized by the
    /// scheduler owner.
    [[nodiscard]] ProcessorKernelRunLoopError Unbind(Thread& thread) noexcept
    {
        const bool active = m_state == ProcessorKernelRunLoopState::Active;
        if ((m_state != ProcessorKernelRunLoopState::Idle) && (!active || m_externalFrameResumePending))
        {
            return ProcessorKernelRunLoopError::NotIdle;
        }
        if (active && (m_activeThread == &thread))
        {
            return ProcessorKernelRunLoopError::WrongActiveThread;
        }
        for (Foundation::Size index{0U}; index < m_bindingCount; ++index)
        {
            if (m_bindings[index].ThreadStorage != &thread)
            {
                continue;
            }
            if (m_bindings[index].RetainedExternalFrame.ExternalFrameToken != 0U)
            {
                return ProcessorKernelRunLoopError::ExternalFrameRetained;
            }
            if (active &&
                ((m_bindings[index].Generation == m_pendingExternalBindingGeneration) ||
                 (m_bindings[index].ThreadIdentity.IsValid() && (thread.GetSnapshot().State != ThreadState::Exited))))
            {
                return ProcessorKernelRunLoopError::NotIdle;
            }
            for (Foundation::Size move{index + 1U}; move < m_bindingCount; ++move)
            {
                m_bindings[move - 1U] = m_bindings[move];
            }
            --m_bindingCount;
            static_cast<void>(__atomic_fetch_sub(&m_bindingAllocations, 1U, __ATOMIC_ACQ_REL));
            m_bindings[m_bindingCount] = Binding{};
            return ProcessorKernelRunLoopError::None;
        }
        return ProcessorKernelRunLoopError::ThreadNotBound;
    }

    /// Enqueues one exact scheduler identity/storage pair through the composed
    /// run-loop owner and records those identities in the native binding only
    /// after ProcessorRunController accepts the same pair. Composed users must
    /// use this operation instead of calling the controller directly so even a
    /// never-admitted binding can later be removed without caller-paired
    /// identity input.
    [[nodiscard]] bool EnqueueBoundThread(SchedulingContextId contextIdentity, SchedulingContext& context,
                                          ThreadId threadIdentity, Thread& thread,
                                          ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        if (m_externalFrameResumePending || (m_state == ProcessorKernelRunLoopState::ExternalFramePending))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameResumePending, &thread);
            EnterFaulted(failure);
            return false;
        }
        Binding* const binding = FindBinding(&thread);
        if (binding == nullptr)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ThreadNotBound, &thread);
            return false;
        }
        if (!BindingIdentitiesCanMatch(*binding, threadIdentity, contextIdentity))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::BindingIdentityMismatch, &thread);
            return false;
        }
        const auto enqueued = m_controller.Enqueue(contextIdentity, context, threadIdentity, thread);
        if (!enqueued.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ControllerRejected, &thread, 0U,
                         &enqueued.GetErrorReference());
            return false;
        }
        binding->ThreadIdentity = threadIdentity;
        binding->SchedulingContextIdentity = contextIdentity;
        return true;
    }

    /// Removes one bound Ready placement through the exact identity previously accepted by
    /// EnqueueBoundThread or observed in a validated admission. Retained, selected, or active
    /// ownership is rejected before the controller and ready queue are touched. Composed users must
    /// not call ProcessorRunController::Remove directly.
    [[nodiscard]] bool RemoveBoundThread(Thread& thread, ReadyQueueState& state,
                                         ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        Binding* const binding = FindBinding(&thread);
        if (binding == nullptr)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ThreadNotBound, &thread);
            return false;
        }
        if (binding->RetainedExternalFrame.ExternalFrameToken != 0U)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameRetained, &thread);
            return false;
        }
        if (m_externalFrameResumePending || (m_state == ProcessorKernelRunLoopState::ExternalFramePending))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameResumePending, &thread);
            EnterFaulted(failure);
            return false;
        }
        if (m_activeThread == &thread)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::WrongActiveThread, &thread);
            return false;
        }
        if (!binding->ThreadIdentity.IsValid())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::BindingIdentityUnavailable, &thread);
            return false;
        }
        const auto removed = m_controller.Remove(binding->ThreadIdentity);
        if (!removed.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ControllerRejected, &thread, 0U,
                         &removed.GetErrorReference());
            return false;
        }
        state = removed.GetValue();
        return true;
    }

    /// Dispatches from the CPU's idle/coordinator continuation. A cooperative
    /// admission switches away and returns only when a thread later returns
    /// the CPU to idle. A retained-frame admission instead returns true with
    /// ResumeKind ExternalFrame and never calls SwitchTo on its destination.
    /// This entry also activates an already-selected ExternalFramePending
    /// admission after an active peer has switched to the coordinator.
    /// A false return explains the idle transition (commonly NoRunnableThread
    /// with an installed idle deadline).
    /// On true only receipt is meaningful; on false only failure is meaningful.
    [[nodiscard]] bool DispatchFromIdle(ProcessorKernelRunLoopReceipt& receipt,
                                        ProcessorKernelRunLoopFailure& failure) noexcept
    {
        return DispatchFromIdleTransaction(receipt, failure);
    }

    /// Completes the run-loop half of an already-selected, target-published
    /// fresh idle landing after that landing replaced the suspended
    /// DispatchFromIdle continuation. The run loop must be logically Idle and
    /// own the pending typed result from the switch that entered this landing.
    /// This invokes the same final transaction as an ordinary native-switch
    /// return: it validates exact logical/address-space idle ownership and
    /// consumes that result exactly once. No controller, timer, queue, or
    /// native-switch operation is performed. On true only receipt is
    /// meaningful; on false only failure is meaningful.
    [[nodiscard]] bool FinishSelectedFreshIdleLanding(ProcessorKernelRunLoopReceipt& receipt,
                                                      ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        if ((m_state != ProcessorKernelRunLoopState::Idle) || !m_idleFailurePending)
        {
            WriteFailure(failure, m_state == ProcessorKernelRunLoopState::Idle
                                      ? ProcessorKernelRunLoopError::InvariantViolation
                                      : ProcessorKernelRunLoopError::NotIdle);
            return false;
        }
        return FinishIdleResume(receipt, failure);
    }

    /// Accounts the currently running thread and immediately dispatches the
    /// next eligible context. Ready may return directly when the same thread is
    /// selected again. Otherwise this call returns only when a later dispatch
    /// resumes the supplied current context. Blocked cannot return immediately
    /// and returns only after a valid Wake and re-admission. Exited never
    /// returns under the valid lifecycle.
    /// On true only receipt is meaningful; on false only failure is meaningful.
    [[nodiscard]] bool FinishAndDispatch(Thread& currentThread, TNativeContext& currentContext,
                                         ThreadRunDisposition disposition, Foundation::UInt64 exitCode,
                                         ProcessorKernelRunLoopReceipt& receipt,
                                         ProcessorKernelRunLoopFailure& failure) noexcept
    {
        const auto ignoreCompletion = [](const DispatchCompletion&) noexcept
        {
            return BlockedRunCommitOutcome::Accepted;
        };
        return FinishAndDispatchTransaction(currentThread, currentContext, disposition, exitCode, ignoreCompletion,
                                            receipt, failure);
    }

    /// Completes an externally retained frame as Ready without saving the scheduler continuation
    /// into currentContext and without invoking TNativeContext::SwitchTo. The caller is a dedicated
    /// non-ISR continuation on the owning CPU with interrupts disabled. This first corridor is
    /// limited to a known PL0 worker loop that holds no kernel lock and has no preempt-disable
    /// state. Success leaves the idle address-space root active and this run loop logically Idle;
    /// target code separately transfers control to its scheduler/idle continuation. Each Binding
    /// can own one retained frame; other bindings may also retain independent frames until normal
    /// policy selects them.
    [[nodiscard]] Foundation::Result<DispatchCompletion, ProcessorKernelRunLoopFailure>
    CompleteExternallyStoppedRunAsReady(Thread& currentThread, TNativeContext& currentContext,
                                        const ProcessorRunExternalStopObservation& observation) noexcept
    {
        using ResultType = Foundation::Result<DispatchCompletion, ProcessorKernelRunLoopFailure>;

        DispatchCompletion completion{};
        ProcessorKernelRunLoopFailure failure{};
        if (!CompleteExternalStopTransaction(currentThread, currentContext, observation, completion, failure))
        {
            return ResultType::Failure(failure);
        }
        return ResultType::Success(completion);
    }

    /// Completes a user page fault whose complete architecture frame was retained before this
    /// synchronous coordinator was entered. Unlike the cooperative fault path, this operation must
    /// not save or switch the interrupted native context: that context is represented only by the
    /// opaque frame token. Success commits the exact scheduler-created block token to ingress,
    /// selects the idle address-space root, and leaves this run loop logically Idle. The caller
    /// then continues from its existing scheduler/idle continuation and must immediately call
    /// DispatchFromIdle before enabling local interrupts so the processor deadline is replaced.
    /// ingress.Prepare succeeded for the active identities before this call, and the active
    /// controller uses HardwareDeadline mode so a retained-frame readmission can install a nonzero
    /// deadline. The observation carries no timer evidence; its timing words are zero and
    /// HardwareState is SynchronousPageFaultCaptured. Any contradiction is terminal because the
    /// target already owns the displaced user frame.
    template <typename TPageFaultIngress>
    [[nodiscard]] Foundation::Result<DispatchCompletion, ProcessorKernelRunLoopFailure>
    CompletePageFaultedRunAsBlocked(Thread& currentThread, TNativeContext& currentContext,
                                    const ProcessorRunExternalStopObservation& observation,
                                    TPageFaultIngress& ingress) noexcept
    {
        using ResultType = Foundation::Result<DispatchCompletion, ProcessorKernelRunLoopFailure>;

        DispatchCompletion completion{};
        ProcessorKernelRunLoopFailure failure{};
        if (!CompletePageFaultTransaction(currentThread, currentContext, observation, ingress, completion, failure))
        {
            return ResultType::Failure(failure);
        }
        return ResultType::Success(completion);
    }

    /// Selects the sole retained frame from a live target coordinator without
    /// any reachable cooperative native switch. This deliberately narrow BSP
    /// proof requires exactly one bound, Ready, retained owner and no other
    /// queue placement. A scope contradiction is terminal before DispatchNext
    /// can claim or program anything. Success is an ExternalFrame receipt;
    /// this call never transfers away from its C++ coordinator frame. As from
    /// idle, an admission that cannot be entered before its stop (the
    /// processor stalled before the timer was programmed or before the final
    /// entry check) or an empty dispatch returns false with this run loop
    /// Idle and the frame still retained; the caller retries once the owner's
    /// budget is available again.
    [[nodiscard]] bool DispatchRetainedExternalFrameFromCoordinator(Thread& expectedThread,
                                                                    TNativeContext& expectedContext,
                                                                    ProcessorKernelRunLoopReceipt& receipt,
                                                                    ProcessorKernelRunLoopFailure& failure) noexcept
    {
        return DispatchRetainedExternalFrameFromCoordinatorTransaction(expectedThread, expectedContext, receipt,
                                                                       failure);
    }

    /// Asks the exact idle-context object to save and publish a fresh resumable
    /// coordinator continuation for the selected ExternalFrame action. The
    /// context publishes the action generation only after its save succeeds;
    /// unsupported target contexts reject the operation. Failure is terminal
    /// and retains frame/admission ownership.
    [[nodiscard]] bool RefreshExternalFrameIdleContinuation(const ProcessorKernelRunLoopReceipt& receipt,
                                                            ProcessorKernelRunLoopFailure& failure) noexcept
    {
        return RefreshExternalFrameIdleContinuationTransaction(receipt, failure);
    }

    /// Consumes the exact ExternalFrame receipt immediately before the target
    /// restores its retained architecture frame. The continuation evidence is
    /// read back from the exact idle-context object and must match the current
    /// action generation. No native switch or clock read occurs. Success clears
    /// the common ownership marker and leaves the exact re-admitted Thread,
    /// native binding, address-space root, and controller generation Active.
    /// The target's next control transfer must restore that exact frame and
    /// execute rfi; inability to do so is fail-stop because common ownership
    /// has been consumed. A stale, replayed, or mismatched receipt or missing
    /// continuation is terminal and retains the frame marker and active
    /// admission for diagnostics.
    [[nodiscard]] bool CommitExternalFrameResume(Thread& expectedThread, TNativeContext& expectedContext,
                                                 const ProcessorKernelRunLoopReceipt& receipt,
                                                 ProcessorKernelRunLoopFailure& failure) noexcept
    {
        return CommitExternalFrameResumeTransaction(expectedThread, expectedContext, receipt, failure);
    }

    /// Completes an exact prepared WaitSet run as Blocked and commits its scheduler-created block
    /// token before any next dispatch is attempted. BSP sequence and outcomes: - Prepare used
    /// identities from GetActiveExecutionBinding. - FinishWaitAndDispatch calls Finish(Blocked),
    /// then the observer's CommitBlockedFromFinishedRun with that exact completion token. -
    /// Accepted may immediately dispatch idle work. An Accepted/Blocked observer remains dormant
    /// until a later signal is progressed by Poll; Accepted/Completed already published its wake
    /// before dispatch. - RetryableRejected does not dispatch: the current context switches to idle
    /// while the observer retains its block token and pins. The idle owner calls Poll under the
    /// same owning-CPU serialization, then calls DispatchFromIdle after the wake is published.
    /// TerminalRejected does not dispatch and faults this run loop; any retained observer claim or
    /// reservation is actionable only for fatal-domain recovery. - An immediate Prepare completion
    /// returns without this operation. For a blocked wait, the caller invokes ConsumeCompletion
    /// exactly once after the woken thread is admitted again, before dispatcher reuse.
    template <typename TWaitObserver>
    [[nodiscard]] bool FinishWaitAndDispatch(Thread& currentThread, TNativeContext& currentContext,
                                             TWaitObserver& observer, ProcessorKernelRunLoopReceipt& receipt,
                                             ProcessorKernelRunLoopFailure& failure) noexcept
    {
        const auto commitWait = [&observer](const DispatchCompletion& completion) noexcept
        {
            return observer.CommitBlockedFromFinishedRun(completion.Thread.BlockToken);
        };
        return FinishAndDispatchTransaction(currentThread, currentContext, ThreadRunDisposition::Blocked, 0U,
                                            commitWait, receipt, failure);
    }

    /// Completes the current run as Blocked, passes the exact scheduler-created
    /// block generation to ingress, and only then selects the next context.
    /// ingress.Prepare succeeded for this exact running Thread, and its
    /// CommitBlocked(ThreadBlockToken) retains any retryable pager transition.
    template <typename TPageFaultIngress>
    [[nodiscard]] bool FinishFaultAndDispatch(Thread& currentThread, TNativeContext& currentContext,
                                              TPageFaultIngress& ingress, ProcessorKernelRunLoopReceipt& receipt,
                                              ProcessorKernelRunLoopFailure& failure) noexcept
    {
        const auto commitFault = [&ingress](const DispatchCompletion& completion) noexcept
        {
            static_cast<void>(ingress.CommitBlocked(completion.Thread.BlockToken));
            return BlockedRunCommitOutcome::Accepted;
        };
        return FinishAndDispatchTransaction(currentThread, currentContext, ThreadRunDisposition::Blocked, 0U,
                                            commitFault, receipt, failure);
    }

    /// Publishes the trusted active execution identity used by portable
    /// syscall ingress. The controller revalidates the exact scheduler
    /// admission first; this layer then requires the exact active native
    /// context, ProtectionDomain, and AddressSpace binding. Any active-state
    /// contradiction is terminal and no pointer is returned.
    [[nodiscard]] Foundation::Result<ProcessorActiveExecutionBinding, ProcessorActiveExecutionBindingFailure>
    GetActiveExecutionBinding() noexcept
    {
        using ResultType = Foundation::Result<ProcessorActiveExecutionBinding, ProcessorActiveExecutionBindingFailure>;

        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            return ResultType::Failure(MakeActiveBindingFailure(ProcessorActiveExecutionBindingError::Faulted));
        }
        if (m_state != ProcessorKernelRunLoopState::Active)
        {
            return ResultType::Failure(MakeActiveBindingFailure(ProcessorActiveExecutionBindingError::NotActive));
        }
        if (m_externalFrameResumePending)
        {
            return ResultType::Failure(
                MakeActiveBindingFailure(ProcessorActiveExecutionBindingError::NativeContextMismatch));
        }

        const auto active = m_controller.GetActiveExecutionBinding();
        if (!active.HasValue())
        {
            ProcessorKernelRunLoopFailure terminal{};
            WriteFailure(terminal, ProcessorKernelRunLoopError::InvariantViolation, m_activeThread);
            EnterFaulted(terminal);
            return ResultType::Failure(active.GetErrorReference());
        }

        return ValidateRunLoopActiveExecutionBinding(active.GetValue());
    }

    [[nodiscard]] constexpr ProcessorKernelRunLoopSnapshot GetSnapshot() const noexcept
    {
        return ProcessorKernelRunLoopSnapshot{
            .ActiveThread = m_activeThread,
            .ActiveProtectionDomain = m_activeProtectionDomain,
            .ActiveProtectionDomainIdentity = m_activeProtectionDomainIdentity,
            .ActiveAddressSpace = m_activeAddressSpace,
            .ActiveAddressSpaceIdentity = m_activeAddressSpaceIdentity,
            .BindingCount = m_bindingCount,
            .State = m_state,
            .IdleFailurePending = static_cast<Foundation::UInt8>(m_idleFailurePending ? 1U : 0U),
            .ExternalFrameRetained = static_cast<Foundation::UInt8>(RetainedExternalFrameCount() != 0U ? 1U : 0U),
            .ExternalFrameResumePending = static_cast<Foundation::UInt8>(m_externalFrameResumePending ? 1U : 0U),
            .Reserved = {0U, 0U, 0U, 0U},
        };
    }

    /// Returns the exact retained observation for one binding, including in a
    /// terminal state. This read-only diagnostic lets the architecture owner
    /// prove that a failed settlement did not discard its frame token.
    [[nodiscard]] Foundation::Result<ProcessorRunExternalStopObservation, ProcessorKernelRunLoopError>
    GetRetainedExternalFrame(Thread& thread) const noexcept
    {
        using ResultType = Foundation::Result<ProcessorRunExternalStopObservation, ProcessorKernelRunLoopError>;
        const Binding* const binding = FindBinding(&thread);
        if (binding == nullptr)
        {
            return ResultType::Failure(ProcessorKernelRunLoopError::ThreadNotBound);
        }
        if (binding->RetainedExternalFrame.ExternalFrameToken == 0U)
        {
            return ResultType::Failure(ProcessorKernelRunLoopError::ExternalFrameNotRetained);
        }
        return ResultType::Success(binding->RetainedExternalFrame);
    }

    /// Returns the exact ProtectionDomain binding installed for the currently
    /// executing native context. This owning-CPU observation acquires no lock;
    /// callers keep local interrupts disabled and do not call it from another
    /// CPU. A malformed Active state is an internal invariant failure.
    [[nodiscard]] Foundation::Result<ProcessorActiveProtectionDomainBinding, ProcessorKernelRunLoopError>
    GetActiveProtectionDomainBinding() const noexcept
    {
        using ResultType = Foundation::Result<ProcessorActiveProtectionDomainBinding, ProcessorKernelRunLoopError>;

        if ((m_state != ProcessorKernelRunLoopState::Active) || m_externalFrameResumePending)
        {
            return ResultType::Failure(ProcessorKernelRunLoopError::NotActive);
        }
        if ((m_activeThread == nullptr) || (m_activeContext == nullptr) || (m_activeProtectionDomain == nullptr) ||
            !m_activeProtectionDomainIdentity.IsValid() || (m_activeAddressSpace == nullptr) ||
            !m_activeAddressSpaceIdentity.IsValid())
        {
            return ResultType::Failure(ProcessorKernelRunLoopError::InvariantViolation);
        }
        const Binding* const binding = FindBinding(m_activeThread);
        if ((binding == nullptr) || (binding->ThreadStorage != m_activeThread) ||
            (binding->ContextStorage != m_activeContext) ||
            (binding->ProtectionDomainStorage != m_activeProtectionDomain) ||
            !binding->ProtectionDomainIdentity.IsEqualTo(m_activeProtectionDomainIdentity) ||
            (binding->AddressSpaceStorage != m_activeAddressSpace) ||
            !binding->AddressSpaceIdentity.IsEqualTo(m_activeAddressSpaceIdentity) ||
            !ActiveAddressSpaceInvariantIsValid())
        {
            return ResultType::Failure(ProcessorKernelRunLoopError::InvariantViolation);
        }
        return ResultType::Success(ProcessorActiveProtectionDomainBinding{
            .Storage = m_activeProtectionDomain,
            .Identity = m_activeProtectionDomainIdentity,
            .AddressSpaceStorage = m_activeAddressSpace,
            .AddressSpaceIdentity = m_activeAddressSpaceIdentity,
        });
    }

private:
    struct Binding
    {
        Thread* ThreadStorage;
        ThreadId ThreadIdentity;
        SchedulingContextId SchedulingContextIdentity;
        TNativeContext* ContextStorage;
        Protection::ProtectionDomain* ProtectionDomainStorage;
        Protection::ProtectionDomainId ProtectionDomainIdentity;
        Memory::AddressSpace* AddressSpaceStorage;
        Memory::AddressSpaceId AddressSpaceIdentity;
        Foundation::UInt64 Generation;
        ProcessorRunExternalStopObservation RetainedExternalFrame;
        Foundation::UInt64 ExternalFrameResumeGeneration;
        Foundation::UInt64 ExternalFrameAdmissionRunSequence;
        Foundation::UInt64 ExternalFrameAdmissionDeadlineTicks;
    };

    [[nodiscard, gnu::noinline]] bool
    CompleteExternalStopTransaction(Thread& currentThread, TNativeContext& currentContext,
                                    const ProcessorRunExternalStopObservation& observation,
                                    DispatchCompletion& completion, ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        if (m_state != ProcessorKernelRunLoopState::Active)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::NotActive);
            EnterFaulted(failure);
            return false;
        }
        if (!ActiveMutationIsAllowed(currentThread, failure))
        {
            return false;
        }
        if (m_activeThread != &currentThread)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::WrongActiveThread, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        if (m_activeContext != &currentContext)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::WrongActiveContext, &currentThread);
            EnterFaulted(failure);
            return false;
        }

        Binding* const binding = FindBinding(&currentThread);
        if ((binding == nullptr) || (binding->ContextStorage != &currentContext) ||
            !binding->ThreadIdentity.IsValid() || !binding->SchedulingContextIdentity.IsValid() ||
            (binding->ProtectionDomainStorage != m_activeProtectionDomain) ||
            !binding->ProtectionDomainIdentity.IsEqualTo(m_activeProtectionDomainIdentity) ||
            (binding->AddressSpaceStorage != m_activeAddressSpace) ||
            !binding->AddressSpaceIdentity.IsEqualTo(m_activeAddressSpaceIdentity) ||
            !ActiveAddressSpaceInvariantIsValid())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::InvariantViolation, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        if ((binding->RetainedExternalFrame.ExternalFrameToken != 0U) || m_externalFrameResumePending)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameAlreadyRetained, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        if (observation.ExternalFrameToken == 0U)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameOwnershipMismatch, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        binding->RetainedExternalFrame = observation;
        binding->ExternalFrameResumeGeneration = 0U;
        binding->ExternalFrameAdmissionRunSequence = 0U;
        binding->ExternalFrameAdmissionDeadlineTicks = 0U;
        Foundation::UInt64 futureResumeGeneration{0U};
        if (!SelectExternalFrameResumeGeneration(futureResumeGeneration))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameResumeGenerationExhausted, &currentThread);
            EnterFaulted(failure);
            return false;
        }

        const auto completed = m_controller.FinishExternallyStoppedRunAsReady(observation);
        if (!completed.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ControllerRejected, &currentThread, 0U,
                         &completed.GetErrorReference());
            EnterFaulted(failure);
            return false;
        }

        const auto idleAddressSpace = SwitchToIdleRoot();
        if (!idleAddressSpace.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRejected, &currentThread, 0U,
                         nullptr, &idleAddressSpace.GetErrorReference());
            EnterFaulted(failure);
            return false;
        }

        completion = completed.GetValue();
        SetIdle();
        return true;
    }

    template <typename TPageFaultIngress>
    [[nodiscard, gnu::noinline]] bool
    CompletePageFaultTransaction(Thread& currentThread, TNativeContext& currentContext,
                                 const ProcessorRunExternalStopObservation& observation, TPageFaultIngress& ingress,
                                 DispatchCompletion& completion, ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        if (m_state != ProcessorKernelRunLoopState::Active)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::NotActive, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        if (!ActiveMutationIsAllowed(currentThread, failure))
        {
            return false;
        }

        if (m_activeThread != &currentThread)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::WrongActiveThread, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        if (m_activeContext != &currentContext)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::WrongActiveContext, &currentThread);
            EnterFaulted(failure);
            return false;
        }

        Binding* const binding = FindBinding(&currentThread);
        if ((binding == nullptr) || (binding->ContextStorage != &currentContext) ||
            (binding->ProtectionDomainStorage != m_activeProtectionDomain) ||
            !binding->ProtectionDomainIdentity.IsEqualTo(m_activeProtectionDomainIdentity) ||
            (binding->AddressSpaceStorage != m_activeAddressSpace) ||
            !binding->AddressSpaceIdentity.IsEqualTo(m_activeAddressSpaceIdentity) ||
            !binding->ThreadIdentity.IsValid() || !binding->SchedulingContextIdentity.IsValid() ||
            !ActiveAddressSpaceInvariantIsValid())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::InvariantViolation, &currentThread);
            EnterFaulted(failure);
            return false;
        }

        const auto active = m_controller.GetActiveExecutionBinding();
        const ProcessorRunSnapshot controller = m_controller.GetSnapshot();
        if (!active.HasValue() || (controller.State != ProcessorRunState::Active) ||
            (active.GetValue().ThreadStorage != &currentThread) ||
            !binding->ThreadIdentity.IsEqualTo(active.GetValue().ThreadIdentity) ||
            !binding->SchedulingContextIdentity.IsEqualTo(active.GetValue().SchedulingContextIdentity) ||
            !active.GetValue().Processor.IsValid() ||
            (active.GetValue().Processor.GetValue() != m_controller.GetProcessor().GetValue()))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::InvariantViolation, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        if (binding->RetainedExternalFrame.ExternalFrameToken != 0U)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameAlreadyRetained, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        if (observation.ExternalFrameToken == 0U)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameOwnershipMismatch, &currentThread);
            EnterFaulted(failure);
            return false;
        }

        // Once the exact running binding is known, the captured token is the
        // only remaining ownership evidence for the displaced architecture
        // state. Preserve the complete observation before validating protocol
        // fields so every later fail-stop path retains that evidence.
        binding->RetainedExternalFrame = observation;
        binding->ExternalFrameResumeGeneration = 0U;
        binding->ExternalFrameAdmissionRunSequence = 0U;
        binding->ExternalFrameAdmissionDeadlineTicks = 0U;

        if ((controller.Mode != ProcessorRunDispatchMode::HardwareDeadline) || !observation.Thread.IsValid() ||
            !observation.Thread.IsEqualTo(active.GetValue().ThreadIdentity) ||
            !observation.SchedulingContext.IsValid() ||
            !observation.SchedulingContext.IsEqualTo(active.GetValue().SchedulingContextIdentity) ||
            (observation.RunSequence == 0U) || (observation.RunSequence != active.GetValue().RunSequence) ||
            (observation.ProgrammedDeadlineTicks != 0U) || (observation.ObservedTicks != 0U) ||
            !observation.Processor.IsValid() ||
            (observation.Processor.GetValue() != active.GetValue().Processor.GetValue()) ||
            (observation.Processor.GetValue() != m_controller.GetProcessor().GetValue()) ||
            (observation.HardwareState != ProcessorRunExternalStopHardwareState::SynchronousPageFaultCaptured) ||
            !BytesAreZero(observation.Reserved, sizeof(observation.Reserved)))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameOwnershipMismatch, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        Foundation::UInt64 futureResumeGeneration{0U};
        if (!SelectExternalFrameResumeGeneration(futureResumeGeneration))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameResumeGenerationExhausted, &currentThread);
            EnterFaulted(failure);
            return false;
        }

        const auto completed = m_controller.Finish(ThreadRunDisposition::Blocked, 0U);
        if (!completed.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ControllerRejected, &currentThread, 0U,
                         &completed.GetErrorReference());
            EnterFaulted(failure);
            return false;
        }

        ingress.CommitBlocked(completed.GetValue().Thread.BlockToken);
        const auto idleAddressSpace = SwitchToIdleRoot();
        if (!idleAddressSpace.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRejected, &currentThread, 0U,
                         nullptr, &idleAddressSpace.GetErrorReference());
            EnterFaulted(failure);
            return false;
        }

        completion = completed.GetValue();
        SetIdle();
        return true;
    }

    [[nodiscard]] bool ActiveMutationIsAllowed(Thread& currentThread, ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (!m_externalFrameResumePending)
        {
            return true;
        }
        WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameResumePending, &currentThread);
        EnterFaulted(failure);
        return false;
    }

    [[nodiscard]] bool ExternalFrameReceiptMatchesBinding(const ProcessorKernelRunLoopReceipt& receipt,
                                                          const Binding& binding) const noexcept
    {
        return (receipt.ThreadStorage == binding.ThreadStorage) && (receipt.ExternalFrameToken != 0U) &&
               (receipt.ExternalFrameResumeGeneration != 0U) && (receipt.NativeBindingGeneration != 0U) &&
               (receipt.StoppedRunSequence != 0U) && (receipt.AdmissionRunSequence != 0U) &&
               (receipt.AdmissionDeadlineTicks != 0U) && (receipt.State == ProcessorKernelRunLoopState::Active) &&
               (receipt.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame) &&
               BytesAreZero(receipt.Reserved, sizeof(receipt.Reserved)) &&
               (binding.Generation == receipt.NativeBindingGeneration) &&
               (binding.RetainedExternalFrame.ExternalFrameToken == receipt.ExternalFrameToken) &&
               (binding.ExternalFrameResumeGeneration == receipt.ExternalFrameResumeGeneration) &&
               (binding.RetainedExternalFrame.RunSequence == receipt.StoppedRunSequence) &&
               (binding.ExternalFrameAdmissionRunSequence == receipt.AdmissionRunSequence) &&
               (binding.ExternalFrameAdmissionDeadlineTicks == receipt.AdmissionDeadlineTicks) &&
               (m_activeThread == binding.ThreadStorage) && (m_activeContext == binding.ContextStorage);
    }

    [[nodiscard, gnu::noinline]] bool
    RefreshExternalFrameIdleContinuationTransaction(const ProcessorKernelRunLoopReceipt& receipt,
                                                    ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        Binding* const binding = FindBinding(receipt.ThreadStorage);
        if ((m_state != ProcessorKernelRunLoopState::Active) || !m_externalFrameResumePending || (binding == nullptr) ||
            !ExternalFrameReceiptMatchesBinding(receipt, *binding))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameResumeMismatch, receipt.ThreadStorage);
            EnterFaulted(failure);
            return false;
        }
        if (!m_idleContext.SaveExternalFrameIdleContinuation(receipt.ExternalFrameResumeGeneration) ||
            (m_idleContext.GetExternalFrameIdleContinuationGeneration() != receipt.ExternalFrameResumeGeneration))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameContinuationRejected,
                         receipt.ThreadStorage);
            EnterFaulted(failure);
            return false;
        }
        m_refreshedExternalFrameResumeGeneration = receipt.ExternalFrameResumeGeneration;
        return true;
    }

    [[nodiscard, gnu::noinline]] bool
    CommitExternalFrameResumeTransaction(Thread& expectedThread, TNativeContext& expectedContext,
                                         const ProcessorKernelRunLoopReceipt& receipt,
                                         ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        if ((m_state != ProcessorKernelRunLoopState::Active) || !m_externalFrameResumePending)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameResumeNotPending, &expectedThread);
            EnterFaulted(failure);
            return false;
        }

        Binding* const binding = FindBinding(&expectedThread);
        const auto active = m_controller.GetActiveExecutionBinding();
        const ProcessorRunSnapshot controller = m_controller.GetSnapshot();
        const bool continuationMismatch =
            (m_refreshedExternalFrameResumeGeneration != receipt.ExternalFrameResumeGeneration) ||
            (m_idleContext.GetExternalFrameIdleContinuationGeneration() != receipt.ExternalFrameResumeGeneration);
        if ((binding == nullptr) || (binding->ContextStorage != &expectedContext) ||
            !ExternalFrameReceiptMatchesBinding(receipt, *binding) || continuationMismatch || !active.HasValue())
        {
            RejectExternalFrameResume(expectedThread, failure);
            return false;
        }

        const ProcessorActiveExecutionBinding& execution = active.GetValue();
        const ProcessorRunExternalStopObservation& retained = binding->RetainedExternalFrame;
        if (!execution.ThreadIdentity.IsEqualTo(retained.Thread) || (execution.ThreadStorage != &expectedThread) ||
            !execution.SchedulingContextIdentity.IsEqualTo(retained.SchedulingContext) ||
            !execution.Processor.IsValid() || (execution.Processor.GetValue() != retained.Processor.GetValue()) ||
            (execution.RunSequence != binding->ExternalFrameAdmissionRunSequence) ||
            (controller.State != ProcessorRunState::Active) ||
            (controller.Mode != ProcessorRunDispatchMode::HardwareDeadline) ||
            (controller.Deadline.State != Time::ProcessorDeadlineState::Armed) ||
            (controller.Deadline.SelectedDeadlineTicks != binding->ExternalFrameAdmissionDeadlineTicks) ||
            !ActiveAddressSpaceInvariantIsValid())
        {
            RejectExternalFrameResume(expectedThread, failure);
            return false;
        }

        ClearRetainedExternalFrame(*binding);
        m_externalFrameResumePending = false;
        m_refreshedExternalFrameResumeGeneration = 0U;
        return true;
    }

    [[gnu::noinline]] void RejectExternalFrameResume(Thread& thread, ProcessorKernelRunLoopFailure& failure) noexcept
    {
        WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameResumeMismatch, &thread);
        EnterFaulted(failure);
    }

    [[nodiscard]] bool SelectExternalFrameResumeGeneration(Foundation::UInt64& generation) const noexcept
    {
        constexpr Foundation::UInt64 maximumGeneration{~Foundation::UInt64{0U}};
        const Foundation::UInt64 idleGeneration = m_idleContext.GetExternalFrameIdleContinuationGeneration();
        if ((m_nextExternalFrameResumeGeneration == 0U) || (idleGeneration == maximumGeneration))
        {
            generation = 0U;
            return false;
        }
        generation = m_nextExternalFrameResumeGeneration;
        if (generation <= idleGeneration)
        {
            generation = idleGeneration + 1U;
        }
        return generation != 0U;
    }

    [[nodiscard]] bool ReserveExternalFrameResumeGeneration(Foundation::UInt64& generation,
                                                            Thread* const diagnosticThread,
                                                            ProcessorKernelRunLoopFailure& failure) noexcept
    {
        generation = 0U;
        if (RetainedExternalFrameCount() == 0U)
        {
            return true;
        }
        if (SelectExternalFrameResumeGeneration(generation))
        {
            return true;
        }
        WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameResumeGenerationExhausted, diagnosticThread);
        EnterFaulted(failure);
        return false;
    }

    [[nodiscard, gnu::noinline]] bool ClassifyExternalFrameAdmission(const ProcessorRunAdmission& admission,
                                                                     const Binding& binding, bool& externalFrame,
                                                                     Foundation::UInt64& resumeGeneration,
                                                                     ProcessorKernelRunLoopFailure& failure) noexcept
    {
        externalFrame = false;
        const ProcessorRunExternalStopObservation& retained = binding.RetainedExternalFrame;
        if (retained.ExternalFrameToken == 0U)
        {
            resumeGeneration = 0U;
            return true;
        }
        if (m_externalFrameResumePending)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameOwnershipMismatch, binding.ThreadStorage);
            EnterFaulted(failure);
            return false;
        }
        if (resumeGeneration == 0U)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameResumeGenerationExhausted,
                         binding.ThreadStorage);
            EnterFaulted(failure);
            return false;
        }

        const auto active = m_controller.GetActiveExecutionBinding();
        if (!active.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameOwnershipMismatch, binding.ThreadStorage);
            EnterFaulted(failure);
            return false;
        }
        const ProcessorActiveExecutionBinding& execution = active.GetValue();
        if ((admission.Mode != ProcessorRunDispatchMode::HardwareDeadline) ||
            (admission.Scheduler.ThreadStorage != binding.ThreadStorage) ||
            (admission.Scheduler.ContextStorage != execution.SchedulingContextStorage) ||
            !admission.Scheduler.Context.IsEqualTo(retained.SchedulingContext) ||
            !admission.Scheduler.Dispatch.Context.Thread.IsEqualTo(retained.Thread) ||
            (admission.Scheduler.Dispatch.Thread.RunSequence != execution.RunSequence) ||
            !execution.ThreadIdentity.IsEqualTo(retained.Thread) ||
            (execution.ThreadStorage != binding.ThreadStorage) ||
            !execution.SchedulingContextIdentity.IsEqualTo(retained.SchedulingContext) ||
            !binding.ThreadIdentity.IsEqualTo(retained.Thread) ||
            !binding.SchedulingContextIdentity.IsEqualTo(retained.SchedulingContext) ||
            !execution.Processor.IsValid() || !retained.Processor.IsValid() ||
            (execution.Processor.GetValue() != retained.Processor.GetValue()) ||
            (execution.Processor.GetValue() != m_controller.GetProcessor().GetValue()) ||
            (retained.RunSequence == 0U) || (binding.Generation == 0U))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameOwnershipMismatch, binding.ThreadStorage);
            EnterFaulted(failure);
            return false;
        }

        externalFrame = true;
        return true;
    }

    [[nodiscard, gnu::noinline]] bool StartExternalFrameFromIdle(const ProcessorRunAdmission& admission,
                                                                 Binding& binding,
                                                                 const Foundation::UInt64 resumeGeneration,
                                                                 ProcessorKernelRunLoopReceipt& receipt,
                                                                 ProcessorKernelRunLoopFailure& failure) noexcept
    {
        const auto addressSpace = m_addressSpaces.SwitchTo(*binding.AddressSpaceStorage);
        if (!addressSpace.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRejected, binding.ThreadStorage,
                         0U, nullptr, &addressSpace.GetErrorReference());
            if (!addressSpace.GetError().AdmissionRollbackSafe || !RollbackAdmission(failure))
            {
                EnterFaulted(failure);
            }
            return false;
        }
        if (!ValidateIdleAdmissionEntry(admission, *binding.ThreadStorage, failure))
        {
            return false;
        }

        SetActive(binding);
        PublishExternalFrameResume(admission, binding, resumeGeneration, receipt);
        return true;
    }

    [[nodiscard, gnu::noinline]] bool PendExternalFrameFromActive(Thread& currentThread, TNativeContext& currentContext,
                                                                  const ProcessorRunAdmission& admission,
                                                                  Binding& binding,
                                                                  const Foundation::UInt64 resumeGeneration,
                                                                  ProcessorKernelRunLoopReceipt& receipt,
                                                                  ProcessorKernelRunLoopFailure& failure) noexcept
    {
        const auto idleAddressSpace = SwitchToIdleRoot();
        if (!idleAddressSpace.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRejected, binding.ThreadStorage,
                         0U, nullptr, &idleAddressSpace.GetErrorReference());
            EnterFaulted(failure);
            return false;
        }

        m_pendingExternalAdmission = admission;
        m_pendingExternalBindingGeneration = binding.Generation;
        m_pendingExternalFrameResumeGeneration = resumeGeneration;
        SetExternalFramePending();
        const Foundation::UInt8 nativeError = Switch(currentContext, m_idleContext);
        if (nativeError != 0U)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::NativeSwitchRejected, binding.ThreadStorage,
                         nativeError);
            EnterFaulted(failure);
            return false;
        }

        return ValidateResumedContext(currentThread, currentContext, receipt, failure);
    }

    [[nodiscard, gnu::noinline]] bool ActivatePendingExternalFrame(ProcessorKernelRunLoopReceipt& receipt,
                                                                   ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if ((m_state != ProcessorKernelRunLoopState::ExternalFramePending) || (m_activeThread != nullptr) ||
            (m_activeContext != &m_idleContext) || (m_activeProtectionDomain != nullptr) ||
            m_activeProtectionDomainIdentity.IsValid() || (m_activeAddressSpace != nullptr) ||
            m_activeAddressSpaceIdentity.IsValid() || (m_pendingExternalBindingGeneration == 0U) ||
            (m_pendingExternalFrameResumeGeneration == 0U) || m_externalFrameResumePending || m_idleFailurePending ||
            !IdleAddressSpaceInvariantIsValid())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::InvariantViolation);
            EnterFaulted(failure);
            return false;
        }
        if (!ValidateAdmissionIdentity(m_pendingExternalAdmission, failure))
        {
            EnterFaulted(failure);
            return false;
        }

        Thread* const thread = m_pendingExternalAdmission.Scheduler.ThreadStorage;
        Binding* const binding = FindBinding(thread);
        bool externalFrame{false};
        Foundation::UInt64 classifiedResumeGeneration = m_pendingExternalFrameResumeGeneration;
        if ((binding == nullptr) || (binding->Generation != m_pendingExternalBindingGeneration) ||
            !RecordAdmissionBindingIdentities(m_pendingExternalAdmission, *binding, failure) ||
            !ClassifyExternalFrameAdmission(m_pendingExternalAdmission, *binding, externalFrame,
                                            classifiedResumeGeneration, failure) ||
            (classifiedResumeGeneration != m_pendingExternalFrameResumeGeneration) || !externalFrame)
        {
            if (m_state != ProcessorKernelRunLoopState::Faulted)
            {
                WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameOwnershipMismatch, thread);
                EnterFaulted(failure);
            }
            return false;
        }

        const auto addressSpace = m_addressSpaces.SwitchTo(*binding->AddressSpaceStorage);
        if (!addressSpace.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRejected, thread, 0U, nullptr,
                         &addressSpace.GetErrorReference());
            if (!addressSpace.GetError().AdmissionRollbackSafe || !RollbackAdmission(failure))
            {
                EnterFaulted(failure);
            }
            else
            {
                ClearPendingExternalAdmission();
                SetIdle();
            }
            return false;
        }
        if (!ValidateIdleAdmissionEntry(m_pendingExternalAdmission, *thread, failure))
        {
            if (m_state == ProcessorKernelRunLoopState::Idle)
            {
                ClearPendingExternalAdmission();
            }
            return false;
        }

        const ProcessorRunAdmission admission = m_pendingExternalAdmission;
        const Foundation::UInt64 resumeGeneration = m_pendingExternalFrameResumeGeneration;
        ClearPendingExternalAdmission();
        SetActive(*binding);
        PublishExternalFrameResume(admission, *binding, resumeGeneration, receipt);
        return true;
    }

    void PublishExternalFrameResume(const ProcessorRunAdmission& admission, Binding& binding,
                                    const Foundation::UInt64 generation,
                                    ProcessorKernelRunLoopReceipt& receipt) noexcept
    {
        m_nextExternalFrameResumeGeneration = generation == ~Foundation::UInt64{0U} ? 0U : generation + 1U;
        binding.ExternalFrameResumeGeneration = generation;
        binding.ExternalFrameAdmissionRunSequence = admission.Scheduler.Dispatch.Thread.RunSequence;
        binding.ExternalFrameAdmissionDeadlineTicks = admission.Deadline.SelectedDeadlineTicks;
        m_refreshedExternalFrameResumeGeneration = 0U;
        m_externalFrameResumePending = true;
        receipt = MakeExternalFrameReceipt(binding);
    }

    [[nodiscard]] ProcessorKernelRunLoopReceipt MakeExternalFrameReceipt(const Binding& binding) const noexcept
    {
        return ProcessorKernelRunLoopReceipt{
            .ThreadStorage = binding.ThreadStorage,
            .ExternalFrameToken = binding.RetainedExternalFrame.ExternalFrameToken,
            .ExternalFrameResumeGeneration = binding.ExternalFrameResumeGeneration,
            .NativeBindingGeneration = binding.Generation,
            .StoppedRunSequence = binding.RetainedExternalFrame.RunSequence,
            .AdmissionRunSequence = binding.ExternalFrameAdmissionRunSequence,
            .AdmissionDeadlineTicks = binding.ExternalFrameAdmissionDeadlineTicks,
            .State = ProcessorKernelRunLoopState::Active,
            .ResumeKind = ProcessorKernelRunLoopResumeKind::ExternalFrame,
            .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
        };
    }

    [[nodiscard, gnu::noinline]] bool ValidateResumedContext(Thread& currentThread, TNativeContext& currentContext,
                                                             ProcessorKernelRunLoopReceipt& receipt,
                                                             ProcessorKernelRunLoopFailure& failure) noexcept
    {
        const Binding* const resumedBinding = FindBinding(&currentThread);
        if ((m_state != ProcessorKernelRunLoopState::Active) || m_externalFrameResumePending ||
            (m_activeThread != &currentThread) || (m_activeContext != &currentContext) || (resumedBinding == nullptr) ||
            (resumedBinding->ContextStorage != &currentContext) ||
            (m_activeProtectionDomain != resumedBinding->ProtectionDomainStorage) ||
            !m_activeProtectionDomainIdentity.IsEqualTo(resumedBinding->ProtectionDomainIdentity) ||
            (m_activeAddressSpace != resumedBinding->AddressSpaceStorage) ||
            !m_activeAddressSpaceIdentity.IsEqualTo(resumedBinding->AddressSpaceIdentity) ||
            !ActiveAddressSpaceInvariantIsValid())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::InvariantViolation, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        receipt = MakeReceipt(&currentThread, ProcessorKernelRunLoopState::Active);
        return true;
    }

    [[nodiscard]] constexpr Foundation::Size RetainedExternalFrameCount() const noexcept
    {
        Foundation::Size count{0U};
        for (Foundation::Size index{0U}; index < m_bindingCount; ++index)
        {
            if (m_bindings[index].RetainedExternalFrame.ExternalFrameToken != 0U)
            {
                ++count;
            }
        }
        return count;
    }

    static void ClearRetainedExternalFrame(Binding& binding) noexcept
    {
        binding.RetainedExternalFrame = {};
        binding.ExternalFrameResumeGeneration = 0U;
        binding.ExternalFrameAdmissionRunSequence = 0U;
        binding.ExternalFrameAdmissionDeadlineTicks = 0U;
    }

    [[nodiscard]] static bool BindingIdentitiesCanMatch(const Binding& binding, const ThreadId threadIdentity,
                                                        const SchedulingContextId contextIdentity) noexcept
    {
        return threadIdentity.IsValid() && contextIdentity.IsValid() &&
               (!binding.ThreadIdentity.IsValid() || binding.ThreadIdentity.IsEqualTo(threadIdentity)) &&
               (!binding.SchedulingContextIdentity.IsValid() ||
                binding.SchedulingContextIdentity.IsEqualTo(contextIdentity));
    }

    [[nodiscard]] bool RecordAdmissionBindingIdentities(const ProcessorRunAdmission& admission, Binding& binding,
                                                        ProcessorKernelRunLoopFailure& failure) noexcept
    {
        const ThreadId threadIdentity = admission.Scheduler.Dispatch.Context.Thread;
        const SchedulingContextId contextIdentity = admission.Scheduler.Context;
        if (!BindingIdentitiesCanMatch(binding, threadIdentity, contextIdentity))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::BindingIdentityMismatch, binding.ThreadStorage);
            EnterFaulted(failure);
            return false;
        }
        binding.ThreadIdentity = threadIdentity;
        binding.SchedulingContextIdentity = contextIdentity;
        return true;
    }

    void ClearPendingExternalAdmission() noexcept
    {
        m_pendingExternalAdmission = {};
        m_pendingExternalBindingGeneration = 0U;
        m_pendingExternalFrameResumeGeneration = 0U;
    }

    [[nodiscard]] static constexpr bool BytesAreZero(const Foundation::UInt8* const bytes,
                                                     const Foundation::Size count) noexcept
    {
        for (Foundation::Size index{0U}; index < count; ++index)
        {
            if (bytes[index] != 0U)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool DispatchFromIdleTransaction(ProcessorKernelRunLoopReceipt& receipt,
                                                   ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        if (m_state == ProcessorKernelRunLoopState::ExternalFramePending)
        {
            return ActivatePendingExternalFrame(receipt, failure);
        }
        if (m_state != ProcessorKernelRunLoopState::Idle)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::NotIdle);
            return false;
        }
        if ((m_pendingExternalBindingGeneration != 0U) || (m_pendingExternalFrameResumeGeneration != 0U) ||
            !IdleAddressSpaceInvariantIsValid())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::InvariantViolation);
            EnterFaulted(failure);
            return false;
        }

        m_idleFailurePending = false;
        Foundation::UInt64 reservedResumeGeneration{0U};
        if (!ReserveExternalFrameResumeGeneration(reservedResumeGeneration, nullptr, failure))
        {
            return false;
        }
        ProcessorRunAdmission admission{};
        if (!TryDispatch(admission, failure))
        {
            if (ControllerFailureIsTerminal(failure))
            {
                EnterFaulted(failure);
            }
            return false;
        }
        return StartIdleAdmission(admission, reservedResumeGeneration, receipt, failure);
    }

    [[nodiscard, gnu::noinline]] bool
    DispatchRetainedExternalFrameFromCoordinatorTransaction(Thread& expectedThread, TNativeContext& expectedContext,
                                                            ProcessorKernelRunLoopReceipt& receipt,
                                                            ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }

        Binding* const expectedBinding = FindBinding(&expectedThread);
        const ProcessorRunSnapshot controller = m_controller.GetSnapshot();
        const bool exactSoleRetainedOwner =
            (m_state == ProcessorKernelRunLoopState::Idle) && !m_externalFrameResumePending && !m_idleFailurePending &&
            (m_pendingExternalBindingGeneration == 0U) && (m_pendingExternalFrameResumeGeneration == 0U) &&
            IdleAddressSpaceInvariantIsValid() && (m_bindingCount == 1U) && (RetainedExternalFrameCount() == 1U) &&
            (expectedBinding != nullptr) && (expectedBinding->ContextStorage == &expectedContext) &&
            (expectedBinding->RetainedExternalFrame.ExternalFrameToken != 0U) &&
            (controller.State == ProcessorRunState::Idle) &&
            (controller.Mode == ProcessorRunDispatchMode::HardwareDeadline) &&
            (controller.Scheduler.State == ProcessorSchedulerState::Idle) &&
            (controller.Scheduler.Queue.ReadyEntries == 1U) && (controller.Scheduler.Queue.ClaimedEntries == 0U) &&
            (controller.Scheduler.Queue.ReservedEntries == 0U) && (controller.TimerQueue.ScheduledEntries == 0U) &&
            (controller.TimerQueue.PendingEntries == 0U) && (controller.TimerQueue.ClaimedEntries == 0U) &&
            (controller.Deadline.State == Time::ProcessorDeadlineState::Disarmed);
        if (!exactSoleRetainedOwner)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameCoordinatorScopeMismatch, &expectedThread);
            EnterFaulted(failure);
            return false;
        }

        Foundation::UInt64 reservedResumeGeneration{0U};
        if (!ReserveExternalFrameResumeGeneration(reservedResumeGeneration, &expectedThread, failure))
        {
            return false;
        }
        ProcessorRunAdmission admission{};
        if (!TryDispatch(admission, failure))
        {
            // As from idle: a dispatch the controller rolled back (its stop
            // passed before the timer was programmed) or found nothing to
            // admit leaves the frame retained and this run loop Idle.
            if (ControllerFailureIsTerminal(failure))
            {
                EnterFaulted(failure);
            }
            return false;
        }
        if (!ValidateAdmissionIdentity(admission, failure))
        {
            EnterFaulted(failure);
            return false;
        }
        Thread* const selectedThread = admission.Scheduler.ThreadStorage;
        Binding* const selectedBinding = FindBinding(selectedThread);
        if ((selectedThread != &expectedThread) || (selectedBinding != expectedBinding) ||
            (selectedBinding == nullptr) || (selectedBinding->ContextStorage != &expectedContext) ||
            !RecordAdmissionBindingIdentities(admission, *selectedBinding, failure))
        {
            if (m_state != ProcessorKernelRunLoopState::Faulted)
            {
                WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameCoordinatorScopeMismatch,
                             selectedThread);
                EnterFaulted(failure);
            }
            return false;
        }

        bool externalFrame{false};
        if (!ClassifyExternalFrameAdmission(admission, *selectedBinding, externalFrame, reservedResumeGeneration,
                                            failure) ||
            !externalFrame)
        {
            if (m_state != ProcessorKernelRunLoopState::Faulted)
            {
                WriteFailure(failure, ProcessorKernelRunLoopError::ExternalFrameCoordinatorScopeMismatch,
                             selectedThread);
                EnterFaulted(failure);
            }
            return false;
        }
        // A rollback-safe root rejection or a stop that passed before the
        // final entry check is rolled back there, leaving this run loop Idle
        // with the frame retained; every other failure entered Faulted.
        return StartExternalFrameFromIdle(admission, *selectedBinding, reservedResumeGeneration, receipt, failure);
    }

    [[nodiscard]] bool StartIdleAdmission(const ProcessorRunAdmission& admission,
                                          Foundation::UInt64 reservedResumeGeneration,
                                          ProcessorKernelRunLoopReceipt& receipt,
                                          ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (!ValidateAdmissionIdentity(admission, failure))
        {
            EnterFaulted(failure);
            return false;
        }
        Thread* const thread = admission.Scheduler.ThreadStorage;
        Binding* const binding = FindBinding(thread);
        if ((thread == nullptr) || (binding == nullptr))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ThreadNotBound, thread);
            if (!RejectCandidate(admission, failure))
            {
                EnterFaulted(failure);
            }
            return false;
        }
        if (!RecordAdmissionBindingIdentities(admission, *binding, failure))
        {
            return false;
        }
        bool externalFrame{false};
        if (!ClassifyExternalFrameAdmission(admission, *binding, externalFrame, reservedResumeGeneration, failure))
        {
            return false;
        }
        if (externalFrame)
        {
            return StartExternalFrameFromIdle(admission, *binding, reservedResumeGeneration, receipt, failure);
        }
        TNativeContext* const next = binding->ContextStorage;
        if (!next->IsPrepared())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ContextNotPrepared, thread);
            if (!RejectCandidate(admission, failure))
            {
                EnterFaulted(failure);
            }
            return false;
        }
        const auto addressSpace = m_addressSpaces.SwitchTo(*binding->AddressSpaceStorage);
        if (!addressSpace.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRejected, thread, 0U, nullptr,
                         &addressSpace.GetErrorReference());
            if (!addressSpace.GetError().AdmissionRollbackSafe)
            {
                EnterFaulted(failure);
                return false;
            }
            if (!RollbackAdmission(failure))
            {
                EnterFaulted(failure);
            }
            return false;
        }
        if (!ValidateIdleAdmissionEntry(admission, *thread, failure))
        {
            return false;
        }
        SetActive(*binding);
        const Foundation::UInt8 nativeError = Switch(m_idleContext, *next);
        if (nativeError != 0U)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::NativeSwitchRejected, thread, nativeError);
            const auto idleAddressSpace = SwitchToIdleRoot();
            if (!idleAddressSpace.HasValue())
            {
                WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRollbackRejected, thread,
                             nativeError, nullptr, &idleAddressSpace.GetErrorReference());
                EnterFaulted(failure);
            }
            else if (!RollbackAdmission(failure))
            {
                EnterFaulted(failure);
            }
            else
            {
                SetIdle();
            }
            return false;
        }
        return FinishIdleResume(receipt, failure);
    }

    template <typename TCompletionObserver>
    [[nodiscard]] bool
    FinishAndDispatchTransaction(Thread& currentThread, TNativeContext& currentContext,
                                 const ThreadRunDisposition disposition, const Foundation::UInt64 exitCode,
                                 const TCompletionObserver& completionObserver, ProcessorKernelRunLoopReceipt& receipt,
                                 ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        if (m_state != ProcessorKernelRunLoopState::Active)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::NotActive);
            return false;
        }
        if (!ActiveMutationIsAllowed(currentThread, failure))
        {
            return false;
        }
        if (m_activeThread != &currentThread)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::WrongActiveThread, &currentThread);
            return false;
        }
        if (m_activeContext != &currentContext)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::WrongActiveContext, &currentThread);
            return false;
        }
        Binding* const currentBinding = FindBinding(&currentThread);
        if ((currentBinding == nullptr) || (currentBinding->ContextStorage != &currentContext) ||
            (currentBinding->ProtectionDomainStorage != m_activeProtectionDomain) ||
            !currentBinding->ProtectionDomainIdentity.IsEqualTo(m_activeProtectionDomainIdentity) ||
            (currentBinding->AddressSpaceStorage != m_activeAddressSpace) ||
            !currentBinding->AddressSpaceIdentity.IsEqualTo(m_activeAddressSpaceIdentity) ||
            !ActiveAddressSpaceInvariantIsValid())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::InvariantViolation, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        Foundation::UInt64 reservedResumeGeneration{0U};
        if (!ReserveExternalFrameResumeGeneration(reservedResumeGeneration, &currentThread, failure))
        {
            return false;
        }
        DispatchCompletion completion{};
        if (!FinishCurrentRun(currentThread, disposition, exitCode, completion, failure))
        {
            return false;
        }
        const BlockedRunCommitOutcome completionOutcome = completionObserver(completion);
        if (completionOutcome != BlockedRunCommitOutcome::Accepted)
        {
            WriteFailure(failure,
                         completionOutcome == BlockedRunCommitOutcome::RetryableRejected
                             ? ProcessorKernelRunLoopError::CompletionObserverRetryableRejected
                             : ProcessorKernelRunLoopError::CompletionObserverTerminalRejected,
                         &currentThread, 0U, nullptr, nullptr, completionOutcome);
            return SwitchCurrentToIdle(currentThread, currentContext, failure,
                                       completionOutcome == BlockedRunCommitOutcome::TerminalRejected, receipt);
        }

        ProcessorRunAdmission admission{};
        if (!TryDispatch(admission, failure))
        {
            return SwitchCurrentToIdle(currentThread, currentContext, failure, ControllerFailureIsTerminal(failure),
                                       receipt);
        }
        return ContinueWithAdmission(currentThread, currentContext, admission, reservedResumeGeneration, receipt,
                                     failure);
    }

    [[gnu::noinline]] [[nodiscard]] bool FinishCurrentRun(Thread& currentThread, const ThreadRunDisposition disposition,
                                                          const Foundation::UInt64 exitCode,
                                                          DispatchCompletion& completion,
                                                          ProcessorKernelRunLoopFailure& failure) noexcept
    {
        const auto result = m_controller.Finish(disposition, exitCode);
        if (result.HasValue())
        {
            completion = result.GetValue();
            return true;
        }

        WriteFailure(failure, ProcessorKernelRunLoopError::ControllerRejected, &currentThread, 0U,
                     &result.GetErrorReference());
        if (ControllerFailureIsTerminal(failure) || (failure.ActiveRunRetained == 0U))
        {
            EnterFaulted(failure);
        }
        return false;
    }

    [[gnu::noinline]] [[nodiscard]] bool TryDispatch(ProcessorRunAdmission& admission,
                                                     ProcessorKernelRunLoopFailure& failure) noexcept
    {
        const auto dispatch = m_controller.DispatchNext();
        if (!dispatch.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ControllerRejected, nullptr, 0U,
                         &dispatch.GetErrorReference());
            return false;
        }
        admission = dispatch.GetValue();
        return true;
    }

    [[gnu::noinline]] [[nodiscard]] bool ContinueWithAdmission(Thread& currentThread, TNativeContext& currentContext,
                                                               const ProcessorRunAdmission& admission,
                                                               Foundation::UInt64 reservedResumeGeneration,
                                                               ProcessorKernelRunLoopReceipt& receipt,
                                                               ProcessorKernelRunLoopFailure& failure) noexcept
    {
        Binding* const currentBinding = FindBinding(&currentThread);
        if ((currentBinding == nullptr) || (currentBinding->ContextStorage != &currentContext) ||
            (currentBinding->AddressSpaceStorage != m_activeAddressSpace) ||
            !currentBinding->AddressSpaceIdentity.IsEqualTo(m_activeAddressSpaceIdentity) ||
            !ActiveAddressSpaceInvariantIsValid())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::InvariantViolation, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        if (!ValidateAdmissionIdentity(admission, failure))
        {
            return SwitchCurrentToIdle(currentThread, currentContext, failure, true, receipt);
        }
        Thread* const nextThread = admission.Scheduler.ThreadStorage;
        Binding* const nextBinding = FindBinding(nextThread);
        if ((nextThread == nullptr) || (nextBinding == nullptr))
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ThreadNotBound, nextThread);
            const bool rejected = RejectCandidate(admission, failure);
            return SwitchCurrentToIdle(currentThread, currentContext, failure, !rejected, receipt);
        }
        if (!RecordAdmissionBindingIdentities(admission, *nextBinding, failure))
        {
            return false;
        }
        bool externalFrame{false};
        if (!ClassifyExternalFrameAdmission(admission, *nextBinding, externalFrame, reservedResumeGeneration, failure))
        {
            return false;
        }
        if (externalFrame)
        {
            return PendExternalFrameFromActive(currentThread, currentContext, admission, *nextBinding,
                                               reservedResumeGeneration, receipt, failure);
        }
        TNativeContext* const nextContext = nextBinding->ContextStorage;
        if (!nextContext->IsPrepared())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::ContextNotPrepared, nextThread);
            const bool rejected = RejectCandidate(admission, failure);
            return SwitchCurrentToIdle(currentThread, currentContext, failure, !rejected, receipt);
        }
        const auto addressSpace = m_addressSpaces.SwitchTo(*nextBinding->AddressSpaceStorage);
        if (!addressSpace.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRejected, nextThread, 0U, nullptr,
                         &addressSpace.GetErrorReference());
            if (!addressSpace.GetError().AdmissionRollbackSafe)
            {
                EnterFaulted(failure);
                return false;
            }
            const bool rolledBack = RollbackAdmission(failure);
            return SwitchCurrentToIdle(currentThread, currentContext, failure, !rolledBack, receipt);
        }
        bool entryRecoveryResult{false};
        if (!ValidateActiveAdmissionEntry(currentThread, currentContext, admission, *nextThread, receipt, failure,
                                          entryRecoveryResult))
        {
            return entryRecoveryResult;
        }
        SetActive(*nextBinding);
        if (nextContext == &currentContext)
        {
            receipt = MakeReceipt(&currentThread, ProcessorKernelRunLoopState::Active);
            return true;
        }

        const Foundation::UInt8 nativeError = Switch(currentContext, *nextContext);
        if (nativeError != 0U)
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::NativeSwitchRejected, nextThread, nativeError);
            // The destination root may already be native-active. Restore a
            // root that can safely run the idle context before releasing the
            // scheduler's destination admission. If restoration cannot be
            // proved, retain that admission and stop.
            const auto idleAddressSpace = SwitchToIdleRoot();
            if (!idleAddressSpace.HasValue())
            {
                WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRollbackRejected, nextThread,
                             nativeError, nullptr, &idleAddressSpace.GetErrorReference());
                EnterFaulted(failure);
                return false;
            }
            const bool rolledBack = RollbackAdmission(failure);
            return SwitchCurrentContextToSelectedIdle(currentThread, currentContext, failure, !rolledBack, receipt);
        }

        // A successful switch resumes this frame later. While it is suspended,
        // the idle owner may remove an exited peer and compact m_bindings.
        return ValidateResumedContext(currentThread, currentContext, receipt, failure);
    }

    [[gnu::noinline]] [[nodiscard]] bool ValidateIdleAdmissionEntry(const ProcessorRunAdmission& admission,
                                                                    Thread& thread,
                                                                    ProcessorKernelRunLoopFailure& failure) noexcept
    {
        ProcessorRunEntryFailure controllerFailure{};
        if (m_controller.ValidateActiveAdmission(admission, controllerFailure))
        {
            return true;
        }

        WriteEntryFailure(failure, ProcessorKernelRunLoopError::ControllerEntryRejected, &thread, controllerFailure);
        const auto idleAddressSpace = SwitchToIdleRoot();
        if (!idleAddressSpace.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRollbackRejected, &thread, 0U,
                         nullptr, &idleAddressSpace.GetErrorReference());
            CopyEntryFailure(failure, controllerFailure);
            EnterFaulted(failure);
            return false;
        }
        if (!EntryValidationCanRollback(controllerFailure))
        {
            EnterFaulted(failure);
            return false;
        }
        if (!RollbackAdmission(failure))
        {
            EnterFaulted(failure);
        }
        else
        {
            SetIdle();
        }
        return false;
    }

    [[nodiscard]] bool ValidateAdmissionIdentity(const ProcessorRunAdmission& admission,
                                                 ProcessorKernelRunLoopFailure& failure) noexcept
    {
        ProcessorRunEntryFailure controllerFailure{};
        if (m_controller.ValidateActiveAdmissionIdentity(admission, controllerFailure))
        {
            return true;
        }
        WriteEntryFailure(failure, ProcessorKernelRunLoopError::ControllerEntryRejected, nullptr, controllerFailure);
        return false;
    }

    [[gnu::noinline]] [[nodiscard]] bool
    ValidateActiveAdmissionEntry(Thread& currentThread, TNativeContext& currentContext,
                                 const ProcessorRunAdmission& admission, Thread& nextThread,
                                 ProcessorKernelRunLoopReceipt& receipt, ProcessorKernelRunLoopFailure& failure,
                                 bool& recoveryResult) noexcept
    {
        ProcessorRunEntryFailure controllerFailure{};
        if (m_controller.ValidateActiveAdmission(admission, controllerFailure))
        {
            return true;
        }

        recoveryResult = RecoverActiveEntryValidationFailure(currentThread, currentContext, nextThread,
                                                             controllerFailure, receipt, failure);
        return false;
    }

    [[gnu::noinline]] [[nodiscard]] bool
    RecoverActiveEntryValidationFailure(Thread& currentThread, TNativeContext& currentContext, Thread& nextThread,
                                        const ProcessorRunEntryFailure& controllerFailure,
                                        ProcessorKernelRunLoopReceipt& receipt,
                                        ProcessorKernelRunLoopFailure& failure) noexcept
    {
        WriteEntryFailure(failure, ProcessorKernelRunLoopError::ControllerEntryRejected, &nextThread,
                          controllerFailure);
        const auto idleAddressSpace = SwitchToIdleRoot();
        if (!idleAddressSpace.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRollbackRejected, &nextThread, 0U,
                         nullptr, &idleAddressSpace.GetErrorReference());
            CopyEntryFailure(failure, controllerFailure);
            EnterFaulted(failure);
            return false;
        }

        bool terminal{true};
        if (EntryValidationCanRollback(controllerFailure))
        {
            terminal = !RollbackAdmission(failure);
        }
        return SwitchCurrentContextToSelectedIdle(currentThread, currentContext, failure, terminal, receipt);
    }

    void WriteFailure(ProcessorKernelRunLoopFailure& failure, const ProcessorKernelRunLoopError error,
                      Thread* const thread = nullptr, const Foundation::UInt8 nativeError = 0U,
                      const ProcessorRunFailure* const controller = nullptr,
                      const Memory::ProcessorAddressSpaceControllerFailure* const addressSpaceController = nullptr,
                      const BlockedRunCommitOutcome blockedRunCommit = BlockedRunCommitOutcome::Accepted) noexcept
    {
        if (controller != nullptr)
        {
            CopyControllerFailure(failure, *controller);
        }
        else
        {
            failure.DeadlineReceipt = {};
            failure.ControllerError = ProcessorRunError::None;
            failure.SchedulerError = ProcessorSchedulerError::None;
            failure.DeadlineError = Time::ProcessorDeadlineError::None;
            failure.ExternalStop = ProcessorRunExternalStopError::None;
            failure.ActiveRunRetained =
                static_cast<Foundation::UInt8>(m_controller.GetSnapshot().Scheduler.ActiveThread.IsValid() ? 1U : 0U);
        }
        failure.ThreadStorage = thread;
        failure.Error = error;
        failure.AddressSpaceController = addressSpaceController == nullptr
                                             ? Memory::ProcessorAddressSpaceControllerFailure{}
                                             : *addressSpaceController;
        failure.BlockedRunCommit = blockedRunCommit;
        failure.NativeError = nativeError;
    }

    [[nodiscard]] static ProcessorActiveExecutionBindingFailure
    MakeActiveBindingFailure(const ProcessorActiveExecutionBindingError error,
                             const ProcessorActiveExecutionBinding& execution = {}) noexcept
    {
        const ThreadState thread = execution.ThreadStorage == nullptr ? ThreadState::Uninitialized
                                                                      : execution.ThreadStorage->GetSnapshot().State;
        const SchedulingContextState context = execution.SchedulingContextStorage == nullptr
                                                   ? SchedulingContextState::Uninitialized
                                                   : execution.SchedulingContextStorage->GetSnapshot().State;
        return ProcessorActiveExecutionBindingFailure{
            .Error = error,
            .Thread = thread,
            .SchedulingContext = context,
            .Processor = execution.Processor,
            .Reserved = {0U, 0U, 0U, 0U},
        };
    }

    [[nodiscard,
      gnu::noinline]] Foundation::Result<ProcessorActiveExecutionBinding, ProcessorActiveExecutionBindingFailure>
    ValidateRunLoopActiveExecutionBinding(const ProcessorActiveExecutionBinding& execution) noexcept
    {
        using ResultType = Foundation::Result<ProcessorActiveExecutionBinding, ProcessorActiveExecutionBindingFailure>;
        const Binding* const binding = FindBinding(execution.ThreadStorage);
        if ((execution.ThreadStorage == nullptr) || (m_activeThread != execution.ThreadStorage) ||
            (m_activeContext == nullptr) || (m_activeContext == &m_idleContext) || (binding == nullptr) ||
            (binding->ThreadStorage != execution.ThreadStorage) || (binding->ContextStorage != m_activeContext))
        {
            EnterActiveBindingFault(execution.ThreadStorage);
            return ResultType::Failure(
                MakeActiveBindingFailure(ProcessorActiveExecutionBindingError::NativeContextMismatch, execution));
        }

        const auto protection =
            binding->ProtectionDomainStorage == nullptr
                ? Foundation::Result<Protection::ProtectionDomainBinding, Protection::ProtectionDomainError>::Failure(
                      Protection::ProtectionDomainError::NotReady)
                : binding->ProtectionDomainStorage->GetBinding();
        if ((m_activeProtectionDomain == nullptr) || (binding->ProtectionDomainStorage != m_activeProtectionDomain) ||
            !binding->ProtectionDomainIdentity.IsEqualTo(m_activeProtectionDomainIdentity) || !protection.HasValue() ||
            !protection.GetValue().Identity.IsEqualTo(m_activeProtectionDomainIdentity))
        {
            EnterActiveBindingFault(execution.ThreadStorage);
            return ResultType::Failure(
                MakeActiveBindingFailure(ProcessorActiveExecutionBindingError::ProtectionDomainMismatch, execution));
        }
        if ((m_activeAddressSpace == nullptr) || !m_activeAddressSpaceIdentity.IsValid() ||
            (binding->AddressSpaceStorage != m_activeAddressSpace) ||
            !binding->AddressSpaceIdentity.IsEqualTo(m_activeAddressSpaceIdentity) ||
            (protection.GetValue().AddressSpaceStorage != m_activeAddressSpace) ||
            !protection.GetValue().AddressSpace.IsEqualTo(m_activeAddressSpaceIdentity) ||
            !ActiveAddressSpaceInvariantIsValid())
        {
            EnterActiveBindingFault(execution.ThreadStorage);
            return ResultType::Failure(
                MakeActiveBindingFailure(ProcessorActiveExecutionBindingError::AddressSpaceMismatch, execution));
        }
        return ResultType::Success(execution);
    }

    void EnterActiveBindingFault(Thread* const thread) noexcept
    {
        ProcessorKernelRunLoopFailure terminal{};
        WriteFailure(terminal, ProcessorKernelRunLoopError::InvariantViolation, thread);
        EnterFaulted(terminal);
    }

    [[nodiscard]] static constexpr ProcessorKernelRunLoopReceipt
    MakeReceipt(Thread* const thread, const ProcessorKernelRunLoopState state) noexcept
    {
        return ProcessorKernelRunLoopReceipt{
            .ThreadStorage = thread,
            .ExternalFrameToken = 0U,
            .ExternalFrameResumeGeneration = 0U,
            .NativeBindingGeneration = 0U,
            .StoppedRunSequence = 0U,
            .AdmissionRunSequence = 0U,
            .AdmissionDeadlineTicks = 0U,
            .State = state,
            .ResumeKind = state == ProcessorKernelRunLoopState::Active ? ProcessorKernelRunLoopResumeKind::Cooperative
                                                                       : ProcessorKernelRunLoopResumeKind::None,
            .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
        };
    }

    static void CopyControllerFailure(ProcessorKernelRunLoopFailure& failure,
                                      const ProcessorRunFailure& controller) noexcept
    {
        failure.DeadlineReceipt = controller.DeadlineReceipt;
        failure.ControllerError = controller.Error;
        failure.SchedulerError = controller.Scheduler.Error;
        failure.DeadlineError = controller.Deadline.Error;
        failure.ActiveRunRetained = controller.ActiveRunRetained;
        failure.ExternalStop = controller.ExternalStop;
    }

    static void CopyEntryFailure(ProcessorKernelRunLoopFailure& failure, const ProcessorRunEntryFailure& entry) noexcept
    {
        failure.DeadlineReceipt = entry.DeadlineReceipt;
        failure.ControllerError = entry.Error;
        failure.SchedulerError = ProcessorSchedulerError::None;
        failure.DeadlineError = entry.Deadline.Error;
        failure.ActiveRunRetained = entry.ActiveRunRetained;
        failure.ExternalStop = ProcessorRunExternalStopError::None;
    }

    void WriteEntryFailure(ProcessorKernelRunLoopFailure& failure, const ProcessorKernelRunLoopError error,
                           Thread* const thread, const ProcessorRunEntryFailure& entry) noexcept
    {
        WriteFailure(failure, error, thread);
        CopyEntryFailure(failure, entry);
    }

    [[nodiscard]] bool ControllerFailureIsTerminal(const ProcessorKernelRunLoopFailure& failure) noexcept
    {
        return (failure.ActiveRunRetained != 0U) || (m_controller.GetSnapshot().State == ProcessorRunState::Faulted);
    }

    [[nodiscard]] static constexpr bool EntryValidationCanRollback(const ProcessorRunEntryFailure& failure) noexcept
    {
        return (failure.Error == ProcessorRunError::DeadlineReached) &&
               (failure.Deadline.Error == Time::ProcessorDeadlineError::DeadlineReached) &&
               (failure.ActiveRunRetained != 0U);
    }

    /// Selects the exact idle root. A rejection with AdmissionRollbackSafe
    /// left the current root and every reservation unchanged; its usual cause
    /// is another CPU holding the shared idle root's single transition
    /// reservation, which that CPU releases without waiting for this one. The
    /// selection is therefore attempted again, at most
    /// MaximumIdleRootTransitionAttempts times in all, and no lock is held
    /// between attempts. The last rejection, and any rejection without
    /// AdmissionRollbackSafe, is returned to the caller.
    [[nodiscard]] Foundation::Result<Memory::ProcessorAddressSpaceControllerReceipt,
                                     Memory::ProcessorAddressSpaceControllerFailure>
    SwitchToIdleRoot() noexcept
    {
        auto result = m_addressSpaces.SwitchToIdle();
        for (Foundation::Size attempt{1U}; !result.HasValue() && result.GetErrorReference().AdmissionRollbackSafe &&
                                           (attempt < MaximumIdleRootTransitionAttempts);
             ++attempt)
        {
            // A root reservation contender must acknowledge the mapping that prevents its own
            // detach.
            Platform::Synchronization::ProcessorWaitProgress();
            result = m_addressSpaces.SwitchToIdle();
        }
        return result;
    }

    [[nodiscard]] static Foundation::UInt8 Switch(TNativeContext& current, TNativeContext& next) noexcept
    {
        return static_cast<Foundation::UInt8>(current.SwitchTo(next));
    }

    [[nodiscard]] Binding* FindBinding(const Thread* const thread) noexcept
    {
        for (Foundation::Size index{0U}; index < m_bindingCount; ++index)
        {
            if (m_bindings[index].ThreadStorage == thread)
            {
                return &m_bindings[index];
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Binding* FindBinding(const Thread* const thread) const noexcept
    {
        for (Foundation::Size index{0U}; index < m_bindingCount; ++index)
        {
            if (m_bindings[index].ThreadStorage == thread)
            {
                return &m_bindings[index];
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool RollbackAdmission(ProcessorKernelRunLoopFailure& failure) noexcept
    {
        const auto rollback = m_controller.RollbackActiveAdmission();
        if (rollback.HasValue())
        {
            failure.DeadlineReceipt = rollback.GetValue();
            failure.ActiveRunRetained = 0U;
            return true;
        }
        // The controller ended the admission and is idle; it only found the
        // next deadline it would program already due (a TimerQueue entry or
        // a scheduler retry that fell due meanwhile). Reaching a deadline is
        // rollback-safe (ProcessorRunController.md): the next dispatch
        // reports it, so the rollback stands.
        if ((rollback.GetErrorReference().Error == ProcessorRunError::DeadlineReached) &&
            (rollback.GetErrorReference().ActiveRunRetained == 0U))
        {
            failure.DeadlineReceipt = rollback.GetErrorReference().DeadlineReceipt;
            failure.ActiveRunRetained = 0U;
            return true;
        }
        CopyControllerFailure(failure, rollback.GetErrorReference());
        failure.Error = ProcessorKernelRunLoopError::ControllerRollbackRejected;
        return false;
    }

    /// Rolls back an admission that this processor can never enter (no
    /// binding, or an unprepared bound context) and removes the candidate
    /// from this processor's ready queue, so policy does not select it again.
    /// The Thread stays Ready without a placement; failure.ThreadStorage
    /// names it for its owner. False means that the rollback or removal was
    /// rejected, which is terminal.
    [[nodiscard]] bool RejectCandidate(const ProcessorRunAdmission& admission,
                                       ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (!RollbackAdmission(failure))
        {
            return false;
        }
        const auto removed = m_controller.Remove(admission.Scheduler.Dispatch.Context.Thread);
        if (removed.HasValue())
        {
            return true;
        }
        CopyControllerFailure(failure, removed.GetErrorReference());
        failure.Error = ProcessorKernelRunLoopError::ControllerRollbackRejected;
        return false;
    }

    void SetActive(const Binding& binding) noexcept
    {
        Platform::Synchronization::ProcessorWatchdogRunning(binding.ThreadIdentity.GetValue(),
                                                            binding.ProtectionDomainStorage->DiagnosticComponentKind());
        m_activeThread = binding.ThreadStorage;
        m_activeContext = binding.ContextStorage;
        m_activeProtectionDomain = binding.ProtectionDomainStorage;
        m_activeProtectionDomainIdentity = binding.ProtectionDomainIdentity;
        m_activeAddressSpace = binding.AddressSpaceStorage;
        m_activeAddressSpaceIdentity = binding.AddressSpaceIdentity;
        m_state = ProcessorKernelRunLoopState::Active;
    }

    void SetIdle() noexcept
    {
        Platform::Synchronization::ProcessorWatchdogRunning(0U, 0U);
        m_activeThread = nullptr;
        m_activeContext = &m_idleContext;
        m_activeProtectionDomain = nullptr;
        m_activeProtectionDomainIdentity = {};
        m_activeAddressSpace = nullptr;
        m_activeAddressSpaceIdentity = {};
        m_state = ProcessorKernelRunLoopState::Idle;
    }

    void SetExternalFramePending() noexcept
    {
        m_activeThread = nullptr;
        m_activeContext = &m_idleContext;
        m_activeProtectionDomain = nullptr;
        m_activeProtectionDomainIdentity = {};
        m_activeAddressSpace = nullptr;
        m_activeAddressSpaceIdentity = {};
        m_state = ProcessorKernelRunLoopState::ExternalFramePending;
    }

    [[nodiscard]] bool ActiveAddressSpaceInvariantIsValid() const noexcept
    {
        if ((m_activeAddressSpace == nullptr) || !m_activeAddressSpaceIdentity.IsValid())
        {
            return false;
        }
        const Memory::ProcessorAddressSpaceControllerSnapshot snapshot = m_addressSpaces.GetSnapshot();
        const CpuId processor = m_controller.GetProcessor();
        return processor.IsValid() && (m_pendingExternalBindingGeneration == 0U) &&
               (m_pendingExternalFrameResumeGeneration == 0U) && snapshot.Processor.IsValid() &&
               (processor.GetValue() == snapshot.Processor.GetValue()) &&
               (snapshot.State == Memory::ProcessorAddressSpaceControllerState::Ready) &&
               (snapshot.ActiveStorage == m_activeAddressSpace) &&
               snapshot.Active.IsEqualTo(m_activeAddressSpaceIdentity);
    }

    [[nodiscard]] bool IdleAddressSpaceInvariantIsValid() const noexcept
    {
        const Memory::ProcessorAddressSpaceControllerSnapshot snapshot = m_addressSpaces.GetSnapshot();
        const CpuId processor = m_controller.GetProcessor();
        return processor.IsValid() && snapshot.Processor.IsValid() &&
               (processor.GetValue() == snapshot.Processor.GetValue()) &&
               (snapshot.State == Memory::ProcessorAddressSpaceControllerState::Ready) &&
               (snapshot.IdleStorage != nullptr) && snapshot.Idle.IsValid() &&
               (snapshot.ActiveStorage == snapshot.IdleStorage) && snapshot.Active.IsEqualTo(snapshot.Idle);
    }

    void EnterFaulted(const ProcessorKernelRunLoopFailure& failure) noexcept
    {
        m_terminalFailure = failure;
        m_state = ProcessorKernelRunLoopState::Faulted;
    }

    [[nodiscard]] bool FinishIdleResume(ProcessorKernelRunLoopReceipt& receipt,
                                        ProcessorKernelRunLoopFailure& failure) noexcept
    {
        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        if (m_state == ProcessorKernelRunLoopState::ExternalFramePending)
        {
            return ActivatePendingExternalFrame(receipt, failure);
        }
        if ((m_state != ProcessorKernelRunLoopState::Idle) || (m_activeThread != nullptr) ||
            (m_activeContext != &m_idleContext) || (m_activeProtectionDomain != nullptr) ||
            m_activeProtectionDomainIdentity.IsValid() || (m_activeAddressSpace != nullptr) ||
            m_activeAddressSpaceIdentity.IsValid() || !IdleAddressSpaceInvariantIsValid())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::InvariantViolation);
            EnterFaulted(failure);
            return false;
        }
        if (m_idleFailurePending)
        {
            m_idleFailurePending = false;
            failure = m_idleFailure;
            return false;
        }
        receipt = MakeReceipt(nullptr, ProcessorKernelRunLoopState::Idle);
        return true;
    }

    [[gnu::noinline]] [[nodiscard]] bool SwitchCurrentToIdle(Thread& currentThread, TNativeContext& currentContext,
                                                             ProcessorKernelRunLoopFailure& failure,
                                                             const bool terminal,
                                                             ProcessorKernelRunLoopReceipt& receipt) noexcept
    {
        const BlockedRunCommitOutcome blockedRunCommit = failure.BlockedRunCommit;
        const auto idleAddressSpace = SwitchToIdleRoot();
        if (!idleAddressSpace.HasValue())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::AddressSpaceControllerRollbackRejected, &currentThread,
                         0U, nullptr, &idleAddressSpace.GetErrorReference(), blockedRunCommit);
            EnterFaulted(failure);
            return false;
        }
        return SwitchCurrentContextToSelectedIdle(currentThread, currentContext, failure, terminal, receipt);
    }

    /// The caller already selected the exact idle root and has not released
    /// the owning-CPU serialization domain since that successful operation.
    [[nodiscard]] bool SwitchCurrentContextToSelectedIdle(Thread& currentThread, TNativeContext& currentContext,
                                                          ProcessorKernelRunLoopFailure& failure, const bool terminal,
                                                          ProcessorKernelRunLoopReceipt& receipt) noexcept
    {
        m_idleFailure = failure;
        m_idleFailurePending = true;
        m_activeThread = nullptr;
        m_activeContext = &m_idleContext;
        m_activeProtectionDomain = nullptr;
        m_activeProtectionDomainIdentity = {};
        m_activeAddressSpace = nullptr;
        m_activeAddressSpaceIdentity = {};
        if (terminal)
        {
            EnterFaulted(failure);
        }
        else
        {
            m_state = ProcessorKernelRunLoopState::Idle;
        }

        const Foundation::UInt8 nativeError = Switch(currentContext, m_idleContext);
        if (nativeError != 0U)
        {
            failure = m_idleFailure;
            failure.ThreadStorage = &currentThread;
            failure.Error = ProcessorKernelRunLoopError::NativeSwitchRejected;
            failure.NativeError = nativeError;
            EnterFaulted(failure);
            return false;
        }

        if (m_state == ProcessorKernelRunLoopState::Faulted)
        {
            failure = m_terminalFailure;
            return false;
        }
        if ((m_state != ProcessorKernelRunLoopState::Active) || (m_activeThread != &currentThread) ||
            (m_activeContext != &currentContext) || !ActiveAddressSpaceInvariantIsValid())
        {
            WriteFailure(failure, ProcessorKernelRunLoopError::InvariantViolation, &currentThread);
            EnterFaulted(failure);
            return false;
        }
        receipt = MakeReceipt(&currentThread, ProcessorKernelRunLoopState::Active);
        return true;
    }

    ProcessorRunController& m_controller;
    Memory::ProcessorAddressSpaceController& m_addressSpaces;
    TNativeContext& m_idleContext;
    Binding m_bindings[MaximumReadyQueueEntries]{};
    Foundation::Size m_bindingCount{0U};
    Foundation::Size m_bindingAllocations{0U};
    Thread* m_activeThread{nullptr};
    TNativeContext* m_activeContext;
    Protection::ProtectionDomain* m_activeProtectionDomain{nullptr};
    Protection::ProtectionDomainId m_activeProtectionDomainIdentity{};
    Memory::AddressSpace* m_activeAddressSpace{nullptr};
    Memory::AddressSpaceId m_activeAddressSpaceIdentity{};
    ProcessorRunAdmission m_pendingExternalAdmission{};
    Foundation::UInt64 m_pendingExternalBindingGeneration{0U};
    Foundation::UInt64 m_pendingExternalFrameResumeGeneration{0U};
    Foundation::UInt64 m_nextBindingGeneration{1U};
    Foundation::UInt64 m_nextExternalFrameResumeGeneration{1U};
    Foundation::UInt64 m_refreshedExternalFrameResumeGeneration{0U};
    ProcessorKernelRunLoopFailure m_idleFailure{};
    ProcessorKernelRunLoopFailure m_terminalFailure{};
    ProcessorKernelRunLoopState m_state{ProcessorKernelRunLoopState::Idle};
    bool m_idleFailurePending{false};
    bool m_externalFrameResumePending{false};
};

static_assert(sizeof(ProcessorKernelRunLoopState) == 1U);
static_assert(sizeof(ProcessorKernelRunLoopResumeKind) == 1U);
static_assert(sizeof(ProcessorKernelRunLoopError) == 1U);
static_assert(sizeof(ProcessorActiveProtectionDomainBinding) == 48U);
static_assert(alignof(ProcessorActiveProtectionDomainBinding) == 8U);
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_KERNEL_SCHEDULER_PROCESSOR_KERNEL_RUN_LOOP_HPP_INCLUDED
