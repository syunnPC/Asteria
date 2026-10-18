// SPDX-License-Identifier: Apache-2.0
#include "User/Component/NetworkService.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Component/NetFrame.hpp"
#include "User/Component/SocketFrame.hpp"
#include "User/Network/NetworkStack.hpp"

#include "Support/TestContext.hpp"
#include "User/Network/PacketFixtures.hpp"

#include <cstring>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
namespace Component = User::Component;
namespace Fixtures = Network;
using Abi::SystemCallError;
using Abi::SystemCallNumber;
using User::System::CapabilityHandle;

constexpr Size Header{Component::EchoDeliveryHeaderBytes};
constexpr Size DeliveryCapacity{Header + Component::EchoMaximumFrameBytes};
constexpr Size PageBytes{4096U};
constexpr Size WindowPages{4U};
// The window's pages (NetworkService.hpp): two receive pages, the generated
// frames' transmit page, the clients' transmit page.
constexpr Size GeneratedPage{2U};
constexpr Size ClientPage{3U};
// The start block's handles, the driver instance's tag, the stack's
// incarnation, two clients' sender identities and the stack's configuration
// (10.0.2.15/24, the fixtures' own address).
constexpr UInt64 Account{0x10U};
constexpr UInt64 Space{0x20U};
constexpr UInt64 Driver{0x34U};
constexpr UInt64 DriverReply{0x38U};
constexpr UInt64 WaitSet{0x60U};
constexpr UInt64 Endpoint{0x80U};
constexpr UInt64 DriverTag{7U};
constexpr UInt64 Incarnation{0x21U};
constexpr UInt64 Root{0x5100U};
constexpr UInt64 Other{0x5200U};
constexpr UInt64 Configuration{Fixtures::OwnAddress | (UInt64{24U} << 32U)};
// The fixtures' own MAC address, 02:00:00:00:00:01, its first byte in bits
// 0-7, as the driver's Query answers it.
constexpr UInt64 Mac{0x0100'0000'0002ULL};
// A request's reply capability is FirstReply + its transaction.
constexpr UInt64 FirstReply{0x7000U};
constexpr UInt16 InvokeRight{0x0002U};
constexpr UInt16 ManageRights{0x0802U};
constexpr UInt64 EchoPort{7U};
constexpr UInt64 PeerPort{9000U};
constexpr UInt64 FailedDetail{0x51U};
// The handles a foreign frame carries.
constexpr UInt64 ForeignHandle{0x9002U};
constexpr Size Records{48U};

// The service's window, a host buffer its kernel model maps nothing into,
// and the frames the driver model was asked to send.
alignas(4096) UInt8 g_window[WindowPages][PageBytes]{};
UInt8 g_sent[Records][User::Network::MaximumFrameBytes]{};

