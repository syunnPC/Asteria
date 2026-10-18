// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_DIRECTORY_DIRECTORY_PROGRAM_HPP_INCLUDED
#define ASTERIA_USER_DIRECTORY_DIRECTORY_PROGRAM_HPP_INCLUDED

#include "Abi/ComponentBootstrap.hpp"
#include "Executive/ObjectDirectory/ObjectDirectoryServiceCore.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Ipc/EndpointTypes.hpp"
#include "Kernel/Ipc/IpcDeliveryCodec.hpp"
#include "Kernel/Ipc/MessageCodec.hpp"
#include "User/Directory/DirectoryAbi.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Directory
{
/// What the Directory works on between requests: a component keeps it in
///.bss, since its two-page stack could not hold it.
struct DirectoryBuffers
{
    alignas(8) Foundation::UInt8 Delivery[Kernel::Ipc::IpcDeliveryCodec::MaximumWireSizeBytes];
    Kernel::Ipc::MessageEnvelope Message;
    Kernel::Ipc::EndpointReceiveMetadata Metadata;
    Executive::ObjectDirectory::ObjectDirectoryServiceDispatchOutcome Outcome;
    alignas(8) Foundation::UInt8 Reply[Kernel::Ipc::MessageCodec::MaximumWireSizeBytes];
};

/// The requests the Directory received, the replies it sent and dropped (a
/// full reply queue), the deliveries it could not decode, and
/// the handles it failed to close (a target already destroyed).
struct DirectoryCounters
{
    Foundation::UInt64 Requests;
    Foundation::UInt64 Replies;
    Foundation::UInt64 DroppedReplies;
    Foundation::UInt64 Malformed;
    Foundation::UInt64 CloseFailures;
};

/// The Object Directory as a component: waits on the WaitSet
/// the start block names (words 6-7) for the readiness of the Endpoint it
/// receives on (words 8-9), receives until that Endpoint is empty, hands
/// each request to service, sends the reply it prepared, then closes what
/// the dispatch released. Returns, with DirectoryFailedExitCode, only when a
/// wait or a receive fails.
[[nodiscard]] Foundation::UInt64 RunDirectory(System::SystemCallTransport transport,
                                              const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                                              Executive::ObjectDirectory::ObjectDirectoryServiceCore& service,
                                              DirectoryBuffers& buffers, DirectoryCounters& counters) noexcept;

/// The configuration of the Directory the root service starts.
[[nodiscard]] constexpr Executive::ObjectDirectory::ObjectDirectoryServiceConfiguration
MakeDirectoryConfiguration() noexcept
{
    return Executive::ObjectDirectory::ObjectDirectoryServiceConfiguration{
        .Core = {.DirectoryGeneration = DirectoryGeneration, .LastIssuedRevision = 0U},
        .ExpectedSealedObjectSelector = DirectorySealedSelector,
    };
}
} // Namespace Asteria::User::Directory

#endif // ASTERIA_USER_DIRECTORY_DIRECTORY_PROGRAM_HPP_INCLUDED
