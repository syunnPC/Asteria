// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/XhciEvent.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
namespace X = Asteria::User::Usb;
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
    if (size < 16U)
    {
        return 0;
    }
    const X::Trb trb{.Parameter = X::Load64(data), .Status = X::Load32(data + 8U), .Control = X::Load32(data + 12U)};
    X::XhciEvent output{.Kind = X::EventKind::Controller,
                        .Pointer = 0xA5U,
                        .Residual = 0xA5U,
                        .Code = 0xA5U,
                        .Slot = 0xA5U,
                        .Endpoint = 0xA5U,
                        .Port = 0xA5U};
    const auto before = output;
    if (!X::DecodeEvent(trb, output))
    {
        Require(std::memcmp(&output, &before, sizeof(output)) == 0);
        return 0;
    }
    Require(output.Code != 0U);
    X::Trb canonical{.Parameter = output.Pointer,
                     .Status = (X::UInt32{output.Code} << 24U) | output.Residual,
                     .Control = (X::UInt32{static_cast<X::UInt8>(output.Kind)} << 10U) |
                                (X::UInt32{output.Slot} << 24U) | (X::UInt32{output.Endpoint} << 16U)};
    X::XhciEvent again{};
    Require(X::DecodeEvent(canonical, again));
    Require((again.Kind == output.Kind) && (again.Pointer == output.Pointer) && (again.Residual == output.Residual) &&
            (again.Code == output.Code) && (again.Slot == output.Slot) && (again.Endpoint == output.Endpoint) &&
            (again.Port == output.Port));
    static_cast<void>(X::CompletionStatus(output.Code));
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr std::uint8_t seeds[][16U]{
        {0x00U, 0x10U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0x80U, 3U, 1U},
        {0x00U, 0x10U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0x84U, 0U, 1U},
        {0U, 0U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 0x88U, 0U, 0U},
        {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 21U, 1U, 0x94U, 0U, 0U},
    };
    for (const auto& seed : seeds)
    {
        emit(seed, sizeof(seed));
    }
}
