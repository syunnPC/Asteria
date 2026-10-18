// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <memory>

#include "Executive/ObjectDirectory/ObjectDirectoryProtocol.hpp"
#include "User/Client/ClientProgram.hpp"
#include "User/Component/EchoFrame.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Executive::ObjectDirectory::DirectoryStatus;
using Executive::ObjectDirectory::ObjectDirectoryProtocol;
using Executive::ObjectDirectory::ObjectDirectoryProtocolError;
using Executive::ObjectDirectory::ResolveReply;
using Executive::ObjectDirectory::ResolveRequest;
using Foundation::Size;
using Foundation::UInt64;
using Foundation::UInt8;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::ReadOnlyByteView;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::MonotonicTime;
using User::System::SystemCallTransport;

constexpr UInt64 ReportSelector{0x0202U};
constexpr UInt64 WaitSetSelector{0x0606U};
constexpr UInt64 ReceiveSelector{0x0808U};
constexpr UInt64 DirectorySelector{0x0A0AU};
constexpr UInt64 ReplySelector{0x0C0CU};
constexpr UInt64 EchoSelector{0x0E0EU};
constexpr UInt64 RestartedEchoSelector{0x0F0FU};
constexpr UInt64 EchoGeneration{7U};
constexpr UInt64 RestartedEchoGeneration{8U};
constexpr Size HeaderBytes{User::Component::EchoDeliveryHeaderBytes};
constexpr Size DeliveryBytes{HeaderBytes + MessageCodec::MaximumWireSizeBytes};

