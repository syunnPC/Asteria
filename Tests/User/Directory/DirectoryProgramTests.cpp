// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <memory>
#include <new>

#include "Executive/ObjectDirectory/ObjectDirectoryProtocol.hpp"
#include "User/Directory/DirectoryProgram.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Executive::ObjectDirectory::BindIfAbsentRequest;
using Executive::ObjectDirectory::DirectoryMessageContext;
using Executive::ObjectDirectory::DirectoryName;
using Executive::ObjectDirectory::DirectoryNameView;
using Executive::ObjectDirectory::DirectoryStatus;
using Executive::ObjectDirectory::InterfaceVersionRange;
using Executive::ObjectDirectory::MutationReply;
using Executive::ObjectDirectory::ObjectDirectoryProtocol;
using Executive::ObjectDirectory::ObjectDirectoryProtocolError;
using Executive::ObjectDirectory::ObjectDirectoryServiceCore;
using Executive::ObjectDirectory::ResolveReply;
using Executive::ObjectDirectory::ResolveRequest;
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Ipc::EndpointReceiveMetadata;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::IpcDeliveryCodec;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ResourceDomainId;
using User::System::SystemCallTransport;

constexpr UInt64 EndpointSelector{0x0808U};
constexpr UInt64 WaitSetSelector{0x0606U};
constexpr CapabilityHandle Target{0x0501U, 0xAAAAU};
constexpr CapabilityHandle RootReply{0x0502U, 0xBBBBU};
constexpr CapabilityHandle ClientReply{0x0503U, 0xCCCCU};

[[nodiscard]] DirectoryName MakeName(const char* const text) noexcept
{
    UInt8 bytes[DirectoryName::MaximumSizeBytes]{};
    Size size{0U};
    while (text[size] != '\0')
    {
        bytes[size] = static_cast<UInt8>(text[size]);
        ++size;
    }
    const auto created = DirectoryName::Create(DirectoryNameView{.Data = bytes, .SizeBytes = size});
    return created.HasValue() ? created.GetValue() : DirectoryName{};
}

[[nodiscard]] constexpr InterfaceIdentifier EchoInterface() noexcept
{
    return InterfaceIdentifier{0x4543'484F'0000'0001ULL, 0x4153'5445'5249'4101ULL};
}

[[nodiscard]] constexpr DirectoryMessageContext MakeContext(const UInt64 transaction) noexcept
{
    // Infinite deadlines, as natively: a zero deadline has passed.
    return DirectoryMessageContext{
        .Trace = {}, .Deadline = MonotonicTime::Infinite(), .TransactionIdentifier = transaction};
}

