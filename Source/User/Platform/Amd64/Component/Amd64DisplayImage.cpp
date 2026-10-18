// SPDX-License-Identifier: Apache-2.0

#include "User/Component/DisplayBroker.hpp"
#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ComponentClock() noexcept;

// Root-supervised `display`, using the component entry.
extern "C" void AsteriaAmd64ComponentImageMain(const Asteria::Foundation::UInt64 address) noexcept
{
    const Asteria::User::System::SystemCallClient client{
        Asteria::User::Platform::Amd64::System::GetAmd64SystemCallTransport()};
    const auto& block = *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::ComponentBlockWords]>(
        static_cast<Asteria::Foundation::UIntPointer>(address));
    const auto code =
        block[Asteria::Abi::ComponentStartMagicWord] != Asteria::Abi::ComponentStartMagic
            ? Asteria::Abi::ComponentBadStartExitCode
            : Asteria::User::Component::ServeDisplay(client, block, block[Asteria::Abi::ComponentScanoutAddressWord],
                                                     AsteriaAmd64ComponentClock);
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
