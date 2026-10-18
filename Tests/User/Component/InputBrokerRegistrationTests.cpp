// SPDX-License-Identifier: Apache-2.0
#include "User/Component/InputBroker.hpp"

#include <cstring>
#include "Support/TestContext.hpp"
#include "User/Component/InputFrame.hpp"

// A source registered again for a restarted driver (RegisterSource naming its id;
// once the
// root fenced the source (a Query on it, answered after whatever the dead instance queued),
// registration again releases the dead instance's held keys and clears the replay fence, so the new
// instance's first transaction is accepted although it is below the dead instance's last. Three
// sources fit, a fourth is Exhausted.
namespace
{
namespace C = Asteria::User::Component;
namespace A = Asteria::Abi;
namespace F = Asteria::Foundation;
namespace S = Asteria::User::System;
namespace In = Asteria::User::Input;
using Asteria::TestSupport::TestContext;

struct Delivery
{
    F::UInt64 Endpoint;
    F::Size Wait;
    F::Size Size;
    F::UInt8 Bytes[C::EchoDeliveryHeaderBytes + C::EchoMaximumFrameBytes];
    bool Taken;
};

class RegistrationKernel final
{
public:
    // A request on endpoint, received from the wait'th wait on; answered on
    // reply selector 100 + its index.
    void Queue(const F::UInt64 endpoint, const F::Size wait, const F::UInt64 selector, const bool manage,
               const F::UInt32 method, const F::UInt64 argument = 0U, const F::UInt64 transaction = 1U,
               const F::UInt16 usage = 0U)
    {
        Delivery& next = Deliveries[Count];
        next.Endpoint = endpoint;
        next.Wait = wait;
        S::StoreWord(next.Bytes + 24U, 10U);
        S::StoreWord(next.Bytes + 32U, selector);
        next.Bytes[14U] = 2U;
        next.Bytes[15U] = manage ? 8U : 0U;
        const F::UInt64 words[4U]{9U, argument, 0U, 0U};
        const S::CapabilityHandle reply{100U + Count, 777U};
        const In::InputEvent key{.Kind = In::EventKind::KeyDown,
                                 .Modifiers = 0U,
                                 .Usage = usage,
                                 .Source = 0U,
                                 .Dx = 0,
                                 .Dy = 0,
                                 .Buttons = 0U,
                                 .Reserved = {},
                                 .Wheel = 0};
        const bool publish = method == C::InputPublishMethod;
        auto event = key;
        if (usage == 0U)
        {
            event.Kind = In::EventKind::Buttons;
            event.Buttons = 1U;
        }
        next.Size = C::EchoDeliveryHeaderBytes + C::WriteInputFrame(next.Bytes + C::EchoDeliveryHeaderBytes, method,
                                                                    transaction, words, publish ? &event : nullptr,
                                                                    publish ? 1U : 0U, &reply);
        ++Count;
    }
    static void Transport(void* context, const A::SystemCallRequest* request, A::SystemCallResponse* response) noexcept
    {
        static_cast<RegistrationKernel*>(context)->Answer(*request, *response);
    }
    void Answer(const A::SystemCallRequest& request, A::SystemCallResponse& response) noexcept
    {
        response = A::SystemCallResponse{.Error = A::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        switch (static_cast<A::SystemCallNumber>(request.Number))
        {
        case A::SystemCallNumber::WaitSetWait:
            // A bounded receive pass leaves the WaitSet signalled. Six
            // sources require more than one pass before the next time step.
            if (++WaitCalls > 32U)
            {
                response.Error = A::SystemCallError::InvalidHandle;
                return;
            }
            for (F::Size i{0U}; i < Count; ++i)
            {
                if (!Deliveries[i].Taken && Deliveries[i].Wait <= Waits)
                {
                    return;
                }
            }
            ++Waits;
            response.Error = (Waits == 5U) ? A::SystemCallError::InvalidHandle : A::SystemCallError::None;
            return;
        case A::SystemCallNumber::ObjectCreate:
            response.Value0 = 20U + ++Created;
            response.Value1 = 999U;
            return;
        case A::SystemCallNumber::WaitSetBind:
            return;
        case A::SystemCallNumber::CapabilityClose:
            ++Closed;
            return;
        case A::SystemCallNumber::IpcReceive:
            if (request.Arguments[0U] != 1U)
            {
                for (F::Size i{0U}; i < Count; ++i)
                {
                    if (Deliveries[i].Endpoint == 1U && !Deliveries[i].Taken && Deliveries[i].Wait <= Waits)
                    {
                        response.Error = A::SystemCallError::WouldBlock;
                        return;
                    }
                }
            }
            for (F::Size index{0U}; index < Count; ++index)
            {
                Delivery& next = Deliveries[index];
                if (!next.Taken && (next.Endpoint == request.Arguments[0U]) && (next.Wait <= Waits))
                {
                    std::memcpy(reinterpret_cast<void*>(static_cast<F::UIntPointer>(request.Arguments[2U])), next.Bytes,
                                next.Size);
                    next.Taken = true;
                    response.Value1 = next.Size;
                    return;
                }
            }
            response.Error = A::SystemCallError::WouldBlock;
            return;
        case A::SystemCallNumber::IpcSend:
        {
            F::UInt8 delivery[C::EchoDeliveryHeaderBytes + C::InputMaximumFrameBytes]{};
            const F::Size size = static_cast<F::Size>(request.Arguments[3U]);
            std::memcpy(delivery + C::EchoDeliveryHeaderBytes,
                        reinterpret_cast<const void*>(static_cast<F::UIntPointer>(request.Arguments[2U])), size);
            C::InputMessage message{};
            if (!C::ReadInputFrame(delivery, C::EchoDeliveryHeaderBytes + size, message))
            {
                ++Unexpected;
                return;
            }
            if (message.Method == C::InputEventsMethod)
            {
                for (F::Size index{0U}; index < message.Count; ++index)
                {
                    Events[EventCount++] = C::ReadInputEvent(message.Events + index * 24U);
                }
                return;
            }
            Answers[request.Arguments[0U] - 100U] = message.Words[1U];
            Ids[request.Arguments[0U] - 100U] = message.Words[2U];
            Endpoints[request.Arguments[0U] - 100U] = message.Reply.Selector;
            return;
        }
        default:
            ++Unexpected;
            response.Error = A::SystemCallError::UnknownSystemCall;
            return;
        }
    }
    Delivery Deliveries[24U]{};
    F::UInt64 Answers[24U]{};
    F::UInt64 Ids[24U]{};
    F::UInt64 Endpoints[24U]{};
    In::InputEvent Events[12U]{};
    F::Size EventCount{0U};
    F::Size Count{0U};
    F::Size Created{0U};
    F::Size Closed{0U};
    F::Size Waits{0U};
    F::Size WaitCalls{0U};
    F::Size Unexpected{0U};
};
} // Namespace

int main()
{
    TestContext context{"InputBrokerRegistrationTests"};
    static RegistrationKernel kernel{};
    // Wait 1: all bounded sources register, the next is Exhausted, client 1
    // on the following Endpoint focused; the first driver's key goes down at transaction 5.
    for (F::Size source{0U}; source < C::InputMaximumSources + 1U; ++source)
    {
        kernel.Queue(1U, 1U, C::InputSealedSelector, true, C::InputRegisterSourceMethod);
    }
    kernel.Queue(1U, 1U, C::InputSealedSelector, false, C::InputRegisterClientMethod);
    kernel.Queue(1U, 1U, C::InputSealedSelector, true, C::InputSetFocusMethod, 1U);
    kernel.Queue(21U, 1U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 5U, 4U);
    // Wait 2: the driver died with a Publish still queued; the root's fence
    // (a Query on the source) is answered after it.
    const F::Size stale = kernel.Count;
    kernel.Queue(21U, 2U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 6U, 5U);
    kernel.Queue(21U, 2U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 7U);
    const F::Size fence = kernel.Count;
    kernel.Queue(21U, 2U, C::InputSourceSelector + 1U, false, C::InputQueryMethod);
    // Wait 3: registering the source again without Manage is Refused, with
    // Manage Done with the same id and Endpoint.
    const F::Size invoked = kernel.Count;
    kernel.Queue(1U, 3U, C::InputSealedSelector, false, C::InputRegisterSourceMethod, 1U);
    const F::Size again = kernel.Count;
    kernel.Queue(1U, 3U, C::InputSealedSelector, true, C::InputRegisterSourceMethod, 1U);
    // Wait 4: the restarted driver starts its transactions afresh; the
    // client's credit collects what reached it.
    const F::Size restarted = kernel.Count;
    kernel.Queue(21U, 4U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 1U, 4U);
    kernel.Queue(21U + C::InputMaximumSources, 4U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U);
    const S::SystemCallClient client{
        S::SystemCallTransport{.Function = RegistrationKernel::Transport, .Context = &kernel}};
    F::UInt64 block[A::ComponentBlockWords]{};
    block[A::ComponentIncarnationWord] = 9U;
    block[A::ComponentReceiveWord] = 1U;
    block[A::ComponentWaitSetWord] = 2U;
    block[A::ComponentAccountWord] = 42U;
    ASTERIA_EXPECT(context, "the broker ran until its wait failed",
                   C::ServeInput(client, block) == (A::ComponentInputFailedExitCode | (1ULL << 32U) | (4ULL << 40U)),
                   kernel.Unexpected == 0U, kernel.Created == C::InputMaximumSources + 1U);
    ASTERIA_EXPECT(context, "all sources register and the next is Exhausted", kernel.Answers[0U] == 1U,
                   kernel.Ids[0U] == 1U, kernel.Answers[1U] == 1U, kernel.Ids[1U] == 2U, kernel.Answers[2U] == 1U,
                   kernel.Ids[2U] == 3U, kernel.Endpoints[2U] == 23U, kernel.Answers[C::InputMaximumSources] == 3U);
    ASTERIA_EXPECT(context, "registering a source again needs Manage and returns its own id and Endpoint",
                   kernel.Answers[invoked] == 2U, kernel.Answers[again] == 1U, kernel.Ids[again] == 1U,
                   kernel.Endpoints[again] == 21U);
    ASTERIA_EXPECT(context, "the dead instance's queued Publish is served before the fence's answer",
                   kernel.Answers[stale] == 1U, kernel.Answers[fence] == 1U);
    ASTERIA_EXPECT(context, "the restarted driver's first transaction is accepted", kernel.Answers[restarted] == 1U);
    ASTERIA_EXPECT(context, "both held keys were released and the new make delivered", kernel.EventCount == 7U,
                   kernel.Events[0U].Kind == In::EventKind::KeyDown, kernel.Events[1U].Kind == In::EventKind::KeyDown,
                   kernel.Events[1U].Usage == 5U, kernel.Events[3U].Kind == In::EventKind::KeyUp,
                   kernel.Events[3U].Usage == 4U, kernel.Events[4U].Kind == In::EventKind::KeyUp,
                   kernel.Events[4U].Usage == 5U, kernel.Events[4U].Source == 1U, kernel.Events[4U].Modifiers == 0U,
                   kernel.Events[6U].Kind == In::EventKind::KeyDown, kernel.Events[6U].Usage == 4U,
                   kernel.Events[6U].Source == 1U);
    ASTERIA_EXPECT(context, "source reset balances pointer buttons too",
                   kernel.Events[2U].Kind == In::EventKind::Buttons, kernel.Events[2U].Buttons == 1U,
                   kernel.Events[5U].Kind == In::EventKind::Buttons, kernel.Events[5U].Buttons == 0U,
                   kernel.Events[5U].Source == 1U);
    return context.Finish();
}
