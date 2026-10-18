// SPDX-License-Identifier: Apache-2.0

#include "User/Component/InputBroker.hpp"

#include "User/Component/ServiceScratch.hpp"

#include "User/Component/InputBrokerLogic.hpp"
#include "User/Component/PointerRoute.hpp"
#include "User/Component/ServiceCapabilities.hpp"

namespace Asteria::User::Component
{
namespace
{
struct Credit
{
    System::CapabilityHandle Reply{};
    Foundation::UInt64 Transaction{0U};
    Foundation::Size Count{0U};
    Foundation::Size Frozen{0U};
    Foundation::Size Bytes{0U};
    Foundation::UInt32 Method{0U};
    Foundation::UInt32 Id{0U};
    alignas(8) Foundation::UInt8 Frame[InputMaximumFrameBytes]{};
};
struct Registration
{
    System::CapabilityHandle Endpoint{};
    Foundation::UInt64 LastPublish{0U};
    Foundation::Size LastCount{0U};
    Foundation::UInt8 LastEvents[InputMaximumEvents * 24U]{};
};
constexpr Foundation::Size Sources{InputBrokerLogic::MaximumSources};
constexpr Foundation::Size Clients{InputBrokerLogic::MaximumClients};
constexpr Foundation::Size ReplyRoutes{1U + Sources + Clients};
// One immutable immediate reply per receive route. A stalled client/source never occupies another
// route's reply storage.
struct PendingAnswer
{
    System::CapabilityHandle Reply{};
    Foundation::Size Bytes{0U};
    alignas(8) Foundation::UInt8 Frame[InputMaximumFrameBytes]{};
};
// The shared enrollment Endpoint must keep draining too. Reserve separate
// manager and Invoke overflow slots so an unread client's enrollment reply
// cannot obstruct seat management.
constinit PendingAnswer g_answers[ReplyRoutes + 2U * Clients]{};
Foundation::Size g_route{0U};
Foundation::UInt64 g_refusedRequests{0U};
constinit System::CapabilityHandle g_pointerSink{};
constinit Registration g_sources[Sources]{};
constinit System::CapabilityHandle g_clients[Clients]{};
constinit Credit g_credits[Clients]{};
constinit Foundation::UInt64 g_baselines[Clients][3U]{};
constinit bool g_resumed[Clients]{};
constinit Credit g_manager{};
constinit Foundation::UInt32 g_focusRequest{0U};
alignas(8) constinit Foundation::UInt8 g_delivery[EchoDeliveryHeaderBytes + EchoMaximumFrameBytes]{};
alignas(8) constinit Foundation::UInt8 g_frame[InputMaximumFrameBytes]{};
constinit Input::InputEvent g_events[InputMaximumEvents]{};
constinit Foundation::Size g_deliveryBytes{0U};

// A kernel-decoded foreign frame may have variable payload descriptors.
// Locate its actual attachments before closing every installed handle.
[[nodiscard]] bool CloseInputAttachments(const System::SystemCallClient& client) noexcept
{
    if (g_deliveryBytes < EchoDeliveryHeaderBytes + EchoHeaderBytes)
    {
        return false;
    }
    const Foundation::UInt8* const frame = g_delivery + EchoDeliveryHeaderBytes;
    const Foundation::Size descriptors = static_cast<Foundation::Size>(frame[80U]) * 8U;
    const Foundation::Size count = frame[81U];
    if ((count > 4U) || (EchoDeliveryHeaderBytes + EchoHeaderBytes + descriptors + count * 32U > g_deliveryBytes))
    {
        return false;
    }
    bool closed{true};
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        const Foundation::UInt8* const record = frame + EchoHeaderBytes + descriptors + index * 32U;
        closed =
            Closed(client, System::CapabilityHandle{System::LoadWord(record), System::LoadWord(record + 8U)}) && closed;
    }
    return closed;
}

[[nodiscard]] bool Answer(const System::SystemCallClient& client, const InputMessage& message, Foundation::UInt64 epoch,
                          InputAnswer answer, Foundation::UInt32 id = 0U, Foundation::UInt64 count = 0U,
                          const System::CapabilityHandle* endpoint = nullptr) noexcept
{
    const Foundation::UInt64 words[4U]{epoch, static_cast<Foundation::UInt64>(answer), id, count};
    const Foundation::Size size =
        WriteInputFrame(g_frame, InputReplyMethod, message.Transaction, words, nullptr, 0U, endpoint);
    const auto sent = client.Send(message.Reply, System::AddressOf(g_frame), size).Error;
    if (sent == Abi::SystemCallError::WouldBlock)
    {
        auto& pending = g_answers[g_route];
        pending.Reply = message.Reply;
        pending.Bytes = size;
        for (Foundation::Size i{0U}; i < size; ++i)
        {
            pending.Frame[i] = g_frame[i];
        }
        return true;
    }
    return Closed(client, message.Reply);
}

// A source registered again (RegisterSource naming its id; the codec bounds
// it) belongs to a restarted driver: its held keys are released and its
// replay fence cleared, its Endpoint and id kept. The root fenced the source
// first (a Query on it), so nothing the dead instance queued is processed
// later.
[[nodiscard, gnu::noinline]] bool Register(const System::SystemCallClient& client, const InputMessage& message,
                                           const Foundation::UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const bool source = message.Method == InputRegisterSourceMethod;
    const bool again = source && (message.Words[1U] != 0U);
    const Foundation::UInt64 epoch = block[Abi::ComponentIncarnationWord];
    if (!source && message.Words[1U] != 0U)
    {
        for (Foundation::UInt32 i{1U}; i <= Clients; ++i)
        {
            if (Scratch().Input().Logic.BrokerPointer(i))
            {
                return Answer(client, message, epoch, InputAnswer::Refused);
            }
        }
    }
    const Foundation::UInt32 capacity = static_cast<Foundation::UInt32>(source ? Sources : Clients);
    Foundation::UInt32 index{again ? static_cast<Foundation::UInt32>(message.Words[1U] - 1U) : 0U};
    while (!again && (index < capacity) && ((source ? g_sources[index].Endpoint : g_clients[index]).Selector != 0U))
    {
        ++index;
    }
    if (index == capacity)
    {
        return Answer(client, message, epoch, InputAnswer::Exhausted);
    }
    System::CapabilityHandle& endpoint = source ? g_sources[index].Endpoint : g_clients[index];
    if (again)
    {
        const bool buttons = Scratch().Input().Logic.SourceButtons(index + 1U) != 0U;
        const InputAnswer answer =
            (endpoint.Selector == 0U) ? InputAnswer::Refused
            : buttons && !Scratch().Input().Route.CanQueue(1U)
                ? InputAnswer::Exhausted
                : Scratch().Input().Logic.ResetSource(index + 1U, Scratch().Input().Route.Sequence(),
                                                      Scratch().Input().Route.Enabled());
        if (answer != InputAnswer::Done)
        {
            return Answer(client, message, epoch, answer);
        }
        if (buttons)
        {
            Input::InputEvent released{};
            released.Kind = Input::EventKind::Buttons;
            Scratch().Input().Route.Queue(released, index + 1U, Scratch().Input().Logic.PointerStamp(0U));
        }
        g_sources[index].LastPublish = 0U;
        g_sources[index].LastCount = 0U;
    }
    else if (endpoint.Selector == 0U)
    {
        alignas(8) Foundation::UInt8 request[Abi::ObjectCreateEndpointRequestSizeBytes]{};
        System::BuildDefaultEndpointRequest(request);
        System::StoreWord(request + Abi::ObjectCreateEndpointSealedSelectorOffset,
                          (source ? InputSourceSelector : InputClientSelector) + index + 1U);
        const Abi::SystemCallResponse created = client.CreateObject(
            System::CapabilityHandle{block[Abi::ComponentAccountWord], block[Abi::ComponentAccountWord + 1U]},
            Abi::ObjectTypeEndpoint, System::AddressOf(request), sizeof(request));
        if (created.Error != Abi::SystemCallError::None)
        {
            return Answer(client, message, epoch, InputAnswer::Exhausted);
        }
        endpoint = System::CapabilityHandle{created.Value0, created.Value1};
        if (!source)
        {
            g_baselines[index][1U] = Scratch().Input().Route.Sequence();
            g_baselines[index][2U] = Scratch().Input().Logic.Buttons();
        }
        const Foundation::UInt32 id =
            source ? Scratch().Input().Logic.RegisterSource()
                   : Scratch().Input().Logic.RegisterClient(message.Words[1U] == InputBrokerPointer);
        if ((id != index + 1U) || (client
                                       .BindWaitSet(System::CapabilityHandle{block[Abi::ComponentWaitSetWord],
                                                                             block[Abi::ComponentWaitSetWord + 1U]},
                                                    endpoint, (source ? 0x100U : 0x200U) + id)
                                       .Error != Abi::SystemCallError::None))
        {
            return false;
        }
    }
    return Answer(client, message, epoch, InputAnswer::Done, index + 1U, InputProtocolVersion, &endpoint);
}

[[nodiscard]] bool SendCredit(const System::SystemCallClient& client, Credit& credit, Foundation::UInt32 method,
                              const Foundation::UInt64 (&words)[4U], Foundation::Size count) noexcept
{
    // Freeze the entire answer, including manager attention versus focus selection. New input
    // cannot change a retained retry.
    if (credit.Bytes == 0U)
    {
        credit.Bytes = WriteInputFrame(credit.Frame, method, credit.Transaction, words, g_events, count);
        credit.Method = method;
        credit.Id = static_cast<Foundation::UInt32>(words[1U]);
        credit.Frozen = count;
    }
    const Abi::SystemCallError sent = client.Send(credit.Reply, System::AddressOf(credit.Frame), credit.Bytes).Error;
    // WouldBlock retains both credit and events; the timed Wait retries even
    // without new input. A dead receiver additionally loses its focus/queue:
    // it must not obstruct the physical source.
    if (sent == Abi::SystemCallError::WouldBlock)
    {
        return true;
    }
    if (!Closed(client, credit.Reply))
    {
        return false;
    }
    credit.Reply = {};
    if (sent == Abi::SystemCallError::None)
    {
        if (credit.Method == InputEventsMethod)
        {
            Scratch().Input().Logic.Take(credit.Id, credit.Frozen);
        }
        else if (credit.Method == InputAttentionMethod)
        {
            Scratch().Input().Logic.TakeAttention();
        }
        else
        {
            if (g_focusRequest == credit.Id)
            {
                g_focusRequest = 0U;
            }
        }
    }
    else if (credit.Method == InputEventsMethod &&
             (sent == Abi::SystemCallError::Revoked || sent == Abi::SystemCallError::ObjectNotFound ||
              sent == Abi::SystemCallError::InvalidHandle || sent == Abi::SystemCallError::Expired))
    {
        // The session manager can rebind this receiver; preserve its pending keys and focus
        // meanwhile, bounded at 64.
        if (!Scratch().Input().Logic.BrokerPointer(credit.Id) || !g_resumed[credit.Id - 1U])
        {
            Scratch().Input().Logic.DetachClient(credit.Id);
        }
    }
    credit.Bytes = 0U;
    return true;
}
[[nodiscard]] bool RetryNeeded() noexcept
{
    for (const auto& answer : g_answers)
    {
        if (answer.Reply.Selector != 0U)
        {
            return true;
        }
    }
    for (Foundation::UInt32 index{0U}; index < Clients; ++index)
    {
        if ((g_credits[index].Reply.Selector != 0U) && (Scratch().Input().Logic.Queued(index + 1U) != 0U))
        {
            return true;
        }
    }
    return (Scratch().Input().Route.Peek() != nullptr) ||
           ((g_manager.Reply.Selector != 0U) &&
            (g_manager.Bytes != 0U || Scratch().Input().Logic.AttentionSource() != 0U || g_focusRequest != 0U));
}
// Retry a full sink without acknowledging or losing its pointer event. Fatal
// sink failure clears the route; the manager must rebind with the new epoch.
[[nodiscard]] bool DeliverPointer(const System::SystemCallClient& client) noexcept
{
    if (Scratch().Input().Logic.FocusLocked())
    {
        Scratch().Input().Route.Discard();
        return true;
    }
    for (Foundation::Size index{0U}; index < PointerRoute::Capacity; ++index)
    {
        const Foundation::UInt64* const words = Scratch().Input().Route.Peek();
        if (words == nullptr)
        {
            return true;
        }
        const Foundation::Size bytes = WriteDisplayPointer(g_frame, words[1U], words);
        const Abi::SystemCallError sent = client.Send(g_pointerSink, System::AddressOf(g_frame), bytes).Error;
        if (sent == Abi::SystemCallError::WouldBlock)
        {
            return true;
        }
        if (sent != Abi::SystemCallError::None)
        {
            const bool closed = Closed(client, g_pointerSink);
            g_pointerSink = {};
            Scratch().Input().Route.Configure(0U, 0U);
            return closed;
        }
        Scratch().Input().Route.Take();
    }
    return true;
}
[[nodiscard]] bool DeliverCredits(const System::SystemCallClient& client, Foundation::UInt64 epoch) noexcept
{
    for (auto& answer : g_answers)
    {
        if (answer.Reply.Selector == 0U)
        {
            continue;
        }
        const auto sent = client.Send(answer.Reply, System::AddressOf(answer.Frame), answer.Bytes).Error;
        if (sent == Abi::SystemCallError::WouldBlock)
        {
            continue;
        }
        if (!Closed(client, answer.Reply))
        {
            return false;
        }
        answer.Reply = {};
    }
    if (!DeliverPointer(client))
    {
        return false;
    }
    for (Foundation::UInt32 index{0U}; index < Clients; ++index)
    {
        Credit& credit = g_credits[index];
        // Manager attention remains independent of a congested pointer sink.
        if ((Scratch().Input().Logic.BrokerPointer(index + 1U) || Scratch().Input().Route.Peek() == nullptr) &&
            (credit.Reply.Selector != 0U) && (Scratch().Input().Logic.Queued(index + 1U) != 0U))
        {
            // End at a horizon change.: freeze the count on WouldBlock, even if further events
            // arrive meanwhile.
            const Foundation::Size count =
                Scratch().Input().Logic.Peek(index + 1U, g_events, credit.Frozen == 0U ? credit.Count : credit.Frozen);
            credit.Frozen = count;
            const Foundation::UInt64 words[4U]{epoch, index + 1U, count, Scratch().Input().Logic.Horizon(index + 1U)};
            if (!SendCredit(client, credit, InputEventsMethod, words, count))
            {
                return false;
            }
        }
    }
    const Foundation::UInt32 attention = Scratch().Input().Logic.AttentionSource();
    if ((g_manager.Reply.Selector != 0U) && (g_manager.Bytes != 0U || attention != 0U || g_focusRequest != 0U))
    {
        const Foundation::UInt64 words[4U]{epoch, attention != 0U ? attention : g_focusRequest,
                                           attention != 0U ? Scratch().Input().Logic.AttentionUsage() : 0U, 0U};
        return SendCredit(client, g_manager, attention != 0U ? InputAttentionMethod : InputFocusRequestMethod, words,
                          0U);
    }
    return true;
}

[[nodiscard, gnu::noinline]] bool Process(const System::SystemCallClient& client, const InputMessage& message,
                                          const Foundation::UInt64 (&block)[Abi::ComponentBlockWords],
                                          Foundation::UInt64 selector, bool manage) noexcept
{
    const Foundation::UInt64 epoch = block[Abi::ComponentIncarnationWord];
    if ((message.Method > InputQueryMethod) && (message.Method != InputSetPointerRouteMethod) &&
        message.Method != InputRebindClientMethod && message.Method != InputResumeMethod)
    {
        return CloseInputAttachments(client);
    }
    if (message.Words[0U] != epoch)
    {
        if ((message.PointerSink.Selector != 0U) && !Closed(client, message.PointerSink))
        {
            return false;
        }
        return Answer(client, message, epoch, InputAnswer::Restarted);
    }
    const bool main = selector == InputSealedSelector;
    const Foundation::UInt64 source = selector - InputSourceSelector;
    const Foundation::UInt64 destination = selector - InputClientSelector;
    const bool isSource = (source >= 1U) && (source <= Sources) && (g_sources[source - 1U].Endpoint.Selector != 0U);
    const bool isClient =
        (destination >= 1U) && (destination <= Clients) && (g_clients[destination - 1U].Selector != 0U);
    InputAnswer answer{InputAnswer::Refused};
    Foundation::UInt32 id{0U};
    Foundation::UInt64 count{0U};
    switch (message.Method)
    {
    case InputRebindClientMethod:
        if (main && manage && Scratch().Input().Logic.BrokerPointer(static_cast<Foundation::UInt32>(message.Words[1U])))
        {
            const auto index = static_cast<Foundation::Size>(message.Words[1U] - 1U);
            auto& credit = g_credits[index];
            if (credit.Reply.Selector != 0U && !Closed(client, credit.Reply))
            {
                return false;
            }
            credit = {};
            auto& pending = g_answers[1U + Sources + index];
            if (pending.Reply.Selector != 0U && !Closed(client, pending.Reply))
            {
                return false;
            }
            pending = {};
            // The owner stopped the sole consumer and fenced this minted
            // endpoint with Query before requesting rebind.
            // No old request can pass that FIFO fence; keep the registration.
            auto& endpoint = g_clients[index];
            if (message.Words[2U] != 0U)
            {
                // Exhausted restarts discard the abandoned bounded queue so console focus cannot
                // wait on a dead reader.
                Scratch().Input().Logic.DetachClient(static_cast<Foundation::UInt32>(index + 1U));
                g_resumed[index] = false;
                if (g_focusRequest == index + 1U)
                {
                    g_focusRequest = 0U;
                }
                return Answer(client, message, epoch, InputAnswer::Done);
            }
            answer = Scratch().Input().Logic.RebindClient(static_cast<Foundation::UInt32>(index + 1U));
            g_baselines[index][0U] = Scratch().Input().Logic.Delivered(static_cast<Foundation::UInt32>(index + 1U));
            g_baselines[index][1U] = Scratch().Input().Route.Sequence();
            g_baselines[index][2U] = Scratch().Input().Logic.Buttons();
            g_resumed[index] = false;
            if (g_focusRequest == index + 1U)
            {
                g_focusRequest = 0U;
            }
            return Answer(client, message, epoch, answer, static_cast<Foundation::UInt32>(index + 1U),
                          InputProtocolVersion, &endpoint);
        }
        break;
    case InputResumeMethod:
        if (isClient && Scratch().Input().Logic.BrokerPointer(static_cast<Foundation::UInt32>(destination)) &&
            !g_resumed[destination - 1U])
        {
            g_resumed[destination - 1U] = true;
            const auto* bases = g_baselines[destination - 1U];
            const Foundation::UInt64 words[4U]{epoch, bases[0U], bases[1U], bases[2U]};
            auto& pending = g_answers[g_route];
            pending.Reply = message.Reply;
            pending.Bytes =
                WriteInputFrame(pending.Frame, InputBaselineMethod, message.Transaction, words, nullptr, 0U);
            return true;
        }
        break;
    case InputSetPointerRouteMethod:
        if (main && manage)
        {
            if ((g_pointerSink.Selector != 0U) && !Closed(client, g_pointerSink))
            {
                return false;
            }
            g_pointerSink = message.PointerSink;
            Scratch().Input().Route.Configure(message.Words[1U], message.Words[2U]);
            Scratch().Input().Logic.SetRouteSequence(message.Words[2U]);
            answer = InputAnswer::Done;
        }
        else if ((message.PointerSink.Selector != 0U) && !Closed(client, message.PointerSink))
        {
            return false;
        }
        break;
    case InputRegisterSourceMethod:
    case InputRegisterClientMethod:
        if (main && ((message.Method == InputRegisterClientMethod) || manage))
        {
            return Register(client, message, block);
        }
        break;
    case InputPublishMethod:
        if (isSource)
        {
            Registration& registration = g_sources[source - 1U];
            if (message.Transaction <= registration.LastPublish)
            {
                bool same =
                    (message.Transaction == registration.LastPublish) && (message.Count == registration.LastCount);
                for (Foundation::Size index{0U}; index < message.Count * 24U; ++index)
                {
                    same = same && (registration.LastEvents[index] == message.Events[index]);
                }
                answer = same ? InputAnswer::Done : InputAnswer::Refused;
                break;
            }
            for (Foundation::Size index{0U}; index < message.Count; ++index)
            {
                g_events[index] = ReadInputEvent(message.Events + index * 24U);
            }
            Foundation::Size pointerCount{0U};
            for (Foundation::Size index{0U}; index < message.Count; ++index)
            {
                if ((g_events[index].Kind != Input::EventKind::KeyDown) &&
                    (g_events[index].Kind != Input::EventKind::KeyUp))
                {
                    ++pointerCount;
                }
            }
            const bool trusted = Scratch().Input().Logic.FocusLocked();
            if (!trusted && !Scratch().Input().Route.CanQueue(pointerCount))
            {
                answer = InputAnswer::Exhausted;
                break;
            }
            answer = Scratch().Input().Logic.Publish(static_cast<Foundation::UInt32>(source), g_events, message.Count,
                                                     Scratch().Input().Route.Sequence(),
                                                     !trusted && Scratch().Input().Route.Enabled());
            if (answer == InputAnswer::Done)
            {
                for (Foundation::Size index{0U}; !trusted && index < message.Count; ++index)
                {
                    Scratch().Input().Route.Queue(g_events[index], static_cast<Foundation::UInt32>(source),
                                                  Scratch().Input().Logic.PointerStamp(index));
                }
                registration.LastPublish = message.Transaction;
                registration.LastCount = message.Count;
                for (Foundation::Size index{0U}; index < message.Count * 24U; ++index)
                {
                    registration.LastEvents[index] = message.Events[index];
                }
            }
        }
        break;
    case InputCreditMethod:
    case InputAttentionCreditMethod:
        if (((message.Method == InputCreditMethod) && isClient) ||
            ((message.Method == InputAttentionCreditMethod) && main && manage))
        {
            Credit& credit = message.Method == InputCreditMethod ? g_credits[destination - 1U] : g_manager;
            if (credit.Reply.Selector == 0U)
            {
                // The large answer storage stays static (512-byte stack).
                credit.Reply = message.Reply;
                credit.Transaction = message.Transaction;
                credit.Count = static_cast<Foundation::Size>(message.Words[1U]);
                credit.Frozen = 0U;
                credit.Bytes = 0U;
                return true; // Retained Invoke reply Endpoint is the only event channel.
            }
            answer = InputAnswer::Busy;
        }
        break;
    case InputRequestFocusMethod:
        if (isClient)
        {
            if ((g_focusRequest == 0U) || (g_focusRequest == destination))
            {
                g_focusRequest = static_cast<Foundation::UInt32>(destination);
                answer = InputAnswer::Done;
            }
            else
            {
                answer = InputAnswer::Busy;
            }
        }
        break;
    case InputSetFocusMethod:
        if (main && manage)
        {
            answer = Scratch().Input().Logic.SetFocus(static_cast<Foundation::UInt32>(message.Words[1U]),
                                                      message.Words[2U] != 0U);
        }
        break;
    case InputQueryMethod:
        if (main || isClient || isSource)
        {
            answer = InputAnswer::Done;
            id = Scratch().Input().Logic.Focus();
            count = InputProtocolVersion;
        }
        break;
    default:
        break;
    }
    return Answer(client, message, epoch, answer, id, count);
}
} // Namespace

