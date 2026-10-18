// SPDX-License-Identifier: Apache-2.0

#include "User/Component/UsbHostDriver.hpp"
#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ComponentClock() noexcept;

// The `usb` module is linked
// with the component image's script and entry (Amd64ComponentImage.S), which
// pass the start block's address in RDI; ThreadExit returns only if refused.
extern "C" void AsteriaAmd64ComponentImageMain(const Asteria::Foundation::UInt64 startBlockAddress) noexcept
{
    const auto transport = Asteria::User::Platform::Amd64::System::GetAmd64SystemCallTransport();
    const auto& startBlock = *reinterpret_cast<const Asteria::Foundation::UInt64(*)[Asteria::Abi::ComponentBlockWords]>(
        static_cast<Asteria::Foundation::UIntPointer>(startBlockAddress));
    const Asteria::Foundation::UInt64 code =
        Asteria::User::Component::RunUsbHostComponent(transport, startBlock, AsteriaAmd64ComponentClock);
    const Asteria::User::System::SystemCallClient client{transport};
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
