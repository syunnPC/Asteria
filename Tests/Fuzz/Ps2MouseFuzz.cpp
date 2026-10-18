// SPDX-License-Identifier: Apache-2.0
#include "User/Input/Ps2Mouse.hpp"

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
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) noexcept
{
    In::Ps2Mouse mouse{2U};
    In::EventBatch output{};
    for (In::Size mode = 0U; mode < 2U; ++mode)
    {
        mouse.Reset(mode == 0U ? In::MouseMode::Standard : In::MouseMode::IntelliMouse);
        for (std::size_t i = 0U; (i < size) && (i < 4096U); ++i)
        {
            const auto result = mouse.Feed(data[i], output);
            Require(output.Count <= 3U);
            if ((result == In::MouseResult::Pending) || (result == In::MouseResult::Unsynchronised))
            {
                Require(output.Count == 0U);
            }
            for (In::Size at = 0U; at < output.Count; ++at)
            {
                const auto& event = output.Events[at];
                Require(event.Source == 2U);
                switch (event.Kind)
                {
                case In::EventKind::Motion:
                    Require((result != In::MouseResult::Overflow) && ((event.Dx != 0) || (event.Dy != 0)));
                    Require((event.Dx >= -256) && (event.Dx <= 255) && (event.Dy >= -255) && (event.Dy <= 256));
                    break;
                case In::EventKind::Buttons:
                    Require(event.Buttons <= 7U);
                    break;
                case In::EventKind::Wheel:
                    Require((mode == 1U) && (event.Wheel != 0) && (event.Wheel >= -127) && (event.Wheel <= 128));
                    break;
                case In::EventKind::KeyDown:
                case In::EventKind::KeyUp:
                    Require(false);
                    break;
                }
            }
        }
    }
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr std::uint8_t bytes[]{0U, 0x09U, 5U, 3U, 0x38U, 0U, 0U, 0xC8U, 0xFFU, 0xFFU, 8U, 0U, 0U, 0x80U};
    emit(bytes, sizeof(bytes));
    emit(bytes, 0U);
}
