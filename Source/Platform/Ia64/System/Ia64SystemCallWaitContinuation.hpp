// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_WAIT_CONTINUATION_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_WAIT_CONTINUATION_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp"

namespace Asteria::Platform::Ia64::System
{
struct Ia64SystemCallEntryFrame;

class Ia64SystemCallWaitEpochPolicy final
{
public:
    static constexpr Foundation::UInt64 Maximum{~Foundation::UInt64{0U}};

    Ia64SystemCallWaitEpochPolicy() = delete;

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

enum class Ia64SystemCallWaitContinuationState : Foundation::UInt8
{
    Uninitialized,
    Idle,
    EntryReserved,
    WaitPrepared,
    SwitchingToSupervisor,
    SupervisorActive,
    WaitingReadmission,
    ResumeReady,
    SwitchingToSystemCall,
    Faulted,
};

enum class Ia64SystemCallWaitContinuationError : Foundation::UInt8
{
    None,
    AlreadyInitialized,
    InvalidConfiguration,
    InvalidProcessor,
    InvalidEntryFrame,
    InvalidMemoryStack,
    InvalidRseBackingStore,
    StorageRangesOverlap,
    InvalidSupervisorContinuation,
    NotInitialized,
    InvalidState,
    EpochExhausted,
    EntryFrameNotActive,
    InvalidCorrelation,
    RunSequenceExhausted,
    InvalidSupervisorState,
    InvalidSupervisorRegisterState,
    InvalidSystemCallRegisterState,
    NativeTransitionUnavailable,
    Faulted,
};

enum class Ia64SystemCallWaitSupervisorResumeSite : Foundation::UInt8
{
    InitialLaunch,
    RepeatedSwitch,
};

struct Ia64SystemCallWaitCorrelation
{
    Kernel::Scheduler::ThreadId Thread;
    Kernel::Scheduler::SchedulingContextId SchedulingContext;
    Foundation::UInt64 RunSequence;
    Kernel::Scheduler::CpuId Processor;
};

struct Ia64SystemCallWaitContinuationConfiguration
{
    Ia64SystemCallEntryFrame* EntryFrame;
    Scheduler::Ia64UserModeSupervisorContinuation* SupervisorContinuation;
    Scheduler::Ia64UserModeVirtualRange SystemCallMemoryStack;
    Scheduler::Ia64UserModeVirtualRange SystemCallRseBackingStore;
    Kernel::Scheduler::CpuId Processor;
};

struct Ia64SystemCallWaitReservationReceipt
{
    Ia64SystemCallWaitCorrelation Correlation;
    Foundation::UInt64 PendingEpoch;
};

struct Ia64SystemCallWaitCommitReceipt
{
    Ia64SystemCallWaitCorrelation Correlation;
    Foundation::UInt64 Epoch;
};

struct Ia64SystemCallWaitSupervisorReceipt
{
    Ia64SystemCallWaitCorrelation Correlation;
    Foundation::UInt64 Epoch;
    Ia64SystemCallWaitSupervisorResumeSite Site;
};

struct Ia64SystemCallWaitContinuationFailure
{
    Foundation::UInt64 Epoch;
    Ia64SystemCallWaitContinuationError Error;
    Ia64SystemCallWaitContinuationState State;
};

struct Ia64SystemCallWaitContinuationSnapshot
{
    Kernel::Scheduler::ThreadId Thread;
    Kernel::Scheduler::SchedulingContextId SchedulingContext;
    Foundation::UInt64 Epoch;
    Foundation::UInt64 PendingEpoch;
    Foundation::UInt64 PreparedRunSequence;
    Foundation::UInt64 ReadmittedRunSequence;
    Ia64SystemCallWaitContinuationState State;
    Ia64SystemCallWaitContinuationError TerminalError;
    Scheduler::Ia64UserModeSupervisorContinuationState SupervisorState;
    Foundation::UInt8 SystemCallRecordActionable;
    Foundation::UInt8 EntryFrameRetained;
};

using Ia64SystemCallWaitReservationResult =
    Foundation::Result<Ia64SystemCallWaitReservationReceipt, Ia64SystemCallWaitContinuationFailure>;
using Ia64SystemCallWaitCommitResult =
    Foundation::Result<Ia64SystemCallWaitCommitReceipt, Ia64SystemCallWaitContinuationFailure>;
using Ia64SystemCallWaitSupervisorResult =
    Foundation::Result<Ia64SystemCallWaitSupervisorReceipt, Ia64SystemCallWaitContinuationFailure>;
using Ia64SystemCallWaitResponseResult =
    Foundation::Result<Kernel::System::SystemCallResponse, Ia64SystemCallWaitContinuationFailure>;

/// Owns the sole processor-local suspended native syscall continuation.
///
/// Capability authorization and WaitSet preparation precede CommitPrepared.
/// This class carries no syscall authority and never runs in interrupt context.
/// The caller disables local interrupts, excludes same-CPU re-entry, retains
/// every configured storage range, and admits no second syscall-capable Thread
/// while a transaction is not Idle.
///
/// The dedicated syscall stack and RSE are saved only in m_systemCallRegisters.
/// They are never installed in Ia64KernelContext. The retained supervisor
/// continuation and the syscall record alternate ownership through the raw P64
/// synchronous switch. Any contradiction after a committed wait is terminal.
class Ia64SystemCallWaitContinuation final
{
public:
    constexpr Ia64SystemCallWaitContinuation() noexcept = default;