[[nodiscard]] UInt64 Load(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

void Store(UInt8* const bytes, const UInt64 value) noexcept
{
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] UInt64 Address(const void* const pointer) noexcept
{
    return reinterpret_cast<Foundation::UIntPointer>(pointer);
}

[[nodiscard]] constexpr UInt64 Failure(const UInt64 step, const SystemCallError error = SystemCallError::None) noexcept
{
    return Abi::ComponentNetworkFailedExitCode | (step << 32U) | (static_cast<UInt64>(error) << 40U);
}

// How a run ends once its script is played: the kernel fails its wait.
[[nodiscard]] constexpr UInt64 Served() noexcept
{
    return Failure(Abi::ComponentNetworkWaitStep, SystemCallError::InvalidHandle);
}

// One request the service sent the driver, decoded; a Send's frame is in
// g_sent at the record's index.
struct DriverRecord
{
    UInt64 Method;
    UInt64 Transaction;
    UInt64 Words[4U];
    UInt64 Reply;
    UInt64 Data;
    UInt16 ReplyRights;
    UInt16 DataRights;
    Size Page;
    Size Sequence;
};

// One reply the service sent a client, decoded.
struct ClientReply
{
    UInt64 Endpoint;
    UInt64 Transaction;
    UInt64 Kind;
    UInt64 Detail;
    UInt64 Extra;
    UInt8 Inline[Component::SocketInlineBytes];
    Size InlineLength;
    Size Sequence;
};

struct MapRecord
{
    UInt64 Space;
    UInt64 Memory;
    UInt64 Frame;
    UInt64 Start;
    UInt64 Pages;
};

// What the kernel model does when the service waits with nothing ready.
enum class Act : UInt8
{
    // The next scripted request is published on the service Endpoint.
    Publish,
    // A frame of Length bytes fills the oldest receive credit.
    Frame,
    // The oldest Send posted for window page Length is answered with kind
    // Value.
    Complete,
    // The driver answers its Query.
    AnswerQuery,
    // The driver answers a credit NicDriverRestarted.
    Restart,
    // A frame that is no NIC reply arrives on the driver-reply Endpoint.
    Foreign,
    // The wait times out.
    Timeout,
};

struct Event
{
    Act Kind;
    const UInt8* Bytes;
    Size Length;
    UInt64 Value;
};

struct Posted
{
    UInt64 Transaction;
    Size Page;
    Size Length;
};

// The kernel the service runs on, with the NIC driver played by a model. Its creates and maps
// succeed and are logged (a Frame mapped at a window page is that page). A request to the driver is
// decoded and logged: its Query is answered at once (or by an event), a ReceiveCredit waits for a
// Frame event, a Send's frame is copied out of its page and waits for a Complete event. Readiness
// is the kernel's: each message published signals its Endpoint once and a wait consumes one signal,
// the service Endpoint's first; with no signal left a wait plays the next event, and with no event
// left it fails, which ends the run.
struct NetKernel
{
    UInt8 Script[24U][DeliveryCapacity];
    Size ScriptBytes[24U];
    Size Scripted;
    Size Published;
    Size Received;
    UInt64 ScriptSender;
    UInt16 ScriptRights;
    UInt64 ScriptSelector;
    UInt8 Queue[8U][DeliveryCapacity];
    Size QueueBytes[8U];
    Size Queued;
    Size ServiceSignals;
    Size DriverSignals;
    Event Events[32U];
    Size EventCount;
    Size NextEvent;
    bool HoldQuery;
    UInt64 QueryKind;
    UInt64 QueryMac;
    UInt64 QueryTransaction;
    bool QueryPending;
    Posted Credits[8U];
    Size CreditCount;
    Posted Transmits[8U];
    Size TransmitCount;
    // The driver Send attempt (from 1) that fails with FailError, and the
    // first of BlockCount attempts answered WouldBlock.
    Size FailSend;
    SystemCallError FailError;
    Size BlockFrom;
    Size BlockCount;
    Size Attempts;
    UInt64 RefuseReplyTo;
    DriverRecord Drivers[Records];
    Size DriverCount;
    ClientReply Clients[Records];
    Size ClientCount;
    UInt64 Closed[64U];
    Size Closes;
    UInt64 Types[16U];
    Size Creates;
    MapRecord Maps[8U];
    Size MapCount;
    Size Waits;
    Size TimedWaits;
    Size Sequence;
    Size CompletedAt;
    bool Missed;

    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        static_cast<NetKernel*>(context)->Answer(*request, *response);
    }

    void Answer(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        response = Abi::SystemCallResponse{.Error = SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        switch (static_cast<SystemCallNumber>(request.Number))
        {
        case SystemCallNumber::ObjectCreate:
            Types[Creates % 16U] = request.Arguments[2U];
            ++Creates;
            response.Value0 = 0x1000U + Creates;
            response.Value1 = 0x2000U + Creates;
            return;
        case SystemCallNumber::MemoryMap:
            Map(reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[3U])));
            return;
        case SystemCallNumber::WaitSetWait:
            Wait(request, response);
            return;
        case SystemCallNumber::IpcReceive:
            Receive(request, response);
            return;
        case SystemCallNumber::IpcSend:
            Send(request, response);
            return;
        case SystemCallNumber::CapabilityClose:
            Closed[Closes % 64U] = request.Arguments[0U];
            ++Closes;
            return;
        default:
            response.Error = SystemCallError::UnknownSystemCall;
            return;
        }
    }

    void Map(const UInt8* const bytes) noexcept
    {
        MapRecord& record = Maps[MapCount % 8U];
        record = MapRecord{.Space = Load(bytes + Abi::FaultResolveAddressSpaceOffset),
                           .Memory = Load(bytes + Abi::FaultResolveMemoryObjectOffset),
                           .Frame = Load(bytes + Abi::FaultResolveFrameOffset),
                           .Start = Load(bytes + Abi::FaultResolveVirtualStartOffset),
                           .Pages = Load(bytes + Abi::FaultResolvePageCountOffset)};
        ++MapCount;
    }

    // The window page a Frame is mapped at, or WindowPages.
    [[nodiscard]] Size PageOf(const UInt64 frame) const noexcept
    {
        for (Size index{0U}; (index < MapCount) && (index < 8U); ++index)
        {
            const MapRecord& record = Maps[index];
            if ((record.Frame == frame) && (record.Start >= Address(g_window)) &&
                (record.Start < (Address(g_window) + sizeof(g_window))))
            {
                return static_cast<Size>((record.Start - Address(g_window)) / PageBytes);
            }
        }
        return WindowPages;
    }

    void Wait(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        ++Waits;
        TimedWaits += (request.Arguments[2U] != 0U) ? 1U : 0U;
        if ((request.Arguments[0U] != WaitSet) || (Waits > 512U))
        {
            response.Error = SystemCallError::InvalidHandle;
            return;
        }
        for (;;)
        {
            if (ServiceSignals != 0U)
            {
                --ServiceSignals;
                response.Value0 = Component::SocketServiceWaitTag;
                return;
            }
            if (DriverSignals != 0U)
            {
                --DriverSignals;
                response.Value0 = Component::SocketDriverReplyWaitTag;
                return;
            }
            if (NextEvent == EventCount)
            {
                response.Error = SystemCallError::InvalidHandle;
                return;
            }
            const Event event = Events[NextEvent];
            ++NextEvent;
            ++Sequence;
            if (event.Kind == Act::Timeout)
            {
                response.Error =
                    (request.Arguments[2U] != 0U) ? SystemCallError::TimedOut : SystemCallError::InvalidHandle;
                return;
            }
            Play(event);
        }
    }

    void Play(const Event& event) noexcept
    {
        switch (event.Kind)
        {
        case Act::Publish:
            Missed = Missed || (Published == Scripted);
            Published += (Published < Scripted) ? 1U : 0U;
            ++ServiceSignals;
            return;
        case Act::Frame:
            FillCredit(event.Bytes, event.Length);
            return;
        case Act::Complete:
            CompleteSend(event.Length, event.Value);
            return;
        case Act::AnswerQuery:
            Missed = Missed || !QueryPending;
            QueryPending = false;
            Reply(QueryTransaction, QueryKind, QueryMac, 1U);
            return;
        case Act::Restart:
            Reply((CreditCount != 0U) ? Credits[0U].Transaction : 0x99U, Component::NicDriverRestarted, 0U, 0U);
            return;
        case Act::Foreign:
            Foreign();
            return;
        case Act::Timeout:
            return;
        }
    }

    void FillCredit(const UInt8* const bytes, const Size length) noexcept
    {
        if (CreditCount == 0U)
        {
            Missed = true;
            return;
        }
        const Posted credit = Credits[0U];
        for (Size index{1U}; index < CreditCount; ++index)
        {
            Credits[index - 1U] = Credits[index];
        }
        --CreditCount;
        std::memcpy(g_window[credit.Page], bytes, length);
        Reply(credit.Transaction, Component::NicDone, length, 0U);
    }

    void CompleteSend(const Size page, const UInt64 kind) noexcept
    {
        for (Size index{0U}; index < TransmitCount; ++index)
        {
            if (Transmits[index].Page != page)
            {
                continue;
            }
            const Posted send = Transmits[index];
            for (Size next{index + 1U}; next < TransmitCount; ++next)
            {
                Transmits[next - 1U] = Transmits[next];
            }
            --TransmitCount;
            CompletedAt = Sequence;
            Reply(send.Transaction, kind, (kind == Component::NicDone) ? send.Length : FailedDetail, 0U);
            return;
        }
        Missed = true;
    }

    // Queues a NIC reply on the driver-reply Endpoint.
    void Reply(const UInt64 transaction, const UInt64 kind, const UInt64 detail, const UInt64 extra) noexcept
    {
        UInt8* const delivery = Queue[Queued % 8U];
        std::memset(delivery, 0, Header);
        const UInt64 words[Component::NicPayloadWords]{transaction, kind, detail, extra};
        QueueBytes[Queued % 8U] = Header + Component::WriteNicFrame(delivery + Header, Component::NicReplyMethod,
                                                                    transaction, words, nullptr, nullptr);
        ++Queued;
        ++DriverSignals;
    }

    // A socket request with a reply capability, on the driver-reply
    // Endpoint.
    void Foreign() noexcept
    {
        UInt8* const delivery = Queue[Queued % 8U];
        std::memset(delivery, 0, Header);
        const UInt64 words[Component::SocketPayloadWords]{Incarnation, 0U, 0U, 0U};
        const CapabilityHandle reply{ForeignHandle, 1U};
        QueueBytes[Queued % 8U] = Header + Component::WriteSocketFrame(delivery + Header, Component::SocketQueryMethod,
                                                                       1U, words, nullptr, 0U, &reply);
        ++Queued;
        ++DriverSignals;
    }

    void Receive(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        UInt8* const target = reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[2U]));
        const UInt8* source{nullptr};
        Size bytes{0U};
        if ((request.Arguments[0U] == Endpoint) && (Received < Published))
        {
            source = Script[Received % 24U];
            bytes = ScriptBytes[Received % 24U];
            ++Received;
        }
        else if ((request.Arguments[0U] == DriverReply) && (Queued != 0U))
        {
            source = Queue[0U];
            bytes = QueueBytes[0U];
        }
        if ((source == nullptr) || (bytes > request.Arguments[3U]))
        {
            response.Error = ((request.Arguments[0U] == Endpoint) || (request.Arguments[0U] == DriverReply))
                                 ? SystemCallError::WouldBlock
                                 : SystemCallError::InvalidHandle;
            return;
        }
        std::memcpy(target, source, bytes);
        response.Value0 = 1U;
        response.Value1 = bytes;
        if (source == Queue[0U])
        {
            for (Size index{1U}; index < Queued; ++index)
            {
                std::memcpy(Queue[index - 1U], Queue[index], DeliveryCapacity);
                QueueBytes[index - 1U] = QueueBytes[index];
            }
            --Queued;
        }
    }

    void Send(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        const UInt8* const frame =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[2U]));
        const Size bytes = static_cast<Size>(request.Arguments[3U]);
        UInt8 delivery[DeliveryCapacity]{};
        std::memcpy(delivery + Header, frame,
                    (bytes < Component::EchoMaximumFrameBytes) ? bytes : Component::EchoMaximumFrameBytes);
        ++Sequence;
        if (request.Arguments[0U] == Driver)
        {
            DriverRequest(delivery, Header + bytes, response);
            return;
        }
        Component::SocketMessage reply{};
        const bool decoded = Component::ReadSocketFrame(delivery, Header + bytes, reply) &&
                             (reply.Method == Component::SocketReplyMethod);
        ClientReply& record = Clients[ClientCount % Records];
        record = ClientReply{.Endpoint = request.Arguments[0U],
                             .Transaction = decoded ? reply.Words[0U] : ~UInt64{0U},
                             .Kind = reply.Words[1U],
                             .Detail = reply.Words[2U],
                             .Extra = reply.Words[3U],
                             .Inline = {},
                             .InlineLength = reply.InlineLength,
                             .Sequence = Sequence};
        std::memcpy(record.Inline, reply.Inline, sizeof(record.Inline));
        ++ClientCount;
        response.Error =
            (request.Arguments[0U] == RefuseReplyTo) ? SystemCallError::ResourceExhausted : SystemCallError::None;
    }

    void DriverRequest(const UInt8* const delivery, const Size bytes, Abi::SystemCallResponse& response) noexcept
    {
        ++Attempts;
        if ((Attempts >= BlockFrom) && (Attempts < (BlockFrom + BlockCount)))
        {
            response.Error = SystemCallError::WouldBlock;
            return;
        }
        if (Attempts == FailSend)
        {
            response.Error = FailError;
            return;
        }
        Component::NicMessage message{};
        if (!Component::ReadNicFrame(delivery, bytes, message) || (message.Method == Component::NicReplyMethod))
        {
            Missed = true;
            response.Error = SystemCallError::InvalidArguments;
            return;
        }
        const UInt8* const attachments = delivery + Header + Component::EchoHeaderBytes;
        const Size page = PageOf(message.Data.Selector);
        const Size index = DriverCount % Records;
        Drivers[index] =
            DriverRecord{.Method = message.Method,
                         .Transaction = message.Transaction,
                         .Words = {message.Words[0U], message.Words[1U], message.Words[2U], message.Words[3U]},
                         .Reply = message.Reply.Selector,
                         .Data = message.Data.Selector,
                         .ReplyRights = static_cast<UInt16>(attachments[24U] | (attachments[25U] << 8U)),
                         .DataRights = static_cast<UInt16>(attachments[56U] | (attachments[57U] << 8U)),
                         .Page = page,
                         .Sequence = Sequence};
        ++DriverCount;
        if (message.Method == Component::NicQueryMethod)
        {
            QueryTransaction = message.Transaction;
            QueryPending = HoldQuery;
            if (!HoldQuery)
            {
                Reply(message.Transaction, QueryKind, QueryMac, 1U);
            }
            return;
        }
        const Posted posted{
            .Transaction = message.Transaction, .Page = page, .Length = static_cast<Size>(message.Words[1U])};
        if (message.Method == Component::NicReceiveMethod)
        {
            Credits[CreditCount % 8U] = posted;
            ++CreditCount;
            return;
        }
        std::memcpy(g_sent[index], g_window[page % WindowPages], posted.Length);
        Transmits[TransmitCount % 8U] = posted;
        ++TransmitCount;
    }

    void Then(const Act kind, const UInt8* const bytes = nullptr, const Size length = 0U,
              const UInt64 value = 0U) noexcept
    {
        Events[EventCount % 32U] = Event{.Kind = kind, .Bytes = bytes, .Length = length, .Value = value};
        ++EventCount;
    }

    // A request of method with words {generation, first, second, third} and
    // length inline bytes, from the script's sender with its rights through
    // its selector, published by the next Publish event, which this adds.
    void Request(const UInt32 method, const UInt64 transaction, const UInt64 first, const UInt64 second,
                 const UInt64 third, const UInt8* const bytes = nullptr, const Size length = 0U,
                 const UInt64 generation = Incarnation) noexcept
    {
        UInt8* const delivery = Script[Scripted % 24U];
        std::memset(delivery, 0, DeliveryCapacity);
        const CapabilityHandle reply{FirstReply + transaction, 1U};
        const UInt64 words[Component::SocketPayloadWords]{generation, first, second, third};
        const Size written =
            Component::WriteSocketFrame(delivery + Header, method, transaction, words, bytes, length, &reply);
        delivery[14U] = static_cast<UInt8>(ScriptRights);
        delivery[15U] = static_cast<UInt8>(ScriptRights >> 8U);
        Store(delivery + 24U, ScriptSender);
        Store(delivery + Component::EchoDeliverySealedSelectorOffset, ScriptSelector);
        ScriptBytes[Scripted % 24U] = Header + written;
        ++Scripted;
        Then(Act::Publish);
    }

    void Rule(const UInt64 transaction, const UInt64 identity, const UInt64 local, const UInt64 remotePort,
              const UInt64 remoteAddress) noexcept
    {
        Request(Component::SocketRuleMethod, transaction, identity, local | (remotePort << 16U), remoteAddress);
    }

    void Bind(const UInt64 transaction, const UInt64 port) noexcept
    {
        Request(Component::SocketBindMethod, transaction, port, 0U, 0U);
    }

    void SendTo(const UInt64 transaction, const UInt64 local, const UInt64 remotePort, const UInt64 remoteAddress,
                const char* const text) noexcept
    {
        const Size length = std::strlen(text);
        Request(Component::SocketSendMethod, transaction, local | (remotePort << 16U), remoteAddress, length,
                reinterpret_cast<const UInt8*>(text), length);
    }

    void Listen(const UInt64 transaction, const UInt64 port) noexcept
    {
        Request(Component::SocketReceiveMethod, transaction, port, 0U, 0U);
    }

    void Query(const UInt64 transaction, const UInt64 generation = Incarnation) noexcept
    {
        Request(Component::SocketQueryMethod, transaction, 0U, 0U, 0U, nullptr, 0U, generation);
    }

    // The rule and binding of the echo port for Root, from the peer's port.
    void Echo(const UInt64 first) noexcept
    {
        Rule(first, 0U, EchoPort, PeerPort, Fixtures::PeerAddress);
        Bind(first + 1U, EchoPort);
    }

    // The first reply to transaction, on its reply capability, or null.
    [[nodiscard]] const ClientReply* Find(const UInt64 transaction) const noexcept
    {
        for (Size index{0U}; (index < ClientCount) && (index < Records); ++index)
        {
            if (Clients[index].Transaction == transaction)
            {
                return (Clients[index].Endpoint == (FirstReply + transaction)) ? &Clients[index] : nullptr;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool Replied(const UInt64 transaction, const UInt64 kind, const UInt64 detail = 0U,
                               const UInt64 extra = 0U) const noexcept
    {
        const ClientReply* const reply = Find(transaction);
        return (reply != nullptr) && (reply->Kind == kind) && (reply->Detail == detail) && (reply->Extra == extra) &&
               ClosedHandle(FirstReply + transaction);
    }

    [[nodiscard]] bool ClosedHandle(const UInt64 selector) const noexcept
    {
        bool found{false};
        for (Size index{0U}; (index < Closes) && (index < 64U); ++index)
        {
            found = found || (Closed[index] == selector);
        }
        return found;
    }

    // The driver requests of method for window page page, and the last one.
    [[nodiscard]] Size CountDriver(const UInt64 method, const Size page) const noexcept
    {
        Size count{0U};
        for (Size index{0U}; (index < DriverCount) && (index < Records); ++index)
        {
            count += ((Drivers[index].Method == method) && (Drivers[index].Page == page)) ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] Size LastDriver(const UInt64 method, const Size page) const noexcept
    {
        Size last{Records};
        for (Size index{0U}; (index < DriverCount) && (index < Records); ++index)
        {
            last = ((Drivers[index].Method == method) && (Drivers[index].Page == page)) ? index : last;
        }
        return last;
    }

    // The last Send for page carried length bytes equal to frame's.
    [[nodiscard]] bool SentFrame(const Size page, const UInt8* const frame, const Size length) const noexcept
    {
        const Size index = LastDriver(Component::NicSendMethod, page);
        return (index != Records) && (Drivers[index].Words[1U] == length) &&
               (std::memcmp(g_sent[index], frame, length) == 0);
    }

    // Every driver request named the driver's tag, a copy of the
    // driver-reply Endpoint with Invoke and a Map-only Frame of a window page
    // (none for the Query), with transactions that never repeat.
    [[nodiscard]] bool DriverRequestsWellFormed() const noexcept
    {
        bool formed{DriverCount != 0U};
        for (Size index{0U}; (index < DriverCount) && (index < Records); ++index)
        {
            const DriverRecord& record = Drivers[index];
            const bool query = record.Method == Component::NicQueryMethod;
            formed = formed && (record.Words[0U] == DriverTag) && (record.Reply == DriverReply) &&
                     (record.ReplyRights == InvokeRight) && (record.Transaction != 0U) &&
                     (query ? (record.Data == 0U)
                            : ((record.DataRights == Component::NicMapRight) && (record.Page < WindowPages)));
            for (Size other{0U}; other < index; ++other)
            {
                formed = formed && (Drivers[other].Transaction != record.Transaction);
            }
        }
        return formed;
    }

    // The counts a Query's answer carried, at their protocol indices.
    [[nodiscard]] UInt64 Drop(const UInt64 transaction, const Size index) const noexcept
    {
        const ClientReply* const reply = Find(transaction);
        return (reply == nullptr) ? ~UInt64{0U} : Load(reply->Inline + (index * 8U));
    }
};

NetKernel g_kernel{};

// A fresh kernel for a run: Root sends with Manage through the sealed
// Endpoint, and the driver answers its Query with Mac at once.
void Begin() noexcept
{
    g_kernel = NetKernel{};
    g_kernel.ScriptSender = Root;
    g_kernel.ScriptRights = ManageRights;
    g_kernel.ScriptSelector = Component::SocketSealedSelector;
    g_kernel.QueryKind = Component::NicDone;
    g_kernel.QueryMac = Mac;
    std::memset(g_window, 0, sizeof(g_window));
}

[[nodiscard]] UInt64 Run(const UInt64 windowPages = WindowPages, const UInt64 configuration = Configuration) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    block[Abi::ComponentStartMagicWord] = Abi::ComponentStartMagic;
    block[Abi::ComponentIncarnationWord] = Incarnation;
    block[Abi::ComponentBehaviorWord] = Abi::ComponentBehaviorNetwork;
    block[Abi::ComponentWaitSetWord] = WaitSet;
    block[Abi::ComponentReceiveWord] = Endpoint;
    block[Abi::ComponentAccountWord] = Account;
    block[Abi::ComponentAccountSpaceWord] = Space;
    block[Abi::ComponentWindowStartWord] = Address(g_window);
    block[Abi::ComponentWindowPagesWord] = windowPages;
    block[Abi::ComponentBlockServiceWord] = Driver;
    block[Abi::ComponentBlockGenerationWord] = DriverTag;
    block[Abi::ComponentNetworkConfigurationWord] = configuration;
    block[Abi::ComponentBlockReplyWord] = DriverReply;
    const User::System::SystemCallClient client{User::System::SystemCallTransport{&NetKernel::Transport, &g_kernel}};
    const Component::NetworkPlatform platform{.Window = Address(g_window)};
    return Component::ServeNetwork(client, block, platform);
}

// A UDP datagram from the peer to port with a payload of length bytes of
// value, its UDP checksum zero (not checked), built on UdpIncoming's
// headers.
Size Datagram(UInt8* const frame, const UInt16 sourcePort, const UInt16 port, const Size length,
              const UInt8 value) noexcept
{
    std::memcpy(frame, Fixtures::UdpIncoming, 42U);
    Fixtures::Put16(frame + 16U, static_cast<UInt16>(28U + length));
    Fixtures::Put16(frame + 34U, sourcePort);
    Fixtures::Put16(frame + 36U, port);
    Fixtures::Put16(frame + 38U, static_cast<UInt16>(8U + length));
    Fixtures::Put16(frame + 40U, 0U);
    std::memset(frame + 42U, value, length);
    Fixtures::FixIpChecksum(frame);
    const Size total = 42U + length;
    if (total < 60U)
    {
        std::memset(frame + total, 0, 60U - total);
        return 60U;
    }
    return total;
}

void TestStartBlock(TestContext& runner) noexcept
{
    Begin();
    const UInt64 small = Run(3U);
    ASTERIA_EXPECT(runner, "a window of three pages ends the service before it creates anything",
                   small == Failure(Abi::ComponentNetworkWindowStep), g_kernel.Creates == 0U, g_kernel.Attempts == 0U);

    const UInt64 prefixes[3U]{Fixtures::OwnAddress, Fixtures::OwnAddress | (UInt64{33U} << 32U),
                              Fixtures::OwnAddress | (UInt64{0x118U} << 32U)};
    bool refused{true};
    for (const UInt64 configuration : prefixes)
    {
        Begin();
        refused = refused && (Run(WindowPages, configuration) == Failure(Abi::ComponentNetworkConfigurationStep)) &&
                  (g_kernel.Creates == 0U);
    }
    ASTERIA_EXPECT(runner, "a prefix length of 0 or 33, or a bit beyond it, ends the service before it creates",
                   refused);

    Begin();
    const UInt64 network = Run(WindowPages, (Fixtures::OwnAddress & 0xFFFF'FF00U) | (UInt64{24U} << 32U));
    Begin();
    g_kernel.QueryMac = 0x03U | (UInt64{1U} << 40U);
    const UInt64 multicast = Run();
    ASTERIA_EXPECT(runner, "an address or MAC address the stack refuses ends the service after the Query",
                   network == Failure(Abi::ComponentNetworkConfigurationStep),
                   multicast == Failure(Abi::ComponentNetworkConfigurationStep), g_kernel.Creates == 8U,
                   g_kernel.DriverCount == 1U);

    Begin();
    g_kernel.QueryKind = Component::NicFailed;
    const UInt64 failedQuery = Run();
    Begin();
    g_kernel.QueryKind = Component::NicDriverRestarted;
    const UInt64 restartedQuery = Run();
    Begin();
    g_kernel.FailSend = 1U;
    g_kernel.FailError = SystemCallError::InvalidHandle;
    const UInt64 gone = Run();
    Begin();
    g_kernel.FailSend = 1U;
    g_kernel.FailError = SystemCallError::InvalidArguments;
    const UInt64 refusedQuery = Run();
    ASTERIA_EXPECT(runner, "a Query the driver fails, a restarted driver, and a Query the kernel refuses end it",
                   failedQuery == Failure(Abi::ComponentNetworkQueryStep),
                   restartedQuery == Failure(Abi::ComponentNetworkDriverStep),
                   gone == Failure(Abi::ComponentNetworkDriverStep, SystemCallError::InvalidHandle),
                   refusedQuery == Failure(Abi::ComponentNetworkQueryStep, SystemCallError::InvalidArguments));
}

void TestBringUp(TestContext& runner) noexcept
{
    Begin();
    g_kernel.Query(1U);
    const UInt64 ended = Run();
    bool mapped{g_kernel.MapCount == WindowPages};
    for (Size page{0U}; page < WindowPages; ++page)
    {
        const MapRecord& record = g_kernel.Maps[page];
        mapped = mapped && (g_kernel.Types[2U * page] == Abi::ObjectTypeFrame) &&
                 (g_kernel.Types[(2U * page) + 1U] == Abi::ObjectTypeMemoryObject) &&
                 (record.Frame == (0x1000U + (2U * page) + 1U)) && (record.Memory == (0x1000U + (2U * page) + 2U)) &&
                 (record.Space == Space) && (record.Start == Address(g_window[page])) && (record.Pages == 1U);
    }
    ASTERIA_EXPECT(runner, "four one-page Frames are created and mapped at the window's four pages", ended == Served(),
                   g_kernel.Creates == (2U * WindowPages), mapped);
    ASTERIA_EXPECT(runner, "the driver is queried, then both receive pages are lent with a credit each",
                   g_kernel.DriverCount == 3U, g_kernel.Drivers[0U].Method == Component::NicQueryMethod,
                   g_kernel.Drivers[0U].Transaction == 1U, g_kernel.Drivers[1U].Method == Component::NicReceiveMethod,
                   g_kernel.Drivers[1U].Page == 0U, g_kernel.Drivers[2U].Method == Component::NicReceiveMethod,
                   g_kernel.Drivers[2U].Page == 1U, g_kernel.DriverRequestsWellFormed(), g_kernel.CreditCount == 2U);
    bool zero{true};
    for (Size index{0U}; index < Component::SocketDropCounts; ++index)
    {
        zero = zero && (g_kernel.Drop(1U, index) == 0U);
    }
    const ClientReply* const query = g_kernel.Find(1U);
    ASTERIA_EXPECT(runner, "the stack is configured with the driver's MAC address and word 37",
                   g_kernel.Replied(1U, Component::SocketDone, Mac, Configuration), zero, query != nullptr,
                   (query != nullptr) && (query->InlineLength == Component::SocketInlineBytes));

    // A request that arrives while the bring-up awaits the driver's answer
    // Is served afterwards: its wake was consumed by a bring-up wait, so the loop takes what the
    // Endpoint holds before it waits.
    Begin();
    g_kernel.HoldQuery = true;
    g_kernel.Then(Act::Timeout);
    g_kernel.Query(1U);
    g_kernel.Then(Act::AnswerQuery);
    const UInt64 woken = Run();
    ASTERIA_EXPECT(runner, "a request that arrived during the bring-up is served after it, with timed waits",
                   woken == Served(), g_kernel.Replied(1U, Component::SocketDone, Mac, Configuration),
                   g_kernel.TimedWaits == 3U, g_kernel.Waits == 4U, !g_kernel.Missed, g_kernel.CreditCount == 2U);
}

void TestGeneratedFrames(TestContext& runner) noexcept
{
    Begin();
    g_kernel.Then(Act::Frame, Fixtures::ArpRequest, sizeof(Fixtures::ArpRequest));
    g_kernel.Then(Act::Complete, nullptr, GeneratedPage, Component::NicDone);
    g_kernel.Then(Act::Frame, Fixtures::IcmpRequest, sizeof(Fixtures::IcmpRequest));
    const UInt64 ended = Run();
    const Size arp = 3U;
    const Size echo = 5U;
    ASTERIA_EXPECT(runner, "an ARP request for the stack's address is answered byte-exact through the generated page",
                   ended == Served(), !g_kernel.Missed, g_kernel.DriverCount == 7U,
                   g_kernel.Drivers[arp].Method == Component::NicSendMethod,
                   g_kernel.Drivers[arp].Page == GeneratedPage,
                   g_kernel.Drivers[arp].Words[1U] == sizeof(Fixtures::ArpReply),
                   std::memcmp(g_sent[arp], Fixtures::ArpReply, sizeof(Fixtures::ArpReply)) == 0,
                   g_kernel.Drivers[arp + 1U].Method == Component::NicReceiveMethod,
                   g_kernel.Drivers[arp + 1U].Page == 0U, g_kernel.DriverRequestsWellFormed());
    ASTERIA_EXPECT(runner, "an ICMP echo is answered through the same page once it is free, the credit lent again",
                   g_kernel.Drivers[echo].Method == Component::NicSendMethod,
                   g_kernel.Drivers[echo].Page == GeneratedPage,
                   std::memcmp(g_sent[echo], Fixtures::IcmpReply, sizeof(Fixtures::IcmpReply)) == 0,
                   g_kernel.Drivers[echo + 1U].Method == Component::NicReceiveMethod,
                   g_kernel.Drivers[echo + 1U].Page == 1U, g_kernel.CreditCount == 2U);
}

void TestRules(TestContext& runner) noexcept
{
    Begin();
    g_kernel.ScriptRights = InvokeRight;
    g_kernel.Rule(1U, 0U, EchoPort, PeerPort, Fixtures::PeerAddress);
    g_kernel.ScriptRights = ManageRights;
    g_kernel.Rule(2U, 5U, EchoPort, PeerPort, Fixtures::PeerAddress);
    g_kernel.Rule(3U, 0U, 0U, PeerPort, Fixtures::PeerAddress);
    g_kernel.Rule(4U, 0U, EchoPort, PeerPort, Fixtures::PeerAddress);
    g_kernel.Bind(5U, EchoPort);
    g_kernel.Bind(6U, 0U);
    g_kernel.Bind(7U, 8U);
    g_kernel.Listen(8U, 8U);
    g_kernel.Query(9U, Incarnation + 1U);
    g_kernel.ScriptSender = Other;
    g_kernel.Bind(10U, EchoPort);
    g_kernel.Listen(11U, EchoPort);
    g_kernel.Rule(12U, 0U, EchoPort, 0U, 0U);
    g_kernel.Bind(13U, EchoPort);
    const UInt64 ended = Run();
    ASTERIA_EXPECT(runner, "a Rule without Manage or for another identity is refused, with Manage it is Done",
                   ended == Served(), g_kernel.Replied(1U, Component::SocketRefused, Component::SocketRefusedIdentity),
                   g_kernel.Replied(2U, Component::SocketRefused, Component::SocketRefusedIdentity),
                   g_kernel.Replied(3U, Component::SocketRefused, Component::SocketRefusedMalformed),
                   g_kernel.Replied(4U, Component::SocketDone));
    ASTERIA_EXPECT(runner, "the rule names the delivery's sender: its Bind is Done, another sender's refused",
                   g_kernel.Replied(5U, Component::SocketDone),
                   g_kernel.Replied(6U, Component::SocketRefused, Component::SocketRefusedMalformed),
                   g_kernel.Replied(7U, Component::SocketRefused, Component::SocketRefusedNoRule),
                   g_kernel.Replied(8U, Component::SocketRefused, Component::SocketRefusedNotBound),
                   g_kernel.Replied(9U, Component::SocketRestarted),
                   g_kernel.Replied(10U, Component::SocketRefused, Component::SocketRefusedNoRule),
                   g_kernel.Replied(11U, Component::SocketRefused, Component::SocketRefusedNotBound),
                   g_kernel.Replied(12U, Component::SocketDone),
                   g_kernel.Replied(13U, Component::SocketRefused, Component::SocketRefusedNoRule),
                   g_kernel.ClientCount == 13U);
}

void TestSends(TestContext& runner) noexcept
{
    // The peer's ARP request teaches its MAC address; the Send under the
    // rule goes out byte-exact through the client page and is answered only
    // after the driver's Done. A sender without a rule sends nothing.
    Begin();
    g_kernel.Then(Act::Frame, Fixtures::ArpRequest, sizeof(Fixtures::ArpRequest));
    g_kernel.Rule(1U, 0U, EchoPort, PeerPort, Fixtures::PeerAddress);
    g_kernel.SendTo(2U, EchoPort, PeerPort, Fixtures::PeerAddress, "hello");
    g_kernel.ScriptSender = Other;
    g_kernel.SendTo(3U, EchoPort, PeerPort, Fixtures::PeerAddress, "hello");
    g_kernel.ScriptSender = Root;
    g_kernel.SendTo(4U, EchoPort, PeerPort + 1U, Fixtures::PeerAddress, "hello");
    g_kernel.SendTo(5U, 0U, PeerPort, Fixtures::PeerAddress, "hello");
    const UInt64 unanswered = Run();
    ASTERIA_EXPECT(runner, "a Send under a rule goes to the driver byte-exact and waits for its Done",
                   unanswered == Served(), g_kernel.CountDriver(Component::NicSendMethod, ClientPage) == 1U,
                   g_kernel.SentFrame(ClientPage, Fixtures::UdpOutgoing, sizeof(Fixtures::UdpOutgoing)),
                   g_kernel.Find(2U) == nullptr, !g_kernel.ClosedHandle(FirstReply + 2U));
    ASTERIA_EXPECT(runner, "a Send no rule of its sender allows is refused, and nothing is sent for it",
                   g_kernel.Replied(3U, Component::SocketRefused, Component::SocketRefusedNoRule),
                   g_kernel.Replied(4U, Component::SocketRefused, Component::SocketRefusedNoRule),
                   g_kernel.Replied(5U, Component::SocketRefused, Component::SocketRefusedMalformed),
                   g_kernel.DriverCount == 6U, g_kernel.DriverRequestsWellFormed());

    Begin();
    g_kernel.Then(Act::Frame, Fixtures::ArpRequest, sizeof(Fixtures::ArpRequest));
    g_kernel.Rule(1U, 0U, EchoPort, PeerPort, Fixtures::PeerAddress);
    g_kernel.SendTo(2U, EchoPort, PeerPort, Fixtures::PeerAddress, "hello");
    g_kernel.Then(Act::Complete, nullptr, ClientPage, Component::NicDone);
    const UInt64 done = Run();
    const ClientReply* const answer = g_kernel.Find(2U);
    ASTERIA_EXPECT(runner, "the Send is answered Done with its length after the driver's Done", done == Served(),
                   g_kernel.Replied(2U, Component::SocketDone, 5U), answer != nullptr,
                   (answer != nullptr) && (answer->Sequence > g_kernel.CompletedAt) &&
                       (answer->InlineLength == Component::SocketInlineBytes));

    // Pending: the destination is unknown, so the stack's ARP request goes
    // out through the generated page and the client is told to resend.
    Begin();
    g_kernel.Rule(1U, 0U, EchoPort, PeerPort, Fixtures::PeerAddress);
    g_kernel.SendTo(2U, EchoPort, PeerPort, Fixtures::PeerAddress, "hello");
    const UInt64 pending = Run();
    ASTERIA_EXPECT(runner, "a Send to an unresolved peer is Pending after the stack's ARP request", pending == Served(),
                   g_kernel.Replied(2U, Component::SocketPending),
                   g_kernel.SentFrame(GeneratedPage, Fixtures::ResolutionRequest, sizeof(Fixtures::ResolutionRequest)),
                   g_kernel.CountDriver(Component::NicSendMethod, ClientPage) == 0U);

    // A second Send while the client page is busy is held, not refused, and
    // goes out once the first's Done frees the page; a third is beyond the
    // credit.
    Begin();
    g_kernel.Then(Act::Frame, Fixtures::ArpRequest, sizeof(Fixtures::ArpRequest));
    g_kernel.Rule(1U, 0U, EchoPort, PeerPort, Fixtures::PeerAddress);
    g_kernel.SendTo(2U, EchoPort, PeerPort, Fixtures::PeerAddress, "hello");
    g_kernel.SendTo(3U, EchoPort, PeerPort, Fixtures::PeerAddress, "world");
    g_kernel.SendTo(4U, EchoPort, PeerPort, Fixtures::PeerAddress, "third");
    g_kernel.Then(Act::Complete, nullptr, ClientPage, Component::NicDone);
    g_kernel.Then(Act::Complete, nullptr, ClientPage, Component::NicFailed);
    const UInt64 held = Run();
    const Size second = g_kernel.LastDriver(Component::NicSendMethod, ClientPage);
    const ClientReply* const first = g_kernel.Find(2U);
    ASTERIA_EXPECT(runner, "a Send held while the client page is busy is sent when it frees", held == Served(),
                   !g_kernel.Missed, g_kernel.CountDriver(Component::NicSendMethod, ClientPage) == 2U,
                   second != Records, (second != Records) && (std::memcmp(g_sent[second] + 42U, "world", 5U) == 0),
                   (second != Records) && (first != nullptr) && (g_kernel.Drivers[second].Sequence > first->Sequence),
                   g_kernel.Replied(2U, Component::SocketDone, 5U),
                   g_kernel.Replied(3U, Component::SocketFailed, Component::NicFailed, FailedDetail),
                   g_kernel.Replied(4U, Component::SocketRefused, Component::SocketRefusedCredit));

    // A Send the kernel refuses to pass to the driver (not for a gone
    // Endpoint) is answered Failed with the error, and the service goes on.
    Begin();
    g_kernel.Then(Act::Frame, Fixtures::ArpRequest, sizeof(Fixtures::ArpRequest));
    g_kernel.Rule(1U, 0U, EchoPort, PeerPort, Fixtures::PeerAddress);
    g_kernel.SendTo(2U, EchoPort, PeerPort, Fixtures::PeerAddress, "hello");
    g_kernel.Query(3U);
    g_kernel.FailSend = 6U;
    g_kernel.FailError = SystemCallError::InvalidArguments;
    const UInt64 refused = Run();
    ASTERIA_EXPECT(
        runner, "a Send the kernel refuses is answered Failed with the error", refused == Served(),
        g_kernel.Replied(2U, Component::SocketFailed, 0U, static_cast<UInt64>(SystemCallError::InvalidArguments)),
        g_kernel.Replied(3U, Component::SocketDone, Mac, Configuration));
}

void TestReceives(TestContext& runner) noexcept
{
    static UInt8 unauthorized[60U];
    static_cast<void>(Datagram(unauthorized, static_cast<UInt16>(PeerPort + 1U), EchoPort, 5U, 'x'));
    Begin();
    g_kernel.Echo(1U);
    g_kernel.Listen(3U, EchoPort);
    g_kernel.Then(Act::Frame, Fixtures::UdpIncoming, sizeof(Fixtures::UdpIncoming));
    g_kernel.Then(Act::Frame, Fixtures::UdpIncoming, sizeof(Fixtures::UdpIncoming));
    g_kernel.Listen(4U, EchoPort);
    g_kernel.Then(Act::Frame, unauthorized, sizeof(unauthorized));
    g_kernel.Listen(5U, EchoPort);
    g_kernel.Listen(6U, EchoPort);
    g_kernel.Query(4U);
    g_kernel.Query(7U);
    const UInt64 ended = Run();
    const ClientReply* const datagram = g_kernel.Find(3U);
    ASTERIA_EXPECT(
        runner, "a datagram for the bound port answers the Receive with its bytes inline and its source",
        ended == Served(), !g_kernel.Missed, g_kernel.Replied(1U, Component::SocketDone),
        g_kernel.Replied(2U, Component::SocketDone),
        g_kernel.Replied(3U, Component::SocketDone, 5U, Fixtures::PeerAddress | (static_cast<UInt64>(PeerPort) << 32U)),
        datagram != nullptr, (datagram != nullptr) && (datagram->InlineLength == Component::SocketInlineBytes),
        (datagram != nullptr) && (std::memcmp(datagram->Inline, "hello", 5U) == 0),
        g_kernel.CountDriver(Component::NicReceiveMethod, 0U) == 3U,
        g_kernel.CountDriver(Component::NicReceiveMethod, 1U) == 2U);
    bool duplicate{false};
    for (Size index{0U}; index < g_kernel.ClientCount; ++index)
    {
        const ClientReply& reply = g_kernel.Clients[index];
        duplicate = duplicate || ((reply.Transaction == 4U) && (reply.Kind == Component::SocketRefused) &&
                                  (reply.Detail == Component::SocketRefusedCredit));
    }
    ASTERIA_EXPECT(runner, "a datagram with no Receive, or from a source no rule allows, is dropped and counted",
                   g_kernel.Drop(7U, Component::SocketDropNoReceive) == 1U,
                   g_kernel.Drop(7U, Component::SocketDropUnauthorized) == 1U, g_kernel.Find(5U) == nullptr,
                   g_kernel.Replied(6U, Component::SocketRefused, Component::SocketRefusedCredit), duplicate);
}

void TestCounts(TestContext& runner) noexcept
{
    static UInt8 elsewhere[60U];
    static UInt8 badEcho[60U];
    static UInt8 unauthorized[60U];
    static UInt8 unbound[60U];
    static UInt8 longest[42U + Component::SocketInlineBytes + 1U];
    std::memcpy(elsewhere, Fixtures::ArpRequest, sizeof(elsewhere));
    elsewhere[41U] = 99U;
    std::memcpy(badEcho, Fixtures::IcmpRequest, sizeof(badEcho));
    badEcho[36U] ^= 1U;
    static_cast<void>(Datagram(unauthorized, static_cast<UInt16>(PeerPort + 1U), EchoPort, 5U, 'x'));
    static_cast<void>(Datagram(unbound, PeerPort, EchoPort + 1U, 5U, 'x'));
    const Size longBytes = Datagram(longest, PeerPort, EchoPort, Component::SocketInlineBytes + 1U, 'y');
    Begin();
    g_kernel.Echo(1U);
    g_kernel.Listen(3U, EchoPort);
    g_kernel.Then(Act::Frame, elsewhere, sizeof(elsewhere));
    g_kernel.Then(Act::Frame, badEcho, sizeof(badEcho));
    g_kernel.Then(Act::Frame, unauthorized, sizeof(unauthorized));
    g_kernel.Then(Act::Frame, longest, longBytes);
    g_kernel.Then(Act::Frame, Fixtures::ArpRequest, sizeof(Fixtures::ArpRequest));
    g_kernel.Then(Act::Frame, Fixtures::IcmpRequest, sizeof(Fixtures::IcmpRequest));
    g_kernel.Then(Act::Frame, unbound, sizeof(unbound));
    g_kernel.Then(Act::Frame, Fixtures::UdpIncoming, 13U);
    g_kernel.Then(Act::Frame, Fixtures::UdpIncoming, sizeof(Fixtures::UdpIncoming));
    g_kernel.Then(Act::Frame, Fixtures::UdpIncoming, sizeof(Fixtures::UdpIncoming));
    g_kernel.Query(4U);
    const UInt64 ended = Run();
    ASTERIA_EXPECT(
        runner, "Query reports each drop at its named index", ended == Served(), !g_kernel.Missed,
        g_kernel.Replied(4U, Component::SocketDone, Mac, Configuration),
        g_kernel.Drop(4U, Component::SocketDropNotForUs) == 1U, g_kernel.Drop(4U, Component::SocketDropBadIcmp) == 1U,
        g_kernel.Drop(4U, Component::SocketDropUnauthorized) == 1U,
        g_kernel.Drop(4U, Component::SocketDropNoReceive) == 1U, g_kernel.Drop(4U, Component::SocketDropTooLong) == 1U,
        g_kernel.Drop(4U, Component::SocketDropNoTransmit) == 1U,
        g_kernel.Drop(4U, Component::SocketDropMalformed) == 2U);
    ASTERIA_EXPECT(runner, "the generated page busy with the ARP reply drops the echo reply; the Receive is answered",
                   g_kernel.CountDriver(Component::NicSendMethod, GeneratedPage) == 1U,
                   g_kernel.SentFrame(GeneratedPage, Fixtures::ArpReply, sizeof(Fixtures::ArpReply)),
                   g_kernel.Replied(3U, Component::SocketDone, 5U,
                                    Fixtures::PeerAddress | (static_cast<UInt64>(PeerPort) << 32U)));
}

void TestDriverEnds(TestContext& runner) noexcept
{
    Begin();
    g_kernel.Then(Act::Restart);
    const UInt64 restarted = Run();
    Begin();
    g_kernel.Then(Act::Frame, Fixtures::ArpRequest, sizeof(Fixtures::ArpRequest));
    g_kernel.FailSend = 4U;
    g_kernel.FailError = SystemCallError::ObjectNotFound;
    const UInt64 gone = Run();
    Begin();
    g_kernel.FailSend = 2U;
    g_kernel.FailError = SystemCallError::Revoked;
    const UInt64 revoked = Run();
    Begin();
    g_kernel.FailSend = 3U;
    g_kernel.FailError = SystemCallError::ResourceExhausted;
    const UInt64 exhausted = Run();
    ASTERIA_EXPECT(runner, "a DriverRestarted reply, and a send to a gone driver, end the instance",
                   restarted == Failure(Abi::ComponentNetworkDriverStep),
                   gone == Failure(Abi::ComponentNetworkDriverStep, SystemCallError::ObjectNotFound),
                   revoked == Failure(Abi::ComponentNetworkDriverStep, SystemCallError::Revoked),
                   exhausted == Failure(Abi::ComponentNetworkDriverStep, SystemCallError::ResourceExhausted));

    // The driver's Endpoint full: the request is kept and retried, the wait
    // meanwhile bounded, under the same transaction.
    Begin();
    g_kernel.BlockFrom = 3U;
    g_kernel.BlockCount = 2U;
    const UInt64 retried = Run();
    ASTERIA_EXPECT(runner, "a credit the driver's Endpoint refuses with WouldBlock is retried after a bounded wait",
                   retried == Served(), g_kernel.Attempts == 5U, g_kernel.DriverCount == 3U, g_kernel.TimedWaits == 1U,
                   g_kernel.Waits == 2U, g_kernel.Drivers[2U].Page == 1U, g_kernel.Drivers[2U].Transaction == 3U,
                   g_kernel.CreditCount == 2U);
}

void TestForeignFrames(TestContext& runner) noexcept
{
    Begin();
    // A NIC Query on the service Endpoint, a socket Query through another
    // selector, a reply and a Send with a byte past its datagram.
    UInt8* const nic = g_kernel.Script[0U];
    const UInt64 nicWords[4U]{DriverTag, 0U, 0U, 0U};
    const CapabilityHandle foreign{ForeignHandle + 1U, 1U};
    g_kernel.ScriptBytes[0U] =
        Header + Component::WriteNicFrame(nic + Header, Component::NicQueryMethod, 1U, nicWords, &foreign, nullptr);
    g_kernel.Scripted = 1U;
    g_kernel.Then(Act::Publish);
    g_kernel.ScriptSelector = Component::FsSealedSelector;
    g_kernel.Query(2U);
    g_kernel.ScriptSelector = Component::SocketSealedSelector;
    g_kernel.Request(Component::SocketReplyMethod, 3U, Component::SocketDone, 0U, 0U);
    g_kernel.SendTo(4U, EchoPort, PeerPort, Fixtures::PeerAddress, "hello");
    g_kernel.Script[g_kernel.Scripted - 1U][Header + Component::EchoHeaderBytes + Component::EchoAttachmentBytes +
                                            Component::SocketPayloadBytes + 5U] = 'x';
    g_kernel.Then(Act::Foreign);
    g_kernel.Query(5U);
    const UInt64 ended = Run();
    ASTERIA_EXPECT(runner, "a foreign or malformed frame is dropped with its capabilities closed, unanswered",
                   ended == Served(), g_kernel.ClosedHandle(ForeignHandle + 1U), g_kernel.ClosedHandle(FirstReply + 2U),
                   g_kernel.ClosedHandle(FirstReply + 4U), g_kernel.ClosedHandle(ForeignHandle),
                   g_kernel.Find(2U) == nullptr, g_kernel.Find(4U) == nullptr, g_kernel.ClientCount == 1U,
                   g_kernel.Replied(5U, Component::SocketDone, Mac, Configuration));
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"NetworkServiceTests"};
    Asteria::Test::TestStartBlock(runner);
    Asteria::Test::TestBringUp(runner);
    Asteria::Test::TestGeneratedFrames(runner);
    Asteria::Test::TestRules(runner);
    Asteria::Test::TestSends(runner);
    Asteria::Test::TestReceives(runner);
    Asteria::Test::TestCounts(runner);
    Asteria::Test::TestDriverEnds(runner);
    Asteria::Test::TestForeignFrames(runner);
    return runner.Finish();
}