Foundation::UInt64 ServeInput(const System::SystemCallClient& client,
                              const Foundation::UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    Scratch().InitializeInput();
    const System::CapabilityHandle waitSet{block[Abi::ComponentWaitSetWord], block[Abi::ComponentWaitSetWord + 1U]};
    const System::CapabilityHandle service{block[Abi::ComponentReceiveWord], block[Abi::ComponentReceiveWord + 1U]};
    for (;;)
    {
        const Foundation::UInt64 timeout = RetryNeeded() ? 10'000U : 0U;
        const Abi::SystemCallResponse waited = client.Wait(waitSet, timeout);
        if ((waited.Error != Abi::SystemCallError::None) &&
            !((timeout != 0U) && (waited.Error == Abi::SystemCallError::TimedOut)))
        {
            return Abi::ComponentInputFailedExitCode | (1ULL << 32U) |
                   (static_cast<Foundation::UInt64>(waited.Error) << 40U);
        }
        for (Foundation::Size slot{0U}; slot < 1U + Sources + Clients; ++slot)
        {
            g_route = slot;
            if (slot != 0U && g_answers[slot].Reply.Selector != 0U)
            {
                continue;
            }
            const System::CapabilityHandle endpoint =
                slot == 0U ? service
                           : (slot <= Sources ? g_sources[slot - 1U].Endpoint : g_clients[slot - 1U - Sources]);
            if (endpoint.Selector == 0U)
            {
                continue;
            }
            // Desktop session: even a continuously publishing source cannot starve the seat credit
            // or the manager's secure attention.
            for (Foundation::Size round{0U}; round < 4U; ++round)
            {
                const Abi::SystemCallResponse received =
                    client.Receive(endpoint, System::AddressOf(g_delivery), sizeof(g_delivery));
                if (received.Error == Abi::SystemCallError::WouldBlock)
                {
                    break;
                }
                if (received.Error != Abi::SystemCallError::None)
                {
                    return Abi::ComponentInputFailedExitCode | (2ULL << 32U) |
                           (static_cast<Foundation::UInt64>(received.Error) << 40U);
                }
                g_deliveryBytes = static_cast<Foundation::Size>(received.Value1);
                InputMessage message{};
                const bool decoded =
                    ReadInputFrame(g_delivery, static_cast<Foundation::Size>(received.Value1), message);
                const Foundation::UInt64 selector = ReadEchoSealedSelector(g_delivery);
                const Foundation::UInt64 expected =
                    slot == 0U ? InputSealedSelector
                               : (slot <= Sources ? InputSourceSelector + slot : InputClientSelector + slot - Sources);
                const bool manage = ((g_delivery[14U] | (static_cast<Foundation::UInt64>(g_delivery[15U]) << 8U)) &
                                     EchoManageRight) != 0U;
                bool available{true};
                if (slot == 0U)
                {
                    g_route = manage ? 0U : ReplyRoutes + Clients;
                    if (g_answers[g_route].Reply.Selector != 0U)
                    {
                        const auto first = ReplyRoutes + (manage ? 0U : Clients);
                        const auto end = first + Clients;
                        g_route = first;
                        while (g_route < end && g_answers[g_route].Reply.Selector != 0U)
                        {
                            ++g_route;
                        }
                        available = g_route < end;
                    }
                    // Bounded overflow is rejected before any side effect;
                    // it cannot consume the manager's reserved reply slots.
                    if (!available)
                    {
                        ++g_refusedRequests;
                    }
                }
                const bool processed = decoded && (selector == expected) && available
                                           ? Process(client, message, block, selector, manage)
                                           : CloseInputAttachments(client);
                if (!processed || !DeliverCredits(client, block[Abi::ComponentIncarnationWord]))
                {
                    return Abi::ComponentInputFailedExitCode | (3ULL << 32U);
                }
                if (slot != 0U && g_answers[slot].Reply.Selector != 0U)
                {
                    break;
                }
            }
        }
        if (!DeliverCredits(client, block[Abi::ComponentIncarnationWord]))
        {
            return Abi::ComponentInputFailedExitCode | (3ULL << 32U);
        }
    }
}

