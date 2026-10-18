// SPDX-License-Identifier: Apache-2.0

#include "User/Pager/ObjectCalls.hpp"

#include "User/Pager/PagerWords.hpp"

namespace Asteria::User::Pager
{
void ObjectCalls::Configure(const System::SystemCallClient& client) noexcept
{
    m_client = &client;
}

const System::SystemCallClient& ObjectCalls::GetClient() const noexcept
{
    return *m_client;
}

Foundation::UInt8* ObjectCalls::GetRequest() noexcept
{
    return m_request;
}

void ObjectCalls::ClearRequest() noexcept
{
    for (Foundation::UInt8& byte : m_request)
    {
        byte = 0U;
    }
}

bool ObjectCalls::Create(const System::CapabilityHandle authority, const Foundation::UInt64 type,
                         const Foundation::Size requestBytes, System::CapabilityHandle& handle,
                         Abi::SystemCallError& systemCall) noexcept
{
    const Abi::SystemCallResponse created = m_client->CreateObject(authority, type, AddressOf(m_request), requestBytes);
    handle = System::CapabilityHandle{created.Value0, created.Value1};
    return Check(created, systemCall);
}

bool ObjectCalls::CreateEndpoint(const System::CapabilityHandle authority, System::CapabilityHandle& handle,
                                 Abi::SystemCallError& systemCall) noexcept
{
    Foundation::UInt8(&request)[Abi::ObjectCreateEndpointRequestSizeBytes] =
        *reinterpret_cast<Foundation::UInt8(*)[Abi::ObjectCreateEndpointRequestSizeBytes]>(m_request);
    System::BuildDefaultEndpointRequest(request);
    return Create(authority, Abi::ObjectTypeEndpoint, Abi::ObjectCreateEndpointRequestSizeBytes, handle, systemCall);
}

bool ObjectCalls::CreateMemory(const System::CapabilityHandle authority, const Foundation::UInt64 type,
                               const Foundation::UInt64 pages, const Foundation::UInt8 permissions,
                               System::CapabilityHandle& handle, Abi::SystemCallError& systemCall) noexcept
{
    ClearRequest();
    StoreWord(m_request + Abi::ObjectCreatePagesOffset, pages);
    m_request[Abi::ObjectCreatePermissionsOffset] = permissions;
    return Create(authority, type,
                  (type == Abi::ObjectTypeFrame) ? Abi::ObjectCreateFrameRequestSizeBytes
                                                 : Abi::ObjectCreateMemoryObjectRequestSizeBytes,
                  handle, systemCall);
}

bool ObjectCalls::Map(const System::CapabilityHandle space, const ComponentMapping& mapping,
                      const Foundation::UInt64 virtualStart, const Foundation::UInt8 permissions,
                      Abi::SystemCallError& systemCall) noexcept
{
    ClearRequest();
    StoreHandle(m_request + Abi::FaultResolveAddressSpaceOffset, space);
    StoreHandle(m_request + Abi::FaultResolveMemoryObjectOffset, mapping.Memory);
    StoreHandle(m_request + Abi::FaultResolveFrameOffset, mapping.Frame);
    StoreWord(m_request + Abi::FaultResolveVirtualStartOffset, virtualStart);
    StoreWord(m_request + Abi::FaultResolvePageCountOffset, mapping.Pages);
    m_request[Abi::FaultResolvePermissionsOffset] = permissions;
    return Check(m_client->MapMemory(AddressOf(m_request)), systemCall);
}

bool ObjectCalls::Destroy(const System::CapabilityHandle handle, const Foundation::UInt64 type,
                          Abi::SystemCallError& systemCall) noexcept
{
    return Check(m_client->DestroyObject(handle, type), systemCall) &&
           Check(m_client->CloseCapability(handle), systemCall);
}

bool ObjectCalls::Close(const System::CapabilityHandle handle, Abi::SystemCallError& systemCall) noexcept
{
    return (handle.Selector == 0U) || Check(m_client->CloseCapability(handle), systemCall);
}
} // namespace Asteria::User::Pager