/// Answers the Directory: one readiness, then the scripted deliveries, then
/// WouldBlock, then a failed wait that ends the loop; records the replies it
/// was sent and the handles it was asked to close.
class DirectoryKernel final
{
public:
    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        static_cast<DirectoryKernel*>(context)->Handle(*request, *response);
    }

    void Handle(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        switch (static_cast<Abi::SystemCallNumber>(request.Number))
        {
        case Abi::SystemCallNumber::WaitSetWait:
            ++Waits;
            if ((Waits > 1U) || (request.Arguments[0U] != WaitSetSelector))
            {
                response.Error = Abi::SystemCallError::InvalidArguments;
                return;
            }
            response.Value0 = User::Directory::DirectoryWaitTag;
            return;
        case Abi::SystemCallNumber::IpcReceive:
            if ((request.Arguments[0U] != EndpointSelector) || (Delivered == DeliveryCount))
            {
                response.Error = Abi::SystemCallError::WouldBlock;
                return;
            }
            std::memcpy(reinterpret_cast<UInt8*>(request.Arguments[2U]), Deliveries[Delivered],
                        DeliveryBytes[Delivered]);
            response.Value1 = DeliveryBytes[Delivered];
            ++Delivered;
            return;
        case Abi::SystemCallNumber::IpcSend:
            if (Sent < 4U)
            {
                ReplyTo[Sent] = request.Arguments[0U];
                std::memcpy(Replies[Sent], reinterpret_cast<const UInt8*>(request.Arguments[2U]),
                            request.Arguments[3U]);
                ReplyBytes[Sent] = request.Arguments[3U];
            }
            ++Sent;
            return;
        case Abi::SystemCallNumber::CapabilityClose:
            ++Closes;
            return;
        default:
            ++Other;
            return;
        }
    }

    // Queues one request as its delivery: the given rights on a capability
    // sealed with selector, from the components' sender.
    bool Queue(const MessageEnvelope& message, const CapabilityRights rights, const UInt64 selector) noexcept
    {
        const EndpointReceiveMetadata metadata{.Sequence = DeliveryCount + 1U,
                                               .Sender = ResourceDomainId{2U},
                                               .SenderEndpointSealedObjectSelector = selector,
                                               .SenderEndpointRights = rights,
                                               .FrameSizeBytes = 0U,
                                               .AttachmentCount = message.AttachmentCount,
                                               .SendClass = EndpointSendClass::Normal};
        EndpointReceiveMetadata framed = metadata;
        UInt8 frame[MessageCodec::MaximumWireSizeBytes]{};
        const auto size = MessageCodec::Encode(message, Kernel::Ipc::WritableByteView{frame, sizeof(frame)});
        if (!size.HasValue())
        {
            return false;
        }
        framed.FrameSizeBytes = static_cast<Foundation::UInt16>(size.GetValue());
        const auto encoded = IpcDeliveryCodec::Encode(
            framed, message, Kernel::Ipc::WritableByteView{Deliveries[DeliveryCount], sizeof(Deliveries[0U])});
        if (!encoded.HasValue())
        {
            return false;
        }
        DeliveryBytes[DeliveryCount] = encoded.GetValue();
        ++DeliveryCount;
        return true;
    }

    UInt8 Deliveries[4U][IpcDeliveryCodec::MaximumWireSizeBytes]{};
    Size DeliveryBytes[4U]{};
    Size DeliveryCount{0U};
    Size Delivered{0U};
    Size Waits{0U};
    Size Sent{0U};
    UInt64 ReplyTo[4U]{};
    UInt8 Replies[4U][MessageCodec::MaximumWireSizeBytes]{};
    Size ReplyBytes[4U]{};
    Size Closes{0U};
    Size Other{0U};
};

struct DirectoryFixture
{
    alignas(ObjectDirectoryServiceCore) UInt8 Storage[sizeof(ObjectDirectoryServiceCore)]{};
    User::Directory::DirectoryBuffers Buffers{};
    User::Directory::DirectoryCounters Counters{};
    UInt64 StartBlock[Abi::ComponentStartBlockWords]{};

    DirectoryFixture() noexcept
    {
        StartBlock[Abi::ComponentReceiveWord] = EndpointSelector;
        StartBlock[Abi::ComponentWaitSetWord] = WaitSetSelector;
    }

    [[nodiscard]] UInt64 Run(DirectoryKernel& kernel) noexcept
    {
        auto* const service = ::new (static_cast<void*>(Storage))
            ObjectDirectoryServiceCore{User::Directory::MakeDirectoryConfiguration()};
        return User::Directory::RunDirectory(SystemCallTransport{&DirectoryKernel::Transport, &kernel}, StartBlock,
                                             *service, Buffers, Counters);
    }
};

[[nodiscard]] MessageEnvelope MakeBind(const char* const name, const UInt64 transaction) noexcept
{
    MessageEnvelope message{};
    const BindIfAbsentRequest request{
        .Name = MakeName(name),
        .ServiceInterface = EchoInterface(),
        .ServiceGeneration = 1U,
        .Version = InterfaceVersionRange{.Major = 1U, .MinimumMinor = 0U, .MaximumMinor = 0U, .Reserved = 0U},
        .PublishedRights = CapabilityRights{CapabilityRight::Invoke},
        .TargetEndpoint = Target,
        .TargetDeadline = MonotonicTime::Infinite(),
        .ReplyEndpoint = RootReply,
        .Context = MakeContext(transaction),
    };
    static_cast<void>(ObjectDirectoryProtocol::BuildBindIfAbsentRequest(request, message));
    return message;
}

[[nodiscard]] MessageEnvelope MakeResolve(const char* const name, const UInt64 transaction) noexcept
{
    MessageEnvelope message{};
    const ResolveRequest request{
        .Name = MakeName(name),
        .ServiceInterface = EchoInterface(),
        .RequestedDeadline = MonotonicTime::Infinite(),
        .RequiredVersion = InterfaceVersionRange{.Major = 1U, .MinimumMinor = 0U, .MaximumMinor = 0U, .Reserved = 0U},
        .RequiredRights = CapabilityRights{CapabilityRight::Invoke},
        .ReplyEndpoint = ClientReply,
        .Context = MakeContext(transaction),
    };
    static_cast<void>(ObjectDirectoryProtocol::BuildResolveRequest(request, message));
    return message;
}

