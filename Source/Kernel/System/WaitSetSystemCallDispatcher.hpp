// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SYSTEM_WAIT_SET_SYSTEM_CALL_DISPATCHER_HPP_INCLUDED
#define ASTERIA_KERNEL_SYSTEM_WAIT_SET_SYSTEM_CALL_DISPATCHER_HPP_INCLUDED

#include "Hal/Time/IMonotonicClock.hpp"
#include "Foundation/Compiler.hpp"
#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/System/SystemCallDispatcher.hpp"

namespace Asteria::Kernel::Scheduler
{
struct ProcessorActiveExecutionBinding;
}

namespace Asteria::Kernel::System
{
enum class WaitSetSystemCallDisposition : Foundation::UInt8
{
    Completed,
    SuspendPrepared,
};

enum class WaitSetSystemCallDispatchError : Foundation::UInt8
{
    None,
    InvalidConfiguration,
    InvalidActiveBinding,
    ThreadPinRejected,
    SchedulingContextPinRejected,
    ThreadInstanceMismatch,
    SchedulingContextInstanceMismatch,
    ActiveThreadStateMismatch,
    ActiveSchedulingContextStateMismatch,
    WaitSetPinRejected,
    WaitSetInstanceMismatch,
    DispatcherRejected,
    DispatcherReceiptMismatch,
    UnexpectedDispatcherState,
};

struct WaitSetSystemCallDispatchFailure
{
    Ipc::WaitSetThreadDispatcherFailure Dispatcher;
    Object::ObjectPinError WaitSetPin;
    Object::ObjectPinError ThreadPin;
    Object::ObjectPinError SchedulingContextPin;
    WaitSetSystemCallDispatchError Error;
    Foundation::UInt8 Reserved[2];
};

/// One architecture-neutral WaitSet syscall transition. A Completed result is
/// ready for target return using Response. SuspendPrepared means the target
/// must finish the trusted active run as Blocked and pass its exact block token
/// to the same WaitSetThreadDispatcher before switching away. Response is not
/// user-visible in SuspendPrepared. Wait is valid for SuspendPrepared and for
/// successful Completed results; a user-error Completed result carries an
/// empty Wait receipt.
struct WaitSetSystemCallDispatchResult
{
    SystemCallResponse Response;
    Ipc::WaitSetThreadDispatcherReceipt Wait;
    WaitSetSystemCallDisposition Disposition;
    Foundation::UInt8 Reserved[7];
};

/// Authorizes and prepares one WaitSet wait for the trusted active execution. The CapabilitySpace
/// lookup is dispatch-scoped. Its authority lease is destroyed before Dispatch returns
/// SuspendPrepared; the composed WaitSetThreadDispatcher independently retains exact WaitSet,
/// Thread, and SchedulingContext lifetime pins. This object and its collaborators are persistent
/// per resumable syscall transaction and must not be stack-local in a target entry path. Calls may
/// spin, do not allocate, and are forbidden from interrupt context.
class WaitSetSystemCallDispatcher final
{
public:
    /// Preconditions:
    /// - objectRegistry is the registry used by dispatcher and every caller;
    /// - objectRegistry and dispatcher outlive this object;
    /// - dispatcher is dedicated to one exact resumable syscall at a time.
    constexpr WaitSetSystemCallDispatcher(Object::ObjectRegistry& objectRegistry,
                                          Ipc::WaitSetThreadDispatcher& dispatcher) noexcept
        : m_objectRegistry{objectRegistry}, m_dispatcher{dispatcher}
    {
    }

    WaitSetSystemCallDispatcher(const WaitSetSystemCallDispatcher&) = delete;
    WaitSetSystemCallDispatcher& operator=(const WaitSetSystemCallDispatcher&) = delete;
    WaitSetSystemCallDispatcher(WaitSetSystemCallDispatcher&&) = delete;
    WaitSetSystemCallDispatcher& operator=(WaitSetSystemCallDispatcher&&) = delete;

    [[nodiscard]] static constexpr bool IsWaitSetSystemCall(const Foundation::UInt64 number) noexcept
    {
        return number == static_cast<Foundation::UInt64>(SystemCallNumber::WaitSetWait);
    }

