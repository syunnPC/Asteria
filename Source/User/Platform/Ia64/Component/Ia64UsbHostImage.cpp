// SPDX-License-Identifier: Apache-2.0

#include "User/Component/UsbHostDriver.hpp"
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ComponentClock() noexcept;

// The `usb` module is linked
// with the component image's script and entry (Ia64ComponentImage.S), which
// pass the start block's address in r4; ThreadExit returns only if refused.
extern "C" void AsteriaIa64ComponentImageMain(const Asteria::Foundation::UInt64 startBlockAddress) noexcept
{
    const auto transport = Asteria::User::Platform::Ia64::System::GetIa64SystemCallTransport();
    const auto& startBlock = *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::ComponentBlockWords]>(
        static_cast<Asteria::Foundation::UIntPointer>(startBlockAddress));
    const Asteria::Foundation::UInt64 code =
        Asteria::User::Component::RunUsbHostComponent(transport, startBlock, AsteriaIa64ComponentClock);
    const Asteria::User::System::SystemCallClient client{transport};
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
