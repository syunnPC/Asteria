// SPDX-License-Identifier: Apache-2.0

#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::System
{
Abi::SystemCallResponse SystemCallClient::Invoke(const Abi::SystemCallNumber number, const CapabilityHandle handle,
                                                 const Foundation::UInt64 argument2, const Foundation::UInt64 argument3,
                                                 const Foundation::UInt64 argument4,
                                                 const Foundation::UInt64 argument5) const noexcept
{
    if (!IsReady())
    {
        return Abi::SystemCallResponse{
            .Error = Abi::SystemCallError::InvalidConfiguration,
            .Value0 = 0U,
            .Value1 = 0U,
        };
    }

    const Abi::SystemCallRequest request{
        .Number = static_cast<Foundation::UInt64>(number),
        .Arguments =
            {
                handle.Selector,
                handle.Authenticator,
                argument2,
                argument3,
                argument4,
                argument5,
            },
    };
    Abi::SystemCallResponse response{
        .Error = Abi::SystemCallError::InvalidConfiguration,
        .Value0 = 0U,
        .Value1 = 0U,
    };
    m_transport.Function(m_transport.Context, &request, &response);
    return response;
}

Abi::SystemCallResponse SystemCallClient::CloseCapability(const CapabilityHandle handle) const noexcept
{
    return Invoke(Abi::SystemCallNumber::CapabilityClose, handle, 0U, 0U);
}

Abi::SystemCallResponse SystemCallClient::QueryCapability(const CapabilityHandle handle) const noexcept
{
    return Invoke(Abi::SystemCallNumber::CapabilityQuery, handle, 0U, 0U);
}

Abi::SystemCallResponse SystemCallClient::DeriveCapability(const CapabilityHandle handle,
                                                           const Foundation::UInt64 rights,
                                                           const Foundation::UInt64 leaseDeadlineTicks,
                                                           const Foundation::UInt64 flags) const noexcept
{
    return Invoke(Abi::SystemCallNumber::CapabilityDerive, handle, rights, leaseDeadlineTicks, flags);
}

Abi::SystemCallResponse
SystemCallClient::DuplicateCapability(const CapabilityHandle handle, const Foundation::UInt64 rights,
                                      const Foundation::UInt64 leaseDeadlineTicks) const noexcept
{
    return Invoke(Abi::SystemCallNumber::CapabilityDuplicate, handle, rights, leaseDeadlineTicks);
}

Abi::SystemCallResponse SystemCallClient::RevokeCapability(const CapabilityHandle handle) const noexcept
{
    return Invoke(Abi::SystemCallNumber::CapabilityRevoke, handle, 0U, 0U);
}

Abi::SystemCallResponse SystemCallClient::Send(const CapabilityHandle endpoint, const Foundation::UInt64 frameAddress,
                                               const Foundation::UInt64 frameSizeBytes) const noexcept
{
    return Invoke(Abi::SystemCallNumber::IpcSend, endpoint, frameAddress, frameSizeBytes);
}

Abi::SystemCallResponse SystemCallClient::Receive(const CapabilityHandle endpoint,
                                                  const Foundation::UInt64 deliveryAddress,
                                                  const Foundation::UInt64 deliveryCapacityBytes) const noexcept
{
    return Invoke(Abi::SystemCallNumber::IpcReceive, endpoint, deliveryAddress, deliveryCapacityBytes);
}

Abi::SystemCallResponse SystemCallClient::Wait(const CapabilityHandle waitSet,
                                               const Foundation::UInt64 timeoutMicroseconds) const noexcept
{
    return Invoke(Abi::SystemCallNumber::WaitSetWait, waitSet, timeoutMicroseconds, 0U);
}

Abi::SystemCallResponse SystemCallClient::DebugWrite(const CapabilityHandle channel,
                                                     const Foundation::UInt64 textAddress,
                                                     const Foundation::UInt64 textSizeBytes) const noexcept
{
    return Invoke(Abi::SystemCallNumber::DebugWrite, channel, textAddress, textSizeBytes);
}

Abi::SystemCallResponse SystemCallClient::FaultResolve(const CapabilityHandle faultToken,
                                                       const Abi::FaultResolveKind kind,
                                                       const Foundation::UInt64 requestAddress) const noexcept
{
    return Invoke(Abi::SystemCallNumber::FaultResolve, faultToken, static_cast<Foundation::UInt64>(kind),
                  requestAddress, Abi::FaultResolveRequestSizeBytes);
}

Abi::SystemCallResponse SystemCallClient::FaultTerminate(const CapabilityHandle faultToken) const noexcept
{
    return Invoke(Abi::SystemCallNumber::FaultResolve, faultToken,
                  static_cast<Foundation::UInt64>(Abi::FaultResolveKind::Terminate), 0U, 0U);
}

Abi::SystemCallResponse SystemCallClient::CreateEndpoint(const CapabilityHandle domain,
                                                         const Foundation::UInt64 requestAddress) const noexcept
{
    return Invoke(Abi::SystemCallNumber::ObjectCreate, domain, Abi::ObjectTypeEndpoint, requestAddress,
                  Abi::ObjectCreateEndpointRequestSizeBytes);
}

Abi::SystemCallResponse SystemCallClient::CreateObject(const CapabilityHandle domain, const Foundation::UInt64 type,
                                                       const Foundation::UInt64 requestAddress,
                                                       const Foundation::UInt64 requestSizeBytes) const noexcept
{
    return Invoke(Abi::SystemCallNumber::ObjectCreate, domain, type, requestAddress, requestSizeBytes);
}

Abi::SystemCallResponse SystemCallClient::DestroyObject(const CapabilityHandle object,
                                                        const Foundation::UInt64 type) const noexcept
{
    return Invoke(Abi::SystemCallNumber::ObjectDestroy, object, type, 0U, 0U);
}

Abi::SystemCallResponse SystemCallClient::TerminateThread(const CapabilityHandle thread) const noexcept
{
    return Invoke(Abi::SystemCallNumber::ThreadTerminate, thread, 0U, 0U, 0U);
}

Abi::SystemCallResponse SystemCallClient::BindWaitSet(const CapabilityHandle waitSet, const CapabilityHandle endpoint,
                                                      const Foundation::UInt64 tag) const noexcept
{
    return Invoke(Abi::SystemCallNumber::WaitSetBind, waitSet, endpoint.Selector, endpoint.Authenticator, tag, 0U);
}

Abi::SystemCallResponse SystemCallClient::ControlIommu(const CapabilityHandle domain,
                                                       const Foundation::UInt64 operation,
                                                       const Foundation::UInt64 argument3,
                                                       const Foundation::UInt64 argument4) const noexcept
{
    return Invoke(Abi::SystemCallNumber::IommuControl, domain, operation, argument3, argument4, 0U);
}

Abi::SystemCallResponse SystemCallClient::ControlInterrupt(const CapabilityHandle interrupt,
                                                           const Foundation::UInt64 operation) const noexcept
{
    return Invoke(Abi::SystemCallNumber::InterruptControl, interrupt, operation, 0U, 0U, 0U);
}

Abi::SystemCallResponse SystemCallClient::ControlDevice(const CapabilityHandle function,
                                                        const Foundation::UInt64 operation,
                                                        const CapabilityHandle domain) const noexcept
{
    return Invoke(Abi::SystemCallNumber::DeviceControl, function, operation, domain.Selector, domain.Authenticator, 0U);
}

Abi::SystemCallResponse SystemCallClient::MapMemory(const Foundation::UInt64 requestAddress) const noexcept
{
    return Invoke(Abi::SystemCallNumber::MemoryMap, CapabilityHandle{}, 0U, requestAddress,
                  Abi::FaultResolveRequestSizeBytes);
}

Abi::SystemCallResponse SystemCallClient::CreateThread(const CapabilityHandle domain,
                                                       const Foundation::UInt64 requestAddress) const noexcept
{
    return Invoke(Abi::SystemCallNumber::ObjectCreate, domain, Abi::ObjectTypeThread, requestAddress,
                  Abi::ObjectCreateThreadRequestSizeBytes);
}

Abi::SystemCallResponse SystemCallClient::ExitThread(const Foundation::UInt64 exitCode) const noexcept
{
    return Invoke(Abi::SystemCallNumber::ThreadExit, CapabilityHandle{exitCode, 0U}, 0U, 0U, 0U);
}

Abi::SystemCallResponse SystemCallClient::UnmapMemory(const CapabilityHandle space,
                                                      const Foundation::UInt64 virtualStart,
                                                      const Foundation::UInt64 pages) const noexcept
{
    return Invoke(Abi::SystemCallNumber::MemoryUnmap, space, virtualStart, pages, 0U);
}
Abi::SystemCallResponse SystemCallClient::GrantCapability(const CapabilityHandle source,
                                                          const CapabilityHandle destination,
                                                          const Foundation::UInt16 rights,
                                                          const Foundation::UInt8 objectType,
                                                          const Foundation::UInt64 disposition) const noexcept
{
    return Invoke(Abi::SystemCallNumber::CapabilityGrant, source, destination.Selector, destination.Authenticator,
                  static_cast<Foundation::UInt64>(rights) |
                      (static_cast<Foundation::UInt64>(objectType) << Abi::CapabilityGrantTypeShift),
                  disposition);
}
Abi::SystemCallResponse SystemCallClient::PortIo(const CapabilityHandle region, const Foundation::UInt64 offset,
                                                 const Foundation::UInt64 width, const Foundation::UInt64 operation,
                                                 const Foundation::UInt64 value) const noexcept
{
    return Invoke(Abi::SystemCallNumber::PortIo, region, offset, width, operation, value);
}

} // namespace Asteria::User::System
