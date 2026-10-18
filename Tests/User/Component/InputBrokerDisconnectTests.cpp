// SPDX-License-Identifier: Apache-2.0
#include "User/Component/InputBroker.hpp"

#include <cstring>
#include "Support/TestContext.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Component/InputFrame.hpp"

namespace
{
namespace C = Asteria::User::Component;
namespace A = Asteria::Abi;
namespace F = Asteria::Foundation;
namespace S = Asteria::User::System;
namespace In = Asteria::User::Input;
class Kernel final
{
public:
    static void Transport(void* opaque, const A::SystemCallRequest* request, A::SystemCallResponse* response) noexcept
    {
        static_cast<Kernel*>(opaque)->Call(*request, *response);
    }
    void Call(const A::SystemCallRequest& request, A::SystemCallResponse& response) noexcept
    {
        response = {.Error = A::SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        switch (static_cast<A::SystemCallNumber>(request.Number))
        {
        case A::SystemCallNumber::WaitSetWait:
            if (++Waits == 80U)
            {
                response.Error = A::SystemCallError::InvalidHandle;
            }
            return;
        case A::SystemCallNumber::ObjectCreate:
            response.Value0 = 20U + ++Created;
            response.Value1 = 99U;
            return;
        case A::SystemCallNumber::WaitSetBind:
        case A::SystemCallNumber::CapabilityClose:
            return;
        case A::SystemCallNumber::IpcReceive:
            Receive(request, response);
            return;
        case A::SystemCallNumber::IpcSend:
        {
            const auto* bytes = reinterpret_cast<const F::UInt8*>(static_cast<F::UIntPointer>(request.Arguments[2U]));
            const auto count = static_cast<F::Size>(request.Arguments[3U]);
            std::memcpy(Frame + C::EchoDeliveryHeaderBytes, bytes, count);
            if (request.Arguments[0U] == 555U)
            {
                C::DisplayMessage pointer{};
                if (!C::ReadDisplayFrame(Frame, C::EchoDeliveryHeaderBytes + count, pointer))
                {
                    ++Unexpected;
                }
                else
                {
                    ++Pointers;
                }
                return;
            }
            C::InputMessage message{};
            if (!C::ReadInputFrame(Frame, C::EchoDeliveryHeaderBytes + count, message))
            {
                ++Unexpected;
                return;
            }
            if (message.Method == C::InputEventsMethod)
            {
                ++Revoked;
                response.Error = A::SystemCallError::Revoked;
            }
            else if (message.Transaction >= 100U && message.Words[1U] == static_cast<F::UInt64>(C::InputAnswer::Done))
            {
                ++Accepted;
            }
            return;
        }
        default:
            ++Unexpected;
            response.Error = A::SystemCallError::UnknownSystemCall;
            return;
        }
    }
    void Receive(const A::SystemCallRequest& request, A::SystemCallResponse& response) noexcept
    {
        F::UInt64 words[4U]{9U, 0U, 0U, 0U};
        S::CapabilityHandle reply{900U, 99U};
        const S::CapabilityHandle sink{555U, 99U};
        F::UInt32 method{0U};
        F::UInt64 transaction{0U};
        F::UInt64 selector{0U};
        F::Size events{0U};
        bool manage{false};
        if (request.Arguments[0U] == 1U && Enrollment < 4U)
        {
            selector = C::InputSealedSelector;
            transaction = ++Enrollment;
            if (Enrollment == 1U)
            {
                method = C::InputRegisterSourceMethod;
                manage = true;
            }
            if (Enrollment == 2U)
            {
                method = C::InputRegisterClientMethod;
                words[1U] = C::InputBrokerPointer;
            }
            if (Enrollment == 3U)
            {
                method = C::InputSetFocusMethod;
                words[1U] = 1U;
                manage = true;
            }
            if (Enrollment == 4U)
            {
                method = C::InputSetPointerRouteMethod;
                words[1U] = 256U;
                manage = true;
            }
        }
        else if (Enrollment == 4U && request.Arguments[0U] == 22U && !Credit)
        {
            selector = C::InputClientSelector + 1U;
            transaction = 50U;
            method = C::InputCreditMethod;
            words[1U] = 8U;
            Credit = true;
        }
        else if (Credit && request.Arguments[0U] == 21U && Published < 80U)
        {
            selector = C::InputSourceSelector + 1U;
            transaction = 100U + Published++;
            method = C::InputPublishMethod;
            words[1U] = 3U;
            events = 3U;
        }
        else
        {
            response.Error = A::SystemCallError::WouldBlock;
            return;
        }
        std::memset(Frame, 0, sizeof(Frame));
        S::StoreWord(Frame + 24U, 10U);
        S::StoreWord(Frame + 32U, selector);
        Frame[14U] = 2U;
        Frame[15U] = manage ? 8U : 0U;
        const In::InputEvent batch[]{{.Kind = In::EventKind::KeyDown,
                                      .Modifiers = 0U,
                                      .Usage = 4U,
                                      .Source = 0U,
                                      .Dx = 0,
                                      .Dy = 0,
                                      .Buttons = 0U,
                                      .Reserved = {},
                                      .Wheel = 0},
                                     {.Kind = In::EventKind::KeyUp,
                                      .Modifiers = 0U,
                                      .Usage = 4U,
                                      .Source = 0U,
                                      .Dx = 0,
                                      .Dy = 0,
                                      .Buttons = 0U,
                                      .Reserved = {},
                                      .Wheel = 0},
                                     {.Kind = In::EventKind::Motion,
                                      .Modifiers = 0U,
                                      .Usage = 0U,
                                      .Source = 0U,
                                      .Dx = 1,
                                      .Dy = 1,
                                      .Buttons = 0U,
                                      .Reserved = {},
                                      .Wheel = 0}};
        const auto size =
            method == C::InputSetPointerRouteMethod
                ? C::WriteInputPointerRoute(Frame + C::EchoDeliveryHeaderBytes, transaction, words, reply, &sink)
                : C::WriteInputFrame(Frame + C::EchoDeliveryHeaderBytes, method, transaction, words,
                                     events != 0U ? batch : nullptr, events, &reply);
        std::memcpy(reinterpret_cast<void*>(static_cast<F::UIntPointer>(request.Arguments[2U])), Frame,
                    size + C::EchoDeliveryHeaderBytes);
        response.Value1 = size + C::EchoDeliveryHeaderBytes;
    }
    F::Size Waits{};
    F::Size Created{};
    F::Size Enrollment{};
    F::Size Published{};
    F::Size Accepted{};
    F::Size Pointers{};
    F::Size Revoked{};
    F::Size Unexpected{};
    bool Credit{false};
    F::UInt8 Frame[C::EchoDeliveryHeaderBytes + C::InputMaximumFrameBytes]{};
};
} // namespace
int main()
{
    Asteria::TestSupport::TestContext context{"InputBrokerDisconnectTests"};
    static Kernel kernel{};
    const S::SystemCallClient client{S::SystemCallTransport{.Function = Kernel::Transport, .Context = &kernel}};
    F::UInt64 block[A::ComponentBlockWords]{};
    block[A::ComponentIncarnationWord] = 9U;
    block[A::ComponentAccountWord] = 42U;
    block[A::ComponentWaitSetWord] = 2U;
    block[A::ComponentReceiveWord] = 1U;
    static_cast<void>(C::ServeInput(client, block));
    ASTERIA_EXPECT(context, "dead key consumer cannot block mixed physical input", kernel.Unexpected == 0U,
                   kernel.Revoked == 1U, kernel.Published == 80U, kernel.Accepted == 80U, kernel.Pointers == 80U);
    return context.Finish();
}
