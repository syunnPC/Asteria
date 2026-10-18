// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_SERVICE_CAPABILITIES_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_SERVICE_CAPABILITIES_HPP_INCLUDED

#include "Abi/SystemCallAbi.hpp"
#include "Foundation/Types.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// A service survives what one request can do to it: a request it cannot serve is dropped with
/// every capability it carried closed, and a reply it cannot send is dropped; neither ends the
/// service. What ends it is a failure of its own: a wait that fails, a Receive error (a failed
/// Receive keeps the message queued, so continuing would spin on it), a Close that fails other than
/// on a handle already gone, or, for a driver, a device that no longer behaves. Every service
/// receives into a buffer that holds the largest frame, so no message can be too large for it.
[[nodiscard]] constexpr bool IsStaleHandle(const Abi::SystemCallError error) noexcept
{
    return (error == Abi::SystemCallError::InvalidHandle) || (error == Abi::SystemCallError::ObjectNotFound) ||
           (error == Abi::SystemCallError::Revoked) || (error == Abi::SystemCallError::Expired);
}

/// Closes handle: false only when the Close failed other than on a stale
/// handle.
[[nodiscard]] inline bool Closed(const System::SystemCallClient& client, const System::CapabilityHandle handle) noexcept
{
    const Abi::SystemCallError error = client.CloseCapability(handle).Error;
    return (error == Abi::SystemCallError::None) || IsStaleHandle(error);
}

/// Closes the capabilities a delivery installed from attachment first on;
/// false when a Close failed other than on a stale handle.
[[nodiscard, gnu::noinline]] inline bool CloseAttachments(const System::SystemCallClient& client,
                                                          const Foundation::UInt8* const delivery,
                                                          const Foundation::Size first) noexcept
{
    // The kernel delivered a frame it decoded, so its count is at most four.
    const Foundation::Size count = GetEchoAttachmentCount(delivery);
    bool closed{true};
    for (Foundation::Size index{first}; (index < count) && (index < 4U); ++index)
    {
        closed = Closed(client, ReadEchoHandle(delivery, index)) && closed;
    }
    return closed;
}
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_SERVICE_CAPABILITIES_HPP_INCLUDED
