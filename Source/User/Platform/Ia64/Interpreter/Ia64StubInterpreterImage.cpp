// SPDX-License-Identifier: Apache-2.0

#include "User/Interpreter/StubInterpreter.hpp"
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"

namespace
{
// The stub's message and the guest's description live in.bss (the image
// has no initialized data).
constinit Asteria::User::Interpreter::StubBuffers g_buffers{};
} // Namespace

// The root service passes the start block's address in r4
// (Ia64StubInterpreterImage.S); the extension record follows the 16 words
// in the same page. ThreadExit returns only if refused.
extern "C" void AsteriaIa64StubInterpreterImageMain(const Asteria::Foundation::UInt64 startBlockAddress) noexcept
{
    const auto transport = Asteria::User::Platform::Ia64::System::GetIa64SystemCallTransport();
    const auto& block =
        *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::ComponentInterpreterBlockWords]>(
            static_cast<Asteria::Foundation::UIntPointer>(startBlockAddress));
    const Asteria::Foundation::UInt64 code = Asteria::User::Interpreter::RunStubInterpreter(
        transport, block, g_buffers, Asteria::User::Interpreter::Amd64GuestRules);
    const Asteria::User::System::SystemCallClient client{transport};
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