    ~Ia64SystemCallWaitContinuation() noexcept = default;

    Ia64SystemCallWaitContinuation(const Ia64SystemCallWaitContinuation&) = delete;
    Ia64SystemCallWaitContinuation& operator=(const Ia64SystemCallWaitContinuation&) = delete;
    Ia64SystemCallWaitContinuation(Ia64SystemCallWaitContinuation&&) = delete;
    Ia64SystemCallWaitContinuation& operator=(Ia64SystemCallWaitContinuation&&) = delete;

    /// Binds exact boot-lifetime storage. A failed call changes no byte.
    [[nodiscard]] Ia64SystemCallWaitContinuationError
    Initialize(const Ia64SystemCallWaitContinuationConfiguration& configuration) noexcept;

    /// Reserves the one slot and preflights, but does not consume, the next
    /// nonwrapping suspension epoch. Failure changes no transaction state.
    [[nodiscard]] Ia64SystemCallWaitReservationResult
    ReserveEntry(Ia64SystemCallEntryFrame& frame, const Ia64SystemCallWaitCorrelation& correlation) noexcept;

    /// Cancels a reservation after capability or immediate-ready completion.
    [[nodiscard]] Ia64SystemCallWaitContinuationError
    CancelReservedEntry(Ia64SystemCallEntryFrame& frame, const Ia64SystemCallWaitCorrelation& correlation) noexcept;

    /// Commits the preflighted epoch only after the exact WaitSet dispatcher
    /// has retained a Prepared transaction for the same correlation.
    [[nodiscard]] Ia64SystemCallWaitCommitResult
    CommitPrepared(Ia64SystemCallEntryFrame& frame, const Ia64SystemCallWaitCorrelation& correlation) noexcept;

    /// Saves the syscall bridge call chain and restores the retained supervisor.
    /// On target success this returns only after PrepareSystemCallResume and
    /// ResumeSystemCall restore this exact syscall transaction.
    [[nodiscard]] Ia64SystemCallWaitResponseResult SuspendToSupervisor(Ia64SystemCallEntryFrame& frame) noexcept;

    /// Accepts the first supervisor restoration, whose native return site is
    /// Ia64UserModeLaunch::LaunchResumable.
    [[nodiscard]] Ia64SystemCallWaitSupervisorResult AcceptInitialSupervisorResume() noexcept;

    /// Marks the point immediately before the worker completes the Thread as
    /// Blocked through ProcessorKernelRunLoop.
    [[nodiscard]] Ia64SystemCallWaitContinuationError BeginWaitingForReadmission() noexcept;

