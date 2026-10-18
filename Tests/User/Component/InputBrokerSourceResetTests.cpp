// SPDX-License-Identifier: Apache-2.0
#include "User/Component/InputBroker.hpp"

#include <cstring>
#include "Support/TestContext.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Component/DisplayFrame.hpp"

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
        if (usage == 1U)
        {
            event = {};
            event.Kind = In::EventKind::Motion;
            event.Dx = 1;
        }
        if (usage == 2U)
        {
            event = {};
            event.Kind = In::EventKind::Buttons;
            event.Buttons = 2U;
        }
        if (method == C::InputSetPointerRouteMethod)
        {
            const S::CapabilityHandle sink{500U, 999U};
            next.Size = C::EchoDeliveryHeaderBytes + C::WriteInputPointerRoute(next.Bytes + C::EchoDeliveryHeaderBytes,
                                                                               transaction, words, reply, &sink);
            ++Count;
            return;
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
            if (++WaitCalls > 128U)
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
            response.Error = (Waits == 8U) ? A::SystemCallError::InvalidHandle : A::SystemCallError::None;
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
            if (request.Arguments[0U] == 500U)
            {
                if (Waits < 4U)
                {
                    response.Error = A::SystemCallError::WouldBlock;
                    return;
                }
                C::DisplayMessage pointer{};
                if (!C::ReadDisplayFrame(delivery, C::EchoDeliveryHeaderBytes + size, pointer))
                {
                    ++Unexpected;
                    return;
                }
                ++Pointers;
                LastButtons = pointer.Words[4U];
                LastStamp = pointer.Words[7U];
                return;
            }
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
    Delivery Deliveries[128U]{};
    F::UInt64 Answers[128U]{};
    F::UInt64 Ids[128U]{};
    F::UInt64 Endpoints[128U]{};
    In::InputEvent Events[12U]{};
    F::Size EventCount{0U};
    F::Size Pointers{};
    F::UInt64 LastButtons{};
    F::UInt64 LastStamp{};
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
    TestContext context{"InputBrokerSourceResetTests"};
    static RegistrationKernel kernel{};
    kernel.Queue(1U, 1U, C::InputSealedSelector, true, C::InputRegisterSourceMethod);
    kernel.Queue(1U, 1U, C::InputSealedSelector, true, C::InputRegisterSourceMethod);
    kernel.Queue(1U, 1U, C::InputSealedSelector, false, C::InputRegisterClientMethod, C::InputBrokerPointer);
    kernel.Queue(1U, 1U, C::InputSealedSelector, true, C::InputSetFocusMethod, 1U);
    kernel.Queue(1U, 1U, C::InputSealedSelector, true, C::InputSetPointerRouteMethod, 256U);
    kernel.Queue(21U, 2U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 1U, 4U);
    kernel.Queue(21U, 2U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 2U);
    for (F::Size i{0U}; i < 62U; ++i)
    {
        kernel.Queue(21U, 2U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 3U + i, 1U);
    }
    kernel.Queue(22U, 2U, C::InputSourceSelector + 2U, false, C::InputPublishMethod, 1U, 1U, 2U);
    const auto full = kernel.Count;
    kernel.Queue(1U, 3U, C::InputSealedSelector, true, C::InputRegisterSourceMethod, 1U);
    // Wait 4 drains the full sink. Retrying reset must emit the release once,
    // with source 2's button still held, after source 1's synthesized key-up.
    kernel.Queue(1U, 4U, C::InputSealedSelector, true, C::InputQueryMethod);
    const auto retry = kernel.Count;
    kernel.Queue(1U, 5U, C::InputSealedSelector, true, C::InputRegisterSourceMethod, 1U);
    kernel.Queue(23U, 6U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U);
    kernel.Queue(23U, 7U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U);
    const S::SystemCallClient client{{RegistrationKernel::Transport, &kernel}};
    F::UInt64 block[A::ComponentBlockWords]{};
    block[A::ComponentIncarnationWord] = 9U;
    block[A::ComponentReceiveWord] = 1U;
    block[A::ComponentWaitSetWord] = 2U;
    block[A::ComponentAccountWord] = 42U;
    static_cast<void>(C::ServeInput(client, block));
    ASTERIA_EXPECT(context, "a saturated route rejects reset atomically and retry releases exactly once",
                   kernel.Answers[full] == static_cast<F::UInt64>(C::InputAnswer::Exhausted),
                   kernel.Answers[retry] == static_cast<F::UInt64>(C::InputAnswer::Done), kernel.Pointers == 65U,
                   kernel.LastButtons == 2U, kernel.LastStamp == 2U);
    ASTERIA_EXPECT(context, "the rejected reset retains its held key for the retry", kernel.EventCount == 2U,
                   kernel.Events[0U].Kind == In::EventKind::KeyDown, kernel.Events[1U].Kind == In::EventKind::KeyUp,
                   kernel.Events[1U].Usage == 4U, kernel.Unexpected == 0U);
    return context.Finish();
}
