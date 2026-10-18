// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PAGER_OBJECT_CALLS_HPP_INCLUDED
#define ASTERIA_USER_PAGER_OBJECT_CALLS_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "User/Pager/ComponentRecord.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Pager
{
/// Whether response succeeded; its error into systemCall otherwise.
[[nodiscard]] inline bool Check(const Abi::SystemCallResponse& response, Abi::SystemCallError& systemCall) noexcept
{
    if (response.Error != Abi::SystemCallError::None)
    {
        systemCall = response.Error;
        return false;
    }
    return true;
}

/// The object calls the root service's parts share (ComponentLoader,
/// ComponentSupervisor, the scenarios), and the one request buffer they
/// build requests in. It lives in static storage of the pager image; it is
/// not thread-safe.
class ObjectCalls final
{
public:
    constexpr ObjectCalls() noexcept = default;

    ObjectCalls(const ObjectCalls&) = delete;
    ObjectCalls& operator=(const ObjectCalls&) = delete;

    void Configure(const System::SystemCallClient& client) noexcept;

    [[nodiscard]] const System::SystemCallClient& GetClient() const noexcept;
    /// The request buffer, of Abi::FaultResolveRequestSizeBytes, which
    /// every request below overwrites.
    [[nodiscard]] Foundation::UInt8* GetRequest() noexcept;
    void ClearRequest() noexcept;

    /// Creates an object of type from the request buffer's first
    /// requestBytes, charged to the ResourceDomain authority names.
    [[nodiscard]] bool Create(System::CapabilityHandle authority, Foundation::UInt64 type,
                              Foundation::Size requestBytes, System::CapabilityHandle& handle,
                              Abi::SystemCallError& systemCall) noexcept;
    /// Creates an Endpoint of the default limits receiving in the caller's
    /// space, charged to authority.
    [[nodiscard]] bool CreateEndpoint(System::CapabilityHandle authority, System::CapabilityHandle& handle,
                                      Abi::SystemCallError& systemCall) noexcept;
    /// Creates a Frame of pages, or a MemoryObject of permissions, charged
    /// to the ResourceDomain authority names.
    [[nodiscard]] bool CreateMemory(System::CapabilityHandle authority, Foundation::UInt64 type,
                                    Foundation::UInt64 pages, Foundation::UInt8 permissions,
                                    System::CapabilityHandle& handle, Abi::SystemCallError& systemCall) noexcept;
    /// Maps mapping's objects into space at virtualStart with permissions.
    [[nodiscard]] bool Map(System::CapabilityHandle space, const ComponentMapping& mapping,
                           Foundation::UInt64 virtualStart, Foundation::UInt8 permissions,
                           Abi::SystemCallError& systemCall) noexcept;
    /// Destroys the object handle names, then closes handle.
    [[nodiscard]] bool Destroy(System::CapabilityHandle handle, Foundation::UInt64 type,
                               Abi::SystemCallError& systemCall) noexcept;
    /// Closes handle unless it is zero.
    [[nodiscard]] bool Close(System::CapabilityHandle handle, Abi::SystemCallError& systemCall) noexcept;

private:
    const System::SystemCallClient* m_client{nullptr};
    alignas(8) Foundation::UInt8 m_request[Abi::FaultResolveRequestSizeBytes]{};
};
} // namespace Asteria::User::Pager

#endif // ASTERIA_USER_PAGER_OBJECT_CALLS_HPP_INCLUDED
