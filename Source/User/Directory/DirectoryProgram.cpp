// SPDX-License-Identifier: Apache-2.0

#include "User/Directory/DirectoryProgram.hpp"

// Target images define placement new themselves (Amd64DirectoryImage.cpp).
#if defined(ASTERIA_TARGET_IA64) || defined(ASTERIA_TARGET_AMD64)
void* operator new(Asteria::Foundation::Size, void*) noexcept;
#else
#include <new>
#endif

namespace Asteria::User::Directory
{
namespace
{
[[nodiscard]] System::CapabilityHandle ToUser(const Kernel::Object::CapabilityHandle handle) noexcept
{
    return System::CapabilityHandle{handle.GetSelector(), handle.GetAuthenticator()};
}

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const pointer) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(pointer));
}

// One delivery: decoded, dispatched, its reply sent (or dropped when the
// reply queue is full) before the handles the dispatch released are closed,
// as the service core requires. A delivery that does not decode is counted
// and dropped.
[[gnu::noinline]] void Serve(const System::SystemCallClient& client,
                             Executive::ObjectDirectory::ObjectDirectoryServiceCore& service, DirectoryBuffers& buffers,
                             const Foundation::Size bytes, DirectoryCounters& counters) noexcept
{
    const auto decoded = Kernel::Ipc::IpcDeliveryCodec::Decode(Kernel::Ipc::ReadOnlyByteView{buffers.Delivery, bytes},
                                                               buffers.Metadata, buffers.Message);
    if (decoded.Error != Kernel::Ipc::IpcDeliveryCodecError::None)
    {
        ++counters.Malformed;
        return;
    }
    ::new (static_cast<void*>(&buffers.Outcome)) Executive::ObjectDirectory::ObjectDirectoryServiceDispatchOutcome{};
    static_cast<void>(service.Dispatch(buffers.Message, buffers.Metadata, buffers.Outcome));
    if (buffers.Outcome.HasPreparedReply)
    {
        const auto encoded = Kernel::Ipc::MessageCodec::Encode(
            buffers.Outcome.PreparedReply, Kernel::Ipc::WritableByteView{buffers.Reply, sizeof(buffers.Reply)});
        const bool sent =
            encoded.HasValue() &&
            (client.Send(ToUser(buffers.Outcome.ReplyEndpoint), AddressOf(buffers.Reply), encoded.GetValue()).Error ==
             Abi::SystemCallError::None);
        ++(sent ? counters.Replies : counters.DroppedReplies);
    }
    for (Foundation::Size index{0U}; index < buffers.Outcome.CapabilityCloseCount; ++index)
    {
        if (client.CloseCapability(ToUser(buffers.Outcome.CapabilitiesToClose[index])).Error !=
            Abi::SystemCallError::None)
        {
            ++counters.CloseFailures;
        }
    }
}
} // namespace

Foundation::UInt64 RunDirectory(const System::SystemCallTransport transport,
                                const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                                Executive::ObjectDirectory::ObjectDirectoryServiceCore& service,
                                DirectoryBuffers& buffers, DirectoryCounters& counters) noexcept
{
    const System::SystemCallClient client{transport};
    const System::CapabilityHandle endpoint{startBlock[Abi::ComponentReceiveWord],
                                            startBlock[Abi::ComponentReceiveWord + 1U]};
    const System::CapabilityHandle waitSet{startBlock[Abi::ComponentWaitSetWord],
                                           startBlock[Abi::ComponentWaitSetWord + 1U]};
    for (;;)
    {
        const Abi::SystemCallResponse waited = client.Wait(waitSet, 0U);
        if ((waited.Error != Abi::SystemCallError::None) || (waited.Value0 != DirectoryWaitTag))
        {
            return DirectoryFailedExitCode;
        }
        // Readiness is level-triggered: receive until the Endpoint is empty.
        for (;;)
        {
            const Abi::SystemCallResponse received =
                client.Receive(endpoint, AddressOf(buffers.Delivery), sizeof(buffers.Delivery));
            if (received.Error == Abi::SystemCallError::WouldBlock)
            {
                break;
            }
            if (received.Error != Abi::SystemCallError::None)
            {
                return DirectoryFailedExitCode;
            }
            ++counters.Requests;
            Serve(client, service, buffers, static_cast<Foundation::Size>(received.Value1), counters);
        }
    }
}
} // namespace Asteria::User::Directory
