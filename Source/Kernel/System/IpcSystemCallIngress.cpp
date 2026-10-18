// SPDX-License-Identifier: Apache-2.0

#include "Kernel/System/SystemCallIngress.hpp"

#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/System/SystemCallScratch.hpp"

namespace Asteria::Kernel::System
{
namespace
{
[[nodiscard]] constexpr IpcSystemCallDispatchFailure EmptyIpcFailure() noexcept
{
    return IpcSystemCallDispatchFailure{
        .Error = IpcSystemCallDispatchError::None,
        .Endpoint = Ipc::EndpointErrorCode::None,
        .UserMemory = Memory::UserMemoryAccessError::None,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr SystemCallIngressFailure
MakeFailure(const SystemCallIngressError error, const IpcSystemCallDispatchFailure ipc = EmptyIpcFailure()) noexcept
{
    return SystemCallIngressFailure{
        .Error = error,
        .ProcessorRunLoop = Scheduler::ProcessorKernelRunLoopError::None,
        .ProtectionDomain = Protection::ProtectionDomainError::None,
        .Ipc = ipc,
        .Reserved = 0U,
    };
}
} // namespace

bool SystemCallIngress::DispatchWithUserMemory(const Scheduler::ProcessorActiveProtectionDomainBinding& active,
                                               Memory::IUserMemoryAccess& userMemory, SystemCallScratch& scratch,
                                               const SystemCallRequest& request, SystemCallResponse& response,
                                               SystemCallIngressFailure& failure,
                                               Memory::IPagerResolution* const pagerResolution,
                                               IObjectManagement* const objectManagement) noexcept
{
    if (!IpcSystemCallDispatcher::UsesUserMemory(request.Number))
    {
        return DispatchWithoutUserMemory(active, request, response, failure);
    }

    Protection::ProtectionDomainBinding protection{};
    Object::CapabilitySpace* capabilities{nullptr};
    if (!ResolveProtectionDomain(active, protection, capabilities, failure))
    {
        return false;
    }
    if (!protection.ResourceDomain.IsValid())
    {
        failure = MakeFailure(SystemCallIngressError::ResourceDomainInvalid);
        return false;
    }
    if (!userMemory.IsReady())
    {
        failure = MakeFailure(SystemCallIngressError::UserMemoryNotReady);
        return false;
    }
    const Memory::AddressSpaceId userAddressSpace = userMemory.GetAddressSpaceId();
    if (!userAddressSpace.IsValid())
    {
        failure = MakeFailure(SystemCallIngressError::UserMemoryAddressSpaceInvalid);
        return false;
    }
    if (!userAddressSpace.IsEqualTo(protection.AddressSpace))
    {
        failure = MakeFailure(SystemCallIngressError::UserMemoryAddressSpaceMismatch);
        return false;
    }

    const SystemCallContext context{
        .Capabilities = capabilities,
        .UserMemory = &userMemory,
        .Scratch = &scratch,
        .ResourceDomain = protection.ResourceDomain,
        .AddressSpace = protection.AddressSpace,
        .PagerResolution = pagerResolution,
        .ObjectManagement = objectManagement,
        .ProtectionDomain = protection.Identity,
    };
    const auto completed = IpcSystemCallDispatcher::Dispatch(context, request);
    if (!completed.HasValue())
    {
        failure = MakeFailure(SystemCallIngressError::IpcDispatchRejected, completed.GetError());
        return false;
    }
    response = completed.GetValue();
    return true;
}
} // namespace Asteria::Kernel::System
