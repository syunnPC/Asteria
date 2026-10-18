// SPDX-License-Identifier: Apache-2.0
#include "User/System/XhciQuiesce.hpp"
#include "User/Usb/XhciMemory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    alignas(8) Asteria::Foundation::UInt32 registers[1024U]{};
    const std::size_t bytes = size < sizeof(registers) ? size : sizeof(registers);
    for (std::size_t index{0U}; index + 4U <= bytes; index += 4U)
    {
        registers[index / 4U] = Asteria::User::Usb::Load32(data + index);
    }
    Asteria::User::System::XhciQuiesceState state{};
    static_cast<void>(Asteria::User::System::BeginXhciQuiesce(registers, state, sizeof(registers)));
    for (std::size_t step{0U}; step < 3U; ++step)
    {
        const bool quiet = Asteria::User::System::PollXhciQuiesce(registers, state);
        if (quiet &&
            (((registers[state.Operational] & 3U) != 0U) || ((registers[state.Operational + 1U] & 0x801U) != 1U)))
        {
            std::abort();
        }
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    std::uint8_t bytes[128U]{};
    Asteria::User::Usb::Store32(bytes, 0x0096'0020U);
    Asteria::User::Usb::Store32(bytes + 32U, 5U);
    Asteria::User::Usb::Store32(bytes + 36U, 1U);
    emit(bytes, sizeof(bytes));
    Asteria::User::Usb::Store32(bytes + 16U, 16U << 16U);
    Asteria::User::Usb::Store32(bytes + 64U, 1U | 0x10000U);
    emit(bytes, sizeof(bytes));
    Asteria::User::Usb::Store32(bytes + 16U, 0xFFFF0000U);
    emit(bytes, sizeof(bytes));
}