Foundation::UInt64 ServeInputClient(const System::SystemCallClient& client,
                                    const Foundation::UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const Foundation::UInt64 epoch = block[Abi::ComponentInputEpochWord];
    const System::CapabilityHandle service{block[Abi::ComponentReceiveWord], block[Abi::ComponentReceiveWord + 1U]};
    const System::CapabilityHandle reply{block[Abi::ComponentReplyWord], block[Abi::ComponentReplyWord + 1U]};
    const System::CapabilityHandle broker{block[Abi::ComponentInputServiceWord],
                                          block[Abi::ComponentInputServiceWord + 1U]};
    const System::CapabilityHandle waitSet{block[Abi::ComponentWaitSetWord], block[Abi::ComponentWaitSetWord + 1U]};
    const Foundation::UInt64 words[4U]{epoch, 0U, 0U, 0U};
    Foundation::Size size = WriteInputFrame(g_frame, InputRegisterClientMethod, 1U, words, nullptr, 0U, &reply);
    if (client.Send(broker, System::AddressOf(g_frame), size).Error != Abi::SystemCallError::None)
    {
        return Abi::ComponentInputFailedExitCode | (2ULL << 32U);
    }
    System::CapabilityHandle control{};
    Foundation::UInt32 id{0U};
    Foundation::UInt64 events{0U};
    for (;;)
    {
        if (client.Wait(waitSet, 0U).Error != Abi::SystemCallError::None)
        {
            return Abi::ComponentInputFailedExitCode | (3ULL << 32U);
        }
        for (;;)
        {
            const Abi::SystemCallResponse received =
                client.Receive(service, System::AddressOf(g_delivery), sizeof(g_delivery));
            if (received.Error == Abi::SystemCallError::WouldBlock)
            {
                break;
            }
            if (received.Error != Abi::SystemCallError::None)
            {
                return Abi::ComponentInputFailedExitCode | (4ULL << 32U);
            }
            g_deliveryBytes = static_cast<Foundation::Size>(received.Value1);
            InputMessage message{};
            if (!ReadInputFrame(g_delivery, static_cast<Foundation::Size>(received.Value1), message))
            {
                if (!CloseInputAttachments(client))
                {
                    return Abi::ComponentInputFailedExitCode | (5ULL << 32U);
                }
                continue;
            }
            if (message.Words[0U] != epoch)
            {
                const bool closed = message.Method == InputQueryMethod
                                        ? Answer(client, message, epoch, InputAnswer::Restarted)
                                        : CloseInputAttachments(client);
                if (!closed)
                {
                    return Abi::ComponentInputFailedExitCode | (6ULL << 32U);
                }
                continue;
            }
            if ((message.Method == InputReplyMethod) && (message.Transaction == 1U) &&
                (message.Words[1U] == static_cast<Foundation::UInt64>(InputAnswer::Done)) &&
                (message.Reply.Selector != 0U) && (control.Selector == 0U))
            {
                control = message.Reply;
                id = static_cast<Foundation::UInt32>(message.Words[2U]);
            }
            else if ((message.Method == InputEventsMethod) && (message.Words[1U] == id))
            {
                events += message.Count;
            }
            else if (message.Method == InputQueryMethod)
            {
                if (!Answer(client, message, epoch, InputAnswer::Done, id, events))
                {
                    return Abi::ComponentInputFailedExitCode | (7ULL << 32U);
                }
                continue;
            }
            else
            {
                if (!CloseInputAttachments(client))
                {
                    return Abi::ComponentInputFailedExitCode | (8ULL << 32U);
                }
                continue;
            }
            const Foundation::UInt64 credit[4U]{epoch, InputMaximumEvents, 0U, 0U};
            size = WriteInputFrame(g_frame, InputCreditMethod, 2U + events, credit, nullptr, 0U, &reply);
            if (client.Send(control, System::AddressOf(g_frame), size).Error != Abi::SystemCallError::None)
            {
                return Abi::ComponentInputFailedExitCode | (9ULL << 32U);
            }
        }
    }
}
} // Namespace Asteria::User::Component
