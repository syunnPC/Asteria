// SPDX-License-Identifier: Apache-2.0
// Desktop session test media only. The existing broker/PS2 implementation runs in two domains of
// this small image; no system capacity is enlarged.
#include "User/Component/InputBroker.hpp"
#include "User/Component/Ps2Driver.hpp"
#if defined(ASTERIA_TARGET_IA64)
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"
extern "C" Asteria::Foundation::UInt64 AsteriaIa64ComponentClock() noexcept;
extern "C" void AsteriaIa64ComponentImageMain(const Asteria::Foundation::UInt64 address) noexcept
#else
#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"
extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ComponentClock() noexcept;
extern "C" void AsteriaAmd64ComponentImageMain(const Asteria::Foundation::UInt64 address) noexcept
#endif
{
    namespace Abi = Asteria::Abi;
    namespace Foundation = Asteria::Foundation;
    namespace User = Asteria::User;
#if defined(ASTERIA_TARGET_IA64)
    const User::System::SystemCallClient client{User::Platform::Ia64::System::GetIa64SystemCallTransport()};
    const auto clock = AsteriaIa64ComponentClock;
#else
    const User::System::SystemCallClient client{User::Platform::Amd64::System::GetAmd64SystemCallTransport()};
    const auto clock = AsteriaAmd64ComponentClock;
#endif
    const auto& block = *reinterpret_cast<const Foundation::UInt64(*)[Abi::ComponentBlockWords]>(
        static_cast<Foundation::UIntPointer>(address));
    const auto code = block[Abi::ComponentBehaviorWord] == Abi::ComponentBehaviorInput
                          ? User::Component::ServeInput(client, block)
                          : User::Component::ServePs2(client, block, clock);
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