// The root service binds `echo` with Manage; a component resolves it with Invoke and gets the
// target back; a component's bind without Manage is refused; replies go to each request's reply
// Endpoint.
void TestBindAndResolve(TestContext& runner) noexcept
{
    const auto kernel = std::make_unique<DirectoryKernel>();
    const auto fixture = std::make_unique<DirectoryFixture>();
    const CapabilityRights manage{CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Manage)};
    const CapabilityRights invoke{CapabilityRight::Invoke};
    const bool queued = kernel->Queue(MakeBind("echo", 1U), manage, User::Directory::DirectorySealedSelector) &&
                        kernel->Queue(MakeResolve("echo", 2U), invoke, User::Directory::DirectorySealedSelector) &&
                        kernel->Queue(MakeBind("other", 3U), invoke, User::Directory::DirectorySealedSelector);
    const UInt64 code = fixture->Run(*kernel);

    MessageEnvelope bound{};
    MessageEnvelope resolved{};
    MessageEnvelope refused{};
    MutationReply bindReply{};
    ResolveReply resolveReply{};
    MutationReply refusedReply{};
    const bool decoded =
        (kernel->Sent == 3U) &&
        (MessageCodec::Decode(Kernel::Ipc::ReadOnlyByteView{kernel->Replies[0U], kernel->ReplyBytes[0U]}, bound) ==
         Kernel::Ipc::MessageCodecError::None) &&
        (MessageCodec::Decode(Kernel::Ipc::ReadOnlyByteView{kernel->Replies[1U], kernel->ReplyBytes[1U]}, resolved) ==
         Kernel::Ipc::MessageCodecError::None) &&
        (MessageCodec::Decode(Kernel::Ipc::ReadOnlyByteView{kernel->Replies[2U], kernel->ReplyBytes[2U]}, refused) ==
         Kernel::Ipc::MessageCodecError::None) &&
        (ObjectDirectoryProtocol::ParseBindIfAbsentReply(bound, bindReply) == ObjectDirectoryProtocolError::None) &&
        (ObjectDirectoryProtocol::ParseResolveReply(resolved, resolveReply) == ObjectDirectoryProtocolError::None) &&
        (ObjectDirectoryProtocol::ParseBindIfAbsentReply(refused, refusedReply) == ObjectDirectoryProtocolError::None);
    ASTERIA_EXPECT(
        runner, "the Directory binds with Manage, resolves for Invoke and refuses a bind without Manage", queued,
        code == User::Directory::DirectoryFailedExitCode, decoded, bindReply.Status == DirectoryStatus::Success,
        bindReply.Version.DirectoryGeneration == 1U, resolveReply.Status == DirectoryStatus::Success,
        resolveReply.TargetEndpoint.GetSelector() == Target.GetSelector(),
        resolveReply.Version.Revision == bindReply.Version.Revision,
        refusedReply.Status == DirectoryStatus::PermissionDenied, kernel->ReplyTo[0U] == RootReply.GetSelector(),
        kernel->ReplyTo[1U] == ClientReply.GetSelector(), fixture->Counters.Requests == 3U,
        fixture->Counters.Replies == 3U, fixture->Counters.Malformed == 0U, kernel->Other == 0U);
}

// A request through a capability sealed otherwise is refused.
void TestForeignSelector(TestContext& runner) noexcept
{
    const auto kernel = std::make_unique<DirectoryKernel>();
    const auto fixture = std::make_unique<DirectoryFixture>();
    const bool queued = kernel->Queue(MakeResolve("echo", 1U), CapabilityRights{CapabilityRight::Invoke},
                                      User::Directory::DirectorySealedSelector + 1U);
    static_cast<void>(fixture->Run(*kernel));
    ASTERIA_EXPECT(runner, "a request through a foreign capability gets no answer and leaks no handle", queued,
                   fixture->Counters.Requests == 1U, fixture->Counters.Replies == 0U, kernel->Sent == 0U,
                   kernel->Closes == 1U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"DirectoryProgramTests"};
    Asteria::Test::TestBindAndResolve(runner);
    Asteria::Test::TestForeignSelector(runner);
    return runner.Finish();
}
