// SPDX-License-Identifier: Apache-2.0
#include "User/Component/Ps2Driver.hpp"

#include <cstring>
#include <cstdio>
#include <vector>

#include "Support/TestContext.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Input/Ps2ControllerModel.hpp"

namespace
{
namespace C = Asteria::User::Component;
namespace A = Asteria::Abi;
namespace F = Asteria::Foundation;
namespace S = Asteria::User::System;
struct Delivery
{
    F::UInt8 Bytes[C::EchoDeliveryHeaderBytes + C::InputMaximumFrameBytes]{};
    F::Size Size{0U};
};
class Kernel final
{
public:
    Asteria::Test::Input::Model Controller{};
    std::vector<Delivery> Replies{};
    std::vector<F::UInt64> Transactions{};
    std::vector<int> Motion{};
    F::Size Created{0U};
    F::Size Bound{0U};
    F::Size Arms{0U};
    F::Size Waits{0U};
    F::Size Publishes{0U};
    F::Size Registrations{0U};
    F::Size Closed{0U};
    F::Size Unexpected{0U};
    bool Ready{false};
    F::Size ReadyAttempts{0U};
    F::UInt8 ReadyBytes[C::EchoHeaderBytes]{};
    bool KeySent{false};
    bool AfterArmDrained{true};
    F::UInt64 FirstTransaction{0U};
    std::vector<F::UInt8> FirstBody{};

