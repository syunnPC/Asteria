// SPDX-License-Identifier: Apache-2.0
#include "User/Component/InputBroker.hpp"

#include <cstring>
#include <cstdio>
#include "Support/TestContext.hpp"
#include "User/Component/InputFrame.hpp"

// After the sole consumer stops, its client Query fences queued requests. Manage RebindClient
// retires its old credit and Resume returns cumulative bases without losing pending keys or a
// registration.
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
               const F::UInt16 usage = 0U, const F::UInt64 mode = 0U)
    {
        Delivery& next = Deliveries[Count];
        next.Endpoint = endpoint;
        next.Wait = wait;
        S::StoreWord(next.Bytes + 24U, 10U);
        S::StoreWord(next.Bytes + 32U, selector);
        next.Bytes[14U] = 2U;
        next.Bytes[15U] = manage ? 8U : 0U;
        const F::UInt64 words[4U]{9U, argument, mode, 0U};
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
            response.Error = (Waits == 15U) ? A::SystemCallError::InvalidHandle : A::SystemCallError::None;
            return;
        case A::SystemCallNumber::ObjectCreate:
            response.Value0 = 20U + ++Created;
            response.Value1 = 999U;
            return;
        case A::SystemCallNumber::ObjectDestroy:
            Destroyed = request.Arguments[0U];
            for (auto& delivery : Deliveries)
            {
                if (delivery.Endpoint == Destroyed)
                {
                    delivery.Taken = true;
                }
            }
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
            if (message.Method == 16U)
            {
                BaselineKeys = message.Words[1U];
                BaselinePointer = message.Words[2U];
                ++Baselines;
                return;
            }
            if (message.Words[1U] == static_cast<F::UInt64>(C::InputAnswer::Busy))
            {
                ++BusyReplies;
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
    Delivery Deliveries[32U]{};
    F::UInt64 Answers[32U]{};
    F::UInt64 Ids[32U]{};
    F::UInt64 Endpoints[32U]{};
    In::InputEvent Events[12U]{};
    F::Size EventCount{0U};
    F::UInt64 Destroyed{};
    F::UInt64 BaselineKeys{};
    F::UInt64 BaselinePointer{};
    F::UInt64 Baselines{};
    F::UInt64 BusyReplies{};
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
    TestContext context{"InputBrokerRecoveryTests"};
    static RegistrationKernel kernel{};
    kernel.Queue(1U, 1U, C::InputSealedSelector, true, C::InputRegisterSourceMethod);
    kernel.Queue(1U, 1U, C::InputSealedSelector, false, C::InputRegisterClientMethod, C::InputBrokerPointer);
    kernel.Queue(1U, 1U, C::InputSealedSelector, true, C::InputSetFocusMethod, 1U);
    kernel.Queue(21U, 2U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 1U, 4U);
    kernel.Queue(22U, 2U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U, 10U);
    // Old compositor has consumed a make and holds its next empty credit.
    kernel.Queue(22U, 3U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U, 11U);
    // A queued old credit is processed before the owner's Query fence.
    kernel.Queue(22U, 4U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U, 11U);
    kernel.Queue(22U, 4U, C::InputClientSelector + 1U, false, C::InputQueryMethod, 0U, 19U);
    const auto refused = kernel.Count;
    kernel.Queue(1U, 5U, C::InputSealedSelector, false, 14U, 1U, 20U);
    const auto rebound = kernel.Count;
    kernel.Queue(1U, 5U, C::InputSealedSelector, true, 14U, 1U, 21U);
    kernel.Queue(22U, 6U, C::InputClientSelector + 1U, false, 15U, 0U, 30U);
    const auto credit = kernel.Count;
    kernel.Queue(22U, 6U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U, 11U);
    kernel.Queue(21U, 7U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 2U, 5U);
    kernel.Queue(21U, 8U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 3U, 6U);
    const auto again = kernel.Count;
    kernel.Queue(1U, 9U, C::InputSealedSelector, true, 14U, 1U, 22U);
    kernel.Queue(22U, 10U, C::InputClientSelector + 1U, false, 15U, 0U, 31U);
    kernel.Queue(22U, 11U, C::InputClientSelector + 1U, false, C::InputCreditMethod, 8U, 11U);
    const auto retired = kernel.Count;
    kernel.Queue(1U, 12U, C::InputSealedSelector, true, 14U, 1U, 23U, 0U, 1U);
    kernel.Queue(1U, 13U, C::InputSealedSelector, false, C::InputRegisterClientMethod);
    kernel.Queue(1U, 13U, C::InputSealedSelector, true, C::InputSetFocusMethod, 2U);
    kernel.Queue(21U, 14U, C::InputSourceSelector + 1U, false, C::InputPublishMethod, 1U, 4U, 7U);
    kernel.Queue(23U, 14U, C::InputClientSelector + 2U, false, C::InputCreditMethod, 8U, 12U);
    const S::SystemCallClient client{{RegistrationKernel::Transport, &kernel}};
    F::UInt64 block[A::ComponentBlockWords]{};
    block[A::ComponentIncarnationWord] = 9U;
    block[A::ComponentReceiveWord] = 1U;
    block[A::ComponentWaitSetWord] = 2U;
    block[A::ComponentAccountWord] = 42U;
    const auto stopped = C::ServeInput(client, block);
    if (kernel.Baselines != 2U)
    {
        std::fprintf(stderr, "waits=%zu calls=%zu busy=%llu credit=%llu exit=%llu\n", kernel.Waits, kernel.WaitCalls,
                     static_cast<unsigned long long>(kernel.BusyReplies),
                     static_cast<unsigned long long>(kernel.Answers[credit]), static_cast<unsigned long long>(stopped));
    }
    ASTERIA_EXPECT(context, "rebind is Manage-only and keeps the fenced registration", kernel.Answers[refused] == 2U,
                   kernel.Answers[rebound] == 1U, kernel.Endpoints[rebound] == 22U, kernel.Answers[again] == 1U,
                   kernel.Endpoints[again] == 22U, kernel.Created == 3U);
    ASTERIA_EXPECT(context, "replacement receives cumulative baselines and accepts credit without Busy",
                   kernel.Baselines == 2U, kernel.BaselineKeys == 2U, kernel.BaselinePointer == 0U,
                   kernel.Answers[credit] == 0U, kernel.EventCount == 4U, kernel.Events[1U].Usage == 5U,
                   kernel.Events[2U].Usage == 6U, kernel.Unexpected == 0U, kernel.Waits == 15U);
    ASTERIA_EXPECT(context, "replacement credit is not blocked by the old outstanding credit",
                   kernel.BusyReplies == 1U);
    ASTERIA_EXPECT(context, "spent restarts retire the registration and console input resumes",
                   kernel.Answers[retired] == 1U, kernel.Endpoints[retired] == 0U, kernel.Events[3U].Usage == 7U);
    return context.Finish();
}
