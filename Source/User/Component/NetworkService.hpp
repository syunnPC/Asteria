// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_NETWORK_SERVICE_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_NETWORK_SERVICE_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Foundation/Types.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// What the network stack service takes from its image besides the start block: the address of its
/// window, the start block's (Abi::ComponentWindowStartWord; a hosted test passes its own buffer's,
/// its kernel model mapping nothing).
struct NetworkPlatform
{
    Foundation::UInt64 Window;
};

/// The network stack service (Source/User/Network/NetworkStack.hpp to 7; Abi:
/// ComponentBehaviorNetwork), a client of the NIC driver its start block names (words 34-36 and
/// 38-39) and a service of the socket protocol (SocketFrame.hpp) on its Endpoint, with the pure
/// stack (User/Network/NetworkStack.hpp) between them, until it is killed. Its window (words 28 and
/// 29, at least four pages) holds four one-page Frames of its own domain (words 24-27), each mapped
/// through a MemoryObject of its own: pages 0 and 1 the two receive pages, each lent to the driver
/// with a receive credit; page 2 the transmit page of the frames the stack generates (ARP replies,
/// echo replies, a Pending Send's ARP request), such a frame finding it busy being dropped and
/// counted; page 3 the transmit page of clients' Sends, a Send the stack can send finding it busy
/// being held, its bytes kept, until it frees (a Send no rule allows, or one to a peer not yet
/// resolved, is answered at once). A transmit page is busy until the driver's reply. At start the
/// service creates and maps the pages, queries the driver for its MAC address (waits of
/// RetryMicroseconds that poll only the driver-reply Endpoint, at most a minute's worth of
/// timeouts), configures the stack with it and with word 37 (Abi:
/// ComponentNetworkConfigurationWord) and posts both receive credits. Then, before every wait, it
/// drains the driver-reply Endpoint and the service Endpoint to WouldBlock, whichever tag woke it:
/// a received frame goes through the stack, whose output frame takes the generated page and whose
/// datagram answers the oldest Receive its owner has outstanding for the port (or is dropped and
/// counted), and the credit is posted again. A request the driver's Endpoint refuses with
/// WouldBlock is kept and retried before the next wait, which then lasts at most RetryMicroseconds.
/// A request whose generation is not the incarnation (word 1) is answered SocketRestarted; a frame
/// that is not a request of the socket protocol through the sealed Endpoint, and a frame on the
/// driver-reply Endpoint that is no NIC reply, is dropped with every capability it carried closed.
/// A Rule needs Manage in the delivery's effective rights and identity zero; it installs a rule for
/// the sender, the ResourceDomain identity the kernel writes at byte 24 of each delivery's header
/// (IpcDeliveryCodec). The instance ends on a NicDriverRestarted reply and when the kernel refuses
/// a request to the driver with ObjectNotFound, InvalidHandle or Revoked (the root service restarts
/// the stack after its driver). Returns only that or another failure's exit code (Abi:
/// ComponentNetworkFailedExitCode with its step and error). Blocks in WaitSetWait.
[[nodiscard]] Foundation::UInt64 ServeNetwork(const System::SystemCallClient& client,
                                              const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                              const NetworkPlatform& platform) noexcept;
} // Namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_NETWORK_SERVICE_HPP_INCLUDED
