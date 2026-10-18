// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/EhciStatus.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace
{
namespace E = Asteria::User::Usb;
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
    if (size < 8U)
    {
        return 0;
    }
    const E::UInt32 token = E::Load32(data);
    const E::Size submitted = E::Load32(data + 4U);
    E::QtdResult result{.State = E::QtdState::Babble, .ActualLength = 0xA5U};
    if (!E::DecodeQtd(token, submitted, result))
    {
        Require(result.State == E::QtdState::Babble && result.ActualLength == 0xA5U);
        return 0;
    }
    Require(result.ActualLength <= submitted);
    Require(submitted <= 0x5000U);
    Require(submitted - result.ActualLength == ((token >> 16U) & 0x7FFFU));
    const E::UInt32 canonical = token & 0xFFFF7FFFU; // IOC does not affect status
    E::QtdResult again{};
    Require(E::DecodeQtd(canonical, submitted, again));
    Require(again.State == result.State && again.ActualLength == result.ActualLength);
    static_cast<void>(E::QtdStatus(result.State));
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    constexpr std::uint8_t seeds[][8U]{
        {0x80U, 1U, 8U, 0U, 8U, 0U, 0U, 0U}, {0x40U, 1U, 0U, 0U, 8U, 0U, 0U, 0U}, {0x50U, 1U, 8U, 0U, 8U, 0U, 0U, 0U},
        {0x60U, 1U, 8U, 0U, 8U, 0U, 0U, 0U}, {0x48U, 1U, 8U, 0U, 8U, 0U, 0U, 0U}, {0x44U, 1U, 8U, 0U, 8U, 0U, 0U, 0U},
        {0U, 3U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
    for (const auto& seed : seeds)
    {
        emit(seed, sizeof(seed));
    }
}
