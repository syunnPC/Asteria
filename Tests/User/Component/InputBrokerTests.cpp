// SPDX-License-Identifier: Apache-2.0
#include "User/Component/InputBroker.hpp"

#include <cstring>
#include "Support/TestContext.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Component/DisplayFrame.hpp"

namespace
{
namespace C = Asteria::User::Component;
namespace A = Asteria::Abi;
namespace F = Asteria::Foundation;
namespace S = Asteria::User::System;
using Asteria::TestSupport::TestContext;
struct Delivery
{
    F::UInt64 Endpoint;
    F::Size Size;
    F::UInt8 Bytes[C::EchoDeliveryHeaderBytes + C::EchoMaximumFrameBytes];
    bool Taken;
};
class BrokerKernel final
{
public:
    void Queue(F::UInt64 endpoint, F::UInt64 selector, bool manage, F::UInt32 method, F::UInt64 argument = 0U,
               F::UInt64 epoch = 9U, bool malformed = false)
    {
        Delivery& next = Deliveries[Count++];
        next.Endpoint = endpoint;
        S::StoreWord(next.Bytes + 24U, 10U);
        S::StoreWord(next.Bytes + 32U, selector);
        next.Bytes[14U] = 2U;
        next.Bytes[15U] = manage ? 8U : 0U;
        const F::UInt64 words[4U]{epoch, argument, 0U, 0U};
        const S::CapabilityHandle reply{100U + Count, 777U};
        const Asteria::User::Input::InputEvent motion{.Kind = Asteria::User::Input::EventKind::Motion,
                                                      .Modifiers = 0U,
                                                      .Usage = 0U,
                                                      .Source = 0xDEADU,
                                                      .Dx = 5,
                                                      .Dy = -3,
                                                      .Buttons = 0U,
                                                      .Reserved = {},
                                                      .Wheel = 0};
        next.Size =
            C::EchoDeliveryHeaderBytes + C::WriteInputFrame(next.Bytes + C::EchoDeliveryHeaderBytes, method, Count,
                                                            words, method == C::InputPublishMethod ? &motion : nullptr,
                                                            method == C::InputPublishMethod ? 1U : 0U, &reply);
        if (malformed)
        {
            next.Bytes[next.Size - 1U] = 1U;
        }
    }
    void QueueRoute(bool manage, F::UInt64 epoch = 9U)
    {
        Queue(1U, C::InputSealedSelector, manage, C::InputSetPointerRouteMethod);
        Delivery& next = Deliveries[Count - 1U];
        const F::UInt64 words[4U]{epoch, 256U, 0U, 0U};
        const S::CapabilityHandle reply{100U + Count, 777U};
        const S::CapabilityHandle sink{555U, 888U};
        next.Size = C::EchoDeliveryHeaderBytes +
                    C::WriteInputPointerRoute(next.Bytes + C::EchoDeliveryHeaderBytes, Count, words, reply, &sink);
    }
    static void Transport(void* context, const A::SystemCallRequest* request, A::SystemCallResponse* response) noexcept
    {
        static_cast<BrokerKernel*>(context)->Answer(*request, *response);
    }
    void Answer(const A::SystemCallRequest& request, A::SystemCallResponse& response) noexcept
    {
        response = A::SystemCallResponse{.Error = A::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        switch (static_cast<A::SystemCallNumber>(request.Number))
        {
        case A::SystemCallNumber::WaitSetWait:
            ++Waits;
            response.Error = (Waits == 14U)
                                 ? A::SystemCallError::InvalidHandle
                                 : (request.Arguments[2U] != 0U && Waits % 2U == 0U ? A::SystemCallError::TimedOut
                                                                                    : A::SystemCallError::None);
            return;
        case A::SystemCallNumber::ObjectCreate:
            if ((request.Arguments[0U] != 42U) || (request.Arguments[2U] != A::ObjectTypeEndpoint))
            {
                ++Unexpected;
            }
            response.Value0 = 20U + ++Created;
            response.Value1 = 999U;
            return;
        case A::SystemCallNumber::WaitSetBind:
            ++Bindings;
            return;
        case A::SystemCallNumber::CapabilityClose:
            ++Closed;
            return;
        case A::SystemCallNumber::IpcReceive:
            // Enrollment/route requests finish before simulated driver traffic;
            // the production broker deliberately drains only four per wake.
            if (request.Arguments[0U] != 1U)
            {
                for (F::Size i{0U}; i < Count; ++i)
                {
                    if (Deliveries[i].Endpoint == 1U && !Deliveries[i].Taken)
                    {
                        response.Error = A::SystemCallError::WouldBlock;
                        return;
                    }
                }
            }
            for (F::Size index{0U}; index < Count; ++index)
            {
                Delivery& next = Deliveries[index];
                if (!next.Taken && (next.Endpoint == request.Arguments[0U]))
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
            if (request.Arguments[0U] == 555U)
            {
                C::DisplayMessage pointer{};
                if (!C::ReadDisplayFrame(delivery, C::EchoDeliveryHeaderBytes + size, pointer) ||
                    (pointer.Method != C::DisplayPointerMethod) || (pointer.Words[0U] != 256U) ||
                    (pointer.Words[5U] != 1U))
                {
                    ++Unexpected;
                }
                ++PointerAttempts;
                if (PointerAttempts <= 2U)
                {
                    response.Error = A::SystemCallError::WouldBlock;
                    return;
                }
                ++PointerEvents;
                return;
            }
            if (request.Arguments[0U] == 102U)
            {
                ++ReplyAttempts;
                if (ReplyAttempts == 1U)
                {
                    FrozenReplyBytes = size;
                    std::memcpy(FrozenReply, delivery, sizeof(FrozenReply));
                }
                else
                {
                    ReplyImmutable = ReplyImmutable && size == FrozenReplyBytes &&
                                     std::memcmp(FrozenReply, delivery, sizeof(FrozenReply)) == 0;
                }
                if (ReplyAttempts <= 2U)
                {
                    response.Error = A::SystemCallError::WouldBlock;
                    return;
                }
            }
            C::InputMessage message{};
            if (!C::ReadInputFrame(delivery, C::EchoDeliveryHeaderBytes + size, message))
            {
                ++Unexpected;
                return;
            }
            if (message.Method == C::InputEventsMethod)
            {
                if (++EventAttempts <= 4U)
                {
                    response.Error = A::SystemCallError::WouldBlock;
                    return;
                }
                ++Events;
                Stamped = C::ReadInputEvent(message.Events).Source;
            }
            else
            {
                Answers[request.Arguments[0U] - 100U] = message.Words[1U];
            }
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
    F::Size Count{0U};
    F::Size Created{0U};
    F::Size Bindings{0U};
    F::Size Closed{0U};
    F::Size Waits{0U};
    F::Size Unexpected{0U};
    F::Size Events{0U};
    F::Size PointerEvents{0U};
    F::Size PointerAttempts{0U};
    F::Size EventAttempts{0U};
    F::UInt32 Stamped{0U};
    F::Size ReplyAttempts{};
    F::Size FrozenReplyBytes{};
    bool ReplyImmutable{true};
    F::UInt8 FrozenReply[C::EchoDeliveryHeaderBytes + C::InputMaximumFrameBytes]{};
};
} // namespace
int main()
{
    TestContext context{"InputBrokerTests"};
    static BrokerKernel kernel{};
    // Source enrollment is Manage-only; client enrollment is Invoke-only.
    kernel.Queue(1U, C::InputSealedSelector, false, C::InputRegisterSourceMethod);
    kernel.Queue(1U, C::InputSealedSelector, true, C::InputRegisterSourceMethod);
    kernel.Queue(1U, C::InputSealedSelector, false, C::InputRegisterClientMethod);
    kernel.Queue(1U, C::InputSealedSelector, false, C::InputSetFocusMethod, 1U);
    kernel.Queue(1U, C::InputSealedSelector, true, C::InputSetFocusMethod, 1U);
    kernel.Queue(1U, C::InputSealedSelector, true, C::InputQueryMethod, 0U, 8U);
    kernel.Queue(1U, C::InputSealedSelector, true, C::InputQueryMethod, 0U, 9U, true);
    kernel.Queue(21U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U);
    kernel.Queue(22U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U);
    // A source capability cannot request focus, even with Manage metadata.
    kernel.Queue(21U, C::InputSourceSelector + 1U, true, C::InputSetFocusMethod, 0U);
    kernel.Queue(21U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U);
    S::StoreWord(kernel.Deliveries[10U].Bytes + C::EchoDeliveryHeaderBytes + 40U, 8U);
    kernel.Queue(21U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U);
    S::StoreWord(kernel.Deliveries[11U].Bytes + C::EchoDeliveryHeaderBytes + 40U, 8U);
    kernel.Deliveries[11U].Bytes[C::EchoDeliveryHeaderBytes + 120U + 32U + 8U] = 6U;
    kernel.Queue(21U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U);
    S::StoreWord(kernel.Deliveries[12U].Bytes + C::EchoDeliveryHeaderBytes + 40U, 7U);
    kernel.Queue(22U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U);
    kernel.Queue(22U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U);
    kernel.Queue(1U, C::InputSealedSelector, false, C::InputAttentionCreditMethod);
    kernel.Queue(1U, C::InputSealedSelector, false, C::InputQueryMethod);
    Delivery& foreign = kernel.Deliveries[16U];
    F::UInt8* const foreignFrame = foreign.Bytes + C::EchoDeliveryHeaderBytes;
    std::memmove(foreignFrame + C::EchoHeaderBytes + 8U, foreignFrame + C::EchoHeaderBytes,
                 C::EchoAttachmentBytes + C::InputPayloadBytes);
    std::memset(foreignFrame + C::EchoHeaderBytes, 0, 8U);
    foreignFrame[80U] = 1U;
    foreign.Size += 8U;
    C::Detail::InputStore32(foreignFrame + 8U, static_cast<F::UInt32>(foreign.Size - C::EchoDeliveryHeaderBytes));
    kernel.QueueRoute(false);
    const F::Size refusedRoute = kernel.Count;
    kernel.QueueRoute(true, 8U);
    const F::Size staleRoute = kernel.Count;
    kernel.QueueRoute(true);
    const F::Size acceptedRoute = kernel.Count;
    const S::SystemCallClient client{S::SystemCallTransport{.Function = BrokerKernel::Transport, .Context = &kernel}};
    F::UInt64 block[A::ComponentBlockWords]{};
    block[A::ComponentIncarnationWord] = 9U;
    block[A::ComponentReceiveWord] = 1U;
    block[A::ComponentWaitSetWord] = 2U;
    block[A::ComponentAccountWord] = 42U;
    ASTERIA_EXPECT(context, "broker owns Endpoints, binds them and never accesses hardware",
                   C::ServeInput(client, block) == (A::ComponentInputFailedExitCode | (1ULL << 32U) | (4ULL << 40U)),
                   kernel.Created == 2U, kernel.Bindings == 2U, kernel.Unexpected == 0U);
    ASTERIA_EXPECT(context, "Manage-only methods, epoch restart and malformed-frame closure", kernel.Answers[1U] == 2U,
                   kernel.Answers[2U] == 1U, kernel.Answers[3U] == 1U, kernel.Answers[4U] == 2U,
                   kernel.Answers[5U] == 1U, kernel.Answers[6U] == 5U, kernel.Answers[7U] == 0U,
                   kernel.Answers[10U] == 2U, kernel.Closed == 22U);
    ASTERIA_EXPECT(context, "publish replay does not duplicate motion and different/older payloads are refused",
                   kernel.Answers[11U] == 1U, kernel.Answers[12U] == 2U, kernel.Answers[13U] == 2U);
    ASTERIA_EXPECT(context, "duplicate client credit is Busy and attention channel needs Manage",
                   kernel.Answers[14U] == 4U, kernel.Answers[15U] == 4U, kernel.Answers[16U] == 2U);
    ASTERIA_EXPECT(context, "one credit delivers one source-stamped batch", kernel.Events == 1U, kernel.Stamped == 1U,
                   kernel.Waits == 14U, kernel.EventAttempts == 5U);
    ASTERIA_EXPECT(context, "Manage sets pointer route; Invoke refused; motion is source-stamped on both routes",
                   kernel.Answers[refusedRoute] == 2U, kernel.Answers[acceptedRoute] == 1U, kernel.PointerEvents == 1U,
                   kernel.PointerAttempts == 3U, kernel.Answers[staleRoute] == 5U);
    ASTERIA_EXPECT(context, "registration WouldBlock retains the complete reply and still makes bounded progress",
                   kernel.ReplyAttempts == 3U, kernel.ReplyImmutable);
    return context.Finish();
}