[[nodiscard]] UInt64 LoadWord(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

/// Plays the client's kernel, the Directory, Echo and the root service: the
/// Directory answers a resolve of `echo` with ResolveStatus and a send
/// capability on the running Echo, and Echo answers each request with itself
/// (WriteEchoReply), its payload's last byte flipped from CorruptEcho on.
/// After the first report the root service restarts Echo, under a new
/// selector and generation, and sends the client its go; a send to the old
/// Echo then fails with ObjectNotFound. A message becomes receivable only
/// after the client waited, so each exchange goes through its WaitSet; a wait
/// with nothing pending fails.
class ClientKernel final
{
public:
    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        static_cast<ClientKernel*>(context)->Handle(*request, *response);
    }

    void Handle(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        response = Abi::SystemCallResponse{.Error = Abi::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        switch (static_cast<Abi::SystemCallNumber>(request.Number))
        {
        case Abi::SystemCallNumber::WaitSetWait:
            ++Waits;
            if ((request.Arguments[0U] != WaitSetSelector) || (Pending == 0U))
            {
                response.Error = Abi::SystemCallError::InvalidArguments;
                return;
            }
            Ready = true;
            response.Value0 = User::Client::ClientWaitTag;
            return;
        case Abi::SystemCallNumber::IpcReceive:
            if ((request.Arguments[0U] != ReceiveSelector) || !Ready || (Pending == 0U))
            {
                response.Error = Abi::SystemCallError::WouldBlock;
                return;
            }
            std::memcpy(reinterpret_cast<UInt8*>(request.Arguments[2U]), Queued, QueuedBytes);
            response.Value1 = QueuedBytes;
            Pending = 0U;
            Ready = false;
            return;
        case Abi::SystemCallNumber::IpcSend:
            if (Restarted && (request.Arguments[0U] == EchoSelector))
            {
                ++StaleSends;
                response.Error = Abi::SystemCallError::ObjectNotFound;
                return;
            }
            Answer(request.Arguments[0U], reinterpret_cast<const UInt8*>(request.Arguments[2U]),
                   static_cast<Size>(request.Arguments[3U]));
            return;
        case Abi::SystemCallNumber::CapabilityClose:
            Closed = request.Arguments[0U];
            return;
        default:
            ++Other;
            return;
        }
    }

    void Answer(const UInt64 endpoint, const UInt8* const frame, const Size bytes) noexcept
    {
        if (endpoint == ReportSelector)
        {
            if (Reports < 2U)
            {
                std::memcpy(Report[Reports], frame, bytes);
                ReportBytes[Reports] = bytes;
            }
            if (++Reports == 1U)
            {
                Restart();
            }
            return;
        }
        if (endpoint == DirectorySelector)
        {
            ++Resolves;
            AnswerResolve(frame, bytes);
            return;
        }
        if (endpoint == (Restarted ? RestartedEchoSelector : EchoSelector))
        {
            UInt8 delivery[DeliveryBytes]{};
            std::memcpy(delivery + HeaderBytes, frame, bytes);
            UInt8 reply[User::Component::EchoMaximumFrameBytes]{};
            const Size replied = User::Component::WriteEchoReply(delivery, HeaderBytes + bytes, reply);
            ++Echoes;
            if (replied == 0U)
            {
                ++Other;
                return;
            }
            if (Echoes >= CorruptEcho)
            {
                reply[replied - 1U] ^= 0x01U;
            }
            EchoReplyTo = User::Component::ReadEchoHandle(delivery, 0U).Selector;
            Queue(reply, replied);
            return;
        }
        ++Other;
    }

    void AnswerResolve(const UInt8* const frame, const Size bytes) noexcept
    {
        MessageEnvelope message{};
        ResolveRequest request{};
        if ((MessageCodec::Decode(ReadOnlyByteView{frame, bytes}, message) != MessageCodecError::None) ||
            (ObjectDirectoryProtocol::ParseResolveRequest(message, request) != ObjectDirectoryProtocolError::None))
        {
            return;
        }
        ResolveReplyTo = request.ReplyEndpoint.GetSelector();
        ResolvedName = (request.Name.GetSizeBytes() == 4U) && (std::memcmp(request.Name.GetData(), "echo", 4U) == 0);
        const bool success = ResolveStatus == DirectoryStatus::Success;
        const ResolveReply reply{
            .Status = ResolveStatus,
            .Version = success ? Executive::ObjectDirectory::BindingVersion{.DirectoryGeneration = 1U, .Revision = 1U}
                               : Executive::ObjectDirectory::BindingVersion{},
            .ServiceInterface = success ? User::Client::EchoInterface : Kernel::Ipc::InterfaceIdentifier{},
            .ServiceGeneration = success ? (Restarted ? RestartedEchoGeneration : EchoGeneration) : 0U,
            .GrantedDeadline = success ? MonotonicTime::Infinite() : MonotonicTime{},
            .Major = static_cast<Foundation::UInt16>(success ? 1U : 0U),
            .SelectedMinor = 0U,
            .GrantedRights = success ? CapabilityRights{CapabilityRight::Invoke} : CapabilityRights{},
            .TargetEndpoint = success ? CapabilityHandle{Restarted ? RestartedEchoSelector : EchoSelector, 0x5555U}
                                      : CapabilityHandle{},
            .Context = request.Context,
        };
        MessageEnvelope answer{};
        UInt8 encoded[MessageCodec::MaximumWireSizeBytes]{};
        if (ObjectDirectoryProtocol::BuildResolveReply(reply, answer) != ObjectDirectoryProtocolError::None)
        {
            return;
        }
        const auto size = MessageCodec::Encode(answer, Kernel::Ipc::WritableByteView{encoded, sizeof(encoded)});
        if (size.HasValue())
        {
            Queue(encoded, size.GetValue());
        }
    }

    // The root service restarts Echo and sends the client its go.
    void Restart() noexcept
    {
        Restarted = true;
        MessageEnvelope go{};
        go.Interface = User::Client::ClientControlInterface;
        go.TransactionIdentifier = 1U;
        go.Deadline = MonotonicTime::Infinite();
        go.MethodIdentifier = User::Client::ClientGoMethod;
        go.InterfaceVersionMajor = 1U;
        go.Kind = Kernel::Ipc::MessageKind::Asynchronous;
        go.Cancellation = Kernel::Ipc::CancellationState::None;
        UInt8 encoded[MessageCodec::MaximumWireSizeBytes]{};
        const auto size = MessageCodec::Encode(go, Kernel::Ipc::WritableByteView{encoded, sizeof(encoded)});
        if (size.HasValue())
        {
            Queue(encoded, size.GetValue());
        }
    }

    void Queue(const UInt8* const frame, const Size bytes) noexcept
    {
        std::memset(Queued, 0, sizeof(Queued));
        std::memcpy(Queued + HeaderBytes, frame, bytes);
        QueuedBytes = HeaderBytes + bytes;
        Pending = 1U;
    }

    DirectoryStatus ResolveStatus{DirectoryStatus::Success};
    bool Restarted{false};
    Size StaleSends{0U};
    UInt64 Closed{0U};
    Size CorruptEcho{~Size{0U}};
    UInt8 Queued[DeliveryBytes]{};
    Size QueuedBytes{0U};
    Size Pending{0U};
    bool Ready{false};
    Size Waits{0U};
    Size Resolves{0U};
    Size Echoes{0U};
    bool ResolvedName{false};
    UInt64 ResolveReplyTo{0U};
    UInt64 EchoReplyTo{0U};
    UInt8 Report[2U][MessageCodec::MaximumWireSizeBytes]{};
    Size ReportBytes[2U]{};
    Size Reports{0U};
    Size Other{0U};
};

struct ClientFixture
{
    User::Client::ClientBuffers Buffers{};
    UInt64 StartBlock[Abi::ComponentStartBlockWords]{};

    ClientFixture() noexcept
    {
        StartBlock[Abi::ComponentStartMagicWord] = Abi::ComponentStartMagic;
        StartBlock[Abi::ComponentReportWord] = ReportSelector;
        StartBlock[Abi::ComponentWaitSetWord] = WaitSetSelector;
        StartBlock[Abi::ComponentReceiveWord] = ReceiveSelector;
        StartBlock[Abi::ComponentDirectoryWord] = DirectorySelector;
        StartBlock[Abi::ComponentReplyWord] = ReplySelector;
    }

    [[nodiscard]] UInt64 Run(ClientKernel& kernel) noexcept
    {
        return User::Client::RunClient(SystemCallTransport{&ClientKernel::Transport, &kernel}, StartBlock, Buffers);
    }
};

// A report's words, or all ones when it is not the client's.
struct ReportWords
{
    UInt64 Resolved;
    UInt64 Echoes;
    UInt64 Generation;
    UInt64 Stale;
};

[[nodiscard]] ReportWords ReadReport(const ClientKernel& kernel, const Size index) noexcept
{
    MessageEnvelope report{};
    if ((MessageCodec::Decode(ReadOnlyByteView{kernel.Report[index], kernel.ReportBytes[index]}, report) !=
         MessageCodecError::None) ||
        (report.Interface.GetLow() != User::Client::ClientReportInterface.GetLow()) ||
        (report.PayloadSizeBytes != User::Client::ClientReportPayloadBytes) || (report.AttachmentCount != 0U))
    {
        return ReportWords{
            .Resolved = ~UInt64{0U}, .Echoes = ~UInt64{0U}, .Generation = ~UInt64{0U}, .Stale = ~UInt64{0U}};
    }
    return ReportWords{.Resolved = LoadWord(report.Payload),
                       .Echoes = LoadWord(report.Payload + 8U),
                       .Generation = LoadWord(report.Payload + 16U),
                       .Stale = LoadWord(report.Payload + 24U)};
}

// Service IPC and: the client resolves `echo` with its reply capability and gets three echoes
// through the capability the Directory returned, each after a wait on its WaitSet, and reports;
// after the go, its send through that capability fails, it closes it, resolves the restarted Echo
// and gets three echoes again.
void TestTwoRounds(TestContext& runner) noexcept
{
    const auto kernel = std::make_unique<ClientKernel>();
    const auto fixture = std::make_unique<ClientFixture>();
    const UInt64 code = fixture->Run(*kernel);
    const ReportWords first = ReadReport(*kernel, 0U);
    const ReportWords second = ReadReport(*kernel, 1U);
    ASTERIA_EXPECT(runner, "the client resolves echo, gets three echoes and reports them", kernel->Resolves == 2U,
                   kernel->ResolvedName, kernel->ResolveReplyTo == ReplySelector, kernel->EchoReplyTo == ReplySelector,
                   first.Resolved == 1U, first.Echoes == User::Client::ClientEchoes, first.Generation == EchoGeneration,
                   first.Stale == 0U, kernel->Other == 0U);
    ASTERIA_EXPECT(runner, "after the go its stale send fails, and it resolves the restarted Echo again",
                   code == User::Client::ClientExitCode, kernel->StaleSends == 1U, kernel->Closed == EchoSelector,
                   kernel->Echoes == (2U * User::Client::ClientEchoes), kernel->Waits == 9U, second.Resolved == 1U,
                   second.Echoes == User::Client::ClientEchoes, second.Generation == RestartedEchoGeneration,
                   second.Stale == static_cast<UInt64>(Abi::SystemCallError::ObjectNotFound));
}

// A resolve the Directory refuses ends the client after its first report,
// without a request to Echo; an echo that does not come back as sent ends
// the requests.
void TestFailures(TestContext& runner) noexcept
{
    const auto refused = std::make_unique<ClientKernel>();
    refused->ResolveStatus = DirectoryStatus::NotFound;
    const auto first = std::make_unique<ClientFixture>();
    const UInt64 refusedCode = first->Run(*refused);
    const ReportWords refusedReport = ReadReport(*refused, 0U);
    ASTERIA_EXPECT(runner, "a refused resolve is reported, and Echo gets no request",
                   refusedCode == User::Client::ClientFailedExitCode, refused->Echoes == 0U, refused->Reports == 1U,
                   refusedReport.Resolved == 0U, refusedReport.Echoes == 0U);

    const auto corrupt = std::make_unique<ClientKernel>();
    corrupt->CorruptEcho = 2U;
    const auto second = std::make_unique<ClientFixture>();
    const UInt64 corruptCode = second->Run(*corrupt);
    const ReportWords corruptReport = ReadReport(*corrupt, 0U);
    ASTERIA_EXPECT(runner, "an echo that differs from its request ends the requests",
                   corruptCode == User::Client::ClientFailedExitCode, corrupt->Echoes == 2U, corrupt->Reports == 1U,
                   corruptReport.Resolved == 1U, corruptReport.Echoes == 1U);

    const auto unstarted = std::make_unique<ClientKernel>();
    const auto third = std::make_unique<ClientFixture>();
    third->StartBlock[Abi::ComponentStartMagicWord] = 0U;
    ASTERIA_EXPECT(runner, "a start block without the magic starts nothing",
                   third->Run(*unstarted) == User::Client::ClientFailedExitCode, unstarted->Resolves == 0U,
                   unstarted->Reports == 0U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ClientProgramTests"};
    Asteria::Test::TestTwoRounds(runner);
    Asteria::Test::TestFailures(runner);
    return runner.Finish();
}
