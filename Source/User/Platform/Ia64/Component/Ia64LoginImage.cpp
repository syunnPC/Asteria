// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/LoginService.hpp"
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"
extern "C" Asteria::Foundation::UInt64 AsteriaIa64ComponentClock() noexcept;
extern "C" void AsteriaIa64ComponentImageMain(Asteria::Foundation::UInt64 address) noexcept
{
    const Asteria::User::System::SystemCallClient client{
        Asteria::User::Platform::Ia64::System::GetIa64SystemCallTransport()};
    const auto& block = *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::ComponentBlockWords]>(
        static_cast<Asteria::Foundation::UIntPointer>(address));
    const auto code = Asteria::User::Identity::ServeLogin(client, block, 0x2000'0000'0000'0000ULL);
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
