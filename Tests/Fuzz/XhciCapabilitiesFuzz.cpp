// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Xhci.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "User/Usb/FakeXhci.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    namespace X = Asteria::User::Usb;
    Asteria::Test::Usb::Fixture fixture;
    // Capability, protocol and legacy words are byte-decoded into model MMIO.
    // Operational registers and the DMA mappings remain owned by the fixture.
    const std::size_t count = size < 128U ? size : 128U;
    for (std::size_t i{0U}; i + 4U <= count; i += 4U)
    {
        fixture.Model.Registers[i / 4U] = X::Load32(data + i);
    }
    if (fixture.Controller.Initialize(fixture.Memory, 0x2100U, 4U))
    {
        fixture.Controller.PowerPorts();
        if ((fixture.Controller.GetState() != X::ControllerState::Running) || !fixture.Controller.Halt(4U) ||
            !fixture.Model.Good)
        {
            std::abort();
        }
    }
    else if (fixture.Model.Resets != 0U)
    {
        // A controller may reject malformed capabilities or unsupported DMA
        // resources only before reset. The fixed model's reset always succeeds.
        std::abort();
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    Asteria::Test::Usb::Fixture fixture;
    std::uint8_t bytes[128U]{};
    fixture.Model.Registers[0U] = (0x96U << 16U) | Asteria::Test::Usb::FakeXhci::Operational;
    for (std::size_t i{0U}; i < sizeof(bytes); i += 4U)
    {
        Asteria::User::Usb::Store32(bytes + i, fixture.Model.Registers[i / 4U]);
    }
    emit(bytes, sizeof(bytes));
    emit(bytes, 0U);
}
