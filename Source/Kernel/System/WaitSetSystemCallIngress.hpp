// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SYSTEM_WAIT_SET_SYSTEM_CALL_INGRESS_HPP_INCLUDED
#define ASTERIA_KERNEL_SYSTEM_WAIT_SET_SYSTEM_CALL_INGRESS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/ProcessorKernelRunLoop.hpp"
#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/System/WaitSetSystemCallDispatcher.hpp"

namespace Asteria::Kernel::System
{
enum class WaitSetSystemCallIngressError : Foundation::UInt8
{
    None,
    AuthorityIngressRejected,
    ActiveExecutionRejected,
    WaitSetDispatchRejected,
};

struct WaitSetSystemCallIngressFailure
{
    SystemCallIngressFailure Authority;
    Scheduler::ProcessorActiveExecutionBindingFailure ActiveExecution;
    WaitSetSystemCallDispatchFailure WaitSet;
    WaitSetSystemCallIngressError Error;
    Foundation::UInt8 Reserved[7];
};

/// Resumable ingress for syscall 8. The initial call resolves capability
/// authority and active execution independently from the owning run loop. The
/// resumed path consumes only the exact retained completion and never repeats
/// user-selected capability lookup.
class WaitSetSystemCallIngress final
{
public:
    WaitSetSystemCallIngress() = delete;

    /// On true, result is either a completed user return or one exact suspend
    /// preparation. On false, result is unchanged and failure is canonical.
    template <typename TNativeContext>
    [[nodiscard]] static bool Dispatch(Scheduler::ProcessorKernelRunLoop<TNativeContext>& runLoop,
                                       WaitSetSystemCallDispatcher& dispatcher, const SystemCallRequest& request,
                                       WaitSetSystemCallDispatchResult& result,
                                       WaitSetSystemCallIngressFailure& failure) noexcept
    {
        const auto protection = runLoop.GetActiveProtectionDomainBinding();
        if (!protection.HasValue())
        {
            SystemCallIngressFailure authority{};
            SystemCallIngress::WriteProcessorFailure(authority, protection.GetError());
            WriteAuthorityFailure(failure, authority);
            return false;
        }

        const auto execution = runLoop.GetActiveExecutionBinding();
        if (!execution.HasValue())
        {
            WriteExecutionFailure(failure, execution.GetError());
            return false;
        }
        return DispatchResolved(protection.GetValue(), execution.GetValue(), dispatcher, request, result, failure);
    }

    /// Completes the suspended syscall after its exact Thread resumes. No ABI
    /// request or capability handle is accepted. On success result is
    /// Completed with Value0=WaitSet tag and Value1=registration sequence.
    template <typename TNativeContext>
    [[nodiscard]] static bool ConsumeCompletion(Scheduler::ProcessorKernelRunLoop<TNativeContext>& runLoop,
                                                WaitSetSystemCallDispatcher& dispatcher,
                                                WaitSetSystemCallDispatchResult& result,
                                                WaitSetSystemCallIngressFailure& failure) noexcept
    {
        const auto execution = runLoop.GetActiveExecutionBinding();
        if (!execution.HasValue())
        {
            WriteExecutionFailure(failure, execution.GetError());
            return false;
        }
        const auto consumed = dispatcher.ConsumeCompletion(execution.GetValue());
        if (!consumed.HasValue())
        {
            WriteDispatchFailure(failure, consumed.GetErrorReference());
            return false;
        }
        result = consumed.GetValue();
        return true;
    }

private:
    static void WriteAuthorityFailure(WaitSetSystemCallIngressFailure& failure,
                                      const SystemCallIngressFailure& authority) noexcept;
    static void WriteExecutionFailure(WaitSetSystemCallIngressFailure& failure,
                                      const Scheduler::ProcessorActiveExecutionBindingFailure& execution) noexcept;
    static void WriteDispatchFailure(WaitSetSystemCallIngressFailure& failure,
                                     const WaitSetSystemCallDispatchFailure& waitSet) noexcept;
    [[nodiscard]] static bool DispatchResolved(const Scheduler::ProcessorActiveProtectionDomainBinding& protection,
                                               const Scheduler::ProcessorActiveExecutionBinding& execution,
                                               WaitSetSystemCallDispatcher& dispatcher,
                                               const SystemCallRequest& request,
                                               WaitSetSystemCallDispatchResult& result,
                                               WaitSetSystemCallIngressFailure& failure) noexcept;
};

static_assert(sizeof(WaitSetSystemCallIngressError) == 1U);
static_assert(sizeof(WaitSetSystemCallIngressFailure) == 56U);
static_assert(alignof(WaitSetSystemCallIngressFailure) == 1U);
} // namespace Asteria::Kernel::System

#endif // ASTERIA_KERNEL_SYSTEM_WAIT_SET_SYSTEM_CALL_INGRESS_HPP_INCLUDED
