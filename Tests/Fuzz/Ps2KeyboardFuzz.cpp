// SPDX-License-Identifier: Apache-2.0
#include "User/Input/Ps2Keyboard.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace
{
namespace In = Asteria::User::Input;
void Require(const bool good) noexcept
{
    if (!good)
    {
        std::abort();
    }
}

// The same properties hold for both decodings of the same bytes.
void Check(const std::uint8_t* data, const std::size_t size, const In::ScanCodeSet set) noexcept
{
    In::Ps2Keyboard keyboard{1U, In::KeyboardGeometry::Ansi, set};
    bool down[256U]{};
    for (std::size_t i = 0U; (i < size) && (i < 4096U); ++i)
    {
        const auto result = keyboard.Feed(data[i]);
        if (result.Result == In::ScanResult::Event)
        {
            const auto& event = result.Event;
            Require((event.Usage > 0U) && (event.Usage < 256U) && (event.Source == 1U));
            const bool pressed = event.Kind == In::EventKind::KeyDown;
            Require(pressed || (event.Kind == In::EventKind::KeyUp));
            // Make-only Pause/Korean language keys have no pressed snapshot.
            if ((event.Usage != 0x48U) && (event.Usage != 0x90U) && (event.Usage != 0x91U))
            {
                Require(down[event.Usage] != pressed);
                down[event.Usage] = pressed;
            }
            Require(event.Modifiers == keyboard.GetModifiers());
        }
        In::UInt8 modifiers = 0U;
        for (In::UInt8 bit = 0U; bit < 8U; ++bit)
        {
            if (keyboard.IsPressed(static_cast<In::UInt8>(0xE0U + bit)))
            {
                modifiers = static_cast<In::UInt8>(modifiers | (1U << bit));
            }
        }
        Require(modifiers == keyboard.GetModifiers());
    }
    keyboard.Reset();
    Require(keyboard.GetModifiers() == 0U);
    for (In::Size i = 0U; i < 256U; ++i)
    {
        Require(!keyboard.IsPressed(static_cast<In::UInt8>(i)));
    }
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    Check(data, size, In::ScanCodeSet::Set2);
    Check(data, size, In::ScanCodeSet::Set1);
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr std::uint8_t bytes[]{0x1CU, 0x1CU, 0xF0U, 0x1CU, 0xE0U, 0x12U, 0xE0U, 0x7CU, 0xE0U, 0xF0U, 0x7CU,
                                   0xE0U, 0xF0U, 0x12U, 0xE1U, 0x14U, 0x77U, 0xE1U, 0xF0U, 0x14U, 0xF0U, 0x77U};
    emit(bytes, sizeof(bytes));
    emit(bytes, 0U);
    // Set 1: a make/break, Print Screen with its fake shifts, Pause.
    constexpr std::uint8_t set1[]{0x1EU, 0x9EU, 0xE0U, 0x2AU, 0xE0U, 0x37U, 0xE0U, 0xB7U, 0xE0U,
                                  0xAAU, 0xE1U, 0x1DU, 0x45U, 0xE1U, 0x9DU, 0xC5U, 0x2AU, 0xAAU};
    emit(set1, sizeof(set1));
}
