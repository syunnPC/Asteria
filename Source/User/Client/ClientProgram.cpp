// SPDX-License-Identifier: Apache-2.0

#include "User/Client/ClientProgram.hpp"

#include "Executive/ObjectDirectory/ObjectDirectoryProtocol.hpp"

namespace Asteria::User::Client
{
namespace
{
constexpr Foundation::Size DeliveryHeaderBytes{48U};
constexpr Foundation::UInt32 EchoMethod{1U};
constexpr Foundation::UInt32 ReportMethod{1U};
constexpr Foundation::UInt64 FirstTransaction{1U};
// What each request's payload word mixes into its transaction.
constexpr Foundation::UInt64 EchoPattern{0x4543'484F'0000'0000ULL};
constexpr char EchoName[] = "echo";

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const pointer) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(pointer));
}

[[nodiscard]] System::CapabilityHandle GetHandle(const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                                                 const Foundation::Size word) noexcept
{
    return System::CapabilityHandle{startBlock[word], startBlock[word + 1U]};
}

[[nodiscard]] Kernel::Object::CapabilityHandle ToKernel(const System::CapabilityHandle handle) noexcept
{
    return Kernel::Object::CapabilityHandle{handle.Selector, handle.Authenticator};
}

void StoreWord(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index << 3U));
    }
}

[[nodiscard]] Foundation::UInt64 LoadWord(const Foundation::UInt8* const bytes) noexcept
{
    Foundation::UInt64 value{0U};
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<Foundation::UInt64>(bytes[index]) << (index << 3U);
    }
    return value;
}

// An asynchronous message of interface with payloadBytes of payload and no
// attachment; its deadline is infinite (a zero one has passed).
void StartMessage(Kernel::Ipc::MessageEnvelope& message, const Kernel::Ipc::InterfaceIdentifier interface,
                  const Foundation::UInt32 method, const Foundation::UInt64 transaction,
                  const Foundation::UInt16 payloadBytes) noexcept
{
    message = Kernel::Ipc::MessageEnvelope{};
    message.Interface = interface;
    message.TransactionIdentifier = transaction;
    message.Deadline = Kernel::Object::MonotonicTime::Infinite();
    message.MethodIdentifier = method;
    message.InterfaceVersionMajor = 1U;
    message.Kind = Kernel::Ipc::MessageKind::Asynchronous;
    message.Cancellation = Kernel::Ipc::CancellationState::None;
    message.FixedPayloadSizeBytes = payloadBytes;
    message.PayloadSizeBytes = payloadBytes;
}

// Encodes buffers.Message and sends it on endpoint.
[[nodiscard]] bool Send(const System::SystemCallClient& client, const System::CapabilityHandle endpoint,
                        ClientBuffers& buffers) noexcept
{
    const auto encoded = Kernel::Ipc::MessageCodec::Encode(
        buffers.Message, Kernel::Ipc::WritableByteView{buffers.Frame, sizeof(buffers.Frame)});
    return encoded.HasValue() &&
           (client.Send(endpoint, AddressOf(buffers.Frame), encoded.GetValue()).Error == Abi::SystemCallError::None);
}

// Receives the next message on the client's Endpoint into buffers.Message,
// waiting on its WaitSet while the Endpoint is empty. A wait may return for
// a readiness an earlier receive already consumed, so the client receives
// again after each.
[[nodiscard]] bool Receive(const System::SystemCallClient& client,
                           const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                           ClientBuffers& buffers) noexcept
{
    for (;;)
    {
        const Abi::SystemCallResponse received = client.Receive(GetHandle(startBlock, Abi::ComponentReceiveWord),
                                                                AddressOf(buffers.Delivery), sizeof(buffers.Delivery));
        if (received.Error == Abi::SystemCallError::None)
        {
            return (received.Value1 > DeliveryHeaderBytes) && (received.Value1 <= sizeof(buffers.Delivery)) &&
                   (Kernel::Ipc::MessageCodec::Decode(
                        Kernel::Ipc::ReadOnlyByteView{
                            buffers.Delivery + DeliveryHeaderBytes,
                            static_cast<Foundation::Size>(received.Value1 - DeliveryHeaderBytes)},
                        buffers.Message) == Kernel::Ipc::MessageCodecError::None);
        }
        if (received.Error != Abi::SystemCallError::WouldBlock)
        {
            return false;
        }
        const Abi::SystemCallResponse waited = client.Wait(GetHandle(startBlock, Abi::ComponentWaitSetWord), 0U);
        if ((waited.Error != Abi::SystemCallError::None) || (waited.Value0 != ClientWaitTag))
        {
            return false;
        }
    }
}