    void AnswerFrame(const C::InputMessage& message, C::InputAnswer answer, F::UInt64 epoch = 9U, bool source = false)
    {
        Delivery delivery{};
        const F::UInt64 words[4U]{epoch, static_cast<F::UInt64>(answer), source ? 2U : 0U,
                                  source ? C::InputProtocolVersion : 0U};
        const S::CapabilityHandle endpoint{99U, 1U};
        delivery.Size = C::EchoDeliveryHeaderBytes + C::WriteInputFrame(delivery.Bytes + C::EchoDeliveryHeaderBytes,
                                                                        C::InputReplyMethod, message.Transaction, words,
                                                                        nullptr, 0U, source ? &endpoint : nullptr);
        Replies.push_back(delivery);
    }
    static void Transport(void* context, const A::SystemCallRequest* request, A::SystemCallResponse* response) noexcept
    {
        static_cast<Kernel*>(context)->Call(*request, *response);
    }
    void Call(const A::SystemCallRequest& request, A::SystemCallResponse& response) noexcept
    {
        response = A::SystemCallResponse{A::SystemCallError::None, 0U, 0U};
        switch (static_cast<A::SystemCallNumber>(request.Number))
        {
        case A::SystemCallNumber::PortIo:
        {
            const bool status = request.Arguments[0U] == 31U;
            F::UInt8 value{0U};
            bool success{false};
            if (request.Arguments[4U] == 0U)
            {
                success = Controller.Read(status, value);
                response.Value0 = value;
            }
            else
            {
                success = Controller.Write(status, static_cast<F::UInt8>(request.Arguments[5U]));
            }
            response.Error = success ? A::SystemCallError::None : A::SystemCallError::InvalidConfiguration;
            return;
        }
        case A::SystemCallNumber::ObjectCreate:
        {
            const auto* bytes = reinterpret_cast<const F::UInt8*>(static_cast<F::UIntPointer>(request.Arguments[3U]));
            if ((request.Arguments[0U] != 42U) || (request.Arguments[2U] != A::ObjectTypeInterrupt) ||
                (S::LoadWord(bytes + A::ObjectCreateInterruptSourceOffset) != A::InterruptSourceLine) ||
                (S::LoadWord(bytes) != 38U + Created))
            {
                ++Unexpected;
            }
            response.Value0 = 50U + Created++;
            response.Value1 = 1U;
            return;
        }
        case A::SystemCallNumber::WaitSetBind:
            ++Bound;
            return;
        case A::SystemCallNumber::InterruptControl:
            AfterArmDrained = AfterArmDrained && Controller.Output.empty();
            ++Arms;
            if (Arms == 2U)
            {
                // Edge-wired case: bytes appear on unmask, before Wait.
                for (unsigned index{0U}; index < 10U; ++index)
                {
                    Controller.Output.push_back(Asteria::Test::Input::Byte{32U, 8U});
                    Controller.Output.push_back(Asteria::Test::Input::Byte{32U, 5U});
                    Controller.Output.push_back(Asteria::Test::Input::Byte{32U, 3U});
                    Controller.Output.push_back(Asteria::Test::Input::Byte{32U, 0U});
                }
            }
            return;
        case A::SystemCallNumber::CapabilityClose:
            ++Closed;
            return;
        case A::SystemCallNumber::IpcReceive:
            if (Replies.empty())
            {
                response.Error = A::SystemCallError::WouldBlock;
                return;
            }
            std::memcpy(reinterpret_cast<void*>(static_cast<F::UIntPointer>(request.Arguments[2U])),
                        Replies.front().Bytes, Replies.front().Size);
            response.Value1 = Replies.front().Size;
            Replies.erase(Replies.begin());
            return;
        case A::SystemCallNumber::WaitSetWait:
            ++Waits;
            if (Waits > 12U)
            {
                response.Error = A::SystemCallError::InvalidHandle;
                return;
            }
            if ((Publishes >= 5U) && !KeySent && (Registrations == 0U) && Controller.Output.empty())
            {
                KeySent = true;
                Controller.Output.push_back(Asteria::Test::Input::Byte{0U, 0x1CU});
                response.Value0 = 1U;
            }
            else if (Registrations == 1U && Publishes == 6U)
            {
                // The held a is still a repeated make across epoch renewal.
                Controller.Output.push_back(Asteria::Test::Input::Byte{0U, 0x1CU});
                Controller.Output.push_back(Asteria::Test::Input::Byte{0U, 0x32U});
                response.Value0 = 1U;
            }
            else
            {
                response.Value0 = C::EchoWaitTag;
            }
            return;
        case A::SystemCallNumber::IpcSend:
        {
            const F::Size size = static_cast<F::Size>(request.Arguments[3U]);
            const auto* frame = reinterpret_cast<const F::UInt8*>(static_cast<F::UIntPointer>(request.Arguments[2U]));
            if (size == C::EchoHeaderBytes)
            {
                if (++ReadyAttempts == 1U)
                {
                    std::memcpy(ReadyBytes, frame, size);
                }
                else if (std::memcmp(ReadyBytes, frame, size) != 0)
                {
                    ++Unexpected;
                }
                if (ReadyAttempts <= 2U)
                {
                    response.Error = A::SystemCallError::WouldBlock;
                    return;
                }
                Ready = C::Detail::InputLoad32(frame + 32U) == C::Ps2ReadyMethod;
                return;
            }
            Delivery delivery{};
            std::memcpy(delivery.Bytes + C::EchoDeliveryHeaderBytes, frame, size);
            C::InputMessage message{};
            if (!C::ReadInputFrame(delivery.Bytes, size + C::EchoDeliveryHeaderBytes, message))
            {
                ++Unexpected;
                return;
            }
            if (message.Method == C::InputRegisterSourceMethod)
            {
                ++Registrations;
                if ((request.Arguments[0U] != 2U) || (message.Words[0U] != 10U))
                {
                    ++Unexpected;
                }
                AnswerFrame(message, C::InputAnswer::Done, 10U, true);
                return;
            }
            ++Publishes;
            Transactions.push_back(message.Transaction);
            if (Publishes == 1U)
            {
                FirstTransaction = message.Transaction;
                FirstBody.assign(frame, frame + size);
                AnswerFrame(message, C::InputAnswer::Busy);
            }
            else if (Publishes <= 4U)
            {
                if ((message.Transaction != FirstTransaction) ||
                    (FirstBody != std::vector<F::UInt8>(frame, frame + size)))
                {
                    ++Unexpected;
                }
                // Lost Done retries the exact motion once; broker dedupes it.
                if (Publishes == 2U)
                {
                    AnswerFrame(message, C::InputAnswer::Exhausted);
                }
                if (Publishes == 4U)
                {
                    AnswerFrame(message, C::InputAnswer::Done);
                }
            }
            else if (Publishes == 5U)
            {
                if (message.Count != 2U)
                {
                    ++Unexpected;
                }
                AnswerFrame(message, C::InputAnswer::Done);
            }
            else if (Publishes == 6U)
            {
                AnswerFrame(message, C::InputAnswer::Restarted, 10U);
            }
            else
            {
                if ((request.Arguments[0U] != 99U) || (message.Words[0U] != 10U) || (message.Count != 1U) ||
                    (C::ReadInputEvent(message.Events).Usage != 5U))
                {
                    ++Unexpected;
                }
                AnswerFrame(message, C::InputAnswer::Done, 10U);
            }
            return;
        }
        default:
            ++Unexpected;
            response.Error = A::SystemCallError::UnknownSystemCall;
            return;
        }
    }
};
} // namespace
int main()
{
    Asteria::TestSupport::TestContext context{"Ps2Driver"};
    static Kernel kernel{};
    F::UInt64 block[A::ComponentBlockWords]{};
    for (F::Size word : {2U, 6U, 8U, 12U, 24U, 30U, 32U, 34U, 38U, 40U})
    {
        block[word] = word;
        block[word + 1U] = 1U;
    }
    block[24U] = 42U;
    block[30U] = 30U;
    block[32U] = 31U;
    block[40U] = 39U;
    block[A::ComponentPs2DeclaredWord] = 1U;
    block[A::ComponentInputEpochWord] = 9U;
    const S::SystemCallClient client{S::SystemCallTransport{.Function = Kernel::Transport, .Context = &kernel}};
    const F::UInt64 code = C::ServePs2(client, block);
    const auto failure = Asteria::User::Input::DecodePs2Diagnostic(static_cast<F::UInt32>(code >> 32U));
    ASTERIA_EXPECT(context, "driver terminates on refused Wait, naming the service step",
                   (code & 0xFFFF'FFFFU) == A::ComponentInputFailedExitCode,
                   failure.Step == Asteria::User::Input::Ps2Step::Serve,
                   failure.Reason == Asteria::User::Input::Ps2Reason::Transport);
    ASTERIA_EXPECT(context, "two lines created and bound, ready after controller drain", kernel.Created == 2U,
                   kernel.Bound == 2U, kernel.Ready, kernel.ReadyAttempts == 3U, kernel.AfterArmDrained);
    ASTERIA_EXPECT(context, "batch retries, lost ACK deduplication, epoch source handoff", kernel.Unexpected == 0U,
                   kernel.Publishes >= 7U, kernel.Registrations == 1U,
                   kernel.Transactions[4U] > kernel.Transactions[0U],
                   kernel.Transactions[6U] >= kernel.Transactions[5U]);
    if (context.GetFailureCount() != 0U)
    {
        std::fprintf(stderr,
                     "unexpected=%zu publishes=%zu registrations=%zu waits=%zu transactions:", kernel.Unexpected,
                     kernel.Publishes, kernel.Registrations, kernel.Waits);
        for (auto transaction : kernel.Transactions)
        {
            std::fprintf(stderr, " %llu", static_cast<unsigned long long>(transaction));
        }
        std::fprintf(stderr, "\n");
    }
    return context.Finish();
}
