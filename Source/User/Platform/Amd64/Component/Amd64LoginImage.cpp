// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/LoginService.hpp"
#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"
extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ComponentClock() noexcept;
extern "C" void AsteriaAmd64ComponentImageMain(Asteria::Foundation::UInt64 address) noexcept
{
    const Asteria::User::System::SystemCallClient client{
        Asteria::User::Platform::Amd64::System::GetAmd64SystemCallTransport()};
    const auto& block = *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::ComponentBlockWords]>(
        static_cast<Asteria::Foundation::UIntPointer>(address));
    const auto code = Asteria::User::Identity::ServeLogin(client, block);
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
