// SPDX-License-Identifier: Apache-2.0
#include "User/Component/Ps2Driver.hpp"

#include <algorithm>

#include "Abi/I8042.hpp"
#include "Support/TestContext.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Input/Ps2ControllerModel.hpp"

namespace A = Asteria::Abi;
namespace F = Asteria::Foundation;
namespace C = Asteria::User::Component;
namespace S = Asteria::User::System;
namespace
{
struct Kernel
{
    Asteria::Test::Input::Model Controller{};
    F::UInt32 Ready{0U};
    F::Size Lines{0U};
    static void Call(void* context, const A::SystemCallRequest* request, A::SystemCallResponse* response) noexcept
    {
        auto& self = *static_cast<Kernel*>(context);
        *response = {A::SystemCallError::None, 0U, 1U};
        switch (static_cast<A::SystemCallNumber>(request->Number))
        {
        case A::SystemCallNumber::PortIo:
        {
            const bool command = request->Arguments[0U] == 64U;
            F::UInt8 byte{0U};
            const bool good = request->Arguments[4U] == 0U
                                  ? self.Controller.Read(command, byte)
                                  : self.Controller.Write(command, static_cast<F::UInt8>(request->Arguments[5U]));
            response->Value0 = byte;
            response->Error = good ? A::SystemCallError::None : A::SystemCallError::InvalidConfiguration;
            break;
        }
        case A::SystemCallNumber::ObjectCreate:
            response->Value0 = 100U + self.Lines++;
            break;
        case A::SystemCallNumber::WaitSetBind:
        case A::SystemCallNumber::InterruptControl:
            break;
        case A::SystemCallNumber::IpcSend:
        {
            const auto* const frame = reinterpret_cast<const F::UInt8*>(request->Arguments[2U]);
            self.Ready = C::Detail::InputLoad32(frame + 32U);
            break;
        }
        case A::SystemCallNumber::WaitSetWait:
            response->Error = A::SystemCallError::InvalidHandle;
            break;
        default:
            response->Error = A::SystemCallError::UnknownSystemCall;
            break;
        }
    }
};
} // namespace
int main()
{
    Asteria::TestSupport::TestContext context{"Optional PS/2 auxiliary driver"};
    for (const F::UInt64 declared : {A::I8042KeyboardAml, A::I8042FadtFlag, A::I8042MouseAml})
    {
        Kernel kernel{};
        kernel.Controller.AuxAbsent = true;
        F::UInt64 block[A::ComponentBlockWords]{};
        block[A::ComponentPs2DataWord] = 60U;
        block[A::ComponentPs2CommandWord] = 64U;
        block[A::ComponentPs2DeclaredWord] = declared;
        const S::SystemCallClient client{S::SystemCallTransport{Kernel::Call, &kernel}};
        const F::UInt64 code = C::ServePs2(client, block);
        ASTERIA_EXPECT(context, "optional driver reaches readiness with both handles",
                       (code & 0xFFFF'FFFFU) == A::ComponentInputFailedExitCode,
                       Asteria::User::Input::DecodePs2Diagnostic(static_cast<F::UInt32>(code >> 32U)).Step ==
                           Asteria::User::Input::Ps2Step::Serve,
                       kernel.Lines == 2U,
                       kernel.Ready ==
                           (declared == A::I8042KeyboardAml ? C::Ps2ReadyMethod : C::Ps2ReadyAuxFailedMethod));
        const bool enabled = std::find(kernel.Controller.Writes.begin(), kernel.Controller.Writes.end(), 0x1A8U) !=
                             kernel.Controller.Writes.end();
        ASTERIA_EXPECT(context, "only mouse declarations authorize auxiliary commands",
                       enabled == (declared != A::I8042KeyboardAml));
    }
    return context.Finish();
}
