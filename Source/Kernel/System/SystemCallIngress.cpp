// SPDX-License-Identifier: Apache-2.0

#include "Kernel/System/SystemCallIngress.hpp"

#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"

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
MakeFailure(const SystemCallIngressError error,
            const Scheduler::ProcessorKernelRunLoopError processor = Scheduler::ProcessorKernelRunLoopError::None,
            const Protection::ProtectionDomainError protection = Protection::ProtectionDomainError::None,
            const IpcSystemCallDispatchFailure ipc = EmptyIpcFailure()) noexcept
{
    return SystemCallIngressFailure{
        .Error = error,
        .ProcessorRunLoop = processor,
        .ProtectionDomain = protection,
        .Ipc = ipc,
        .Reserved = 0U,
    };
}
} // namespace

void SystemCallIngress::WriteProcessorFailure(SystemCallIngressFailure& failure,
                                              const Scheduler::ProcessorKernelRunLoopError error) noexcept
{
    failure = MakeFailure(SystemCallIngressError::ProcessorRunLoopRejected, error);
}

bool SystemCallIngress::ResolveProtectionDomain(const Scheduler::ProcessorActiveProtectionDomainBinding& active,
                                                Protection::ProtectionDomainBinding& protection,
                                                Object::CapabilitySpace*& capabilities,
                                                SystemCallIngressFailure& failure) noexcept
{
    if (active.Storage == nullptr)
    {
        failure = MakeFailure(SystemCallIngressError::ProtectionDomainMissing);
        return false;
    }

    const auto binding = active.Storage->GetBinding();
    if (!binding.HasValue())
    {
        failure = MakeFailure(SystemCallIngressError::ProtectionDomainRejected,
                              Scheduler::ProcessorKernelRunLoopError::None, binding.GetError());
        return false;
    }
    const Protection::ProtectionDomainBinding& liveProtection = binding.GetValue();
    if (!active.Identity.IsValid() || !liveProtection.Identity.IsValid() ||
        !liveProtection.Identity.IsEqualTo(active.Identity))
    {
        failure = MakeFailure(SystemCallIngressError::ProtectionDomainIdentityMismatch);
        return false;
    }

    if (active.AddressSpaceStorage == nullptr)
    {
        failure = MakeFailure(SystemCallIngressError::ActiveAddressSpaceMissing);
        return false;
    }
    if (liveProtection.AddressSpaceStorage == nullptr)
    {
        failure = MakeFailure(SystemCallIngressError::ProtectionAddressSpaceMissing);
        return false;
    }
    if (!active.AddressSpaceIdentity.IsValid())
    {
        failure = MakeFailure(SystemCallIngressError::ActiveAddressSpaceIdentityInvalid);
        return false;
    }
    if (!liveProtection.AddressSpace.IsValid())
    {
        failure = MakeFailure(SystemCallIngressError::ProtectionAddressSpaceIdentityInvalid);
        return false;
    }
    if (active.AddressSpaceStorage != liveProtection.AddressSpaceStorage)
    {
        failure = MakeFailure(SystemCallIngressError::AddressSpaceStorageMismatch);
        return false;
    }
    if (!active.AddressSpaceIdentity.IsEqualTo(liveProtection.AddressSpace))
    {
        failure = MakeFailure(SystemCallIngressError::AddressSpaceIdentityMismatch);
        return false;
    }

    Object::CapabilitySpace* const liveCapabilities = liveProtection.Capabilities;
    if (liveCapabilities == nullptr)
    {
        failure = MakeFailure(SystemCallIngressError::CapabilitySpaceMissing);
        return false;
    }
    if (!liveCapabilities->IsReady())
    {
        failure = MakeFailure(SystemCallIngressError::CapabilitySpaceNotReady);
        return false;
    }
    if (!liveProtection.CapabilitySpace.IsValid())
    {
        failure = MakeFailure(SystemCallIngressError::RecordedCapabilitySpaceIdentifierInvalid);
        return false;
    }
    const Object::CapabilitySpaceIdentifier liveIdentifier = liveCapabilities->GetIdentifier();
    if (!liveIdentifier.IsValid())
    {
        failure = MakeFailure(SystemCallIngressError::LiveCapabilitySpaceIdentifierInvalid);
        return false;
    }
    if (liveProtection.CapabilitySpace.GetValue() != liveIdentifier.GetValue())
    {
        failure = MakeFailure(SystemCallIngressError::CapabilitySpaceIdentifierMismatch);
        return false;
    }

    protection = liveProtection;
    capabilities = liveCapabilities;
    return true;
}

bool SystemCallIngress::DispatchWithoutUserMemory(const Scheduler::ProcessorActiveProtectionDomainBinding& active,
                                                  const SystemCallRequest& request, SystemCallResponse& response,
                                                  SystemCallIngressFailure& failure) noexcept
{
    Protection::ProtectionDomainBinding protection{};
    Object::CapabilitySpace* capabilities{nullptr};
    if (!ResolveProtectionDomain(active, protection, capabilities, failure))
    {
        return false;
    }
    if (IpcSystemCallDispatcher::UsesUserMemory(request.Number) ||
        (request.Number == static_cast<Foundation::UInt64>(SystemCallNumber::WaitSetWait)))
    {
        response = SystemCallResponse{
            .Error = SystemCallError::InvalidConfiguration,
            .Value0 = 0U,
            .Value1 = 0U,
        };
        return true;
    }
    response = SystemCallDispatcher::Dispatch(*capabilities, request);
    return true;
}
} // namespace Asteria::Kernel::System