    /// The longest WaitSetWait timeout, in microseconds.
    static constexpr Foundation::UInt64 MaximumTimeoutMicroseconds{0xFFFF'FFFFU};

    /// Lets WaitSetWait take a timeout: clock gives the local ticks a
    /// deadline is set in, ticksPerMicrosecond (nonzero, at most 2^16)
    /// converts it; once only. The owner then expires the waiter's
    /// Dispatcher (WaitSetThreadDispatcher::Expire). Without it a nonzero timeout is
    /// InvalidConfiguration. clock outlives this object.
    [[nodiscard]] bool ConfigureTimeouts(const Hal::Time::IMonotonicClock& clock,
                                         Foundation::UInt64 ticksPerMicrosecond) noexcept;

    /// On SuspendPrepared, the dispatcher retains all lifetime pins and the
    /// authority lease has already ended. Caller identity comes exclusively
    /// from active; no ABI argument is interpreted as a Thread or context.
    [[nodiscard]] Foundation::Result<WaitSetSystemCallDispatchResult, WaitSetSystemCallDispatchFailure>
    Dispatch(Object::CapabilitySpace& caller, const Scheduler::ProcessorActiveExecutionBinding& active,
             const SystemCallRequest& request) noexcept;

    /// Consumes one exact completed wait after the blocked syscall resumes.
    /// This method accepts no user handle or caller identity. active must name
    /// a run of the same Thread/SchedulingContext generation after the
    /// blocked one: the first that entered, since an admission rolled back
    /// before entry also consumes a run sequence.
    /// Success resets the underlying dispatcher for reuse and returns
    /// Value0=WaitSet tag and Value1=registration sequence. A foreign active
    /// binding is rejected before the exact-once completion is consumed.
    [[nodiscard]] Foundation::Result<WaitSetSystemCallDispatchResult, WaitSetSystemCallDispatchFailure>
    ConsumeCompletion(const Scheduler::ProcessorActiveExecutionBinding& active) noexcept;

private:
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    DispatchTransaction(Object::CapabilitySpace& caller, const Scheduler::ProcessorActiveExecutionBinding& active,
                        const SystemCallRequest& request, WaitSetSystemCallDispatchResult& result,
                        WaitSetSystemCallDispatchFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    ValidateActiveBinding(const Scheduler::ProcessorActiveExecutionBinding& active, Object::ObjectPin& threadPin,
                          Object::ObjectPin& schedulingContextPin, WaitSetSystemCallDispatchFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    DispatchAuthorized(Object::CapabilitySpace& caller, const Scheduler::ProcessorActiveExecutionBinding& active,
                       const SystemCallRequest& request, WaitSetSystemCallDispatchResult& result,
                       WaitSetSystemCallDispatchFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    PrepareAuthorized(Ipc::WaitSetId waitSet, const Scheduler::ProcessorActiveExecutionBinding& active,
                      WaitSetSystemCallDispatchResult& result, WaitSetSystemCallDispatchFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    ConsumeImmediateCompletion(const Ipc::WaitSetThreadDispatcherReceipt& prepared,
                               WaitSetSystemCallDispatchResult& result,
                               WaitSetSystemCallDispatchFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    ConsumeValidatedCompletion(const Scheduler::ProcessorActiveExecutionBinding& active,
                               WaitSetSystemCallDispatchResult& result,
                               WaitSetSystemCallDispatchFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    ConsumeExactCompletion(const Ipc::WaitSetThreadDispatcherSnapshot& snapshot,
                           WaitSetSystemCallDispatchResult& result, WaitSetSystemCallDispatchFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    ConsumeUnavailableCompletion(const Ipc::WaitSetThreadDispatcherSnapshot& snapshot,
                                 WaitSetSystemCallDispatchFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    ValidatePreparedReceipt(const Ipc::WaitSetThreadDispatcherReceipt& receipt,
                            const Scheduler::ProcessorActiveExecutionBinding& active, Ipc::WaitSetId waitSet,
                            WaitSetSystemCallDispatchFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] bool
    ValidateCompletionSnapshot(const Ipc::WaitSetThreadDispatcherSnapshot& snapshot,
                               const Scheduler::ProcessorActiveExecutionBinding& active,
                               WaitSetSystemCallDispatchFailure& failure) noexcept;
    ASTERIA_NOINLINE_NOIPA [[nodiscard]] SystemCallError DecodeTimeout(const SystemCallRequest& request) noexcept;
    ASTERIA_NOINLINE_NOIPA void RejectDispatcherReceipt(const Ipc::WaitSetThreadDispatcherReceipt& receipt,
                                                        WaitSetSystemCallDispatchFailure& failure) noexcept;

    Object::ObjectRegistry& m_objectRegistry;
    Ipc::WaitSetThreadDispatcher& m_dispatcher;
    Foundation::UInt64 m_transactionGeneration{0U};
    const Hal::Time::IMonotonicClock* m_clock{nullptr};
    Foundation::UInt64 m_ticksPerMicrosecond{0U};
    // The deadline of the wait being prepared, or zero.
    Foundation::UInt64 m_deadline{0U};
};

/// Internal correlation rules of WaitSetSystemCallDispatcher. Each rule is a
/// pure function of the composed dispatcher's identity and the values it
/// observed; none reads or mutates dispatcher state.
namespace Detail
{
/// Accepts a receipt returned by Prepare only when it names dispatcher's
/// current nonzero transaction generation, as captured in current, for
/// exactly waitSet and the Thread, SchedulingContext, and run sequence of
/// active. A Prepared receipt carries no readiness, block, tag, or queue
/// field; an immediately Completed receipt names its registration and carries
/// no block or queue sequence. Returns DispatcherReceiptMismatch for a
/// contradiction and UnexpectedDispatcherState for any other receipt state.
[[nodiscard]] WaitSetSystemCallDispatchError
ValidatePreparedReceipt(const Ipc::WaitSetThreadDispatcher& dispatcher,
                        const Ipc::WaitSetThreadDispatcherSnapshot& current,
                        const Ipc::WaitSetThreadDispatcherReceipt& receipt,
                        const Scheduler::ProcessorActiveExecutionBinding& active, Ipc::WaitSetId waitSet) noexcept;

/// Accepts a Completed snapshot of dispatcher only for transactionGeneration,
/// the nonzero generation retained by the suspended syscall, after a blocked
/// wake has committed without a retained claim or queue reservation, and only
/// when active is a later run of the same Thread and SchedulingContext.
/// Returns DispatcherReceiptMismatch otherwise.
[[nodiscard]] WaitSetSystemCallDispatchError
ValidateCompletionSnapshot(const Ipc::WaitSetThreadDispatcher& dispatcher, Foundation::UInt64 transactionGeneration,
                           const Ipc::WaitSetThreadDispatcherSnapshot& snapshot,
                           const Scheduler::ProcessorActiveExecutionBinding& active) noexcept;

/// Describes a ConsumeCompletion observation of a transaction that is not
/// Completed. A Faulted snapshot reports its retained diagnostic; every other
/// state reports CompletionUnavailable and leaves a later completion intact.
[[nodiscard]] WaitSetSystemCallDispatchFailure
RejectUnavailableCompletion(const Ipc::WaitSetThreadDispatcherSnapshot& snapshot) noexcept;
} // Namespace Detail

static_assert(sizeof(WaitSetSystemCallDisposition) == 1U);
static_assert(sizeof(WaitSetSystemCallDispatchError) == 1U);
static_assert(sizeof(WaitSetSystemCallDispatchFailure) == 32U);
static_assert(alignof(WaitSetSystemCallDispatchFailure) == 1U);
static_assert(sizeof(WaitSetSystemCallDispatchResult) == 160U);
static_assert(alignof(WaitSetSystemCallDispatchResult) == 8U);
} // Namespace Asteria::Kernel::System

#endif // ASTERIA_KERNEL_SYSTEM_WAIT_SET_SYSTEM_CALL_DISPATCHER_HPP_INCLUDED