    /// Correlates the exact successful re-admission and retains the completed
    /// syscall response before the reverse raw switch.
    [[nodiscard]] Ia64SystemCallWaitContinuationError
    PrepareSystemCallResume(Ia64SystemCallEntryFrame& frame, const Ia64SystemCallWaitCorrelation& readmitted,
                            const Kernel::System::SystemCallResponse& response) noexcept;

    /// Saves the active supervisor into its retained continuation and restores
    /// the syscall bridge. This invocation itself returns only when a later
    /// prepared wait switches back to the supervisor, identifying the repeated
    /// supervisor return site.
    [[nodiscard]] Ia64SystemCallWaitSupervisorResult ResumeSystemCall() noexcept;

    [[nodiscard]] Ia64SystemCallWaitContinuationSnapshot GetSnapshot() const noexcept;

private:
    [[nodiscard]] Ia64SystemCallWaitContinuationFailure
    MakeFailure(Ia64SystemCallWaitContinuationError error) const noexcept;
    [[nodiscard]] bool FrameIsExactAndActive(const Ia64SystemCallEntryFrame& frame) const noexcept;
    [[nodiscard]] static bool CorrelationIsValid(const Ia64SystemCallWaitCorrelation& correlation) noexcept;
    [[nodiscard]] bool CorrelationMatches(const Ia64SystemCallWaitCorrelation& correlation) const noexcept;
    [[nodiscard]] bool SupervisorRegistersAreValid() const noexcept;
    [[nodiscard]] bool SystemCallRegistersAreValid() const noexcept;
    [[nodiscard]] bool BeginSupervisorSwitch(Ia64SystemCallEntryFrame& frame,
                                             Ia64SystemCallWaitContinuationFailure& failure) noexcept;
    [[nodiscard]] bool BeginSystemCallSwitch(Ia64SystemCallWaitContinuationFailure& failure) noexcept;
    [[nodiscard]] Ia64SystemCallWaitSupervisorResult
    FinishSupervisorResume(Ia64SystemCallWaitSupervisorResumeSite site) noexcept;
    [[nodiscard]] Ia64SystemCallWaitResponseResult FinishSystemCallResume() noexcept;
    void ClearSystemCallRegisters() noexcept;
    void ClearTransaction() noexcept;
    void EnterFaulted(Ia64SystemCallWaitContinuationError error) noexcept;

    Scheduler::Ia64KernelRegisterState m_systemCallRegisters{};
    Ia64SystemCallEntryFrame* m_expectedFrame{nullptr};
    Ia64SystemCallEntryFrame* m_activeFrame{nullptr};
    Scheduler::Ia64UserModeSupervisorContinuation* m_supervisorContinuation{nullptr};
    Scheduler::Ia64UserModeVirtualRange m_systemCallMemoryStack{};
    Scheduler::Ia64UserModeVirtualRange m_systemCallRseBackingStore{};
    Ia64SystemCallWaitCorrelation m_preparedCorrelation{};
    Kernel::System::SystemCallResponse m_response{};
    Foundation::UInt64 m_epoch{0U};
    Foundation::UInt64 m_pendingEpoch{0U};
    Foundation::UInt64 m_readmittedRunSequence{0U};
    Kernel::Scheduler::CpuId m_processor{};
    Ia64SystemCallWaitContinuationState m_state{Ia64SystemCallWaitContinuationState::Uninitialized};
    Ia64SystemCallWaitContinuationError m_terminalError{Ia64SystemCallWaitContinuationError::None};
    bool m_systemCallRecordActionable{false};
};

static_assert(sizeof(Ia64SystemCallWaitContinuationState) == 1U);
static_assert(sizeof(Ia64SystemCallWaitContinuationError) == 1U);
static_assert(sizeof(Ia64SystemCallWaitSupervisorResumeSite) == 1U);
static_assert(Ia64SystemCallWaitEpochPolicy::Maximum == ~Foundation::UInt64{0U});
} // namespace Asteria::Platform::Ia64::System

#endif // ASTERIA_PLATFORM_IA64_SYSTEM_IA64_SYSTEM_CALL_WAIT_CONTINUATION_HPP_INCLUDED
