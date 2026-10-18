// SPDX-License-Identifier: Apache-2.0

#include "User/Client/ClientProgram.hpp"
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"

namespace
{
// The client's messages live in.bss (the image has no initialized data).
constinit Asteria::User::Client::ClientBuffers g_buffers{};
} // Namespace

// The root service passes the start block's address in r4
// (Ia64ClientImage.S); ThreadExit returns only if refused.
extern "C" void AsteriaIa64ClientImageMain(const Asteria::Foundation::UInt64 startBlockAddress) noexcept
{
    const auto transport = Asteria::User::Platform::Ia64::System::GetIa64SystemCallTransport();
    const auto& startBlock =
        *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::ComponentStartBlockWords]>(
            static_cast<Asteria::Foundation::UIntPointer>(startBlockAddress));
    const Asteria::Foundation::UInt64 code = Asteria::User::Client::RunClient(transport, startBlock, g_buffers);
    const Asteria::User::System::SystemCallClient client{transport};
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
