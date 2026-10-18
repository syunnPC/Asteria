// SPDX-License-Identifier: Apache-2.0
#include "User/Input/HidBoot.hpp"

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
    In::HidBoot hid{3U};
    In::EventBatch output{};
    In::EventBatch repeated{};
    for (In::Size at = 0U; (at < size) && (at < 4096U); at += 8U)
    {
        const In::Size length = size - at < 8U ? size - at : 8U;
        const auto result = hid.Keyboard(data + at, length, output);
        Require(output.Count <= 20U);
        if (result != In::ReportResult::Accepted)
        {
            Require(output.Count == 0U);
        }
        else
        {
            Require(hid.Keyboard(data + at, length, repeated) == In::ReportResult::Accepted);
            Require(repeated.Count == 0U);
            for (In::Size i = 0U; i < output.Count; ++i)
            {
                Require((output.Events[i].Kind == In::EventKind::KeyDown) ||
                        (output.Events[i].Kind == In::EventKind::KeyUp));
                Require(output.Events[i].Modifiers == data[at]);
                for (In::Size j = 0U; j < i; ++j)
                {
                    Require(output.Events[j].Usage != output.Events[i].Usage);
                }
            }
        }
        static_cast<void>(hid.Mouse(data + at, length, output));
        Require(output.Count <= 3U);
    }
    static_cast<void>(hid.Keyboard(data, size, output));
    static_cast<void>(hid.Mouse(data, size, output));
    return 0;
}

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr std::uint8_t bytes[]{2U, 0U, 4U, 0U, 0U, 0U, 0U, 0U, 0xFFU, 0U, 1U, 1U,
                                   1U, 1U, 1U, 1U, 0U, 0U, 0U, 0U, 0U,    0U, 0U, 0U};
    emit(bytes, sizeof(bytes));
    emit(bytes, 0U);
}