[[nodiscard]] Executive::ObjectDirectory::DirectoryName MakeEchoName() noexcept
{
    Foundation::UInt8 bytes[sizeof(EchoName) - 1U]{};
    for (Foundation::Size index{0U}; index < sizeof(bytes); ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(EchoName[index]);
    }
    const auto created = Executive::ObjectDirectory::DirectoryName::Create(
        Executive::ObjectDirectory::DirectoryNameView{.Data = bytes, .SizeBytes = sizeof(bytes)});
    return created.HasValue() ? created.GetValue() : Executive::ObjectDirectory::DirectoryName{};
}

// What one round found: whether it resolved `echo`, the send capability and
// service generation the Directory returned, and the echoes that came back.
struct Round
{
    bool Resolved;
    System::CapabilityHandle Echo;
    Foundation::UInt64 Generation;
    Foundation::UInt64 Echoes;
};

// Resolves `echo` for Invoke at version 1.0, every deadline infinite; the
// reply's attachment is the client's send capability on Echo's Endpoint.
[[gnu::noinline]] bool Resolve(const System::SystemCallClient& client,
                               const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                               ClientBuffers& buffers, const Foundation::UInt64 transaction, Round& round) noexcept
{
    const Executive::ObjectDirectory::ResolveRequest request{
        .Name = MakeEchoName(),
        .ServiceInterface = EchoInterface,
        .RequestedDeadline = Kernel::Object::MonotonicTime::Infinite(),
        .RequiredVersion = {.Major = 1U, .MinimumMinor = 0U, .MaximumMinor = 0U, .Reserved = 0U},
        .RequiredRights = Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Invoke},
        .ReplyEndpoint = ToKernel(GetHandle(startBlock, Abi::ComponentReplyWord)),
        .Context = {.Trace = {},
                    .Deadline = Kernel::Object::MonotonicTime::Infinite(),
                    .TransactionIdentifier = transaction},
    };
    buffers.Message = Kernel::Ipc::MessageEnvelope{};
    if ((Executive::ObjectDirectory::ObjectDirectoryProtocol::BuildResolveRequest(request, buffers.Message) !=
         Executive::ObjectDirectory::ObjectDirectoryProtocolError::None) ||
        !Send(client, GetHandle(startBlock, Abi::ComponentDirectoryWord), buffers) ||
        !Receive(client, startBlock, buffers))
    {
        return false;
    }
    Executive::ObjectDirectory::ResolveReply reply{};
    if ((Executive::ObjectDirectory::ObjectDirectoryProtocol::ParseResolveReply(buffers.Message, reply) !=
         Executive::ObjectDirectory::ObjectDirectoryProtocolError::None) ||
        (reply.Status != Executive::ObjectDirectory::DirectoryStatus::Success) ||
        (reply.Context.TransactionIdentifier != transaction))
    {
        return false;
    }
    round.Echo = System::CapabilityHandle{reply.TargetEndpoint.GetSelector(), reply.TargetEndpoint.GetAuthenticator()};
    round.Generation = reply.ServiceGeneration;
    return true;
}

// An echo request: a copy of the client's send capability for the echo and
// a payload word of its own.
void StartEcho(const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords], ClientBuffers& buffers,
               const Foundation::UInt64 transaction) noexcept
{
    StartMessage(buffers.Message, EchoInterface, EchoMethod, transaction, 8U);
    StoreWord(buffers.Message.Payload, transaction ^ EchoPattern);
    buffers.Message.AttachmentCount = 1U;
    buffers.Message.Attachments[0U] = Kernel::Ipc::CapabilityAttachment{
        .Handle = ToKernel(GetHandle(startBlock, Abi::ComponentReplyWord)),
        .RequestedDeadline = Kernel::Object::MonotonicTime::Infinite(),
        .RequestedRights = Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Invoke},
        .ExpectedType = Kernel::Object::ObjectType::Endpoint,
        .TransferMode = Kernel::Ipc::CapabilityTransferMode::Copy,
        .Reserved = 0U,
    };
}

// One request; the echo must come back with the request's interface,
// transaction and payload, and no capability.
[[gnu::noinline]] bool EchoOnce(const System::SystemCallClient& client,
                                const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                                ClientBuffers& buffers, const System::CapabilityHandle echo,
                                const Foundation::UInt64 transaction) noexcept
{
    StartEcho(startBlock, buffers, transaction);
    return Send(client, echo, buffers) && Receive(client, startBlock, buffers) &&
           (buffers.Message.Interface.GetLow() == EchoInterface.GetLow()) &&
           (buffers.Message.Interface.GetHigh() == EchoInterface.GetHigh()) &&
           (buffers.Message.TransactionIdentifier == transaction) && (buffers.Message.PayloadSizeBytes == 8U) &&
           (LoadWord(buffers.Message.Payload) == (transaction ^ EchoPattern)) &&
           (buffers.Message.AttachmentCount == 0U);
}

