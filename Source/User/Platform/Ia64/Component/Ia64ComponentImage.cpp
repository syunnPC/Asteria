// SPDX-License-Identifier: Apache-2.0

#include "User/Component/ComponentProgram.hpp"
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"

extern "C" void AsteriaIa64ComponentCrash(Asteria::Foundation::UInt64 kind) noexcept;
extern "C" Asteria::Foundation::UInt64 AsteriaIa64ComponentClock() noexcept;

// The root service passes the start block's address in r4
// (Ia64ComponentImage.S); ThreadExit returns only if refused.
extern "C" void AsteriaIa64ComponentImageMain(const Asteria::Foundation::UInt64 startBlockAddress) noexcept
{
    const auto transport = Asteria::User::Platform::Ia64::System::GetIa64SystemCallTransport();
    const auto& startBlock = *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::ComponentBlockWords]>(
        static_cast<Asteria::Foundation::UIntPointer>(startBlockAddress));
    const Asteria::Foundation::UInt64 code = Asteria::User::Component::RunComponent(
        transport, startBlock, AsteriaIa64ComponentClock, AsteriaIa64ComponentCrash);
    const Asteria::User::System::SystemCallClient client{transport};
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
