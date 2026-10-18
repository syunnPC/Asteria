// SPDX-License-Identifier: Apache-2.0
#include "User/Component/Ps2Driver.hpp"

#include <cstring>
#include <vector>

#include "Abi/I8042.hpp"
#include "Support/TestContext.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Input/Ps2ControllerModel.hpp"

namespace
{
namespace C = Asteria::User::Component;
namespace A = Asteria::Abi;
namespace F = Asteria::Foundation;
namespace S = Asteria::User::System;
namespace T = Asteria::Test::Input;

// The keyboard line as an I/O APIC pin sees it: asserted while a byte waits
// in the controller (a read that leaves another byte keeps it asserted, the
// harsher case). A delivery masks the pin until the driver re-arms it. An
// edge-triggered pin delivers only on assertion while unmasked and drops an
// assertion that happens while masked; a level-triggered pin delivers
// whenever it is unmasked and asserted.
class LineKernel final
{
public:
    T::Model Controller{};
    bool Edge{true};
    std::vector<F::UInt8> Script{};
    F::Size Next{0U};
    F::Size Stalls{0U};
    F::Size Events{0U};
    F::Size Arms{0U};
    F::Size Unexpected{0U};
    bool Ready{false};

    static void Transport(void* context, const A::SystemCallRequest* request, A::SystemCallResponse* response) noexcept
    {
        static_cast<LineKernel*>(context)->Call(*request, *response);
    }

private:
    struct Reply
    {
        F::UInt8 Bytes[C::EchoDeliveryHeaderBytes + C::InputMaximumFrameBytes]{};
        F::Size Size{0U};
    };
    bool m_masked{true};
    bool m_pending{false};
    bool m_asserted{false};
    std::vector<Reply> m_replies{};

    void Update() noexcept
    {
        const bool asserted = !Controller.Output.empty();
        if (!m_masked && asserted && (!Edge || !m_asserted))
        {
            m_pending = true;
            m_masked = true;
        }
        m_asserted = asserted;
    }
    void Inject() noexcept
    {
        if (Next < Script.size())
        {
            Controller.Output.push_back(T::Byte{0U, Script[Next++]});
        }
        Update();
    }
    void Publish(const F::UInt8* const frame, const F::Size size) noexcept
    {
        Reply reply{};
        std::memcpy(reply.Bytes + C::EchoDeliveryHeaderBytes, frame, size);
        C::InputMessage message{};
        if (!C::ReadInputFrame(reply.Bytes, size + C::EchoDeliveryHeaderBytes, message) ||
            (message.Method != C::InputPublishMethod))
        {
            ++Unexpected;
            return;
        }
        Events += message.Count;
        const F::UInt64 words[4U]{message.Words[0U], static_cast<F::UInt64>(C::InputAnswer::Done), 0U, 0U};
        Reply answer{};
        answer.Size = C::EchoDeliveryHeaderBytes + C::WriteInputFrame(answer.Bytes + C::EchoDeliveryHeaderBytes,
                                                                      C::InputReplyMethod, message.Transaction, words,
                                                                      nullptr, 0U);
        m_replies.push_back(answer);
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
            const bool good = (request.Arguments[4U] == 0U)
                                  ? Controller.Read(status, value)
                                  : Controller.Write(status, static_cast<F::UInt8>(request.Arguments[5U]));
            response.Value0 = value;
            response.Error = good ? A::SystemCallError::None : A::SystemCallError::InvalidConfiguration;
            Update();
            return;
        }
        case A::SystemCallNumber::ObjectCreate:
            response.Value0 = 50U + Arms;
            response.Value1 = 1U;
            ++Arms;
            return;
        case A::SystemCallNumber::InterruptControl:
            if (request.Arguments[0U] == 50U)
            {
                // Every other re-arm a key byte arrives while the pin is
                // still masked: its edge is lost.
                if ((++Arms % 2U) == 0U)
                {
                    Inject();
                }
                m_masked = false;
                Update();
            }
            return;
        case A::SystemCallNumber::WaitSetBind:
        case A::SystemCallNumber::CapabilityClose:
            return;
        case A::SystemCallNumber::IpcReceive:
            if (m_replies.empty())
            {
                response.Error = A::SystemCallError::WouldBlock;
                return;
            }
            std::memcpy(reinterpret_cast<void*>(static_cast<F::UIntPointer>(request.Arguments[2U])),
                        m_replies.front().Bytes, m_replies.front().Size);
            response.Value1 = m_replies.front().Size;
            m_replies.erase(m_replies.begin());
            return;
        case A::SystemCallNumber::IpcSend:
        {
            const F::Size size = static_cast<F::Size>(request.Arguments[3U]);
            const auto* frame = reinterpret_cast<const F::UInt8*>(static_cast<F::UIntPointer>(request.Arguments[2U]));
            if (size == C::EchoHeaderBytes)
            {
                Ready = C::Detail::InputLoad32(frame + 32U) == C::Ps2ReadyMethod;
                return;
            }
            Publish(frame, size);
            return;
        }
        case A::SystemCallNumber::WaitSetWait:
            if (!m_pending && (Next < Script.size()))
            {
                Inject();
            }
            if (m_pending)
            {
                m_pending = false;
                response.Value0 = 1U;
                return;
            }
            if (m_asserted && (request.Arguments[2U] == 0U))
            {
                // A byte waits, the pin cannot deliver it and the driver
                // waits without a deadline: input is stuck.
                ++Stalls;
                response.Error = A::SystemCallError::TimedOut;
                return;
            }
            if ((Next < Script.size()) || !m_replies.empty())
            {
                response.Error = A::SystemCallError::TimedOut;
                return;
            }
            response.Error = A::SystemCallError::InvalidHandle;
            return;
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
    Asteria::TestSupport::TestContext context{"PS/2 driver line triggers"};
    static LineKernel kernels[2U]{};
    for (F::Size index{0U}; index < 2U; ++index)
    {
        LineKernel& kernel = kernels[index];
        const bool edge = index == 0U;
        kernel.Edge = edge;
        // Twelve keys made and broken (set 2: make, F0, make).
        constexpr F::UInt8 keys[]{0x1CU, 0x32U, 0x21U, 0x23U, 0x24U, 0x2BU, 0x34U, 0x33U, 0x43U, 0x3BU, 0x42U, 0x4BU};
        for (const F::UInt8 key : keys)
        {
            kernel.Script.insert(kernel.Script.end(), {key, 0xF0U, key});
        }
        F::UInt64 block[A::ComponentBlockWords]{};
        for (F::Size word : {2U, 6U, 8U, 12U, 24U, 30U, 32U, 34U, 38U, 40U})
        {
            block[word] = word;
            block[word + 1U] = 1U;
        }
        block[30U] = 30U;
        block[32U] = 31U;
        block[A::ComponentPs2DeclaredWord] = A::I8042KeyboardAml;
        block[A::ComponentInputEpochWord] = 9U;
        const S::SystemCallClient client{S::SystemCallTransport{.Function = LineKernel::Transport, .Context = &kernel}};
        const F::UInt64 code = C::ServePs2(client, block);
        ASTERIA_EXPECT(context, edge ? "edge pin: re-arm then drain loses no byte" : "level pin: every byte delivered",
                       kernel.Ready, kernel.Unexpected == 0U, kernel.Stalls == 0U, kernel.Next == kernel.Script.size(),
                       kernel.Controller.Output.empty(), kernel.Events == 2U * (sizeof(keys) / sizeof(keys[0U])),
                       (code & 0xFFFF'FFFFU) == A::ComponentInputFailedExitCode);
    }
    return context.Finish();
}
