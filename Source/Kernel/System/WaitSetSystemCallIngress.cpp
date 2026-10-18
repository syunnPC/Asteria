// SPDX-License-Identifier: Apache-2.0

#include "Kernel/System/WaitSetSystemCallIngress.hpp"

namespace Asteria::Kernel::System
{
namespace
{
[[nodiscard]] constexpr WaitSetSystemCallIngressFailure
MakeFailure(const WaitSetSystemCallIngressError error, const SystemCallIngressFailure& authority = {},
            const Scheduler::ProcessorActiveExecutionBindingFailure& execution = {},
            const WaitSetSystemCallDispatchFailure& waitSet = {}) noexcept
{
    return WaitSetSystemCallIngressFailure{
        .Authority = authority,
        .ActiveExecution = execution,
        .WaitSet = waitSet,
        .Error = error,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
}
} // namespace

void WaitSetSystemCallIngress::WriteAuthorityFailure(WaitSetSystemCallIngressFailure& failure,
                                                     const SystemCallIngressFailure& authority) noexcept
{
    failure = MakeFailure(WaitSetSystemCallIngressError::AuthorityIngressRejected, authority);
}

void WaitSetSystemCallIngress::WriteExecutionFailure(
    WaitSetSystemCallIngressFailure& failure,
    const Scheduler::ProcessorActiveExecutionBindingFailure& execution) noexcept
{
    failure = MakeFailure(WaitSetSystemCallIngressError::ActiveExecutionRejected, {}, execution);
}

void WaitSetSystemCallIngress::WriteDispatchFailure(WaitSetSystemCallIngressFailure& failure,
                                                    const WaitSetSystemCallDispatchFailure& waitSet) noexcept
{
    failure = MakeFailure(WaitSetSystemCallIngressError::WaitSetDispatchRejected, {}, {}, waitSet);
}

bool WaitSetSystemCallIngress::DispatchResolved(
    const Scheduler::ProcessorActiveProtectionDomainBinding& activeProtection,
    const Scheduler::ProcessorActiveExecutionBinding& activeExecution, WaitSetSystemCallDispatcher& dispatcher,
    const SystemCallRequest& request, WaitSetSystemCallDispatchResult& result,
    WaitSetSystemCallIngressFailure& failure) noexcept
{
    Protection::ProtectionDomainBinding protection{};
    Object::CapabilitySpace* capabilities{nullptr};
    SystemCallIngressFailure authorityFailure{};
    if (!SystemCallIngress::ResolveProtectionDomain(activeProtection, protection, capabilities, authorityFailure))
    {
        WriteAuthorityFailure(failure, authorityFailure);
        return false;
    }

    const auto dispatched = dispatcher.Dispatch(*capabilities, activeExecution, request);
    if (!dispatched.HasValue())
    {
        WriteDispatchFailure(failure, dispatched.GetErrorReference());
        return false;
    }
    result = dispatched.GetValue();
    return true;
}
} // namespace Asteria::Kernel::System
