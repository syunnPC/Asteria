// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_CLIENT_CLIENT_PROGRAM_HPP_INCLUDED
#define ASTERIA_USER_CLIENT_CLIENT_PROGRAM_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Client/ClientAbi.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Client
{
/// What the client works on: a component keeps it in.bss, since its
/// two-page stack could not hold it.
struct ClientBuffers
{
    Kernel::Ipc::MessageEnvelope Message;
    alignas(8) Foundation::UInt8 Frame[Kernel::Ipc::MessageCodec::MaximumWireSizeBytes];
    /// A delivery: its 48-byte header (IpcDeliveryCodec), then the frame.
    alignas(8) Foundation::UInt8 Delivery[48U + Kernel::Ipc::MessageCodec::MaximumWireSizeBytes];
};

/// A client of a service it finds through the Directory, in two rounds. In each it resolves `echo`
/// through the Directory capability of its start block (words 10 and 11), the reply going to the
/// Endpoint it receives on (words 8 and 9) through the send capability on it the block grants
/// (words 12 and 13); that Endpoint's readiness wakes its WaitSet (words 6 and 7) under
/// ClientWaitTag. It then sends Echo ClientEchoes requests, each with a copy of that send
/// capability and a payload word of its own, requires each echo to come back with the request's
/// transaction and payload, and reports on its report Endpoint (words 2 and 3). Between the rounds
/// it waits for the root service's go; the second round first sends through the capability the
/// first resolved, which a restarted Echo no longer honours, and closes it. Returns the code it
/// exits with: ClientExitCode when both rounds resolved, every echo came back, the stale send
/// failed and both reports were sent; otherwise ClientFailedExitCode.
[[nodiscard]] Foundation::UInt64 RunClient(System::SystemCallTransport transport,
                                           const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                                           ClientBuffers& buffers) noexcept;
} // Namespace Asteria::User::Client

#endif // ASTERIA_USER_CLIENT_CLIENT_PROGRAM_HPP_INCLUDED