// A round: resolves `echo`, then asks for ClientEchoes echoes until one does
// not come back as sent. Advances transaction past every request it sent.
[[gnu::noinline]] void RunRound(const System::SystemCallClient& client,
                                const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                                ClientBuffers& buffers, Foundation::UInt64& transaction, Round& round) noexcept
{
    round = Round{.Resolved = false, .Echo = {}, .Generation = 0U, .Echoes = 0U};
    round.Resolved = Resolve(client, startBlock, buffers, transaction++, round);
    while (round.Resolved && (round.Echoes < ClientEchoes) &&
           EchoOnce(client, startBlock, buffers, round.Echo, transaction++))
    {
        ++round.Echoes;
    }
}

[[gnu::noinline]] bool Report(const System::SystemCallClient& client,
                              const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                              ClientBuffers& buffers, const Foundation::UInt64 transaction, const Round& round,
                              const Abi::SystemCallError stale) noexcept
{
    StartMessage(buffers.Message, ClientReportInterface, ReportMethod, transaction, ClientReportPayloadBytes);
    StoreWord(buffers.Message.Payload, round.Resolved ? 1U : 0U);
    StoreWord(buffers.Message.Payload + 8U, round.Echoes);
    StoreWord(buffers.Message.Payload + 16U, round.Generation);
    StoreWord(buffers.Message.Payload + 24U, static_cast<Foundation::UInt64>(stale));
    return Send(client, GetHandle(startBlock, Abi::ComponentReportWord), buffers);
}

// The root service's go: the next message on the client's Endpoint.
[[gnu::noinline]] bool AwaitGo(const System::SystemCallClient& client,
                               const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                               ClientBuffers& buffers) noexcept
{
    return Receive(client, startBlock, buffers) &&
           (buffers.Message.Interface.GetLow() == ClientControlInterface.GetLow()) &&
           (buffers.Message.Interface.GetHigh() == ClientControlInterface.GetHigh()) &&
           (buffers.Message.MethodIdentifier == ClientGoMethod) && (buffers.Message.AttachmentCount == 0U);
}

// A request through the capability an earlier round resolved: its error.
[[gnu::noinline]] Abi::SystemCallError SendStale(const System::SystemCallClient& client,
                                                 const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                                                 ClientBuffers& buffers, const System::CapabilityHandle echo,
                                                 const Foundation::UInt64 transaction) noexcept
{
    StartEcho(startBlock, buffers, transaction);
    const auto encoded = Kernel::Ipc::MessageCodec::Encode(
        buffers.Message, Kernel::Ipc::WritableByteView{buffers.Frame, sizeof(buffers.Frame)});
    return encoded.HasValue() ? client.Send(echo, AddressOf(buffers.Frame), encoded.GetValue()).Error
                              : Abi::SystemCallError::InvalidArguments;
}
} // namespace

Foundation::UInt64 RunClient(const System::SystemCallTransport transport,
                             const Foundation::UInt64 (&startBlock)[Abi::ComponentStartBlockWords],
                             ClientBuffers& buffers) noexcept
{
    if (startBlock[Abi::ComponentStartMagicWord] != Abi::ComponentStartMagic)
    {
        return ClientFailedExitCode;
    }
    const System::SystemCallClient client{transport};
    Foundation::UInt64 transaction{FirstTransaction};
    Round first{};
    RunRound(client, startBlock, buffers, transaction, first);
    if (!Report(client, startBlock, buffers, transaction++, first, Abi::SystemCallError::None) ||
        (first.Echoes != ClientEchoes) || !AwaitGo(client, startBlock, buffers))
    {
        return ClientFailedExitCode;
    }
    // Echo was restarted: the capability the first round resolved names its
    // destroyed Endpoint, and the client resolves the name again.
    const Abi::SystemCallError stale = SendStale(client, startBlock, buffers, first.Echo, transaction++);
    static_cast<void>(client.CloseCapability(first.Echo));
    Round second{};
    RunRound(client, startBlock, buffers, transaction, second);
    const bool reported = Report(client, startBlock, buffers, transaction, second, stale);
    return (reported && (second.Echoes == ClientEchoes) && (stale != Abi::SystemCallError::None))
               ? ClientExitCode
               : ClientFailedExitCode;
}
} // namespace Asteria::User::Client
